/* MCVT - VT100 terminal for the Psion MC400 (native WIMP application).
 *
 * Copyright (C) 2026 Christian Farrow (zedstarr)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 * VT100 engine in VT.C. Serial input on TTY:A or TTY:B through an OLIB
 * active object scheduled by the WIMP app manager.
 * Menus: File (Clear screen, Reset terminal, Exit),
 *        Options (Serial - the ROM's serial options dialog,
 *                 Terminal - local echo / Backspace / auto wrap).
 * Settings are kept in LOC::M:\MCVT.INI. Resources come from mkrsc.py.
 */
#include <mcvt.g>
#include <wlib.h>
#include <p_serial.h>
#include "vt.h"

GLREF_D VOID *w_ws;
GLREF_D VOID *w_am;
LOCAL_D VOID *mainwin;

#ifndef PRIORITY_ACTIVE_WSERV
#define PRIORITY_ACTIVE_WSERV 60
#endif

/* resource ids (mkrsc.py) */
#define R_TITLE 1
#define R_MENUBAR 2
#define R_TERMDLG 5

#define O_AM_DIALOG 15        /* WIMP app manager: create and show a dialog */
#define O_WS_SERIALDLG 25     /* WIMP ws client: ROM "Serial options" dialog */
#define O_DLG_SET 31          /* dialog: set control value */
#define O_DLG_GET 32          /* dialog: read control value */

#define INIFILE "LOC::M:\\MCVT.INI"
#define LOGFILE "LOC::M:\\MCVT.LOG"
#define INI_MAGIC 0x564d      /* "MV" */
#define INI_VERSION 2

typedef struct
    {
    UWORD magic, version;
    P_SRCHAR ser;
    UBYTE port;               /* 'A' or 'B' */
    UBYTE echo;               /* local echo */
    UBYTE bsctlh;             /* Backspace sends ^H */
    UBYTE wrap;               /* auto wrap */
    UBYTE utf8;               /* UTF-8, else ISO 8859-1 */
    UBYTE spare;
    } SETTINGS;

LOCAL_D SETTINGS set;

LOCAL_D VOID *serh;            /* serial handle, NULL = not open */
LOCAL_D VOID *serobj;          /* active object waiting for serial input */
LOCAL_D UBYTE rxbuf[256];
LOCAL_D UINT rxlen;
LOCAL_D INT rxerrs;
LOCAL_D INT openerr;

/* ---- debug log ---- */
LOCAL_C INT putnum(TEXT *p, INT v)
    {
    TEXT t[8];
    INT n = 0, i = 0;
    UINT u;
    if (v < 0) { p[i++] = '-'; u = (UINT)(-v); } else u = (UINT)v;
    do { t[n++] = (TEXT)('0' + u % 10); u /= 10; } while (u);
    while (n) p[i++] = t[--n];
    return(i);
    }
#ifndef MCVT_DEBUG
#define MCVT_DEBUG 0          /* 1: write LOC::M:MCVT.LOG */
#endif
GLDEF_C VOID dlog(TEXT *s, INT a, INT b)
    {
    TEXT buf[100];
    INT n = 0;
    VOID *h;
    if (!MCVT_DEBUG) return;
    while (s[n] && n < 70) { buf[n] = s[n]; n++; }
    buf[n++] = ' '; n += putnum(buf + n, a);
    buf[n++] = ' '; n += putnum(buf + n, b);
    if (p_open(&h, LOGFILE, P_FAPPEND | P_FTEXT | P_FUPDATE) >= 0)
        { p_write(h, buf, n); p_close(h); }
    }

/* ---- settings ---- */
LOCAL_C VOID defaults(VOID)
    {
    p_bfil(&set, sizeof(set), 0);
    set.magic = INI_MAGIC; set.version = INI_VERSION;
    set.ser.tbaud = set.ser.rbaud = P_BAUD_9600;
    set.ser.frame = P_DATA_8;
    set.ser.hand = P_IGN_CTS | P_OBEY_XOFF | P_SEND_XOFF;   /* XON/XOFF, as TERM.APP */
    set.ser.xon = 0x11; set.ser.xoff = 0x13;
    set.port = 'A';
    set.wrap = 1;
    set.utf8 = 1;
    }

LOCAL_C VOID loadset(VOID)
    {
    VOID *h;
    SETTINGS s;
    defaults();
    if (p_open(&h, INIFILE, P_FOPEN | P_FSTREAM) < 0) return;
    if (p_read(h, &s, sizeof(s)) == sizeof(s) && s.magic == INI_MAGIC && s.version == INI_VERSION)
        set = s;
    p_close(h);
    if (set.port != 'A' && set.port != 'B') set.port = 'A';
    }

LOCAL_C VOID saveset(VOID)
    {
    VOID *h;
    INT r;
    r = p_open(&h, INIFILE, P_FREPLACE | P_FSTREAM | P_FUPDATE);
    if (r >= 0)
        {
        r = p_write(h, &set, sizeof(set));
        p_close(h);
        }
    dlog("settings saved", r, 0);
    }

LOCAL_C VOID applyterm(VOID)
    {
    vt_bs_ctlh = set.bsctlh;
    vt_autowrap = set.wrap;
    vt_utf8 = set.utf8;
    }

/* ---- serial ---- */
GLDEF_C VOID ser_send(UBYTE *buf, INT len)
    {
    if (serh)
        p_write(serh, buf, len);
    }

LOCAL_C WORD *serstat(VOID)
    {
    return((WORD *)((UBYTE *)serobj + 0x0c));         /* active.stat */
    }

LOCAL_C VOID ser_queue(VOID)
    {
    WORD *stat = serstat();
    *stat = E_FILE_PENDING;
    rxlen = 1;
    p_ioa(serh, P_FREAD, stat, &rxbuf[0], &rxlen);
    *((UBYTE *)serobj + 9) = 1;                       /* active.isactive */
    }

/* stop the pending read (P_FSET with a read outstanding panics) */
LOCAL_C VOID ser_stopread(VOID)
    {
    if (!serh || !serobj || !*((UBYTE *)serobj + 9)) return;
    p_iow(serh, P_FCANCEL);
    p_waitstat(serstat());
    *((UBYTE *)serobj + 9) = 0;
    }

LOCAL_C VOID ser_close(VOID)
    {
    ser_stopread();
    if (serh) p_close(serh);
    serh = NULL;
    }

LOCAL_D TEXT portname[] = "TTY:A";

LOCAL_C VOID ser_open(VOID)
    {
    INT r;
    portname[4] = set.port;
    openerr = p_open(&serh, portname, -1);
    dlog(portname, openerr, 0);
    if (openerr < 0) { serh = NULL; return; }
    r = p_iow(serh, P_FSET, &set.ser);
    dlog("P_FSET", r, 0);
    }

/* (re)start reading; creates the active object the first time */
LOCAL_C VOID ser_start(VOID)
    {
    if (!serh) return;
    if (!serobj)
        {
        serobj = p_new(CAT_MCVT_MCVT, C_MCVTSER);
        *((BYTE *)serobj + 8) = PRIORITY_ACTIVE_WSERV - 1;   /* active.priority */
        p_send3(w_am, O_AM_ADD_TASK, serobj);
        }
    rxerrs = 0;
    ser_queue();
    }

LOCAL_C TEXT *baudtext(INT code)
    {
    static TEXT *t[] = {"?", "50", "75", "110", "134", "150", "300", "600", "1200",
        "1800", "2000", "2400", "3600", "4800", "7200", "9600", "19200"};
    return(code >= 1 && code <= 16 ? t[code] : t[0]);
    }

/* one line describing the connection, through the emulator */
LOCAL_C VOID status(VOID)
    {
    TEXT b[90];
    INT n;
    p_scpy(b, "\r\n[MCVT: ");
    p_scat(b, portname);
    n = p_slen(b);
    if (serh)
        {
        b[n++] = ' ';
        p_scpy(b + n, baudtext(set.ser.tbaud)); n = p_slen(b);
        b[n++] = ' ';
        b[n++] = (TEXT)('5' + (set.ser.frame & P_DATA_FRM));
        b[n++] = (TEXT)((set.ser.frame & P_PARITY) ? (set.ser.parity == P_PAR_ODD ? 'O' :
                                                    set.ser.parity == P_PAR_EVEN ? 'E' :
                                                    set.ser.parity == P_PAR_MARK ? 'M' : 'S') : 'N');
        b[n++] = (TEXT)((set.ser.frame & P_TWOSTOP) ? '2' : '1');
        b[n] = 0;
        if (set.ser.hand & (P_OBEY_XOFF | P_SEND_XOFF)) p_scat(b, " XON/XOFF");
        if (!(set.ser.hand & P_IGN_CTS)) p_scat(b, " RTS/CTS");
        }
    else
        {
        p_scpy(b + n, " open failed ("); n = p_slen(b);
        n += putnum(b + n, openerr);
        b[n] = 0;
        p_scat(b, ")");
        }
    if (set.echo) p_scat(b, ", local echo");
    p_scat(b, "]\r\n");
    vt_puts(b);
    }

/* ---- font: monospaced, 80 columns must fit ---- */
LOCAL_D UINT probe_h;
LOCAL_D G_FONT_INFO *probe_fi;
LOCAL_C INT probe(VOID)
    {
    return(gFontInfo(probe_h, G_STY_NORMAL, probe_fi));
    }

LOCAL_C VOID choosefont(VOID)
    {
    INT i, w, best = -1, bestscore = -1, score, mono;
    UINT h;
    G_FONT_INFO f, fi;
    /* the system font and the three ROM fonts (SMALL, BIG, MONO). Asking
     * about a font that does not exist returns an error on v2.60F but kills
     * the process on v2.12F (MC200), so go no further. */
    for (i = 0; i < 4; i++)
        {
        h = (i == 0) ? WS_FONT_SYSTEM : WS_FONT_BASE + i - 1;
        probe_h = h; probe_fi = &f;
        if (p_enter1(probe) < 0) continue;
        mono = gTextWidth(h, G_STY_NORMAL, "iiii", 4) == gTextWidth(h, G_STY_NORMAL, "WWWW", 4);
        w = gTextWidth(h, G_STY_NORMAL, "W", 1);
        score = (mono ? 1000 : 0) + (w * 80 <= 620 ? 500 : 0) + f.height * 10 - w;
        if (score > bestscore) { bestscore = score; best = i; fi = f; vt_font = h; vt_cw = mono ? w : f.max_width; }
        }
    if (best < 0) { vt_font = WS_FONT_SYSTEM; probe_h = vt_font; probe_fi = &fi; p_enter1(probe); vt_cw = fi.max_width; }
    vt_ch = fi.height;
    vt_asc = fi.ascent;
    vt_cols = 620 / vt_cw;
    if (vt_cols > VT_COLS) vt_cols = VT_COLS;
    }

GLDEF_C VOID main(VOID)
    {
    VOID *am;
    INT r;
    VOID *h;
    if (MCVT_DEBUG && p_open(&h, LOGFILE, P_FREPLACE | P_FTEXT | P_FUPDATE) >= 0) p_close(h);
    dlog("MCVT start", 0, 0);
    p_linklib(0);
    loadset();
    applyterm();
    vt_reset();
    ser_open();
    am = p_new(CAT_MCVT_WIMP, C_W69);
    r = p_send5(am, O_AM_INIT, 0x801F, p_getlibh(CAT_MCVT_MCVT), 0);
    dlog("am_init ret", r, 0);
    if (r == 0)
        {
        ser_start();
        p_send2(w_am, O_AM_START);
        }
    }

#pragma METHOD_CALL

/* ---- serial input: active object run when the read completes ---- */
METHOD INT mcvtser_ao_run(PR_MCVTSER *self)
    {
    WORD stat = *(WORD *)((UBYTE *)self + 0x0c);
    UINT more;
    if (stat < 0)
        {
        if (++rxerrs < 8) dlog("serial read error", stat, rxerrs);
        if (rxerrs > 50) return(1);           /* give up: don't spin */
        }
    else
        {
        rxerrs = 0;
        more = 0;
        p_iow(serh, P_FTEST, &more);
        if (more > 0)
            {
            if (more > sizeof(rxbuf) - 1) more = sizeof(rxbuf) - 1;
            if (p_iow(serh, P_FREAD, &rxbuf[1], &more) < 0) more = 0;
            }
        vt_write(rxbuf, more + 1);
        }
    ser_queue();
    return(1);
    }

/* ---- window server client ---- */
METHOD VOID mcvtws_w68_m30(PR_MCVTWS *self)
    {
    UWORD in[4];
    VOID *win;
    in[0] = (UWORD)p_getlibh(CAT_MCVT_MCVT);
    in[1] = C_MCVTCOM;
    in[2] = R_MENUBAR;
    in[3] = 0;
    p_send3(self, O_AO_INIT, &in[0]);
    choosefont();
    vt_puts("\033[1mMCVT\033[0m - VT100 terminal for the Psion MC400");
    status();
    win = p_new(CAT_MCVT_MCVT, C_MCVTWIN);
    p_send2(win, O_W14_M30);
    wClientPosition(0, 0);
    p_send3(*(VOID **)((UBYTE *)win + 0x0e), O_W0_M9, 1);   /* frame visible */
    p_send3(win, O_W0_M9, 1);                               /* client visible */
    vt_wid = *(UINT *)((UBYTE *)win + 4);
    wFlush();
    }

METHOD VOID mcvtws_w68_m29(PR_MCVTWS *self)
    {
    p_supersend2(self, O_W68_M29);
    if (mainwin)
        {
        VOID *top = *(VOID **)((UBYTE *)mainwin + 0x0e);
        if (top) p_send3(top, O_W0_M9, (*((UBYTE *)self + 0x3c) & 2) ? 0 : 1);
        }
    }

/* ---- main window ---- */
METHOD VOID mcvtwin_w14_m30(PR_MCVTWIN *self)
    {
    UWORD loc[4];
    UWORD cfg[6];
    P_EXTENT ext;
    INT w, h;
    loc[0] = loc[1] = loc[2] = loc[3] = 0;
    cfg[0] = 0x2434; cfg[1] = 0; cfg[2] = 0; cfg[3] = 0; cfg[4] = R_TITLE;
    cfg[5] = (UWORD)self;
    p_supersend5(self, O_W14_M30, &loc[2], &loc[0], &cfg[0]);
    w = vt_cols * vt_cw + 4 + 10;           /* frame adds 10 x 28 */
    h = vt_rows * vt_ch + 4 + 28;
    if (w > 640) w = 640;
    if (h > 380) h = 380;
    ext.tl.x = (640 - w) / 2;
    ext.tl.y = 20;
    ext.width = w; ext.height = h;
    p_send3(self, O_W0_M14, &ext);
    *(VOID **)((UBYTE *)w_ws + 0x38) = self;   /* focus window (m33) */
    *(VOID **)((UBYTE *)w_ws + 0x2e) = self;   /* key target (m34) */
    mainwin = self;
    p_send3(self, O_W14_M37, 0);
    }

METHOD VOID mcvtwin_w0_m23(PR_MCVTWIN *self)
    {
    P_RECT r;
    p_supersend2(self, O_W0_M23);
    r.tl.x = 0; r.tl.y = 0;
    r.br.x = *(INT *)((UBYTE *)self + 6);
    r.br.y = *(INT *)((UBYTE *)self + 8);
    gClrRect(&r, G_TRMODE_CLR);
    vt_redraw();
    }

/* frame resized: lay out the client, then fit the terminal to it */
METHOD VOID mcvtwin_w18_m44(PR_MCVTWIN *self, INT *rect)
    {
    INT w, h;
    p_supersend3(self, O_W18_M44, rect);
    w = *(INT *)((UBYTE *)self + 6);
    h = *(INT *)((UBYTE *)self + 8);
    if (w > 0 && h > 0 && vt_wid)
        vt_resize((w - 4) / vt_cw, (h - 4) / vt_ch);
    }

METHOD VOID mcvtwin_w14_m33(PR_MCVTWIN *self, UWORD *st)
    {
    }

/* key press */
METHOD INT mcvtwin_w14_m34(PR_MCVTWIN *self, INT key, INT mods)
    {
    UBYTE out[8];
    INT n;
    if (mods & 0x08)
        return(p_supersend4(self, O_W14_M34, key, mods));
    n = vt_key(key, mods, out);
    if (n == 0)
        return(p_supersend4(self, O_W14_M34, key, mods));
    ser_send(out, n);
    if (set.echo || !serh)
        {
        vt_write(out, n);                    /* local echo */
        if (out[0] == 13 && n == 1) vt_write((UBYTE *)"\n", 1);
        }
    return(1);
    }

/* ---- menu commands (methods of the command manager) ---- */

/* File|Exit (also the method the system "Stop" item uses) */
METHOD VOID mcvtcom_w66_m8(PR_MCVTCOM *self)
    {
    dlog("Exit", 0, 0);
    ser_close();
    p_exit(0);
    }

/* File|Clear screen */
METHOD VOID mcvtcom_mcvtcom_clear(PR_MCVTCOM *self)
    {
    vt_puts("\033[H\033[2J");
    ser_send((UBYTE *)"\014", 1);           /* Ctrl-L: ask the host to redraw */
    }

/* File|Reset terminal */
METHOD VOID mcvtcom_mcvtcom_reset(PR_MCVTCOM *self)
    {
    vt_puts("\033c");
    ser_send((UBYTE *)"\014", 1);
    }

/* Options|Serial: the ROM's "Serial options" dialog (S_.RSC resource 10).
 * Block: +0 result (2 = ENTER), +2 P_SRCHAR, +0x0e port name "TTY:A",
 * +0x14 port locked flag, +0x16 mask of disabled baud rates. */
METHOD VOID mcvtcom_mcvtcom_serial(PR_MCVTCOM *self)
    {
    UBYTE blk[0x18];
    INT r;
    p_bfil(blk, sizeof(blk), 0);
    p_bcpy(blk + 2, &set.ser, sizeof(P_SRCHAR));
    p_scpy((TEXT *)blk + 0x0e, "TTY:A");
    blk[0x12] = set.port;
    r = p_send3(w_ws, O_WS_SERIALDLG, blk);
    dlog("serial dialog", r, *(WORD *)blk);
    if (!r) return;
    ser_close();
    p_bcpy(&set.ser, blk + 2, sizeof(P_SRCHAR));
    if (blk[0x12] == 'A' || blk[0x12] == 'B') set.port = blk[0x12];
    saveset();
    ser_open();
    ser_start();
    status();
    }

/* Options|Terminal: own dialog (resource 5) */
METHOD VOID mcvtcom_mcvtcom_term(PR_MCVTCOM *self)
    {
    UWORD a[3];
    a[0] = R_TERMDLG;
    a[1] = (UWORD)&set;
    a[2] = 0;
    p_send5(w_am, O_AM_DIALOG, p_getlibh(CAT_MCVT_MCVT), C_MCVTDLG, a);
    }

/* ---- Terminal options dialog ---- */
#define D_ECHO 3
#define D_BS 4
#define D_WRAP 5
#define D_UTF8 6

LOCAL_C VOID cbset(VOID *dlg, INT id, INT on)
    {
    UWORD v[2];
    v[0] = 0;
    v[1] = on ? 0x100 : 0;
    p_send5(dlg, O_DLG_SET, id, 0x100, v);
    }

LOCAL_C INT cbget(VOID *dlg, INT id)
    {
    UWORD v[4];
    v[1] = 0;
    p_send4(dlg, O_DLG_GET, id, v);
    return((v[1] & 0x100) != 0);
    }

/* fill the controls */
METHOD VOID mcvtdlg_w31_m62(PR_MCVTDLG *self)
    {
    cbset(self, D_ECHO, set.echo);
    cbset(self, D_BS, set.bsctlh);
    cbset(self, D_WRAP, set.wrap);
    cbset(self, D_UTF8, set.utf8);
    }

/* control activated: 1 = CANCEL, 2 = ENTER (both close it), else a control id */
METHOD VOID mcvtdlg_w31_m63(PR_MCVTDLG *self, INT button)
    {
    INT wrap;
    if (button == 2)
        {
        set.echo = (UBYTE)cbget(self, D_ECHO);
        set.bsctlh = (UBYTE)cbget(self, D_BS);
        wrap = cbget(self, D_WRAP);
        if (wrap != set.wrap)
            vt_puts(wrap ? "\033[?7h" : "\033[?7l");
        set.wrap = (UBYTE)wrap;
        set.utf8 = (UBYTE)cbget(self, D_UTF8);
        applyterm();
        saveset();
        }
    if (button == 1 || button == 2)          /* other ids: a checkbox was toggled */
        p_supersend3(self, O_W31_M63, button);
    }

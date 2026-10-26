/* VT.C - VT100 emulation engine for MCVT, the MC400 WIMP terminal.
 *
 * Copyright 1993-1996 Rudolf Koenig, Michael Schroeder, Juergen Weigert
 *   (vt100.c in nfsc/p3nfs, https://github.com/thelastpsion/nfsc)
 * Copyright (C) 2026 Christian Farrow (zedstarr)
 *
 * This file is a modified version of nfsc's vt100.c, distributed under
 * the GNU General Public License version 2 (see LICENSE). Changes, 2026:
 * reworked for the MC400 WIMP window server with a character/attribute
 * buffer, redraw, jump scrolling and window resize; xterm/ECMA-48 extras
 * (OSC/DCS strings, REP, CNL/CPL, alternate screen, size reports);
 * UTF-8 decoding; keyboard mapping for the MC400.
 *
 * The escape-sequence parser follows the vt100 emulator in nfsc.
 * Unlike the Series 3 original, which drew straight to the screen, this
 * version keeps a character/attribute buffer so that the window can be
 * redrawn after it is uncovered, moved, resized or brought to the front,
 * since MC400 windows are not backed up by the window server.
 */
#include <p_std.h>
#include <wlib.h>
#include "vt.h"
#include "cnv.h"

GLDEF_D INT vt_cols = VT_COLS, vt_rows = VT_ROWS;
GLDEF_D INT vt_cw = 7, vt_ch = 10, vt_asc = 8;
GLDEF_D UINT vt_font;
GLDEF_D UINT vt_wid;
GLDEF_D INT vt_appcur;
GLDEF_D INT vt_bs_ctlh;
GLDEF_D INT vt_autowrap = 1;
GLDEF_D INT vt_utf8 = 1;
GLDEF_D INT vt_altscr;               /* a full-screen program has the alternate screen */

#define X0 2
#define Y0 2
#define A_BOLD G_STY_BOLD           /* 1 */
#define A_UNDER G_STY_UNDERLINE     /* 2 */
#define A_INV G_STY_INVERSE         /* 4 */

LOCAL_D TEXT txt[VT_MAXROWS][VT_MAXCOLS];
LOCAL_D UBYTE att[VT_MAXROWS][VT_MAXCOLS];
LOCAL_D UBYTE dirty[VT_MAXROWS];

#define MAXARGS 10
#define E_NORMAL 255
#define E_ESCAPE 254
#define E_STRING 253          /* inside OSC/DCS/APC/PM/SOS: ignored */
#define E_STRESC 252          /* ESC seen inside a string (ST = ESC \\) */
#define F_WRAP 0x01
#define F_ORIGIN 0x02
#define F_INSERT 0x04
#define F_SCRINV 0x08
#define F_CURSOR 0x10              /* cursor visible (DECTCEM) */

LOCAL_D INT cx, cy, sx, sy, sattr, sr_u, sr_l;
LOCAL_D UBYTE attr;
LOCAL_D UBYTE lastch = ' ';   /* last printed character, for REP */
LOCAL_D UBYTE e_state = E_NORMAL, e_interm, e_flags;
LOCAL_D UINT e_args[MAXARGS];
LOCAL_D UBYTE tabs[VT_MAXCOLS + 1];
LOCAL_D UBYTE gl, gset, charsets[4];
LOCAL_D INT curshown, curx, cury;
LOCAL_D INT ingc;                  /* a GC for our window is current */

/* DEC special graphics 0x5f..0x7e -> code page 850 */
LOCAL_D UBYTE altchar[0x20] = {
  0x20,
  0x04,0xb1,0x12,0x12,0x12,0x12,0xf8,0xf1,
  0x12,0x12,0xd9,0xbf,0xda,0xc0,0xc5,0xc4,
  0xc4,0xc4,0xc4,0xc4,0xc3,0xb4,0xc1,0xc2,
  0xb3,0x11,0x10,0xf4,0xce,0x9c,0xfa
};

/* ------------------------------------------------------------ drawing */

LOCAL_C VOID gcsetup(VOID)
    {
    G_GC gc;
    gc.font = vt_font;
    gc.style = G_STY_NORMAL;
    gc.textmode = G_TRMODE_SET;   /* rows are cleared first; bold may overhang */
    gSetGC(0, G_GC_MASK_FONT | G_GC_MASK_STYLE | G_GC_MASK_TEXTMODE, &gc);
    }

LOCAL_C VOID cellrect(P_RECT *r, INT row, INT c0, INT n)
    {
    r->tl.x = X0 + c0 * vt_cw;
    r->tl.y = Y0 + row * vt_ch;
    r->br.x = r->tl.x + n * vt_cw;
    r->br.y = r->tl.y + vt_ch;
    }

/* draw one row; the caller has already cleared its pixels */
LOCAL_C VOID paintrow(INT row)
    {
    G_GC gc;
    INT c, e, a, cur, inv, s0, s1;
    P_RECT r;
    TEXT *t = txt[row];
    cur = 0;
    for (c = 0; c < vt_cols; c = e)
        {
        a = att[row][c];
        e = c + 1;
        if (!(a & A_BOLD))                    /* bold glyphs are wider: one per cell */
            while (e < vt_cols && att[row][e] == a) e++;
        inv = (a & A_INV) != 0;
        if (e_flags & F_SCRINV) inv = !inv;
        /* plain text: only the part between the first and last non-blank */
        s0 = c; s1 = e;
        if (!(a & A_UNDER))
            {
            while (s0 < s1 && t[s0] == ' ') s0++;
            while (s1 > s0 && t[s1 - 1] == ' ') s1--;
            }
        if (s1 > s0)
            {
            if ((a & (A_BOLD | A_UNDER)) != cur)
                {
                cur = a & (A_BOLD | A_UNDER);
                gc.style = (UBYTE)cur;
                gSetGC(0, G_GC_MASK_STYLE, &gc);
                }
            gPrintText(X0 + s0 * vt_cw, Y0 + row * vt_ch + vt_asc, &t[s0], s1 - s0);
            }
        if (inv)
            {
            cellrect(&r, row, c, e - c);
            gClrRect(&r, G_TRMODE_INV);
            }
        }
    if (cur != 0)
        {
        gc.style = G_STY_NORMAL;
        gSetGC(0, G_GC_MASK_STYLE, &gc);
        }
    dirty[row] = 0;
    }

LOCAL_C VOID invcell(INT x, INT y)
    {
    P_RECT r;
    cellrect(&r, y, x, 1);
    gClrRect(&r, G_TRMODE_INV);
    }

LOCAL_C VOID hidecur(VOID)
    {
    if (curshown) { invcell(curx, cury); curshown = 0; }
    }

LOCAL_C VOID showcur(VOID)
    {
    if (!(e_flags & F_CURSOR)) return;
    curx = cx >= vt_cols ? vt_cols - 1 : cx;
    cury = cy;
    invcell(curx, cury);
    curshown = 1;
    }

/* Deferred ("jump") scrolling: consecutive scrolls of the same region in
 * one batch are accumulated and done with a single wScrollRect when the
 * screen is next brought up to date. Dirty flags travel with the rows, so
 * rows that were already on screen are not repainted. */
LOCAL_D INT ps_n, ps_top, ps_bot, ps_down;

LOCAL_C VOID applyscroll(VOID)
    {
    P_RECT rc;
    P_POINT o;
    INT n = ps_n;
    if (n == 0) return;
    ps_n = 0;
    rc.tl.x = X0; rc.br.x = X0 + vt_cols * vt_cw;
    o.x = 0;
    if (ps_down)
        {
        rc.tl.y = Y0 + ps_top * vt_ch; rc.br.y = Y0 + (ps_bot - n) * vt_ch;
        o.y = n * vt_ch;
        }
    else
        {
        rc.tl.y = Y0 + (ps_top + n) * vt_ch; rc.br.y = Y0 + ps_bot * vt_ch;
        o.y = -n * vt_ch;
        }
    wScrollRect(vt_wid, &rc, &o);
    }

LOCAL_C VOID flushrows(VOID)
    {
    INT r, r0;
    P_RECT rc;
    if (!ingc) return;
    applyscroll();
    for (r = 0; r < vt_rows; )
        {
        if (!dirty[r]) { r++; continue; }
        r0 = r;                               /* one clear for a run of dirty rows */
        while (r < vt_rows && dirty[r]) r++;
        rc.tl.x = X0; rc.br.x = X0 + vt_cols * vt_cw;
        rc.tl.y = Y0 + r0 * vt_ch; rc.br.y = Y0 + r * vt_ch;
        gClrRect(&rc, G_TRMODE_CLR);
        while (r0 < r) paintrow(r0++);
        }
    }

GLDEF_C VOID vt_redraw(VOID)
    {
    INT r;
    gcsetup();                                /* the window was cleared by the caller */
    for (r = 0; r < vt_rows; r++) paintrow(r);
    curshown = 0;
    if (e_flags & F_CURSOR) showcur();
    }

/* ------------------------------------------------------------ buffer ops */

LOCAL_C VOID blankrow(INT r, INT c0, INT c1)    /* c1 exclusive */
    {
    INT c;
    for (c = c0; c < c1; c++) { txt[r][c] = ' '; att[r][c] = 0; }
    dirty[r] = 1;
    }

/* scroll rows top..bot-1 by n: up (content moves up) or down */
LOCAL_C VOID scroll(INT top, INT bot, INT n, INT down)
    {
    INT r, h = bot - top;
    if (n <= 0 || h <= 0) return;
    if (n > h) n = h;
    /* a different kind of scroll: bring the screen up to date first */
    if (ps_n && (ps_top != top || ps_bot != bot || ps_down != down))
        flushrows();
    if (down)
        {
        for (r = bot - 1; r >= top + n; r--)
            {
            p_bcpy(txt[r], txt[r - n], vt_cols);
            p_bcpy(att[r], att[r - n], vt_cols);
            dirty[r] = dirty[r - n];
            }
        for (r = top; r < top + n; r++) blankrow(r, 0, vt_cols);
        }
    else
        {
        for (r = top; r < bot - n; r++)
            {
            p_bcpy(txt[r], txt[r + n], vt_cols);
            p_bcpy(att[r], att[r + n], vt_cols);
            dirty[r] = dirty[r + n];
            }
        for (r = bot - n; r < bot; r++) blankrow(r, 0, vt_cols);
        }
    if (!ingc)
        {
        for (r = top; r < bot; r++) dirty[r] = 1;
        return;
        }
    ps_top = top; ps_bot = bot; ps_down = down;
    ps_n += n;
    if (ps_n >= h)                     /* everything moved: just repaint */
        {
        ps_n = 0;
        for (r = top; r < bot; r++) dirty[r] = 1;
        }
    }

LOCAL_C VOID nl(VOID)
    {
    if (cy == sr_l - 1)
        scroll(sr_u, sr_l, 1, 0);
    else if (cy < vt_rows - 1)
        cy++;
    }

LOCAL_C VOID clear(INT n)
    {
    INT r;
    INT x = cx >= vt_cols ? vt_cols - 1 : cx;
    switch (n)
        {
    case 0:                                   /* cursor to end of screen */
        blankrow(cy, x, vt_cols);
        for (r = cy + 1; r < vt_rows; r++) blankrow(r, 0, vt_cols);
        break;
    case 1:                                   /* start of screen to cursor */
        for (r = 0; r < cy; r++) blankrow(r, 0, vt_cols);
        blankrow(cy, 0, x + 1);
        break;
    case 2:                                   /* whole screen */
        for (r = 0; r < vt_rows; r++) blankrow(r, 0, vt_cols);
        break;
    case 3:                                   /* cursor to end of line */
        blankrow(cy, x, vt_cols);
        break;
    case 4:                                   /* start of line to cursor */
        blankrow(cy, 0, x + 1);
        break;
    case 5:                                   /* whole line */
        blankrow(cy, 0, vt_cols);
        break;
        }
    }

/* insert (right) or delete (left) n characters at the cursor */
LOCAL_C VOID hshift(INT n, INT right)
    {
    INT c, x = cx >= vt_cols ? vt_cols - 1 : cx;
    if (n > vt_cols - x) n = vt_cols - x;
    if (right)
        {
        for (c = vt_cols - 1; c >= x + n; c--) { txt[cy][c] = txt[cy][c - n]; att[cy][c] = att[cy][c - n]; }
        blankrow(cy, x, x + n);
        }
    else
        {
        for (c = x; c < vt_cols - n; c++) { txt[cy][c] = txt[cy][c + n]; att[cy][c] = att[cy][c + n]; }
        blankrow(cy, vt_cols - n, vt_cols);
        }
    dirty[cy] = 1;
    }

LOCAL_C VOID respond(TEXT *s)
    {
    ser_send((UBYTE *)s, p_slen(s));
    }

LOCAL_C INT numfmt(TEXT *p, UINT v)
    {
    TEXT t[6];
    INT n = 0, i = 0;
    do { t[n++] = (TEXT)('0' + v % 10); v /= 10; } while (v);
    while (n) p[i++] = t[--n];
    return(i);
    }

GLDEF_C VOID vt_reset(VOID)
    {
    INT i;
    vt_altscr = 0;
    for (i = 0; i < VT_MAXCOLS + 1; i++) tabs[i] = (UBYTE)(i && (i & 7) == 0);
    cx = cy = sx = sy = 0;
    sattr = 0;
    sr_u = 0; sr_l = vt_rows;
    attr = 0;
    e_state = E_NORMAL;
    e_flags = (UBYTE)((vt_autowrap ? F_WRAP : 0) | F_CURSOR);
    vt_appcur = 0;
    gl = 0;
    gset = charsets[0] = charsets[1] = charsets[2] = charsets[3] = 'B';
    for (i = 0; i < VT_MAXROWS; i++) blankrow(i, 0, VT_MAXCOLS);
    }

/* ------------------------------------------------------------ parser */

LOCAL_C VOID put(UBYTE ch);

/* put one font (code page 850) character at the cursor */
LOCAL_C VOID putcell(UBYTE ch)
    {
    if (cx >= vt_cols)
        {
        if (e_flags & F_WRAP) { nl(); cx = 0; }
        else cx = vt_cols - 1;
        }
    if (e_flags & F_INSERT) hshift(1, 1);
    txt[cy][cx] = (TEXT)ch;
    lastch = ch;
    att[cy][cx] = attr;
    dirty[cy] = 1;
    cx++;
    }

/* an ISO 8859-1 / ASCII character, through the selected VT100 character set */
LOCAL_C VOID printable(UBYTE ch)
    {
    if (ch > 127)
        ch = tbl_iso2cp[ch - 128];
    else if (gset == 'A' && ch == '#')
        ch = 0x9c;                            /* pound sign */
    else if (gset == '0' && ch >= 0x5f && ch <= 0x7e)
        ch = altchar[ch - 0x5f];
    putcell(ch);
    }

/* ---- UTF-8: decode, then map to the font's code page 850 ---- */
LOCAL_D UBYTE u_need;                 /* continuation bytes still expected */
LOCAL_D ULONG u_cp;

/* box drawing U+2500.. (light, heavy as light, double) -> CP850 */
LOCAL_D UWORD boxmap[][2] = {
    {0x2500,0xc4},{0x2501,0xc4},{0x2502,0xb3},{0x2503,0xb3},{0x250c,0xda},{0x250f,0xda},
    {0x2510,0xbf},{0x2513,0xbf},{0x2514,0xc0},{0x2517,0xc0},{0x2518,0xd9},{0x251b,0xd9},
    {0x251c,0xc3},{0x2523,0xc3},{0x2524,0xb4},{0x252b,0xb4},{0x252c,0xc2},{0x2533,0xc2},
    {0x2534,0xc1},{0x253b,0xc1},{0x253c,0xc5},{0x254b,0xc5},{0x2550,0xcd},{0x2551,0xba},
    {0x2554,0xc9},{0x2557,0xbb},{0x255a,0xc8},{0x255d,0xbc},{0x2560,0xcc},{0x2563,0xb9},
    {0x2566,0xcb},{0x2569,0xca},{0x256c,0xce},{0x256d,0xda},{0x256e,0xbf},{0x256f,0xd9},
    {0x2570,0xc0},{0x2574,0xc4},{0x2576,0xc4},{0x2575,0xb3},{0x2577,0xb3},
    {0x2580,0xdf},{0x2584,0xdc},{0x2588,0xdb},{0x258c,0xdd},{0x2590,0xde},
    {0x2591,0xb0},{0x2592,0xb1},{0x2593,0xb2},{0x25a0,0xfe},{0x25aa,0xfe},
    {0x2018,'\''},{0x2019,'\''},{0x201a,','},{0x201c,'"'},{0x201d,'"'},{0x201e,'"'},
    {0x2010,'-'},{0x2011,'-'},{0x2012,'-'},{0x2013,'-'},{0x2014,'-'},{0x2015,'-'},{0x2212,'-'},
    {0x2022,'*'},{0x2026,'.'},{0x2190,'<'},{0x2192,'>'},{0x2191,'^'},{0x2193,'v'},
    {0x2713,'v'},{0x2714,'v'},{0x2717,'x'},{0x2718,'x'},{0x00a0,' '},{0x20ac,'E'},
    {0, 0}};

LOCAL_C VOID ucs(ULONG cp)
    {
    INT i;
    if (cp >= 0xa0 && cp <= 0xff) { putcell(tbl_iso2cp[cp - 128]); return; }
    if ((cp >= 0x300 && cp <= 0x36f) || (cp >= 0x200b && cp <= 0x200f) ||
        (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0xfeff)
        return;                               /* combining / zero width: nothing */
    for (i = 0; boxmap[i][0]; i++)
        if (boxmap[i][0] == cp) { putcell((UBYTE)boxmap[i][1]); return; }
    if (cp >= 0x1f000 || (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) ||
        (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xff00 && cp <= 0xff60))
        {
        putcell(0xfe);                        /* double width (emoji, CJK): keep columns */
        putcell(' ');
        return;
        }
    putcell('?');
    }

/* a byte >= 0x80 in UTF-8 mode; returns once a character is complete */
LOCAL_C VOID utf8(UBYTE ch)
    {
    if (ch >= 0x80 && ch < 0xc0)
        {
        if (u_need == 0) { putcell('?'); return; }       /* stray continuation */
        u_cp = (u_cp << 6) | (ch & 0x3f);
        if (--u_need == 0) ucs(u_cp);
        return;
        }
    if (u_need) putcell('?');                 /* previous sequence cut short */
    if (ch >= 0xf0 && ch < 0xf8) { u_need = 3; u_cp = ch & 0x07; }
    else if (ch >= 0xe0) { u_need = 2; u_cp = ch & 0x0f; }
    else if (ch >= 0xc2) { u_need = 1; u_cp = ch & 0x1f; }
    else { u_need = 0; putcell('?'); }
    }

LOCAL_C VOID control(UBYTE ch)
    {
    if (!(e_flags & F_WRAP) && cx >= vt_cols) cx = vt_cols - 1;
    switch (ch)
        {
    case 7:
        p_sound(5, 320);
        break;
    case 8:
        if (cx >= vt_cols) cx = vt_cols - 1;
        if (cx > 0) cx--;
        break;
    case 9:
        if (cx >= vt_cols) { nl(); cx = 0; }
        cx++;
        while (cx < vt_cols - 1 && !tabs[cx]) cx++;
        if (cx >= vt_cols) cx = vt_cols - 1;
        break;
    case 10: case 11: case 12:
        nl();
        break;
    case 13:
        cx = 0;
        break;
    case 0x0e: case 0x0f:                     /* SO / SI */
        gl = (UBYTE)(ch == 0x0e ? 1 : 0);
        gset = charsets[gl];
        break;
    case 0x18: case 0x1a:                     /* CAN / SUB abort a sequence */
        e_state = E_NORMAL;
        break;
    case 0x1b:
        e_state = E_ESCAPE;
        e_interm = 0;
        break;
        }
    }

LOCAL_C VOID esc_final(UBYTE ch)
    {
    INT r, c;
    e_state = E_NORMAL;
    switch (e_interm)
        {
    case 0:
        switch (ch)
            {
        case 'E': nl(); cx = 0; break;
        case 'D': nl(); break;
        case 'M':
            if (cy == sr_u) scroll(sr_u, sr_l, 1, 1);
            else if (cy > 0) cy--;
            break;
        case 'H': if (cx < vt_cols) tabs[cx] = 1; break;
        case 'Z': respond("\033[?1;2c"); break;
        case '7': sx = cx; sy = cy; sattr = attr; break;
        case '8': cx = sx; cy = sy; attr = (UBYTE)sattr; break;
        case 'c': vt_reset(); break;
        case '[':
            e_state = 0; e_interm = 0;
            for (r = 0; r < MAXARGS; r++) e_args[r] = 0;
            break;
        case ']': case 'P': case '_': case '^': case 'X':   /* OSC DCS APC PM SOS */
            e_state = E_STRING;
            break;
        case '=': break;                      /* keypad modes: ignored */
        case '>': break;
            }
        break;
    case '#':
        if (ch == '8')                        /* DECALN: fill with E */
            for (r = 0; r < vt_rows; r++)
                {
                for (c = 0; c < vt_cols; c++) { txt[r][c] = 'E'; att[r][c] = 0; }
                dirty[r] = 1;
                }
        break;
    case '(': case ')': case '*': case '+':
        charsets[e_interm - '('] = ch;
        gset = charsets[gl];
        break;
        }
    }

LOCAL_C VOID csi_final(UBYTE ch)
    {
    INT i, n, a0, a1, nargs;
    TEXT b[24];

    nargs = e_state + 1;
    if (nargs > MAXARGS) nargs = MAXARGS;
    a0 = e_args[0] ? e_args[0] : 1;
    a1 = e_args[1] ? e_args[1] : 1;
    e_state = E_NORMAL;

    if (e_interm == '?')
        {
        if (ch != 'h' && ch != 'l' && ch != 'n') return;
        for (i = 0; i < nargs; i++)
            switch (e_args[i])
                {
            case 1: vt_appcur = ch == 'h'; break;
            case 5:
                if ((ch == 'h') != ((e_flags & F_SCRINV) != 0))
                    {
                    e_flags ^= F_SCRINV;
                    for (n = 0; n < vt_rows; n++) dirty[n] = 1;
                    }
                break;
            case 6:
                e_flags &= ~F_ORIGIN;
                if (ch == 'h') e_flags |= F_ORIGIN;
                cx = 0; cy = (e_flags & F_ORIGIN) ? sr_u : 0;
                break;
            case 7:
                e_flags &= ~F_WRAP;
                if (ch == 'h') e_flags |= F_WRAP;
                break;
            case 25:
                e_flags &= ~F_CURSOR;
                if (ch == 'h') e_flags |= F_CURSOR;
                break;
            case 47: case 1047: case 1049:    /* alternate screen: no second buffer, */
                if (ch == 'h' && e_args[i] == 1049) { sx = cx; sy = cy; sattr = attr; }
                for (n = 0; n < vt_rows; n++) blankrow(n, 0, vt_cols);   /* just clear */
                if (ch == 'l' && e_args[i] == 1049) { cx = sx; cy = sy; attr = (UBYTE)sattr; }
                vt_altscr = (ch == 'h');
                break;
                }
        if (ch == 'n' && e_args[0] == 6)  /* DECXCPR */
            {
            n = 0;
            b[n++] = 0x1b; b[n++] = '['; b[n++] = '?';
            n += numfmt(b + n, cy + 1);
            b[n++] = ';';
            n += numfmt(b + n, (cx >= vt_cols ? vt_cols - 1 : cx) + 1);
            b[n++] = 'R'; b[n] = 0;
            respond(b);
            }
        return;
        }
    if (e_interm == '>')
        {
        if (ch == 'c' && e_args[0] == 0) respond("\033[>0;10;0c");
        return;
        }
    if (e_interm != 0) return;

    switch (ch)
        {
    case 'H': case 'f':
        cy = a0 - 1; cx = a1 - 1;
        if (e_flags & F_ORIGIN) { cy += sr_u; if (cy > sr_l - 1) cy = sr_l - 1; }
        if (cy > vt_rows - 1) cy = vt_rows - 1;
        if (cx > vt_cols - 1) cx = vt_cols - 1;
        break;
    case 'E':                                  /* CNL */
        cx = 0;
        /* fall through */
    case 'e':                                  /* VPR */
        n = cy < sr_l ? sr_l : vt_rows;
        cy += a0; if (cy >= n) cy = n - 1;
        break;
    case 'F':                                  /* CPL */
        cx = 0;
        n = cy >= sr_u ? sr_u : 0;
        cy -= a0; if (cy < n) cy = n;
        break;
    case 'a':                                  /* HPR */
        cx += a0; if (cx >= vt_cols) cx = vt_cols - 1;
        break;
    case 'b':                                  /* REP: repeat last character */
        if (a0 > vt_cols * vt_rows) a0 = vt_cols * vt_rows;
        while (a0-- > 0) putcell(lastch);
        break;
    case 't':                                  /* window ops: report text size */
        if (e_args[0] == 18)
            {
            n = 0;
            b[n++] = 0x1b; b[n++] = '['; b[n++] = '8'; b[n++] = ';';
            n += numfmt(b + n, vt_rows);
            b[n++] = ';';
            n += numfmt(b + n, vt_cols);
            b[n++] = 't'; b[n] = 0;
            respond(b);
            }
        break;
    case 'd':                                  /* VPA */
        cy = a0 - 1; if (cy > vt_rows - 1) cy = vt_rows - 1;
        break;
    case 'G': case '`':                        /* CHA */
        cx = a0 - 1; if (cx > vt_cols - 1) cx = vt_cols - 1;
        break;
    case 'J': if (e_args[0] < 3) clear(e_args[0]); break;
    case 'K': if (e_args[0] < 3) clear(e_args[0] + 3); break;
    case 'A':
        n = cy >= sr_u ? sr_u : 0;
        cy -= a0; if (cy < n) cy = n;
        if (cx >= vt_cols) cx = vt_cols - 1;
        break;
    case 'B':
        n = cy < sr_l ? sr_l : vt_rows;
        cy += a0; if (cy >= n) cy = n - 1;
        if (cx >= vt_cols) cx = vt_cols - 1;
        break;
    case 'C':
        cx += a0; if (cx >= vt_cols) cx = vt_cols - 1;
        break;
    case 'D':
        if (cx >= vt_cols) cx = vt_cols - 1;
        cx -= a0; if (cx < 0) cx = 0;
        break;
    case 'm':
        for (i = 0; i < nargs; i++)
            switch (e_args[i])
                {
            case 0: attr = 0; break;
            case 38: case 48: case 58:        /* colours: skip their sub-parameters */
                if (i + 1 < nargs && e_args[i + 1] == 5) i += 2;
                else if (i + 1 < nargs && e_args[i + 1] == 2) i += 4;
                break;
            case 1: attr |= A_BOLD; break;
            case 4: attr |= A_UNDER; break;
            case 5: case 7: attr |= A_INV; break;
            case 21: case 22: attr &= ~A_BOLD; break;
            case 24: attr &= ~A_UNDER; break;
            case 25: case 27: attr &= ~A_INV; break;
                }
        break;
    case 'g':
        if (e_args[0] == 3) for (i = 0; i < VT_MAXCOLS; i++) tabs[i] = 0;
        if (e_args[0] == 0 && cx < vt_cols) tabs[cx] = 0;
        break;
    case 'r':
        i = a0 - 1;
        if (e_args[0] == 0 && (nargs == 1 || e_args[1] == 0)) a1 = vt_rows;
        if (i > vt_rows) i = vt_rows;
        if (a1 > vt_rows) a1 = vt_rows;
        if (i >= a1 - 1) break;
        sr_u = i; sr_l = a1;
        cx = 0; cy = (e_flags & F_ORIGIN) ? sr_u : 0;
        break;
    case 'I':
        while (a0-- > 0) control(9);
        break;
    case 'Z':
        if (cx >= vt_cols) cx = vt_cols - 1;
        while (a0-- > 0)
            {
            if (cx > 0) cx--;
            while (cx > 0 && !tabs[cx]) cx--;
            }
        break;
    case 'L': case 'M':                        /* insert / delete lines */
        if (cy < sr_u || cy >= sr_l) break;
        scroll(cy, sr_l, a0, ch == 'L');
        cx = 0;
        break;
    case 'S': scroll(sr_u, sr_l, a0, 0); break;
    case 'T': scroll(sr_u, sr_l, a0, 1); break;
    case '@': hshift(a0, 1); break;
    case 'P': hshift(a0, 0); break;
    case 'X':                                  /* erase characters */
        n = cx >= vt_cols ? vt_cols - 1 : cx;
        blankrow(cy, n, (n + a0 > vt_cols) ? vt_cols : n + a0);
        break;
    case 'h': case 'l':
        for (i = 0; i < nargs; i++)
            if (e_args[i] == 4)
                {
                e_flags &= ~F_INSERT;
                if (ch == 'h') e_flags |= F_INSERT;
                }
        break;
    case 's': sx = cx; sy = cy; sattr = attr; break;
    case 'u': cx = sx; cy = sy; attr = (UBYTE)sattr; break;
    case 'c':
        if (e_args[0] == 0) respond("\033[?1;2c");
        break;
    case 'n':
        if (e_args[0] == 5) respond("\033[0n");
        if (e_args[0] == 6)
            {
            n = 0;
            b[n++] = 0x1b; b[n++] = '[';
            n += numfmt(b + n, (e_flags & F_ORIGIN) ? cy - sr_u + 1 : cy + 1);
            b[n++] = ';';
            n += numfmt(b + n, (cx >= vt_cols ? vt_cols - 1 : cx) + 1);
            b[n++] = 'R'; b[n] = 0;
            respond(b);
            }
        break;
    case 'x':
        if (e_args[0] <= 1)
            respond(e_args[0] ? "\033[3;1;1;120;120;1;0x" : "\033[2;1;1;120;120;1;0x");
        break;
        }
    }

LOCAL_C VOID put(UBYTE ch)
    {
    if (e_state == E_STRING)                  /* swallow until BEL or ST */
        {
        if (ch == 7 || ch == 0x18 || ch == 0x1a) e_state = E_NORMAL;
        else if (ch == 0x1b) e_state = E_STRESC;
        return;
        }
    if (e_state == E_STRESC)
        {
        if (ch == '\\') { e_state = E_NORMAL; return; }
        e_state = E_ESCAPE; e_interm = 0;   /* a new sequence: fall through */
        }
    if (ch == 0x7f) return;                   /* DEL is ignored */
    if (ch < 0x20)
        {
        control(ch);                          /* also inside sequences, as a VT100 */
        return;
        }
    if (e_state == E_NORMAL)
        {
        if (vt_utf8 && (ch >= 0x80 || u_need))
            {
            if (ch < 0x80) { u_need = 0; putcell('?'); printable(ch); }
            else utf8(ch);
            }
        else
            printable(ch);
        return;
        }
    if (e_state == E_ESCAPE)
        {
        if (ch <= '/')                        /* intermediate */
            {
            e_interm = (UBYTE)(e_interm ? 0xff : ch);
            return;
            }
        esc_final(ch);
        return;
        }
    /* CSI: e_state is the argument index */
    if (ch >= '0' && ch <= '9')
        {
        if (e_state < MAXARGS) e_args[e_state] = e_args[e_state] * 10 + (ch - '0');
        return;
        }
    if (ch == ';' || ch == ':')
        {
        e_state++;
        if (e_state < MAXARGS) e_args[e_state] = 0;
        return;
        }
    if (ch <= 0x3f || (ch >= 0x20 && ch <= 0x2f))   /* private / intermediate */
        {
        e_interm = (UBYTE)(e_interm ? 0xff : ch);
        return;
        }
    csi_final(ch);
    }

/* process received bytes and update the window */
GLDEF_C VOID vt_write(UBYTE *buf, INT len)
    {
    if (vt_wid)
        {
        gCreateTempGC0(vt_wid);
        gcsetup();
        ingc = 1;
        hidecur();
        }
    while (len-- > 0)
        put(*buf++);
    if (ingc)
        {
        flushrows();
        showcur();
        gFreeTempGC();
        ingc = 0;
        wFlush();
        }
    }

/* New window size. Keeps the text; if the window got shorter, the screen
 * scrolls so the cursor line stays visible (like xterm). The scroll region
 * is reset. The caller's redraw paints the result. */
GLDEF_C VOID vt_resize(INT cols, INT rows)
    {
    INT r, n;
    if (cols > VT_MAXCOLS) cols = VT_MAXCOLS;
    if (rows > VT_MAXROWS) rows = VT_MAXROWS;
    if (cols < 10 || rows < 2) return;
    if (cols == vt_cols && rows == vt_rows) return;
    if (cy >= rows)
        {
        n = cy - rows + 1;
        for (r = 0; r + n < vt_rows; r++)
            {
            p_bcpy(txt[r], txt[r + n], VT_MAXCOLS);
            p_bcpy(att[r], att[r + n], VT_MAXCOLS);
            }
        cy -= n;
        sy = sy >= n ? sy - n : 0;
        for (r = vt_rows - n; r < vt_rows; r++) blankrow(r, 0, VT_MAXCOLS);
        }
    for (r = vt_rows; r < rows; r++) blankrow(r, 0, VT_MAXCOLS);     /* new rows */
    if (cols > vt_cols)
        for (r = 0; r < VT_MAXROWS; r++) blankrow(r, vt_cols, VT_MAXCOLS);
    vt_cols = cols; vt_rows = rows;
    sr_u = 0; sr_l = rows;
    if (cx > cols) cx = cols;
    if (sx >= cols) sx = cols - 1;
    for (r = 0; r < VT_MAXROWS; r++) dirty[r] = 1;
    }

GLDEF_C VOID vt_puts(TEXT *s)
    {
    vt_write((UBYTE *)s, p_slen(s));
    }

/* ------------------------------------------------------------ keyboard */

/* map an MC key press to the bytes a VT100 would send; 0 = not ours */
GLDEF_C INT vt_key(INT key, INT mods, UBYTE *out)
    {
    if (key >= 256 && key <= 259)              /* up, down, right, left */
        {
        out[0] = 0x1b;
        out[1] = (UBYTE)(vt_appcur ? 'O' : '[');
        out[2] = (UBYTE)('A' + (key - 256));
        return(3);
        }
    switch (key)
        {
    case 260: out[0] = 0x1b; out[1] = '['; out[2] = '5'; out[3] = '~'; return(4);  /* PgUp */
    case 261: out[0] = 0x1b; out[1] = '['; out[2] = '6'; out[3] = '~'; return(4);  /* PgDn */
    case 262: out[0] = 0x1b; out[1] = '['; out[2] = 'H'; return(3);                /* Home */
    case 263: out[0] = 0x1b; out[1] = '['; out[2] = 'F'; return(3);                /* End */
    case 8:                                   /* Bksp: DEL or ^H; Shift gives the other */
        out[0] = (UBYTE)(((mods & 2) != 0) != (vt_bs_ctlh != 0) ? 8 : 0x7f);
        return(1);
        }
    if (key < 256)
        {
        if (key == ' ' && (mods & 4)) { out[0] = 0; return(1); }   /* Ctrl-Space = NUL */
        out[0] = (UBYTE)key;
        if (key > 127)
            {
            out[0] = tbl_cp2iso[key - 128];          /* ISO 8859-1 */
            if (vt_utf8)
                {
                out[1] = (UBYTE)(0x80 | (out[0] & 0x3f));
                out[0] = (UBYTE)(0xc0 | (out[0] >> 6));
                return(2);
                }
            }
        return(1);
        }
    return(0);
    }

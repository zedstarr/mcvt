/* VT.H - VT100 engine interface (MCVT / MC400)
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
 */
#define VT_ROWS 24          /* initial size */
#define VT_COLS 80
#define VT_MAXROWS 40
#define VT_MAXCOLS 90

GLREF_D INT vt_cols, vt_rows;      /* screen size in cells */
GLREF_D INT vt_cw, vt_ch, vt_asc;  /* cell width/height, font ascent */
GLREF_D UINT vt_font;              /* font handle */
GLREF_D UINT vt_wid;               /* window id, 0 = not yet created */
GLREF_D INT vt_appcur;             /* cursor keys in application mode */
GLREF_D INT vt_bs_ctlh;            /* Backspace key sends ^H (else DEL) */
GLREF_D INT vt_autowrap;           /* auto wrap after vt_reset */
GLREF_D INT vt_utf8;               /* UTF-8 (else ISO 8859-1) */

GLREF_C VOID vt_reset(VOID);
GLREF_C VOID vt_write(UBYTE *buf, INT len);   /* draws via a temporary GC */
GLREF_C VOID vt_redraw(VOID);                 /* call inside a redraw */
GLREF_C INT vt_key(INT key, INT mods, UBYTE *out);   /* -> bytes to send */
GLREF_C VOID vt_puts(TEXT *s);
GLREF_C VOID vt_resize(INT cols, INT rows);   /* window was resized */

/* supplied by the application */
GLREF_C VOID ser_send(UBYTE *buf, INT len);

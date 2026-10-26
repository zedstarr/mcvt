# Changelog

## Unreleased

- Font search stops after the four fonts that exist (system, SMALL, BIG,
  MONO). Asking about a missing font kills the process on ROM v2.12F; the
  font chosen on the MC400 is unchanged.
- `spec/`: TLA+ models of the serial input path and of screen updating
  (deferred scrolling, XOR cursor, covered windows), checked with TLC.
- `docs/mc200.md`: notes from trying MCVT on the MC200 (ROM v2.12F), parked.

## 1.0 — 2026-10-19

First release.

- Native WIMP application for the Psion MC400: framed, resizable window,
  task-list entry, background/foreground, maximise and iconise.
- VT100 emulation (after nfsc's vt100.c): cursor addressing, erase, bold,
  underline and inverse, scroll regions, insert/delete lines and characters,
  tabs, DEC line drawing, wrap and origin modes, DA/DSR replies.
- xterm/ECMA-48 extras: OSC/DCS strings swallowed, 256/RGB colour codes
  ignored safely, REP, CNL/CPL, HPR/VPR, ECH, SU/SD, alternate screen,
  size report (`CSI 18 t`).
- UTF-8 decoding (accents and box drawing to the MC's code page 850 font,
  double-width characters kept aligned); ISO 8859-1 as an option.
- Terminal grid follows the window size (up to 90×40 cells).
- Serial on TTY:A or TTY:B, up to 19200 baud, XON/XOFF or RTS/CTS,
  using the MC's own Serial options dialog.
- Terminal options dialog: local echo, Backspace sends DEL or ^H, auto wrap,
  UTF-8.
- Settings saved in `M:\MCVT.INI`.
- File menu: Clear screen and Reset terminal (both send Ctrl-L so the host
  redraws), Exit.

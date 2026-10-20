# MCVT for the Psion MC400 — reverse-engineering log

Oct 19, 2026 · @Christian Farrow

MCVT is a native WIMP VT100 terminal for the MC400, built with the Series 3 SIBO SDK against an MC object interface that has no SDK. This log records how the MC400's window server, WIMP and OLIB libraries, resources and dialogs were worked out from the ROM, from SYS$CON, FLMAN and TERM.APP, and by testing in the SDK's SDBG.EXE.

## Build chain

Each test program answered one question before the next was written. Every build ran in SDBG first where possible, then on the real MC400; logs came back through psibridge.

| Program | Approach | What it proved |
| --- | --- | --- |
| MCTEST v1–v2 | Plain WLIB window, S3 SDK calls only | wStartup, gFontInfo, gSetGC, gPrintText, gClrRect, wScrollRect work on the MC. A bare window has no frame and no task-list entry. |
| CONTEST v1–v5 | Console (`CON:`) with P\_SCR\_\* calls | The console gives an 80×24 framed window but no text attributes; several S3 console calls panic or fail on the MC. |
| ATTRTEST | Console attribute probe | Confirmed no bold, underline or inverse through `CON:`. |
| WT1 | First native WIMP app, own classes over reverse-engineered `wimp.ext` | Framed, titled window in the task list; background/foreground, resize, maximise, iconise and Stop all work. |
| WT2 | 80×24 text grid in the WIMP window | Bold, underline and inverse render; key events reach the window; ROM font list and metrics. |
| WT3 | VT100 engine plus serial active object on `TTY:A` | A Linux shell, top and nano run over serial; XON/XOFF and jump scrolling stop overruns; the grid follows the window size. |
| MCVT | Menus, ROM serial dialog, own Terminal dialog, settings file, UTF-8 | Feature-complete terminal; packaged as MCVT.APP with an edited TERM icon. |

## Toolchain and test beds

The SDK ran unmodified in DOSBox on Linux, driven headlessly by scripts. Three test environments were used, in increasing order of faithfulness.

**Build.** The supplied `sibo-sdk.zip` (drives A, C and M plus a DOSBox config) was mounted in DOSBox 0.74. A wrapper script mounted the drives, set PATH and ran `tsc /m <project>` with output captured to a file, so every build was a single command. The project file chain was: CTRAN (category file to `.c`, `.g` and `.ext`) → TopSpeed C 3.10 → ECOBJ (category object) → TopSpeed C for the methods → resources → link with `olib.lib`. Resources were first built with rcomp; from MCVT on, a Python script writes the `.RSC` directly because rcomp only knows Series 3 structures.

**Test beds.**

- **S3a emulator** (`s3aemul` from the SDK) under DOSBox on a virtual X display, used for the early WLIB-only tests. It runs Series 3a software, so it says nothing about MC-specific calls.
- **SDBG.EXE** from the SDK, which hosts an MC-like environment on the PC: the program under test is copied over `SYS$BOOT.IMG` and boots in place of the debugger console. This became the main test bed from WT1 onwards (section on SDBG below).
- **The real MC400**, with each build copied over by psibridge and logs read back from `M:` (for example `cat mc400/WT1.LOG`).

**Debug logging.** Every test program wrote a line-per-event log to `LOC::M:\<name>.LOG`, opening, appending and closing the file each time so the log survives a panic. The same log ran in SDBG and on the MC, which made the two directly comparable.

**Tooling written along the way** (Python): a ROM file-system extractor, a 16-bit disassembler with object-call annotation (it names `p_send` method numbers and window-server opcodes), a class-record parser, a generator for `olib.ext` / `wimp.ext` / `.g` headers, a compressed-resource decoder, the `.RSC` builder and the icon builder.

## ROM analysis

The MC400 ROM (v2.60F) contains a full object-oriented GUI layer, WIMP.DYL on top of OLIB.DYL, that the Series 3 SDK never documents. Finding it was the turning point of the project.

**Reassembling the dumps.** The MC400 has a 16-bit data bus, so its two chips (`v260f_0.bin`, `v260f_1.bin`) hold the even and odd bytes of each word and were interleaved byte by byte. The Series 3 ROMs are 8-bit, two chips one after the other; their checksums only matched after swapping address lines A6 and A7, which gave the unscrambled v1.77F image.

**ROM file system.** The MC ROM has a directory at 0xD5718 (data base 0xD5710). Each entry is 8 bytes of `[flags, paragraph, size32]` followed by a 12-byte name; the first attempt was misaligned by 8 bytes. The files extracted include `SYS$WSRV.IMG` (window server), `SYS$CON.IMG` (console), `SYS$SHLL.IMG` (system screen), `WIMP.DYL`, `OLIB.DYL`, `OPL.DYL`, `S_.RSC` (system resources), `R_.HFT` (Huffman table for compressed resources), fonts and the `TTY.UAR` serial driver.

**System call layer.** Calls are software interrupts 0x80–0xDE, set up from a vector table at C000:40DC on the MC (C000:55CC on the S3). The MC redirects INT 0x8D, 0xD6 and 0xD8 to segment CF0D. The window server is reached through INT 0x8D (`mov ah,opcode`), with a stub table in WIMP at CE00.

**Window server.** The MC's `SYS$WSRV.IMG` is 38.7 KB against 33.6 KB on the S3 and has 101 opcodes; its command loop rejects any opcode ≥ 0x65. The MC returns error 0x70 for the `ws*` calls and for wSetBusyMsg, wInfoMsgCorner, wDrawButton and wAppKeyHandler. These are exactly the calls the S3's HWIM.DYL makes, which is why HWIM-based S3 apps cannot run on the MC.

**WIMP.DYL and OLIB.DYL.** Both are ROM libraries of classes. WIMP has 77 classes (class table at offset 0xF68) and OLIB 55 (at 0x47B8). The MC's OLIB has the same 55 classes, in the same order and hierarchy, as the S3's OLIB.EXT (root, active, appman, rscfile, consol…), though some method counts differ. WIMP was mapped by tracing `SYS$CON` (which uses WIMP classes #50/#51 for its window) and FLMAN (the File Manager), whose start-up sequence became the template for MCVT.

**Resources.** ROM resource files are compressed: word 4, the index size, `(offset, uncompressed length)` pairs, then Huffman-coded data decoded with the tree in `ROM::R_.HFT` (base 0, bits read LSB-first from 16-bit words). The window server also accepts ordinary uncompressed rcomp `.RSC` files. An image carries its resource file as the second add-file, at header slot 0x2C.

## Console route (MCTEST, CONTEST)

The first plan was to back-port nfsc's VT100 code using only calls the Series 3 SDK documents. It worked as far as it went, but neither route gave a proper MC application with text attributes.

**Plain WLIB (MCTEST).** wStartup, wInquireWindow, gFontInfo, gSetGC, gPrintText, gClrRect, wScrollRect and redraw events all work on the MC. gFontInfo leaves (`p_leave`) on a bad font handle, so font probing needs a `p_enter` frame; without one the app gets PANIC 47. Repaints the program starts itself must not be drawn inside wBeginRedraw, where they are clipped away. The result is a plain 640×400 window with no frame and no task-list entry.

**Console (CONTEST).** Opening `CON:` gives a framed 80×24 window from the system, with cursor positioning, clear, scroll region and box characters. The MC differs from the S3 in several ways:

- `P_SCR_WSET` must be the first call; only the first WSET is honoured, and a second returns −14. Calling anything before it gives a 0×0 console and PANIC 55.
- `P_FINQ` is not supported: it returns −2 and also puts up an “Invalid arguments” alert.
- `P_SCR_FLUSH` panics (PANIC 155).
- There are no text attributes (no bold, underline or inverse).
- A window larger than the screen is simply invisible.
- INT 0x8B AH=01 returns the display type: 0 on the MC400 (640×400), but 10 under the PC emulators, so the MC is detected instead by the presence of `ROM::WIMP.DYL`.

The lack of attributes in the console, and the bare window from WLIB, led to the decision to build a native WIMP application instead.

## Object system

The MC's object system is the same as the Series 3's at the machine level, so the SDK's CTRAN and OLIB library could be reused once WIMP's classes were described to CTRAN.

**Messaging.** An object call is `p_send` (INT 0xCF): BX = object, CL = method number, arguments in DX, SI, DI. `p_supersend` is INT 0xD0 and `p_entersend` INT 0xD9. `p_new` is INT 0x84 AH=05 (BX = category handle, CX = class number), `p_getlibh` INT 0x84 AH=04 and `p_linklib` INT 0x84 AH=02. Methods use the SDK's `#pragma METHOD_CALL` convention with self in BX. Panics are raised as `mov al,code; int 0x90`.

**Class records.** Each class is a record `{category, superclass, property length, first method number, 0x6B, method count, component count}` followed by the method pointers. In ROM libraries the category field is the library's ROM segment (OLIB = 0xE018, WIMP = 0xE49B) and the superclass is an offset within that segment. In a RAM image the category is an index into `ExtCatTable` and the superclass is a class number. The 0x6B marker byte made it easy to scan any image for class records, which is how FLMAN, SYS$CON and TERM.APP were decoded.

**Method numbering.** A class's methods continue the numbering of its parent. A zero method pointer means the method is inherited; if no ancestor implements it, it is abstract and sending it panics. Inheritance has to be followed by hand through WIMP (#18 → #14 → #0) to know which methods are real; that is what caught PANIC 48.

**Generating headers for CTRAN.** A generator wrote `olib.ext`, `wimp.ext`, `olib.g` and `wimp.g` from the class records, naming WIMP classes `w0` to `w76` and their methods `wN_mM`. OLIB methods kept their S3 names (`ao_init`, `ao_run`, `am_init`…) where the class matched. With these, an MC app's category file reads like an S3 one:

```
IMAGE mcvt
EXTERNAL olib
EXTERNAL wimp
INCLUDE wimp.g
CLASS mcvtwin w18 { REPLACE w14_m30  REPLACE w0_m23 … }
```

**CTRAN quirks found.** A class with no methods of its own is dropped from the class table, so the command-manager class needed one `ADD` method. The ECOBJ step must run on the CTRAN output. TS.RED searches the current directory first for `*.G` and `*.H`, so project-local copies of `olib.g` and `wimp.g` are used. Inline system calls need `#pragma save,call(reg_saved=>(bx,cx,dx,si,di,EPOC_SAVE),inline=>on)`, otherwise the inline function falls through into the next one (PANIC 27).

## Native WIMP application (WT1)

A WIMP app is four subclasses of ROM classes plus an app-manager call; the window server does the rest. WT1 copied FLMAN's start-up and was then cut down until each step was understood.

**The WIMP classes MCVT uses.**

| Class | Role | Methods used (numbers as found) |
| --- | --- | --- |
| WIMP #0 | Base window | m8 connect (wCreateWindow), m9 visibility (1 = wMakeVisible), m14 set extent, m21 redraw event, m22 key fallback, m23 draw. Props: +4 window id, +6 width, +8 height, +0x0A owner, +0x0C flags |
| WIMP #14 | Framed-window family | m28 notification from a child, m30 init, m33 status query (abstract), m34 key press, m37 finish init |
| WIMP #15 | Window frame | m23 draws the frame, m28 rubber-band resize, m45/m46 get/set a 28-byte config |
| WIMP #18 | Main application window | m30 init (creates frame and scroll bars from config flags), m44 lay out the client inside the frame. Props: +0x0E frame |
| WIMP #32 | Dialog | m60/m61 set up from resource, m62 fill controls, m63 control activated, m31 set control, m32 read control |
| WIMP #66 | Command manager | m5 runs the method whose number a menu item names; m7/m8 empty hooks |
| WIMP #68 | Window-server client (derived from `active`) | m1 init, m4 queue wGetEvent, m5 event dispatch, m25 ROM serial dialog, m29 foreground/background, m30 app's init hook |
| WIMP #69 | App manager (derived from `appman`) | m1 am\_init, m2 am\_start, m4 add active object, m15 create and show a dialog |

**Start-up sequence.**

1. `p_linklib(0)`, then `p_new` WIMP class #69 and send it `am_init` with flags, own category handle, 0. Flags: 0x01 cleanup, 0x02 system link, 0x04 app resource file, 0x08 `ROM::S_.RSC`, 0x10 IPCs, 0x20 single instance, 0x40 no debug, 0x4000 skip resource 1, 0x8000 create the window-server client (own class 0) and send it m30. FLMAN uses 0x801F, SYS$CON 0x405B; MCVT uses 0x801F.
2. In the client's m30: send `ao_init` (m1) with `{own category, command-manager class, menu-bar resource, 0}`. A negative resource id means a system resource: −21 is the ROM's “Stop” menu bar.
3. `p_new` the main-window class (derived from #18) and send it m30. That supersends m30 with `{0x2434, 0, 0, 0, title resource, self}`, then m14 with the extent and m37(0).
4. Make the frame (+0x0E) and the client visible with m9(1), and call `wClientPosition(0,0)`.
5. Send `am_start` (m2): the app manager's event loop runs the active objects from then on.

**Window-server client fields that matter.** +0x0C holds the current event (type at +0x0C, object at +0x0E, key at +0x12, modifiers at +0x14). +0x2E is the default key target: key events go to that object's m34. +0x38 is the focus window, sent m33 on focus loss. +0x3C bit 2 marks the app as in the background.

**Event types seen.** 1 key, 2 pointer, 3 redraw, 5 focus lost, 6 focus gained, 7 and 13 frame/pointer activity. Redraw events carry the window object itself, set as the window's id when it was created.

**The config word.** FLMAN uses 0x6434. Bit 0x4000 makes class #18 create the window with flag 0x200, and with it set the client never receives redraw events: it appears to be a backed-up window the app is expected to draw into directly. MCVT uses 0x2434, which gives normal redraw events. Bit 0x400 asks for a frame (#15); 0x200, 0x800 and 0x1000 relate to scroll bars.

## Text, keys, serial and VT100 (WT2, WT3)

MC windows are not backed up by the window server, so the terminal keeps every character and attribute in memory and draws from that buffer. That one design choice is the main difference from nfsc, which drew straight to a Series 3 screen.

**Fonts.** Probing WS\_FONT\_SYSTEM and WS\_FONT\_BASE+0… (each probe inside `p_enter`) found four ROM fonts: System 8×15, System (bold) 8×15, System small 8×11 and System mono 7×10. Indices 4–15 fail. None sets the MONO flag, so fixed pitch is detected by comparing the widths of “iiii” and “WWWW”. System mono gives 80 columns in 560 pixels; maximised, the grid reaches about 89×34 on the 640×400 screen.

**Drawing.** The GC style flags work on the MC: G\_STY\_BOLD, G\_STY\_UNDERLINE and G\_STY\_INVERSE all render. Bold glyphs are about 1 pixel wider than the 7-pixel cell, so bold is drawn one character per cell, over a cleared row, in transparent (G\_TRMODE\_SET) mode. Inverse is made by inverting the cell rectangle (`gClrRect` with G\_TRMODE\_INV), which stays on the grid. Outside a redraw, the app draws with `gCreateTempGC0(window id)` … `gFreeTempGC()`; inside a redraw, `gSetGC(0, …)` changes the current GC. The block cursor is an inverted cell. The window server does not clear newly exposed areas, so the draw method clears the client first.

**Scrolling.** Scrolls move the pixels with `wScrollRect` and paint only the new lines. Consecutive scrolls in one burst are added up and done as one `wScrollRect`, with dirty-row flags shifted along with the buffer. Adjacent changed rows share one clear call, and blank stretches are not drawn.

**Keys.** Keys reach the window only when the client's +0x2E field names it; m34 then gets the key code in DX and modifiers in SI (Shift 2, Ctrl 4, Psion 8). Psion-key combinations go to the menus first and never reach m34. Codes seen: arrows 256–259 (up, down, right, left), PgUp/PgDn 260/261, Home/End 262/263, and F1–F4 as 0x131–0x134 in SDBG (the MC400 keyboard has no F-keys).

**Serial.** The ROM has `TTY:A` and `TTY:B` (driver TTY.UAR). On the MC, OLIB's `active` object is 14 bytes: root 4, queue link +4, priority +8, isactive +9, pcb +0x0A, stat +0x0C. The serial reader is an `active` subclass: it is registered with the app manager (`O_AM_ADD_TASK`, priority 59 = just under the window server), issues an async `P_FREAD` of 1 byte on its own `stat`, and sets isactive. When the byte arrives, its `ao_run` uses `P_FTEST` to read whatever else is waiting (up to 255 bytes), feeds the emulator and re-queues. `P_FSET` with a read outstanding panics, so the read is cancelled (`P_FCANCEL` + `p_waitstat`) before settings change.

**Flow control.** At 9600 baud (about 960 bytes a second), drawing can fall behind a burst and the driver's buffer overflows. XON/XOFF in both directions (`P_OBEY_XOFF | P_SEND_XOFF`, CTS ignored, as TERM.APP does) fixed `ls -lR`. Over ssh, the ssh client puts the Pi's tty in raw mode, which clears `ixon`, so the Pi stops obeying XOFF and 5fmn's frames were truncated. That is fixed on the Pi side (re-enable `ixon`, or use RTS/CTS).

**VT100 engine.** The parser follows nfsc's (GPL v2): cursor movement, erase, SGR attributes, scroll regions, insert/delete lines and characters, tabs, DEC line drawing (mapped to CP850 box characters), wrap and origin modes, and replies to DA and DSR. It was extended for current Linux programs:

- OSC, DCS, APC, PM and SOS strings are swallowed, so window titles no longer print as text.
- SGR 38/48 colour sub-parameters are skipped instead of being read as attributes.
- xterm sequences added: REP, CNL/CPL, HPR/VPR, CHA, VPA, ECH, SU/SD, the alternate screen (clear plus save/restore cursor), DECXCPR and the text-area size report (`CSI 18 t`).
- UTF-8 decoding: Latin-1 and box drawing map to CP850, punctuation to ASCII, and double-width characters show as ■ plus a space so columns stay aligned.

**Window size.** The grid follows the window: the frame's resize reaches the client's m44, which recomputes rows and columns. A serial line cannot signal a size change to Linux, so the host is told with a cursor-position query (`ESC[999;999H ESC[6n`) and `stty rows/cols`.

## What TERM.APP revealed

TERM.APP, the MC's built-in dumb terminal, is itself a WIMP/OLIB object application. Taking it apart showed how menus, dialogs, the ROM's serial dialog, resources and icons work, none of which FLMAN or SYS$CON had needed.

**File layout.** TERM.APP (11,792 bytes) is an ordinary image with a `.APP` name. Header slot 0x28 points at the icon (offset 0x40, 148 bytes) and slot 0x2C at the compressed resource file (offset 0xE0, 0x67D bytes); the code starts at 0x760. The image links `olib.DYL` and `wimp.DYL` by name.

**Its classes** (class table at 0x222 in the code): a terminal window (from WIMP #51, a text window), a window-server client overriding only m30, a command manager overriding m7–m28 (one method per menu command), an app manager overriding m11, and seven dialog classes derived from WIMP #32, each overriding m62 and m63. The dialog classes number their methods from 61–62 up, which is why a first scan limited to method numbers below 60 missed them.

**Menus.** A menu bar is a resource of entries `{length, hook, pull-down resource id, 0, flags, title}`. Flags 0x14/0x34 open the pull-down; flag 4 runs a command directly, which is how the ROM's one-item “Stop” bar (S\_.RSC resource 21) calls command 8. A pull-down item is `{length, command, 0, hotkey letter, flags, text}`: flag 4 plain, 5 adds “…” (opens a dialog), and 8 draws a separator below. There is no tick-mark flag. The command number is the method the command manager runs on itself: WIMP #66's m8 is an empty `ret`, so an app that does not implement m8 has a Stop item that highlights but does nothing.

**Opening dialogs.** Options → Serial sends method 25 to the window-server client with a pointer to a 24-byte block. WIMP's m25 opens the ROM's own “Serial options” dialog (S\_.RSC resource 10, dialog class WIMP #72) through the app manager. The block:

| Offset | Contents |
| --- | --- |
| +0 | Result: 2 = ENTER |
| +2 | `P_SRCHAR` (baud, frame, parity, handshake, xon, xoff, flags, mask) |
| +0x0E | Port name, "TTY:A" (letter at +0x12) |
| +0x14 | Non-zero: port choice locked |
| +0x16 | Mask of greyed-out baud rates |

TERM's own dialogs use the general route: app manager method 15 with DX = the app's category handle, SI = the dialog class number and DI = `{resource id, data pointer, 0}`. The app manager creates the dialog, stores the data pointer at +0x4E, sends m60, m61 and m62 (fill the controls), and makes it visible. A dialog whose resource flags include 0x100 is modal: the call returns only when it closes, which is why the serial dialog's result can be read straight back.

**Dialog controls.** m31 sets a control (DX = control id, SI = what to set, DI = value block) and m32 reads one into a block. For a checkbox, SI = 0x100 and bit 0x100 of the block's second word is the tick. m63 is sent with the id of whichever control was activated: 1 = CANCEL and 2 = ENTER (both close the dialog when passed to the superclass), anything else is a control the user changed. In the dialog itself, Tab moves focus, Space ticks, Enter is the default button and Esc is button 1.

**Dialog resource format** (`{width, height, flags, title, control count}`, then per control `{length, x, y, flags, id, type, …}`):

| Type | Control | Extra fields |
| --- | --- | --- |
| 2 | Label | 0x20, width, text |
| 7 | Button | 0x20, width, text; flags 0x0C00 = default (Enter) |
| 8 | Checkbox | 0x20, width, text; flags 1 |
| 9 | Radio button | 0x20, width, text; flags 0x41 first, 0xC1 middle, 0x81 last of a group |
| 0x1E | Choice list | 0x20, width, label; items set by code from a string resource |
| 0x40 | Number editor | digits, min, max |

**Uncompressed resource file.** The format rcomp produces, which the window server accepts: word offset of the index, word index size in bytes, the resources back to back, then the index (one offset per resource plus an end offset). MCVT's menus and its Terminal options dialog are written in this format by `mkrsc.py`.

**Icon.** The icon is a PIC file: “PIC”, 0xDC, “00”, a bitmap count, then per bitmap `{checksum, width, height, byte count, offset}` and 1-bit rows, least significant bit leftmost. TERM's is one 32×32 bitmap (4 bytes per row, 128 bytes). The checksum is CRC-CCITT (polynomial 0x1021, initial value 0) over the bitmap bytes, confirmed against both TERM's icon (0x94E7) and the SDK's HELLO.PIC (0x91A0). MCVT's icon is TERM's with a band of the screen cleared and “VT100” drawn in a 3×5 pixel font.

## SDBG.EXE as a test bed

SDBG, the SDK's DOS-hosted debugger, runs an MC-like environment on the PC with the same window server, WIMP and OLIB behaviour. Using it, following the [2022 zedstarr.com post on hacking the SDK debugger](https://zedstarr.com/2022/06/30/hacking-up-the-psion-sibo-sdk-dos-debugger/), cut the round trip from “build, copy to the MC, report back” to under a minute, and every bug except the focus-loss panic was reproduced and fixed there first.

**How it was run.**

- SDBG boots `SYS$BOOT.IMG` from the SDK's SYS directory. The original was saved as `SYS$BOOT.ORI` and each test build was copied over it, so SDBG starts the app directly instead of its own debugger console.
- A script launched DOSBox on a virtual X display, waited for boot, sent keystrokes with `xdotool` (including Psion-key combinations as Alt), then captured the DOSBox window. Sequences of keys with a capture after each showed how focus and controls behave in a dialog.
- The app's own log on `M:` showed which methods were called, in which order, with which object addresses.
- For hard cases, `dosbox-debug` ran SDBG under DOSBox's debugger in a terminal session: break on INT 0xD3 (`p_leave`) or INT 0x90 (panic), inspect registers and stack, and step until a given error code appeared.

**Serial in SDBG.** `TTY:A` in SDBG maps to the PC's COM1. DOSBox's COM1 was set to a TCP null modem (`serial1=nullmodem port:5000 transparent:1`). A Python client then fed VT100 test scripts in and checked the replies (`ESC[?1;2c`, `ESC[22;33R`). A second script bridged the port to a bash shell on a pseudo-terminal (TERM=vt100, 80×24), so `ls`, `seq`, `top` and `nano` ran end to end through MCVT, typed with `xdotool`. Captures from the real MC (5fmn, an `ls -lR` listing) were replayed the same way.

**Differences from the real MC.**

| Behaviour | SDBG | Real MC400 |
| --- | --- | --- |
| Screen | 640×480 (maximised grid 89×40) | 640×400 (maximised grid 89×34) |
| Display type (INT 0x8B AH=01) | 10 | 0 |
| Other tasks | None, so focus loss (event 5) never happens | The system screen and other apps |
| Serial speed and flow control | Null modem passes bytes at host speed; XON/XOFF untested | Real 9600/19200 baud; overruns appear without flow control |
| Speed | PC-speed emulation | A V30 at its native speed |
| Pointer | Not usable from xdotool | Pen/pointer works |

**Two traps in the harness.** A left-over `dosbox-debug` window sat on top of the test window and made several working builds look blank, until captures were taken of the test window by id. A serial test client could connect to the previous DOSBox just before it was killed; starting the client a few seconds after launch fixed that.

## Differences from the Series 3/3a SDK

The Series 3 SDK's C library, CTRAN and OLIB work on the MC unchanged. What differs is the GUI layer: the MC has WIMP.DYL where the S3 has HWIM, and a window server that refuses the calls HWIM depends on.

| Area | Series 3/3a (SDK) | MC400 (found) |
| --- | --- | --- |
| GUI library | HWIM.DYL / HWIF; documented | WIMP.DYL in ROM, 77 classes; undocumented, no headers |
| Object base library | OLIB, documented | Same 55 classes and order; some method counts differ |
| Window server | SYS$WSRV, 33.6 KB | 38.7 KB, 101 opcodes, ≥ 0x65 rejected |
| Unsupported calls | — | `ws*` calls, wSetBusyMsg, wInfoMsgCorner, wDrawButton, wAppKeyHandler return error 0x70 |
| Screen and windows | One full-screen app at a time | Overlapping, movable, resizable, iconisable windows; 640×400; pointer |
| Window contents | Restored by the system | Not backed up: the app redraws on redraw events (unless created as a backed-up window) |
| App start-up | `hwim` app manager and `wserv` classes | WIMP #69 app manager (`am_init` flags 0x801F) and WIMP #68 client, own class 0 |
| Menus | HWIM menu resources | WIMP menu bar / pull-down resources; commands are command-manager methods; no tick marks |
| Dialogs | HWIM dialog classes, rcomp structures | WIMP #32 subclasses (m62 fill, m63 control activated), launched by app manager m15; own binary resource format |
| Serial settings dialog | App-built | Built into the ROM: client method 25, S\_.RSC resource 10 |
| Resources | rcomp `.RSC`; rcomp knows HWIM structures | Uncompressed rcomp files accepted; ROM files Huffman-compressed with R\_.HFT; MC menus and dialogs written by hand |
| Console `CON:` | Attributes; P\_FINQ and P\_SCR\_FLUSH work | No attributes; P\_FINQ returns −2 with an alert; P\_SCR\_FLUSH panics (155); only the first WSET counts |
| Fonts | Series 3 font set (FONTS.H ids) | Four ROM fonts: System, System bold, System small, System mono 7×10; no MONO flag set |
| Text styles | Bold, underline, inverse | Same flags work in a WIMP window; bold is 1 pixel wider |
| Keys | Menu key, Psion key, Help | No F-keys; Psion+digit opens menus, Psion+letter runs menu hotkeys; arrows 256–259 |
| Display type (INT 0x8B AH=01) | Series 3 values | 0 on the MC400; SDBG and emulators report 10 |
| Serial | `TTY:A` (plus `TTY:I` on some models) | `TTY:A` and `TTY:B` (one per fitted serial/parallel adapter); driver TTY.UAR |
| Active object layout | PRS\_ACTIVE: queue at +2, isactive at +7 | Root is 4 bytes, so queue +4, priority +8, isactive +9, pcb +0x0A, stat +0x0C |
| Packaging | `.APP` with icon and resources | The same image layout; icon is the first add-file, resources the second |

## Bug log

Every failure met, in the order it appeared, with its cause and fix.

| Build | Symptom | Cause | Fix |
| --- | --- | --- | --- |
| ROM work | S3 ROM checksums wrong | Address lines A6/A7 swapped on the board | Unscrambled the dump |
| ROM work | ROM directory misread | Directory start off by 8 bytes | Corrected the offset |
| MCTEST v1 | Blank screen, then PANIC 47 | gFontInfo leaves on a bad font handle with no `p_enter` frame | Probe fonts inside `p_enter1` |
| MCTEST | Crash on the S3a emulator | Font names not NUL-terminated | Copy names with a length limit |
| MCTEST | Program-initiated repaints missing | Drawn inside wBeginRedraw, so clipped | Separate full repaint from redraw handling |
| CONTEST | Log empty | Log file only flushed on close | Open, append and close per line |
| CONTEST v1 | Nothing shown, PANIC 55 | Console used before `P_SCR_WSET` | WSET first, as PANSHLL does |
| CONTEST v2 | PANIC 27 | Inline system call without the save/inline pragma | Added the pragma |
| CONTEST v3 | “Invalid arguments” alert | `P_FINQ` unsupported on the MC (−2) | Skip on the MC |
| CONTEST v4 | Window vanished; PANIC 155 in a helper task | `P_SCR_FLUSH` panics; second WSET returns −14 | Skip FLUSH; set 80×24 in the first WSET |
| CONTEST | MC not detected in SDBG | Display type 10 under emulation | Detect `ROM::WIMP.DYL` instead |
| ATTRTEST | Stays “waiting for input” after exit | `CON:` channel not closed | Not pursued (console route dropped) |
| WT1 | Command class missing | CTRAN drops classes with no methods | Added one `ADD` method |
| WT1 | am\_init returns −69, then −38 | Resource 1 loaded with no resource file; own resources need flag 0x04 | Uncompressed `.RSC` + AFL, flags 0x801F |
| WT1 | Blank screen, wFlush hangs | Visibility message sent to a null top-level pointer; captures also hidden by a stale debugger window | Send m9 to the frame and client; capture the test window by id |
| WT1 | Frame drawn, client never drawn | Config 0x6434 makes a backed-up window with no redraw events | Config 0x2434 |
| WT1 (MC) | PANIC 48 when sent to the background | Focus-lost event sends m33 to the focus window; m33 is abstract in #18/#14/#0 | Implement m33 (as FLMAN does) |
| WT1 (MC) | Ghost frame lines after maximise | Window server does not clear exposed areas | Clear the client in the draw method |
| WT1 (MC) | Stop highlights but does nothing | Stop runs command-manager m8, an empty `ret` in #66 | Implement m8 to exit |
| WT2 | No key presses received | No key target registered | Set the client's +0x2E to the window (m34 gets keys) |
| WT2 | Bold text clipped by the next run | Bold glyphs 1 pixel wider than the cell | One character per cell in SET mode over a cleared row |
| WT2 | F1–F4 not recognised | Codes are 0x131–0x134, not 0x101–0x104 | Corrected codes; Ctrl alternatives (no F-keys on the MC) |
| WT3 (MC) | `ls -lR` corrupted after 7–8 lines | No flow control; receive buffer overruns while drawing | XON/XOFF both ways; deferred scrolling |
| WT3 (MC) | Maximised window uses only 80×24 | Grid fixed at 80×24 | Resize grid in m44; host told via size query and `stty` |
| MCVT | Dialog closes when a checkbox is ticked | m63 passed every control id to the superclass, which closes | Pass only ids 1 and 2 up |
| MCVT (MC) | Clear screen / Reset leaves no prompt | Local only; host not told | Send Ctrl-L after clearing |
| MCVT (MC) | 5fmn draws only 3 lines over ssh | ssh's raw mode clears `ixon` on the Pi, so XOFF ignored; data lost | Pi-side `stty ixon` after ssh starts (or RTS/CTS); faster redraws |
| MCVT (MC) | btop uses part of the window | Host still believes 80×24 | Run the size query (`rsz`) before starting it |
| MCVT | Emoji shown as four junk characters | UTF-8 decoded as ISO 8859-1 | UTF-8 decoder with CP850 mapping |

## Files and open items

MCVT.APP is the deliverable; everything needed to rebuild it is in `mcvt.zip`.

| File | Purpose |
| --- | --- |
| `MCVT.APP` | The application (image with icon and resources attached) |
| `MCVT.CAT` | Category file: the five MCVT classes over `olib` and `wimp` |
| `O_MCVT.C` | Application: start-up, window, keys, serial reader, menu commands, dialogs, settings |
| `VT.C`, `VT.H`, `CNV.H` | VT100 engine (after nfsc, GPL v2) and ISO 8859-1 ↔ CP850 tables |
| `mkrsc.py` | Writes `MCVT.RSC`: title, menu bar, menus, Terminal options dialog |
| `mkicon.py` | Writes `MCVT.PIC` from TERM's icon, adding “VT100” |
| `MCVT.PR`, `MCVT.AFL` | TopSpeed project and add-file list |
| `olib.ext`, `wimp.ext`, `olib.g`, `wimp.g` | Generated descriptions of the ROM classes for CTRAN (in `MCINC`) |
| `M:\MCVT.INI` | Saved settings on the MC (20-byte record, version 2) |

Open items:

- [ ] Confirm the new icon on the MC's system screen.
- [ ] Send the terminal size to the host automatically after a resize, so `rsz` is not needed.
- [ ] Scrollback and capture-to-file.
- [ ] Name the remaining WIMP classes and methods (the `wN_mM` names) as their roles are confirmed.
- [ ] A second screen buffer for the alternate screen, so the shell's text is restored when a full-screen program exits.

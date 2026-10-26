# MCVT formal specification (TLA+)

Two TLA+ models of the parts of MCVT where concurrency or deferred work
could go wrong, written from the 1.0 source and checked with TLC.

| Module | What it models |
| --- | --- |
| `MCVTSerial.tla` | Host → driver buffer → serial reader active object → screen, with XON/XOFF, the window server's priority over the reader, the Serial options dialog's reconfigure, and Exit |
| `MCVTScreen.tla` | VT buffer → window pixels: dirty rows, deferred ("jump") scrolling, the XOR cursor, and other windows covering and uncovering the terminal (also in the middle of a `vt_write`), with redraw events |

## Running

Get `tla2tools.jar` (1.8.0 or later) from
<https://github.com/tlaplus/tlaplus/releases>, then for each config:

```sh
java -cp tla2tools.jar tlc2.TLC -workers auto -deadlock -config serial_flow.cfg MCVTSerial.tla
```

`-deadlock` is needed because each run is bounded (a fixed number of bytes,
operations or window events), so the end of a run is a state with nothing
left to do.

## Configs and results

Each config's first line says what it checks and whether it is meant to
fail. "Expected to fail" configs are there to show that the property
catches the problem: they turn on host behaviour, or put back a bug, that
MCVT's code avoids.

### Serial path (`MCVTSerial.tla`)

| Config | Set-up | Result |
| --- | --- | --- |
| `serial_flow` | Host obeys XON/XOFF | **All hold**: no overrun, bytes shown in order, every byte accounted for, every byte eventually shown |
| `serial_ssh` | Host ignores XOFF (the Pi's tty under ssh, `ixon` off) | `NoOverrun` **fails** (expected): the driver buffer overruns, as seen with `5fmn` over ssh |
| `serial_badorder` | P_FSET with the old read still queued | `NoPanic` **fails** (expected): why `ser_close` cancels the read first |
| `serial_drop` | Settings changed while data flows | `NoReconfigLoss` **fails** (expected): bytes in the driver buffer, and a completed 1-byte read not yet run, are dropped by cancel/close |
| `serial_reconf` | Settings changed; port reopened without sending XON | Safety holds; `HostFinishes` **fails**: if XOFF was in force when the port closed, the host waits for an XON that never comes |
| `serial_reconf_xon` | As above, XON sent after reopening | **All hold**, including `HostFinishes` |

Fairness: the host, driver and scheduler steps are weakly fair; the
serial reader's `ao_run` is strongly fair. With only weak fairness TLC finds
that a user who keeps generating window-server events for ever starves the
reader (the window server has the higher priority), which is true of the
real app but not interesting.

### Screen (`MCVTScreen.tla`)

| Config | Set-up | Result |
| --- | --- | --- |
| `screen_ok` | MCVT's code, window never covered (4×2 cells, 6 operations) | **All hold** (1,468,867 states) |
| `screen_nodirtyshift` | Bug put back: dirty flags stay put when rows scroll | `DeferredOK` **fails** (expected) |
| `screen_noflush` | Bug put back: no flush when the scroll region changes | `DeferredOK` **fails** (expected) |
| `screen_cover` | Windows come and go, even mid-write; window server moves the invalid region with `wScrollRect` | **All hold** (3×2 cells, 4 operations, 2 cover events; 2,610,632 states) |
| `screen_cover_full` | Window server does not move the invalid region, but MCVT's redraw repaints the whole window | `Converged` **holds**: the window is right once the redraw is done (2,978,983 states) |
| `screen_cover_clip` | Window server neither moves the invalid region nor lets the redraw paint outside it | `Converged` **fails**: a scroll copies not-yet-redrawn garbage into a row the redraw will not repaint |

The main invariant is `Consistent`: between `vt_write` calls, every row that
is visible and not waiting for a redraw shows its buffer row, and the only
inverted cell is the cursor, drawn where the buffer's cursor is.
`Converged` is the weaker guarantee of what the user ends up seeing: once
the redraw has been handled, the whole window is right.
`DeferredOK` is the invariant that makes jump scrolling work: during a write,
a clean buffer row is on screen at the row the pending scroll will move it
from.

## What the models found

1. **XON after reopening** (`serial_reconf`). If XOFF is in force when the
   user changes serial settings, the port is closed and reopened with fresh
   driver state, and the host is never sent XON, so it stays stopped.
   `ser_open` sends nothing, and the same applies at start-up if a previous
   session left the host stopped. Fix: send XON (0x11) after `ser_open` when
   XON/XOFF is on. **Fixed in MCVT 1.1.**
2. **Bytes lost on reconfigure** (`serial_drop`). Changing settings drops what
   is in the driver buffer and any completed read not yet processed. Harmless
   in practice (the user is changing the line), but worth knowing; it could be
   avoided by draining the buffer with P_FTEST/P_FREAD before closing.
3. **Window-server assumption** (`screen_cover`, `screen_cover_full`, `screen_cover_clip`). Jump scrolling is safe
   with another window moving over MCVT only if the ROM's window server either
   moves the pending invalid region when `wScrollRect` copies pixels, or lets
   MCVT's redraw paint the whole window. This is untested on the MC; a test is
   to drag a window across MCVT while `ls -lR` scrolls and look for stale
   rows afterwards. If it shows, the fix is to flush (or redraw) before
   `wScrollRect` when a redraw is pending.
4. **ssh overrun** (`serial_ssh`) is the host-side problem the README's
   `stty ixon` advice is for: MCVT cannot prevent it without RTS/CTS.

The models abstract a row's text to an identifier that changes on every
write, the cursor to one cell, and a `vt_write` to a sequence of buffer
operations (print, LF, RI, IL/DL, DECSTBM, CUP, DECTCEM).

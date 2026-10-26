----------------------------- MODULE MCVTSerial -----------------------------
(***************************************************************************)
(* MCVT, the VT100 terminal for the Psion MC400: the serial input path.    *)
(*                                                                         *)
(* Modelled from O_MCVT.C (MCVT 1.0):                                      *)
(*   - the host transmitting bytes, honouring XON/XOFF or not (over ssh    *)
(*     the Pi's tty loses ixon, so XOFF is ignored);                       *)
(*   - the TTY.UAR driver's receive buffer and its XOFF/XON thresholds     *)
(*     (P_SEND_XOFF);                                                      *)
(*   - the serial reader, an OLIB active object: an async 1-byte P_FREAD   *)
(*     on its own stat (ser_queue), then ao_run, which takes the rest of   *)
(*     the buffer with P_FTEST + P_FREAD, draws it (vt_write) and          *)
(*     re-queues;                                                          *)
(*   - the WIMP app manager's scheduler, running one active object at a    *)
(*     time, the window-server client (priority 60) before the reader (59);*)
(*   - Options|Serial: the ROM's modal Serial options dialog, whose nested *)
(*     event loop keeps running the reader; on ENTER, ser_close            *)
(*     (P_FCANCEL + p_waitstat, close), ser_open (open, P_FSET),           *)
(*     ser_start (queue a read);                                           *)
(*   - File|Exit: ser_close, then p_exit.                                  *)
(*                                                                         *)
(* Bytes are numbered 1..MaxBytes so loss and ordering can be checked.     *)
(* Drawing time is not modelled explicitly: the host and driver may take   *)
(* any number of steps between ao_run reading the buffer and the draw      *)
(* finishing, which covers every speed of drawing.                         *)
(*                                                                         *)
(* Copyright (C) 2026 Christian Farrow (zedstarr). GPL-2.0 (see LICENSE).  *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    MaxBytes,        \* bytes the host sends in a run
    BufCap,          \* driver receive buffer size
    HighWater,       \* driver sends XOFF at this fill level
    LowWater,        \* ... and XON again at this level
    Skid,            \* bytes the host may still send after XOFF (latency)
    ChunkMax,        \* most bytes ao_run takes in one go (MCVT: 255)
    HostObeysXoff,   \* FALSE models the Pi's tty under ssh (ixon cleared)
    MaxReconfigs,    \* how many times the user changes serial settings
    CancelBeforeSet, \* TRUE = MCVT's order: cancel the read before P_FSET
    XonOnOpen        \* TRUE = the port is re-opened with the host released

ASSUME /\ LowWater < HighWater /\ HighWater <= BufCap
       /\ ChunkMax >= 1 /\ MaxBytes >= 1

VARIABLES
    sent,       \* bytes the host has transmitted (1..sent)
    rxq,        \* driver receive buffer, oldest first
    xoff,       \* driver's flag: it has sent XOFF and not yet XON
    paused,     \* the host has received XOFF and not yet XON
    skid,       \* bytes the host sent since XOFF
    portOpen,   \* TTY:A/B handle open
    rd,         \* the reader's P_FREAD: "none", "pending" or "done"
    isactive,   \* active.isactive (+9)
    got,        \* the byte a completed 1-byte read delivered (<<>> or <<b>>)
    chunk,      \* bytes taken by ao_run, not yet drawn
    shown,      \* bytes drawn on the terminal, in order
    lost,       \* bytes dropped because the driver buffer was full
    discarded,  \* bytes dropped by cancel/close during a settings change
    wsq,        \* a window-server event is waiting (TRUE/FALSE)
    app,        \* the app's single thread (see AppStates)
    ret,        \* where ao_run returns to: "idle" or "dlg"
    reconfigs,  \* settings changes done
    panic       \* a system panic happened

vars == <<sent, rxq, xoff, paused, skid, portOpen, rd, isactive, got, chunk, shown,
          lost, discarded, wsq, app, ret, reconfigs, panic>>

AppStates == {"idle", "draw", "dlg", "close", "open", "exited"}

Range(s) == {s[i] : i \in DOMAIN s}

TypeOK ==
    /\ sent \in 0..MaxBytes
    /\ rxq \in Seq(1..MaxBytes) /\ Len(rxq) <= BufCap
    /\ xoff \in BOOLEAN /\ paused \in BOOLEAN /\ skid \in 0..MaxBytes
    /\ portOpen \in BOOLEAN
    /\ rd \in {"none", "pending", "done"} /\ isactive \in BOOLEAN
    /\ got \in Seq(1..MaxBytes) /\ Len(got) <= 1
    /\ chunk \in Seq(1..MaxBytes) /\ shown \in Seq(1..MaxBytes)
    /\ lost \subseteq 1..MaxBytes /\ discarded \subseteq 1..MaxBytes
    /\ wsq \in BOOLEAN /\ app \in AppStates /\ ret \in {"idle", "dlg"}
    /\ reconfigs \in 0..MaxReconfigs /\ panic \in BOOLEAN

(* main(): ser_open() then am_init, ser_start() queues the first read. *)
Init ==
    /\ sent = 0 /\ rxq = <<>> /\ xoff = FALSE /\ paused = FALSE /\ skid = 0
    /\ portOpen = TRUE
    /\ rd = "pending" /\ isactive = TRUE /\ got = <<>>
    /\ chunk = <<>> /\ shown = <<>>
    /\ lost = {} /\ discarded = {}
    /\ wsq = FALSE /\ app = "idle" /\ ret = "idle"
    /\ reconfigs = 0 /\ panic = FALSE

-----------------------------------------------------------------------------
(* The host *)

HostMaySend == ~paused \/ ~HostObeysXoff \/ skid < Skid

(* The driver checks its fill level as each byte arrives (receive        *)
(* interrupt) and sends XOFF at HighWater; the host may still send Skid   *)
(* more bytes before it stops (UART FIFO, cable, host latency). *)
HostSend ==
    /\ sent < MaxBytes /\ HostMaySend
    /\ LET b == sent + 1 IN
         IF ~portOpen
           THEN /\ discarded' = discarded \cup {b}          \* nobody listening
                /\ UNCHANGED <<rxq, lost, xoff, paused>>
           ELSE IF Len(rxq) < BufCap
             THEN /\ rxq' = Append(rxq, b) /\ UNCHANGED <<lost, discarded>>
                  /\ IF ~xoff /\ Len(rxq) + 1 >= HighWater
                       THEN xoff' = TRUE /\ paused' = TRUE
                       ELSE UNCHANGED <<xoff, paused>>
             ELSE /\ lost' = lost \cup {b}                   \* overrun
                  /\ UNCHANGED <<rxq, discarded, xoff, paused>>
    /\ sent' = sent + 1
    /\ skid' = IF paused' /\ paused THEN skid + 1 ELSE 0
    /\ UNCHANGED <<portOpen, rd, isactive, got, chunk, shown, wsq, app,
                   ret, reconfigs, panic>>

-----------------------------------------------------------------------------
(* The serial driver *)

DriverSendXon ==
    /\ portOpen /\ xoff /\ Len(rxq) <= LowWater
    /\ xoff' = FALSE /\ paused' = FALSE /\ skid' = 0
    /\ UNCHANGED <<sent, rxq, portOpen, rd, isactive, got, chunk, shown, lost,
                   discarded, wsq, app, ret, reconfigs, panic>>

(* The outstanding 1-byte P_FREAD completes: stat leaves E_FILE_PENDING. *)
DriverCompleteRead ==
    /\ portOpen /\ rd = "pending" /\ rxq # <<>>
    /\ got' = <<Head(rxq)>> /\ rxq' = Tail(rxq) /\ rd' = "done"
    /\ UNCHANGED <<sent, xoff, paused, skid, portOpen, isactive, chunk, shown, lost,
                   discarded, wsq, app, ret, reconfigs, panic>>

-----------------------------------------------------------------------------
(* The window server: events (keys, redraws, menu commands) arrive at any  *)
(* time. *)

WsArrive ==
    /\ ~wsq /\ app # "exited"
    /\ wsq' = TRUE
    /\ UNCHANGED <<sent, rxq, xoff, paused, skid, portOpen, rd, isactive, got, chunk,
                   shown, lost, discarded, app, ret, reconfigs, panic>>

-----------------------------------------------------------------------------
(* The app thread: the app manager's loop (and the dialog's nested loop)   *)
(* runs the highest-priority ready active object. *)

Looping == app \in {"idle", "dlg"}

(* Window-server client first (priority 60): a key, redraw or dialog key. *)
RunWsOther ==
    /\ Looping /\ wsq
    /\ wsq' = FALSE
    /\ UNCHANGED <<sent, rxq, xoff, paused, skid, portOpen, rd, isactive, got, chunk,
                   shown, lost, discarded, app, ret, reconfigs, panic>>

(* Options|Serial: the modal dialog starts its nested loop. *)
OpenSerialDialog ==
    /\ app = "idle" /\ wsq /\ reconfigs < MaxReconfigs
    /\ wsq' = FALSE /\ app' = "dlg"
    /\ UNCHANGED <<sent, rxq, xoff, paused, skid, portOpen, rd, isactive, got, chunk,
                   shown, lost, discarded, ret, reconfigs, panic>>

(* ENTER: the dialog returns 2 and mcvtcom_serial reconfigures the port. *)
DialogEnter ==
    /\ app = "dlg" /\ wsq
    /\ wsq' = FALSE
    /\ app' = IF CancelBeforeSet THEN "close" ELSE "open"
    /\ UNCHANGED <<sent, rxq, xoff, paused, skid, portOpen, rd, isactive, got, chunk,
                   shown, lost, discarded, ret, reconfigs, panic>>

(* File|Exit: ser_close then p_exit (modelled as one step: no other active *)
(* object can run in between). *)
Exit ==
    /\ app = "idle" /\ wsq
    /\ wsq' = FALSE /\ app' = "exited"
    /\ discarded' = discarded \cup Range(got) \cup Range(rxq)
    /\ rd' = "none" /\ isactive' = FALSE /\ got' = <<>>
    /\ rxq' = <<>> /\ portOpen' = FALSE
    /\ UNCHANGED <<sent, xoff, paused, skid, chunk, shown, lost, ret, reconfigs, panic>>

(* ao_run: only when no window-server event is waiting (lower priority). *)
SerAoRun ==
    /\ Looping /\ ~wsq /\ isactive /\ rd = "done"
    /\ LET more == IF Len(rxq) < ChunkMax - 1 THEN Len(rxq) ELSE ChunkMax - 1
       IN /\ chunk' = got \o SubSeq(rxq, 1, more)
          /\ rxq' = SubSeq(rxq, more + 1, Len(rxq))
    /\ got' = <<>> /\ isactive' = FALSE /\ rd' = "none"
    /\ ret' = app /\ app' = "draw"
    /\ UNCHANGED <<sent, xoff, paused, skid, portOpen, shown, lost, discarded, wsq,
                   reconfigs, panic>>

(* vt_write has drawn the chunk; ser_queue issues the next read. *)
SerDrawDone ==
    /\ app = "draw"
    /\ shown' = shown \o chunk /\ chunk' = <<>>
    /\ rd' = IF portOpen THEN "pending" ELSE "none"
    /\ isactive' = portOpen
    /\ app' = ret
    /\ UNCHANGED <<sent, rxq, xoff, paused, skid, portOpen, got, lost, discarded, wsq,
                   ret, reconfigs, panic>>

(* ser_close: ser_stopread (P_FCANCEL; p_waitstat swallows a completion   *)
(* not yet run, so its byte is dropped), then p_close (the driver buffer  *)
(* is discarded with the handle). *)
SerClose ==
    /\ app = "close"
    /\ discarded' = discarded \cup Range(got) \cup Range(rxq)
    /\ rd' = "none" /\ isactive' = FALSE /\ got' = <<>> /\ rxq' = <<>>
    /\ portOpen' = FALSE
    /\ app' = "open"
    /\ UNCHANGED <<sent, xoff, paused, skid, chunk, shown, lost, wsq, ret, reconfigs,
                   panic>>

(* ser_open: p_open, P_FSET (panics if a read is outstanding), then        *)
(* ser_start queues a read. With CancelBeforeSet = FALSE the old handle is *)
(* reconfigured with its read still queued. *)
SerOpenAndStart ==
    /\ app = "open"
    /\ panic' = (rd = "pending")
    /\ portOpen' = TRUE
    /\ xoff' = FALSE                       \* a new handle: fresh driver state
    /\ paused' = IF XonOnOpen THEN FALSE ELSE paused   \* MCVT sends XON?
    /\ skid' = 0
    /\ rd' = "pending" /\ isactive' = TRUE
    /\ app' = "idle" /\ reconfigs' = reconfigs + 1
    /\ UNCHANGED <<sent, rxq, got, chunk, shown, lost, discarded, wsq, ret>>

-----------------------------------------------------------------------------
Next ==
    \/ HostSend \/ DriverSendXon \/ DriverCompleteRead
    \/ WsArrive \/ RunWsOther \/ OpenSerialDialog \/ DialogEnter \/ Exit
    \/ SerAoRun \/ SerDrawDone \/ SerClose \/ SerOpenAndStart
    \/ (app = "exited" /\ sent = MaxBytes /\ UNCHANGED vars)   \* finished

(* The machinery (host, driver, scheduler) is weakly fair; the user is not:*)
(* nothing forces a key press, a dialog or Exit. SerAoRun is strongly     *)
(* fair: the window server outranks the serial reader, so a user who keeps *)
(* typing for ever starves it (TLC shows this with WF); we assume the user *)
(* pauses now and then. *)
Fairness ==
    /\ WF_vars(HostSend) /\ WF_vars(DriverSendXon)
    /\ WF_vars(DriverCompleteRead) /\ WF_vars(RunWsOther)
    /\ WF_vars(DialogEnter)
    /\ SF_vars(SerAoRun) /\ WF_vars(SerDrawDone)
    /\ WF_vars(SerClose) /\ WF_vars(SerOpenAndStart)

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
(* Safety *)

\* P_FSET is never issued with a read outstanding (that panics the MC).
NoPanic == ~panic

\* A read is only outstanding on an open port, and ao_run's flag matches it.
ReadState ==
    /\ rd # "none" => portOpen
    /\ (rd # "none") <=> isactive

\* With flow control honoured and enough headroom, nothing is overrun.
NoOverrun == lost = {}

\* Bytes appear in the order sent, none twice.
InOrder == \A i \in 1..(Len(shown) - 1) : shown[i] < shown[i + 1]

\* Every byte sent is in exactly one place.
Conservation ==
    LET places == <<Range(rxq), Range(got), Range(chunk), Range(shown), lost,
                    discarded>>
    IN /\ UNION {places[i] : i \in 1..6} = 1..sent
       /\ \A i, j \in 1..6 : i # j => places[i] \cap places[j] = {}

\* Exit leaves no read outstanding and the port closed.
ExitClean == app = "exited" => rd = "none" /\ ~portOpen

\* Expected to FAIL when settings are changed: data in flight is dropped.
NoReconfigLoss == app = "exited" \/ discarded = {}

(* Liveness *)

\* Without settings changes, everything sent is eventually shown (unless
\* the user exits first).
AllShown == <>(Len(shown) = MaxBytes \/ app = "exited")

\* The host is never left paused for good: it finishes sending unless the
\* user exits.
HostFinishes == <>(sent = MaxBytes \/ app = "exited")

=============================================================================

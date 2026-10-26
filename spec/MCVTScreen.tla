---------------------------- MODULE MCVTScreen ----------------------------
(***************************************************************************)
(* MCVT's screen update: the VT100 buffer (txt/att in VT.C) against the    *)
(* pixels in the MC400 window, which is not backed up.                     *)
(*                                                                         *)
(* What is modelled, from VT.C and O_MCVT.C:                               *)
(*  - vt_write: hidecur, then the bytes change the buffer only (rows are   *)
(*    marked dirty), then flushrows (applyscroll + repaint dirty rows) and *)
(*    showcur.                                                             *)
(*  - Deferred ("jump") scrolling: scroll() moves buffer rows and their    *)
(*    dirty flags at once but only counts the pixels' scroll in ps_n;      *)
(*    one wScrollRect does it at the flush. A scroll of a different region *)
(*    or direction flushes first. ps_n >= region height: repaint instead.  *)
(*  - XOR block cursor: invcell inverts a cell; curshown/curx/cury record  *)
(*    where it is drawn.                                                   *)
(*  - Other windows cover and uncover rows at any time, even in the middle *)
(*    of a vt_write (the window server is another process). Drawing on a  *)
(*    covered row does nothing; an uncovered row has garbage until the     *)
(*    redraw event (mcvtwin_w0_m23: clear + vt_redraw) repaints it.        *)
(*                                                                         *)
(* A row's text is abstracted to an identifier: every change to a buffer  *)
(* row gives it a fresh one, so "pixels = buffer" means "repainted since  *)
(* the last change, or moved there with its row". 0 is garbage.           *)
(*                                                                         *)
(* Granularity: the window is a column of rows; the cursor is a cell.     *)
(* Each vt_write is split into its steps so that cover/uncover can happen *)
(* between them; within one step the app's drawing is atomic.             *)
(*                                                                         *)
(* Copyright (C) 2026 Christian Farrow (zedstarr). GPL-2.0 (see LICENSE).  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    R,                 \* rows in the terminal
    C,                 \* columns
    MaxOps,            \* VT100 operations in a whole behaviour
    MaxCovers,         \* times another window covers ours
    \* What we do not know about the ROM's window server:
    WservScrollsInvalid, \* wScrollRect moves the pending invalid region with the pixels
    RedrawClipped,       \* drawing during a redraw only reaches the invalid region
    \* Switches that re-introduce bugs (TRUE = MCVT's actual code):
    DirtyTravels,        \* scroll(): dirty[r] = dirty[r-n]
    FlushOnNewRegion     \* scroll(): flushrows() when the region/direction changes

ASSUME R >= 3 /\ C >= 1

Rows == 0..(R - 1)
Min(a, b) == IF a < b THEN a ELSE b

VARIABLES
    buf,        \* [Rows -> Nat]           text of each buffer row
    dirty,      \* [Rows -> BOOLEAN]
    cx, cy,     \* cursor in the buffer (cx = C is the pending-wrap column)
    top, bot,   \* scroll region sr_u .. sr_l - 1
    cursorOn,   \* DECTCEM (F_CURSOR)
    psn, pstop, psbot, psdown,  \* the deferred scroll
    curshown, curx, cury,       \* where the XOR cursor is drawn
    scr,        \* [Rows -> Nat]           pixels of each window row (0 = garbage)
    inv,        \* set of <<row, col>> cells inverted on screen
    covered,    \* rows under another window
    invalid,    \* rows the window server will ask us to redraw
    pc,         \* "idle" | "write"
    ops, next, covers

vars == <<buf, dirty, cx, cy, top, bot, cursorOn, psn, pstop, psbot, psdown,
          curshown, curx, cury, scr, inv, covered, invalid, pc, ops, next, covers>>

TypeOK ==
    /\ buf \in [Rows -> Nat] /\ scr \in [Rows -> Nat]
    /\ dirty \in [Rows -> BOOLEAN]
    /\ cx \in 0..C /\ cy \in Rows
    /\ top \in Rows /\ bot \in 1..R /\ top < bot
    /\ psn \in Nat /\ pc \in {"idle", "write"}
    /\ inv \subseteq (Rows \X (0..(C - 1)))
    /\ covered \subseteq Rows /\ invalid \subseteq Rows

-----------------------------------------------------------------------------
(* Drawing primitives. Each takes the screen state as a record             *)
(* [scr, inv, bad] and a set of rows the drawing can reach (the clip);     *)
(* bad collects rows that the window server marks invalid.                 *)

RowsOf(cells) == {c[1] : c \in cells}

(* paintrow(r) for each r in rs (flushrows also clears the run first). *)
Paint(s, rs, clip) ==
    [s EXCEPT !.scr = [r \in Rows |-> IF r \in rs /\ r \in clip THEN buf[r]
                                                                ELSE s.scr[r]],
              !.inv = {c \in s.inv : ~(c[1] \in rs /\ c[1] \in clip)}]

(* invcell(x, y): gClrRect INV. *)
Invert(s, y, x, clip) ==
    IF y \in clip
      THEN [s EXCEPT !.inv = IF <<y, x>> \in s.inv THEN s.inv \ {<<y, x>>}
                                                  ELSE s.inv \cup {<<y, x>>}]
      ELSE s

(* wScrollRect of rows t..b-1 by n. A destination whose source is covered  *)
(* gets garbage and becomes invalid (the window is not backed up).         *)
(* If WservScrollsInvalid, invalid source rows carry their invalidity.     *)
ScrollPix(s, t, b, n, down, clip) ==
    LET dests == IF down THEN (t + n)..(b - 1) ELSE t..(b - 1 - n)
        src(d) == IF down THEN d - n ELSE d + n
        vis == {d \in dests : d \in clip}
        fromHidden == {d \in vis : src(d) \notin clip}
        carried == IF WservScrollsInvalid
                     THEN {d \in vis : src(d) \in invalid} ELSE {}
    IN [scr |-> [r \in Rows |->
                   IF r \in vis THEN (IF r \in fromHidden THEN 0 ELSE s.scr[src(r)])
                   ELSE s.scr[r]],
        inv |-> {c \in s.inv : c[1] \notin vis}
                \cup UNION {{<<d, c[2]>> : c \in {e \in s.inv : e[1] = src(d)}} :
                            d \in vis \ fromHidden},
        bad |-> s.bad \cup fromHidden \cup carried]

Screen == [scr |-> scr, inv |-> inv, bad |-> {}]
Visible == Rows \ covered

(* flushrows: applyscroll, then repaint dirty rows. *)
Flush(s, d) ==
    LET s1 == IF psn > 0 THEN ScrollPix(s, pstop, psbot, psn, psdown, Visible) ELSE s
    IN Paint(s1, {r \in Rows : d[r]}, Visible)

SetScreen(s) ==
    /\ scr' = s.scr /\ inv' = s.inv /\ invalid' = invalid \cup s.bad

-----------------------------------------------------------------------------
Init ==
    /\ buf = [r \in Rows |-> r + 1] /\ next = R + 1
    /\ dirty = [r \in Rows |-> FALSE]
    /\ cx = 0 /\ cy = 0 /\ top = 0 /\ bot = R /\ cursorOn = TRUE
    /\ psn = 0 /\ pstop = 0 /\ psbot = R /\ psdown = FALSE
    /\ scr = buf /\ inv = {<<0, 0>>}
    /\ curshown = TRUE /\ curx = 0 /\ cury = 0
    /\ covered = {} /\ invalid = {}
    /\ pc = "idle" /\ ops = 0 /\ covers = 0

-----------------------------------------------------------------------------
(* vt_write starts: hidecur. *)
BeginWrite ==
    /\ pc = "idle" /\ ops < MaxOps
    /\ pc' = "write"
    /\ IF curshown THEN SetScreen(Invert(Screen, cury, curx, Visible))
                   ELSE UNCHANGED <<scr, inv, invalid>>
    /\ curshown' = FALSE
    /\ UNCHANGED <<buf, dirty, cx, cy, top, bot, cursorOn, psn, pstop, psbot, psdown,
                   curx, cury, covered, ops, next, covers>>

InWrite == pc = "write" /\ ops < MaxOps /\ ops' = ops + 1

(* scroll(t, b, 1, down): the buffer and dirty flags move now; the pixels  *)
(* are only counted. May flush the previous deferred scroll first.         *)
Scroll(t, b, down) ==
    LET flushFirst == psn > 0 /\ FlushOnNewRegion
                      /\ (pstop # t \/ psbot # b \/ psdown # down)
        s0 == IF flushFirst THEN Flush(Screen, dirty) ELSE Screen
        d0 == IF flushFirst THEN [r \in Rows |-> FALSE] ELSE dirty
        n0 == IF flushFirst THEN 0 ELSE psn
        vac == IF down THEN t ELSE b - 1           \* the blank row
        src(r) == IF down THEN r - 1 ELSE r + 1
        moved(r) == r \in t..(b - 1) /\ r # vac
        d1 == [r \in Rows |-> IF r = vac THEN TRUE
                              ELSE IF moved(r) /\ DirtyTravels THEN d0[src(r)]
                              ELSE d0[r]]
        n1 == n0 + 1
        whole == n1 >= b - t
    IN /\ buf' = [r \in Rows |-> IF r = vac THEN next
                                 ELSE IF moved(r) THEN buf[src(r)] ELSE buf[r]]
       /\ next' = next + 1
       /\ SetScreen(s0)
       /\ IF whole
            THEN psn' = 0 /\ dirty' = [r \in Rows |-> d1[r] \/ r \in t..(b - 1)]
            ELSE psn' = n1 /\ dirty' = d1
       /\ pstop' = t /\ psbot' = b /\ psdown' = down

Unch == <<curshown, curx, cury, covered, covers>>

(* A printable character: written into row cy (wrapping first if pending). *)
Put ==
    /\ InWrite
    /\ IF cx = C
         THEN \* auto wrap: nl() then write at column 0
              IF cy = bot - 1
                THEN /\ Scroll(top, bot, FALSE)
                     /\ cx' = 1 /\ UNCHANGED <<cy, top, bot, cursorOn>>
                     \* the new char lands in the blank row: give it a fresh id
                ELSE /\ cy' = Min(cy + 1, R - 1) /\ cx' = 1
                     /\ buf' = [buf EXCEPT ![Min(cy + 1, R - 1)] = next]
                     /\ next' = next + 1
                     /\ dirty' = [dirty EXCEPT ![Min(cy + 1, R - 1)] = TRUE]
                     /\ UNCHANGED <<top, bot, cursorOn, psn, pstop, psbot, psdown,
                                    scr, inv, invalid>>
         ELSE /\ buf' = [buf EXCEPT ![cy] = next] /\ next' = next + 1
              /\ dirty' = [dirty EXCEPT ![cy] = TRUE]
              /\ cx' = cx + 1
              /\ UNCHANGED <<cy, top, bot, cursorOn, psn, pstop, psbot, psdown,
                             scr, inv, invalid>>
    /\ pc' = pc /\ UNCHANGED Unch

(* LF / IND: nl(). *)
LineFeed ==
    /\ InWrite
    /\ IF cy = bot - 1
         THEN Scroll(top, bot, FALSE) /\ UNCHANGED cy
         ELSE /\ cy' = Min(cy + 1, R - 1)
              /\ UNCHANGED <<buf, next, dirty, psn, pstop, psbot, psdown,
                             scr, inv, invalid>>
    /\ UNCHANGED <<cx, top, bot, cursorOn, pc>> /\ UNCHANGED Unch

(* ESC M (RI): scroll down at the top margin. *)
ReverseIndex ==
    /\ InWrite
    /\ IF cy = top
         THEN Scroll(top, bot, TRUE) /\ UNCHANGED cy
         ELSE /\ cy' = IF cy > 0 THEN cy - 1 ELSE cy
              /\ UNCHANGED <<buf, next, dirty, psn, pstop, psbot, psdown,
                             scr, inv, invalid>>
    /\ UNCHANGED <<cx, top, bot, cursorOn, pc>> /\ UNCHANGED Unch

(* CSI L / CSI M (IL/DL): scroll(cy, sr_l, 1, down) inside the region:    *)
(* a different region from the line feeds', so it exercises the flush.    *)
InsDelLine ==
    /\ InWrite /\ cy >= top /\ cy < bot - 1
    /\ \E down \in BOOLEAN : Scroll(cy, bot, down)
    /\ cx' = 0
    /\ UNCHANGED <<cy, top, bot, cursorOn, pc>> /\ UNCHANGED Unch

(* CSI r (DECSTBM): new region, cursor home. *)
SetRegion ==
    /\ InWrite
    /\ \E t \in Rows, b \in 1..R : b - t >= 2 /\ top' = t /\ bot' = b
    /\ cx' = 0 /\ cy' = 0
    /\ UNCHANGED <<buf, next, dirty, cursorOn, psn, pstop, psbot, psdown,
                   scr, inv, invalid, pc>> /\ UNCHANGED Unch

(* CUP and DECTCEM. *)
MoveCursor ==
    /\ InWrite
    /\ \E r \in Rows, c \in 0..C : cy' = r /\ cx' = c
    /\ UNCHANGED <<buf, next, dirty, top, bot, cursorOn, psn, pstop, psbot, psdown,
                   scr, inv, invalid, pc>> /\ UNCHANGED Unch

ToggleCursor ==
    /\ InWrite
    /\ cursorOn' = ~cursorOn
    /\ UNCHANGED <<buf, next, dirty, cx, cy, top, bot, psn, pstop, psbot, psdown,
                   scr, inv, invalid, pc>> /\ UNCHANGED Unch

(* vt_write ends: flushrows, showcur. *)
EndWrite ==
    /\ pc = "write"
    /\ LET s1 == Flush(Screen, dirty)
           x == Min(cx, C - 1)
           s2 == IF cursorOn THEN Invert(s1, cy, x, Visible) ELSE s1
       IN /\ SetScreen(s2)
          /\ curshown' = cursorOn
          /\ curx' = IF cursorOn THEN x ELSE curx
          /\ cury' = IF cursorOn THEN cy ELSE cury
    /\ psn' = 0 /\ dirty' = [r \in Rows |-> FALSE]
    /\ pc' = "idle"
    /\ UNCHANGED <<buf, cx, cy, top, bot, cursorOn, pstop, psbot, psdown,
                   covered, ops, next, covers>>

-----------------------------------------------------------------------------
(* The window server and other windows. *)

(* Another window comes over some of our rows. *)
Cover ==
    /\ covers < MaxCovers
    /\ \E S \in (SUBSET Rows) \ {{}} :
         /\ covered' = covered \cup S
         /\ scr' = [r \in Rows |-> IF r \in S THEN 0 ELSE scr[r]]
         /\ inv' = {c \in inv : c[1] \notin S}
    /\ covers' = covers + 1
    /\ UNCHANGED <<buf, dirty, cx, cy, top, bot, cursorOn, psn, pstop, psbot, psdown,
                   curshown, curx, cury, invalid, pc, ops, next>>

(* It goes away: those rows show garbage and are invalid. *)
Uncover ==
    /\ covered # {}
    /\ covered' = {} /\ invalid' = invalid \cup covered
    /\ UNCHANGED <<buf, dirty, cx, cy, top, bot, cursorOn, psn, pstop, psbot, psdown,
                   curshown, curx, cury, scr, inv, pc, ops, next, covers>>

(* The redraw event, handled between vt_writes: mcvtwin_w0_m23 clears the  *)
(* window and vt_redraw paints every row, sets curshown = 0, showcur.      *)
Redraw ==
    /\ pc = "idle" /\ invalid # {}
    /\ LET clip == IF RedrawClipped THEN invalid \ covered ELSE Visible
           s1 == Paint(Screen, Rows, clip)
           x == Min(cx, C - 1)
           s2 == IF cursorOn THEN Invert(s1, cy, x, clip) ELSE s1
       IN /\ scr' = s2.scr /\ inv' = s2.inv
          /\ curshown' = cursorOn
          /\ curx' = IF cursorOn THEN x ELSE curx
          /\ cury' = IF cursorOn THEN cy ELSE cury
    /\ invalid' = {}
    /\ dirty' = [r \in Rows |-> FALSE]
    /\ UNCHANGED <<buf, cx, cy, top, bot, cursorOn, psn, pstop, psbot, psdown,
                   covered, pc, ops, next, covers>>

-----------------------------------------------------------------------------
Next ==
    \/ BeginWrite \/ Put \/ LineFeed \/ ReverseIndex \/ InsDelLine
    \/ SetRegion \/ MoveCursor \/ ToggleCursor \/ EndWrite
    \/ Cover \/ Uncover \/ Redraw

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties *)

CursorCells(r) ==
    IF curshown /\ cury = r THEN {<<r, curx>>} ELSE {}

(* Between vt_writes, every row we can see and have not been asked to     *)
(* redraw shows its buffer row, and the only inverted cell is the cursor, *)
(* drawn where the buffer's cursor is.                                    *)
Consistent ==
    pc = "idle" =>
      /\ \A r \in Rows \ (covered \cup invalid) :
            /\ scr[r] = buf[r]
            /\ {c \in inv : c[1] = r} = CursorCells(r)
      /\ curshown = cursorOn
      /\ curshown => cury = cy /\ curx = Min(cx, C - 1)

(* What the user ends up seeing: once the redraw has been handled, the    *)
(* whole visible window is right. Weaker than Consistent, which also asks  *)
(* that rows outside the invalid region are right before the redraw.      *)
Converged ==
    pc = "idle" /\ invalid = {} =>
      /\ \A r \in Visible :
            /\ scr[r] = buf[r]
            /\ {c \in inv : c[1] = r} = CursorCells(r)
      /\ curshown = cursorOn
      /\ curshown => cury = cy /\ curx = Min(cx, C - 1)

(* No scroll is left pending between writes; nothing is left dirty. *)
Settled == pc = "idle" => psn = 0 /\ \A r \in Rows : ~dirty[r]

(* While a write is in progress the cursor is not drawn, so the deferred  *)
(* scroll never moves an inverted cell. *)
CursorHiddenInWrite ==
    pc = "write" => ~curshown /\ \A r \in Visible \ invalid : {c \in inv : c[1] = r} = {}

(* The core of deferred scrolling: during a write, a clean row r of the    *)
(* buffer is on screen at the row it will be scrolled from.                *)
PendingSrc(r) == IF psdown THEN r - psn ELSE r + psn
InScrollRegion(r) == r \in pstop..(psbot - 1)
DeferredOK ==
    pc = "write" =>
      \A r \in Rows :
        ~dirty[r] =>
          LET s == IF psn > 0 /\ InScrollRegion(r) THEN PendingSrc(r) ELSE r
          IN s \in Rows /\ s \notin (covered \cup invalid) => scr[s] = buf[r]
=============================================================================

# MC400 vs Series 3 window server — opcode map

Source: MC400 ROM v2.60F (06/08/92), Series 3 ROM v1.77F (A6/A7-unscrambled). Window server = SYS$WSRV.IMG
(MC: ROM 0xD68F0, 38704 B, code at +0xC90; S3: ROM 0xD4AC0, 33616 B, code at +0x460).
Command loop: MC code 0x07D5, S3 0x077A. Opcode byte >= 0x65 rejected (err 0x66).
Dispatch table: MC cs:0x064D / flags 0x0717; S3 cs:0x05FB / flags 0x06C5. Client opcode = INT 8Dh AH & 0x7F.
Reject stub: MC 0x0648 / S3 0x05F6 — both `mov al,70h; jmp error`.

`sim` = instruction-sequence similarity of the first 20 instructions of the handler (1.00 = same code, relocated).

| op | MC handler | S3 handler | flags | sim | call(s) |
|---|---|---|---|---|---|
| 00 | 2523x1 | 23AEx1 | 00 | 1.00 | wEndRedraw($00) |
| 01 | 4D19x1 | 47CEx1 | 00 | 1.00 | wEraseTextCursor($01) |
| 02 | 5ACBx1 | 0CDEx2 | 00 | 0.67 | wReleaseMouse($02) |
| 03 | 1A01x1 | 18F6x1 | 00 | 1.00 | gFreeTempGC($03) |
| 04 | 0A5Cx1 | 0A17x1 | 00 | 1.00 | wCancel($04) |
| 05 | 3D36x1 | 3A00x1 | 00 | 1.00 | wDisconnect($85) |
| 06 | 66D6x1 | **REJECTED** | 00 | 0.10 | wDetachClient($06) |
| 07 | 0A1Bx1 | 09D6x1 | 00 | 1.00 | wCleanUp($07) |
| 08 | 65EDx1 | **REJECTED** | 00 | 0.15 | wAttachToForegroundClient($08) |
| 09 | 0A54x1 | 0A0Fx1 | 00 | 1.00 | wUserMsg($09) |
| 0A | 0C48x1 | 0BDFx1 | 00 | 0.85 | wStartCompute($8A) |
| 0B | 0C4Fx1 | 0BE6x1 | 00 | 0.82 | wEndCompute($8B) |
| 0C | 0A7Ex1 | 0A39x1 | 00 | 0.71 | wClientInfo($8C) |
| 0D | 0B24x2 | 0AD8x2 | 08 | 1.00 | wCloseWindowTree($0D) |
| 0E | 3614x1 | 33D0x1 | 06 | 1.00 | wInquireWindow($8E) |
| 0F | 5AB9x1 | 0CDEx2 | 02 | 0.20 | wCaptureMouse($0F) |
| 10 | 2417x1 | 22A2x1 | 0A | 1.00 | wBeginRedrawWin($10) |
| 11 | 0B24x2 | 0AD8x2 | 08 | 1.00 | wFree($11) |
| 12 | 371Dx1 | 34DEx1 | 0A | 0.92 | wMakeInvisible($12) |
| 13 | 6619x1 | **REJECTED** | 00 | 0.10 | wAttachtoClient($93) |
| 14 | 3660x1 | 3419x1 | 02 | 1.00 | wInitialiseWindowTree($14) |
| 15 | 23B2x1 | 223Dx1 | 02 | 1.00 | wInvalidateWin($15) |
| 16 | 2435x1 | 22C0x1 | 0A | 1.00 | wValidateWin($16) |
| 17 | 3687x1 | 3440x1 | 02 | 1.00 | wMakeVisible($17) |
| 18 | 0C8Bx1 | 0C23x1 | 00 | 0.57 | wClientIconised($18) |
| 19 | 690Fx1 | 49CBx1 | 00 | 0.57 | wClientPosition($19) |
| 1A | 3A89x1 | 3818x1 | 06 | 1.00 | wInquireWindowOffset($9A) |
| 1B | 37B8x1 | 357Fx1 | 02 | 0.28 | wWindowPosition($1B) |
| 1C | 39E2x1 | 3771x1 | 08 | 1.00 | wScrollRect($1C) |
| 1D | 54E5x1 | **REJECTED** | 03 | 0.29 | wRubberBand($1D) |
| 1E | 296Ax1 | 27A2x1 | 10 | 1.00 | gCopyRect($1E) |
| 1F | 297Bx1 | 27B3x1 | 00 | 1.00 | gPeekBit($9F) |
| 20 | 765Cx1 | 6B94x1 | 10 | 1.00 | gDrawPolyLine($20) |
| 21 | 2655x1 | 24E1x1 | 10 | 1.00 | gFillPattern($21) |
| 22 | 1A62x1 | 1957x1 | 00 | 1.00 | gCreateTempGC($22) |
| 23 | 1A78x1 | 196Dx1 | 00 | 1.00 | gCreateGC($A3) |
| 24 | 1A19x1 | 190Ex1 | 00 | 1.00 | gSetGC($24) |
| 25 | 35DAx1 | 3396x1 | 02 | 1.00 | wSetWindow($25) |
| 26 | 23C4x1 | 224Fx1 | 0B | 1.00 | wBeginRedrawWinGC($26) |
| 27 | 75CBx1 | 6B03x1 | 10 | 1.00 | gDrawLine($27) |
| 28 | 7D0Dx1 | 71F5x1 | 10 | 1.00 | gPrintText($28) |
| 29 | 2942x1 | 277Ax1 | 10 | 1.00 | gCopyBit($29) |
| 2A | 3494x1 | 3288x1 | 06 | 0.48 | wCreateWindow($AA) |
| 2B | 23CAx1 | 2255x1 | 0B | 1.00 | wBeginRedrawGC($2B) |
| 2C | 27B6x1 | 25EEx1 | 10 | 1.00 | gPrintClipText($AC) |
| 2D | 7BCEx1 | 70B6x1 | 10 | 1.00 | gPrintBoxText($2D) |
| 2E | 452Dx1 | 4137x1 | 00 | 1.00 | gOpenBit($AE) |
| 2F | 46D8x1 | 4169x2 | 00 | 0.43 | gOpenMouseIcon($AF) |
| 30 | 04EDx1 | 04A9x1 | 00 | 1.00 | gOpenFont($B0) |
| 31 | 28DFx1 | 2717x1 | 10 | 1.00 | gDrawBox($31) |
| 32 | 61CCx1 | 4169x2 | 00 | 0.48 | wLoadDYL($B2) |
| 33 | 4113x1 | 3D1Dx1 | 00 | 1.00 | gCreateBit($B3) |
| 34 | 3A14x1 | 37A3x1 | 08 | 1.00 | wScrollWin($34) |
| 35 | 4D44x1 | 47F9x1 | 02 | 1.00 | wDrawTextCursor($35) |
| 36 | 2383x1 | 220Ex1 | 02 | 1.00 | wInvalidateRect($36) |
| 37 | 23FEx1 | 2289x1 | 0A | 1.00 | wBeginRedraw($37) |
| 38 | 2429x1 | 22B4x1 | 0A | 1.00 | wValidateRect($38) |
| 39 | 48DBx1 | 431Bx1 | 00 | 1.00 | gSaveBit($B9) |
| 3A | 758Cx1 | 6AC4x1 | 10 | 1.00 | gClrRect($3A) |
| 3B | 6D2Bx1 | **REJECTED** | 10 | 0.10 | wCallDYL($3B) |
| 3C | 6DF9x1 | **REJECTED** | 10 | 0.19 | wCallDYLreply($BC) |
| 3D | 49F1x1 | 4431x1 | 02 | 1.00 | wSetWinBitmap($BD) |
| 3E | 4AEBx1 | 4534x1 | 00 | 1.00 | wChangeWinBitmap($3E) |
| 3F | 0C64x1 | 0BFCx1 | 00 | 1.00 | wEscapeon($BF) |
| 40 | 077Cx1 | 072Ax1 | 00 | 0.52 | RConnect($40) |
| 41 | 0C72x1 | 0C0Ax1 | 00 | 1.00 | wEscapeoff($C1) |
| 42 | 37FCx1 | 3590x1 | 02 | 0.83 | wGetWindowPosition($C2) |
| 43 | 48E8x1 | 4328x1 | 00 | 1.00 | gSaveRect($C3) |
| 44 | 0C7Bx1 | 0C13x1 | 06 | 1.00 | wReassignRootWindow($44) |
| 45 | 17D6x1 | 16DCx1 | 00 | 0.94 | wCaptureKey($C5) |
| 46 | 1807x1 | 1706x1 | 00 | 1.00 | wCancelCaptureKey($C6) |
| 47 | 3BCBx1 | 393Cx1 | 00 | 1.00 | wSystemModal($47) |
| 48 | 3BC0x1 | 3931x1 | 00 | 1.00 | wCancelSystemModal($48) |
| 49 | 2DB1x1 | 2BE9x1 | 10 | 1.00 | gBorder($49) |
| 4A | 2C13x1 | 2A4Bx1 | 10 | 1.00 | gBorderRect($4A) |
| 4B | 2803x1 | 263Bx1 | 10 | 1.00 | gXPrintText($4B) |
| 4C | 2A0Fx1 | 2847x1 | 10 | 1.00 | gInvObloid($4C) |
| 4D | 0C5Fx1 | 0BF7x1 | 00 | 1.00 | wEnablePauseKey($4D) |
| 4E | 0C6Dx1 | 0C05x1 | 00 | 1.00 | wDisablePauseKey($CE) |
| 4F | **REJECTED** | 4D21x1 | 00 | 0.17 | wsEnable($4F) |
| 50 | **REJECTED** | 4D44x1 | 00 | 0.19 | wsDisable($50) |
| 51 | 0D26x1 | 0CAEx1 | 00 | 1.00 | wInformOn($51) |
| 52 | **REJECTED** | 0CB6x1 | 00 | 0.20 | wsUpdate($52) |
| 53 | **REJECTED** | 54A4x1 | 02 | 0.11 | wsCreateClock($D3) |
| 54 | **REJECTED** | 54CAx1 | 00 | 0.18 | wsSetClock($54) |
| 55 | **REJECTED** | 6081x1 | 00 | 0.14 | wSetBusyMsg($D5) |
| 56 | **REJECTED** | 4D82x1 | 00 | 0.08 | wsDisableTemp($56) |
| 57 | **REJECTED** | 4D7Ax1 | 00 | 0.08 | wsEnableTemp($57) |
| 58 | 3ED4x1 | 3B20x1 | 00 | 0.86 | wSystem($D8) |
| 59 | 0C9Fx1 | 0C27x1 | 00 | 1.00 | wGetProcessList($D9) |
| 5A | 0CCCx1 | 0C54x1 | 00 | 1.00 | wSendCommand($DA) |
| 5B | 0D0Ax1 | 0C92x1 | 00 | 1.00 | wGetCommand($DB) |
| 5C | 4D48x1 | 47FDx1 | 02 | 1.00 | wTextCursor($5C) |
| 5D | **REJECTED** | 4BFFx1 | 00 | 0.24 | wAppKeyHandler($DD) |
| 5E | **REJECTED** | 6012x1 | 00 | 0.19 | wInfoMsgCorner($DE) |
| 5F | 496Ex1 | 43AEx1 | 00 | 1.00 | gSetOpenAddress($5F) |
| 60 | **REJECTED** | 2C30x1 | 10 | 0.24 | wDrawButton($60) |
| 61 | 17BBx1 | 16C1x1 | 00 | 1.00 | wSetTaskKey($E1) |
| 62 | 17B7x1 | 16BDx1 | 00 | 1.00 | wSetBackTaskKey($E2) |
| 63 | 1803x1 | 1702x1 | 00 | 1.00 | wCancelTaskKey($E3) |
| 64 | 17FFx1 | 16FEx1 | 00 | 1.00 | wCancelBackTaskKey($E4) |

## Rejected by the MC400 server (S3 status-window / S3 UI features)
wsEnable, wsDisable, wsUpdate, wsCreateClock, wsSetClock, wSetBusyMsg, wsDisableTemp, wsEnableTemp, wAppKeyHandler, wInfoMsgCorner, wDrawButton.
Also never reach the table (opcode >= 0x65): wsStatusWindow ($6B), wInquireStatusWindow ($F0), gShadowText, gDrawObject and other v4 calls.

## Rejected by the S3 server (MC multi-window features)
wDetachClient, wAttachToForegroundClient, wAttachtoClient, wRubberBand, wCallDYL, wCallDYLreply.

## Same code on both (nfsc vt100.c needs only these)
gPrintText 0.98 (146 instr), gClrRect 0.96, wScrollRect 1.00, gSetGC 0.97, gCreateGC 0.97, wTextCursor 0.94, gOpenFont 0.88 (full-handler comparison).
Differs materially: wCreateWindow 0.48, wWindowPosition 0.28, gOpenMouseIcon, wLoadDYL, RConnect, wClientPosition/Iconised.

## ROM file systems
MC: SYS$CTRY.CFO SYS$WSRV.IMG SYS$NTFY.IMG OLIB.DYL WIMP.DYL OPL.DYL S_.RSC SYS$SHLL.IMG OPL.ICN SYS$CON.IMG SYS$OPLT.IMG SYS$OPLR.IMG OPLTERR.RSC SYS$RFSV.IMG R_.HFT BJ.WDR BIG.FON SMALL.FON MONO.FON SYS$ENV.INI (dir 0xD5718, data base 0xD5710, entry = 8-byte [flags, para, size32] + 12-byte name)
S3: SYS$CTRY.CFO SYS$WSRV.IMG OPL.DYL OLIB.DYL FORM.DYL HWIM.DYL SYS$SHLL.IMG SYS$PRGO.IMG CALC.APP SYS$NORM.FON SYS$BOLD.FON R_.HFT RUNOPL.APP + apps in the 128K chip.

## WIMP.DYL (MC, ROM 0xE49B0, 54544 B) — old-format DYL with 0xE018-tagged class records
15 classes; parents are OLIB classes (ref 0x6Bnn = OLIB class nn).
| class | parent | methods | window-server calls attributed |
|---|---|---|---|
| 0 | OLIB#0 | 25 | gFillPattern, gCreateTempGC, wValidateWin, wFlush, wEndRedraw (drawable/window base) |
| 1 | OLIB#26 | 4 | gTextWidth |
| 2 | OLIB#4 | 2 | – |
| 3 | OLIB#0 | 7 | wDetachClient, wAttachToForegroundClient (client/task switching) |
| 4–7, 10, 11, 14 | OLIB#1 | 3–8 | – (non-graphics helpers) |
| 8, 9 | OLIB#2 | 4 | – |
| 10 | OLIB#1 | 5 | gTextWidth |
| 12 | OLIB#1 | 31 | wConnect, wGetEvent loop, gCreateGC/gSetGC, wCaptureMouse, wClientIconised, wClientPosition, wFree, wCancel — the application/window-manager client class; its last method is followed by the thin wrapper block for every draw call (gPrintText, gClrRect, wScrollRect, wScrollWin, wDrawTextCursor, wRubberBand …) |
| 13 | OLIB#1 | 18 | wCleanUp |

S3 HWIM.DYL (52704 B) is a newer DYL format (no 0xE018 records) and calls wsUpdate, wsEnableTemp, wDrawButton, wSetBusyMsg, wInfoMsgCorner — five of the calls the MC server rejects.

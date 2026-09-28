@echo off
setlocal

rem ---------------------------------------------------------------------------
rem The Last of Us (BCUS98174) - Remix backend launch script.
rem Updated 2026-09-20. The title now reaches the menu and Hometown gameplay on
rem update 1.11. Per-title Remix decisions live in bin\BCUS98174.conf; this file
rem only selects diagnostic verbosity and boots the disc copy.
rem ---------------------------------------------------------------------------

rem Stage 1 census run: set this to 1 to emit the per-unique-VP dump into
rem bin\remix_dump.log. Leave at 0 for a plain boot.
rem NOTE: bin\remix_dump.log is opened in APPEND mode and is shared by every
rem title. It was already 425 MB on 2026-08-15. Truncate it before a census run
rem or the TLOU lines will be buried under the Haze/R2 history.
set "RPCS3_REMIX_DUMP=0"

cd /d "%~dp0bin"
start "" "rpcs3.exe" "E:\PS3 Games\Last of Us, The (USA) (En,Fr,Es,Pt)\PS3_GAME\USRDIR\EBOOT.BIN"

@echo off
setlocal

rem ---------------------------------------------------------------------------
rem The Last of Us (BCUS98174) - Remix backend launch script.
rem Created 2026-08-15 for the TLOU bring-up plan, stage 0b/1.
rem
rem STATUS: BLOCKED AT STAGE 0. This script is set up and ready, but the title
rem does NOT currently reach a playable state on this build. TLOU freezes ~25-30
rem seconds after boot (just past the save-slot scan, at the title/menu hand-off)
rem and the freeze reproduces IDENTICALLY on the stock Vulkan renderer, so it is
rem not a Remix defect. Evidence and analysis:
rem   .claude/forge/20260811-0733-tlou-rpcs3-remix-shaders/report.md
rem
rem The freeze signature (Remix run, 2026-08-15 12:32):
rem   Remix: no flip for 2.0s | fifo_state=1 in_begin_end=0 async_flip=0x0
rem          | draws=762 meshes_live=0 created=0 | get=0x10008 put=0x10008
rem   Remix stall/rsx: flip_status=0 vsync=1 vblank=7754 int_flip=376 ...
rem   Remix stall/t: lv2_pending=0 sched_ready=1 lv2_mutex_free=1
rem   Remix stall/t: ppu[0x1000000] 'main_thread' pc=0xe1e140->0xe1e0ec moving
rem The guest's own main thread is spinning; the emulator scheduler is healthy.
rem
rem Do not add per-VP knobs here until the title actually renders world geometry.
rem Every draw in the frozen run was an RSX immediate-mode draw, which the
rem backend skips by design (RemixGSRender.cpp, "skip_immediate"), so there is
rem no census to tune against yet.
rem ---------------------------------------------------------------------------

rem Stage 1 census run: set this to 1 to emit the per-unique-VP dump into
rem bin\remix_dump.log. Leave at 0 for a plain boot.
rem NOTE: bin\remix_dump.log is opened in APPEND mode and is shared by every
rem title. It was already 425 MB on 2026-08-15. Truncate it before a census run
rem or the TLOU lines will be buried under the Haze/R2 history.
set "RPCS3_REMIX_DUMP=0"

cd /d "%~dp0bin"
start "" "rpcs3.exe" "E:\PS3 Games\Last of Us, The (USA) (En,Fr,Es,Pt)\PS3_GAME\USRDIR\EBOOT.BIN"

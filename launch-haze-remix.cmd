@echo off
setlocal

rem ############################################################################
rem ## A/B SWEEP LAUNCHER. The settled configuration is NOT here any more.     ##
rem ############################################################################
rem
rem The 217 knobs this file used to carry now live in  bin\BLUS30094.conf  and are
rem loaded by the backend itself at init, before any knob is read. Two consequences,
rem and the first one retires a trap that cost this project a whole round:
rem
rem   1. THE GAME NO LONGER HAS TO BE STARTED FROM THIS FILE. Launching rpcs3-next.exe
rem      directly, or from the GUI's recent list, now gets the full configuration --
rem      including CAMLOCKVP. The round-48 play-test that came back as "the entire area
rem      is warping like crazy" was exactly that failure: the launcher was bypassed,
rem      camlock read 0000000000000000, cam_fallback was 43.5% against a healthy 3.5%,
rem      and the dev menu showed FOV 114.6 with a 6.1-unit far plane. That specific
rem      failure mode is now structurally impossible for the settled knobs.
rem
rem   2. ANYTHING SET IN THIS FILE STILL WINS. The config loader never overwrites a
rem      variable that is already set, so a knob put here overrides bin\BLUS30094.conf
rem      without editing or commenting anything out there. That is the whole point of
rem      this file now: it is the sweep, not the configuration.
rem
rem STILL CHECK camlock= IN THE FIRST TEN LINES OF bin\remix_dump.log. It should now be
rem non-zero whichever way the game was started. If it is zeros, the CONFIG did not load
rem -- look for the "Remix game-config:" line, which says what it found and how many keys
rem it applied, skipped as already-set, or ignored.
rem
rem To change a settled value permanently, edit bin\BLUS30094.conf. To try one for a
rem single run, put it below. Note the config is read ONCE at init, so either way a
rem change needs a restart.

rem ============================================================================
rem THE ARM. Put the knobs under test here, one per line, and delete them when the
rem round is settled and the value has moved into bin\BLUS30094.conf.
rem ============================================================================

rem ROUND 49 left a (vp, fp, albedo) TRIPLE route deliberately unarmed so it can be
rem turned on from here with no rebuild. It is the one census large enough to answer
rem "does the fragment program separate the player's rig from the NPC bodies that share
rem its albedo". Uncomment to arm:
rem set "RPCS3_REMIX_VMTRIPLEVP=830D7D1B9681C475,F39F504649B6F442"
rem set "RPCS3_REMIX_VMTRIPLEFP=0CCD70030837EE85"
rem set "RPCS3_REMIX_VMTRIPLEALBEDO=86885A0E60751491"

rem OPEN, 2026-09-01: soldiers leave yellow light trails. The round-48 static-fixture
rem gate exists to stop exactly this ("it lit SOLDIERS as they walked... a walking NPC
rem leaves a trail of cells"), but it only runs for AUTO triggers -- see the gate's
rem condition, `stable_frames != 0 && auto_trigger && !trigger_accepted`. The config
rem uses an explicit GUESTLIGHTVP/FP pair, which takes the trigger_accepted path and
rem skips the gate entirely. Not yet confirmed by measurement: watch the guest-light
rem count in remix_dump.log while a soldier walks, and see whether it climbs toward
rem GUESTLIGHTMAX. A short GUESTLIGHTIDLE shortens the trail without stopping the
rem minting; the real fix is a narrower trigger or extending the gate to explicit ones.
rem set "RPCS3_REMIX_GUESTLIGHTIDLE=1"

rem ============================================================================

rem The game. Set RPCS3_HAZE_EBOOT to override without editing this file.
if not defined RPCS3_HAZE_EBOOT (
  set "RPCS3_HAZE_EBOOT=E:\PS3 Games\Haze [BLUS30094]\PS3_GAME\USRDIR\EBOOT.BIN"
)

cd /d "%~dp0bin"
start "" "rpcs3-next.exe" "%RPCS3_HAZE_EBOOT%"

Eat Lead's active authored-light level is keyed on the guest's own `sys_fs_open` of `USRDIR/Maps/<16HEX>.map` (the `M` field of each `bin\eatlead_lights\<Level>.lights`), NOT on eye proximity to the light clouds — proximity elects the wrong level in BOTH the menu and mission 1 (measured 2026-09-11).

Why proximity cannot work on this title: every level's lights are authored around its own region origin, so
the nine placed-light bounding boxes all straddle the origin and overlap. Measured with
`bin\eatlead_lights\*.lights` (2,285 placed lights) against real eyes read off `Remix authored-map: eye=`
and `Remix timing: scene=` (render space == file space here, `identity_l1=9.98e-05`):

- Main menu eye `[3.19 -23.28 6.56]`: nearest-light elects 06_Warehouse_Druglab (3.80 m); 00_MainMenu is
  5th at 22.42 m. Nearest-centroid picks the menu by only 3.96 m (31.43 vs Druglab 35.39). The eye is
  inside 7 of 9 bounding boxes and NOT inside the menu's.
- Mission-1 eye `[-14.29 2.29 0.11]`: nearest-light elects Druglab (5.02 m); 01_JapaneseRestaurant1 is 7th
  at 35.12 m. Centroid elects 00_MainMenu (28.91 vs JR 115.52).
- Leave-one-out proxy test (each placed light as an eye): nearest-light is right 71-96% per level,
  centroid 27-84% (Bel Air 27%, Yacht 38%). Any per-frame score would flap in the ambiguous rooms.
- The comparison is also only as good as the placement transform, which the code marks UNPROVEN.

What works, and is verified in a real run (PID 29720, 2026-09-11 11:25): the guest opens exactly one
map per level load, twice within 3 ms on two fds, never interleaved — menu `CE314EA5F3F1F99E` at
0:22.8, movie `3E7ADF58B645E78E` at 0:37.2, mission 1 `342BE9DB1B25FC10` at 0:42.4 (and the menu again
on quit in an earlier session). It is an event, so there is nothing to hysteresis. Plumbing:
`remix_note_guest_open()` (RemixGSRender.cpp, beside `g_remix_av_seen`) is called from `sys_fs_open`
in `Emu/Cell/lv2/sys_fs.cpp` after a successful open and parses the 16-hex stem into an atomic;
`elect_authored_level()` swaps `m_authored_lights` on the flip after a change, queues the outgoing
level's DestroyLights and SUBMITS NOTHING that flip (destroys drain at Present before the next
frame's creates, and the level name is folded into every light hash so old and new never share one).
Knob `RPCS3_REMIX_AUTHOREDLIGHTAUTO` (default 0; conf sets 1; tri-state, an explicit 0 is a real 0),
banner `authoredauto=`, `elect=` on `Remix live:`, event line `Remix authored-elect: map= level= placed=
catalogue= opens= frame=`. 00_MoviePlayer has 0 placed lights and IS elected during the intro movie:
`placed=0` there is correct, not a failure. Static `AUTHOREDLIGHTLEVEL` is ignored while AUTO=1.

@echo off
setlocal

set "RPCS3_REMIX_CAMLOCKVP=7F3D3ABCEFC8B057"
set "RPCS3_REMIX_CAMFALLBACKVP=AD7CE9D672A0BF6B"
set "RPCS3_REMIX_CAMFALLBACKVP2=0214281B9A7A412D"
set "RPCS3_REMIX_CAMHOLD=900"
rem ROUND 40: turned ON. "fov still isn't fixed" has been on the defect list for five
rem rounds and THE FOV HAS NEVER BEEN MEASURED - this knob is the only thing that
rem prints it ("Remix camera trace: ... fov=%%.3f aspect=%%.5f near=%%.6g"), and it has
rem been 0. MEASURED: zero "fov=" tokens anywhere in bin\log\RPCS3.log for the whole
rem round-39 run. The main camera's FOV is taken straight from the guest projection
rem (no constant, no knob: to_camera_matrix is a plain copy and guarded_setup_camera
rem validates nothing), so the number this prints IS what Remix is being told.
rem Read it, compare against 72.000 horizontal / 44.634 vertical (the values the
rem viewmodel code quotes for the world camera), and put this back to 0.
rem REVERT: set "RPCS3_REMIX_CAMTRACE=0"
set "RPCS3_REMIX_CAMTRACE=1"
rem ============================ ROUND 40 - STAGE 2 RUN AT LAST ======================
rem 4D5A87BFFBCE0717 has been dropped WHOLESALE since before the forge record began.
rem It is one of the four programs the WDIVWALK decode fix repaired (the other three
rem are 57A12323F22F4988, 96EDAAED0C27FD05, 1D9A973AF5CD1514), so the reason it was
rem dropped - undecoded quantised positions, i.e. giant geometry - has been fixed for
rem several rounds. The "STAGE 2" block further down in this file has been asking for
rem this relaunch ever since.
rem MEASURED that it is eating MAIN-PASS geometry, not an aux pass. From the round-39
rem run's own skip census:
rem   Remix skip-census: gate=skipvp vp=4d5a87bffbce0717 fp=97c0b2e9be86fd48 clip=1024x576
rem   Remix skip-census: gate=skipvp vp=4d5a87bffbce0717 fp=aa5f822213201b4b clip=1024x576
rem clip=1024x576 IS the main pass (mainclip=1024x576 on the live line). skip vp=24569
rem draws over the run. This is the strongest single candidate for the copper plant's
rem missing wooden plank floor - "just a black void floor" is what a dropped program
rem looks like.
rem WATCH: whatever it draws comes back, now decoded. If ANYTHING appears at absurd
rem   scale, this line is the first thing to put back. Also watch wext_refused= and
rem   wext_max= on the live line (they read 0 and 24906.5 before this change).
rem REVERT: set "RPCS3_REMIX_SKIPVP=4D5A87BFFBCE0717"
set "RPCS3_REMIX_SKIPVP="
set "RPCS3_REMIX_SKIPALBEDO=0"
rem A/B 2026-08-14 RESTORED. Blanking these two made NPC weapons render at absurd
rem scale: this pair suppresses that draw rather than fixing it. Blank them again to
rem reproduce the giant-weapon case (same failure class as the giant FP helmet).
set "RPCS3_REMIX_SKIPPAIRVP=57A12323F22F4988"
set "RPCS3_REMIX_SKIPPAIRALBEDO=2722B18EB6EEDDF6"
set "RPCS3_REMIX_SKIPUNTEXTUREDVP=7F3D3ABCEFC8B057"
set "RPCS3_REMIX_SKIPUNTEXTUREDFPPAIRVP=A41A18E14C782613"
set "RPCS3_REMIX_SKIPUNTEXTUREDFPPAIRFP=E5D8F51451B96165"
set "RPCS3_REMIX_SKIPUNBOUNDBLENDVP=33AE0895AE9FEF72"
set "RPCS3_REMIX_SKIPRTVP=7F02E76D7369D09E,B01BFCE3FC580E3B"
set "RPCS3_REMIX_UVAFFINEVP=9F591B6A6B825612"
rem ROUND 44, DIAGNOSTIC ONLY - no pixel changes, one "Remix albedo-trace:" line per
rem frame. Re-pointed from 2C6485F7F04591F1 at the teleporting light fixture, on the
rem program that draws the copy no other census can see: worldid-draw only fires for
rem WORLDIDENTITYVP programs and C2003391127734F6 is on no list at all, so this hash
rem has never had a per-frame raw-box + matrix trace taken of it.
rem WHY IT MATTERS: albedo-trace prints raw=[..]..[..] AND matrix=[..] AND cam=[..] on
rem one line. raw moving = the guest moved it and it is not our bug; raw static while
rem matrix moves = ours. pick-follow cannot answer this - it never prints the raw box.
rem PLAY IT WITH THE CAMERA MOVING. The one round ever aimed at this object fired its
rem 181-frame instrument during a window where the camera was frozen for all 191
rem frames, read d_origin=0, and that zero was mistaken for stability.
rem REVERT: set "RPCS3_REMIX_TRACEALBEDO=2C6485F7F04591F1" and TRACEALBEDOVP back to
rem C1D482DCD1B03ED0.
set "RPCS3_REMIX_TRACEALBEDO=E40BF80AF519848A"
set "RPCS3_REMIX_TRACEALBEDOVP=C2003391127734F6"
rem A/B 2026-08-14 RESTORED 2026-08-15. Blanking this did NOT bring the Selva tree
rem tops back (the streak-gate probe cleared that suspect too), and with it blank a
rem ship drew at extent 9211 - vp 57A1... is the same program that draws the giant
rem NPC weapons. This gate is load-bearing until the scale bug itself is fixed.
set "RPCS3_REMIX_SKIPEXTENTVP=57A12323F22F4988"
set "RPCS3_REMIX_SKIPEXTENTMIN=128"
set "RPCS3_REMIX_UIWIDTH=1280"
set "RPCS3_REMIX_WORLDIDENTITYVP=0214281B9A7A412D,24D1BA819F701E47,BD1C10DF5703E559,7F02E76D7369D09E,AD7CE9D672A0BF6B,33AE0895AE9FEF72"
set "RPCS3_REMIX_WORLDIDENTITYPAIRVP=AF06F6D32EC048EE"
set "RPCS3_REMIX_WORLDIDENTITYPAIRALBEDO=F13A7A9C1AC7BFFB"
set "RPCS3_REMIX_WORLDIDENTITYPAIR2VP=D0B6A471BB2D463B"
set "RPCS3_REMIX_WORLDIDENTITYPAIR2ALBEDO=3751A2C61A046097"
set "RPCS3_REMIX_WORLDIDENTITYPAIR3VP=D0B6A471BB2D463B"
set "RPCS3_REMIX_WORLDIDENTITYPAIR3ALBEDO=A26189276C5819BC"
set "RPCS3_REMIX_WORLDIDENTITYPAIR4VP=D0B6A471BB2D463B"
set "RPCS3_REMIX_WORLDIDENTITYPAIR4ALBEDO=C2400BC3175E62D1"
set "RPCS3_REMIX_WORLDIDENTITYPAIR5VP=1F9342DE47AFB400"
set "RPCS3_REMIX_WORLDIDENTITYPAIR5FP=0DBECFFFB16A9D59"
set "RPCS3_REMIX_WORLDIDENTITYPAIR5ALBEDO=A26189276C5819BC"
set "RPCS3_REMIX_WORLDIDENTITYFPPAIRVP=C1D482DCD1B03ED0"
set "RPCS3_REMIX_WORLDIDENTITYFPPAIRFP=2B49F6EF9FC1FC18"
set "RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIRVP=C1D482DCD1B03ED0"
set "RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIRFP=D25ED90322BBB9E1"
set "RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR2VP=1F9342DE47AFB400"
set "RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR2FP=B3CDB1790E947348"
set "RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR3VP=D0B6A471BB2D463B"
set "RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR3FP=65A91390AAF6BEF3"
set "RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR4VP=A7505F7AD3A86838"
set "RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR4FP=A91B57CE21BF0082"
rem ===== ROUND 22: THE LAND CARRIER. The override above is what is moving it. ==========
rem
rem MEASURED (round-21 build, pid 22856, frames 9390-30480, from 'Remix worldid-draw:',
rem which prints the transform the override is ABOUT to discard). Two of the six programs
rem on WORLDIDENTITYVP discard nothing. The rest discard a real placement:
rem
rem   vp                 rows   mean|t|    max|t|   rows |t|>32   max basis_delta
rem   BD1C10DF5703E559    383     0.000     0.000             0           0.00000
rem   7F02E76D7369D09E    289     0.000     0.000             0           0.00000
rem   AD7CE9D672A0BF6B   4654   197.305  2123.000          3875           0.51289
rem   0214281B9A7A412D    931   334.508  3909.850            96           1.98014
rem   C1D482DCD1B03ED0     38   161.688   355.394            32           0.00022
rem   D0B6A471BB2D463B     26   162.833   290.523            26           0.00022
rem
rem AD7CE9D672A0BF6B's raw vertex boxes are a FIXED 220x54x81 volume centred on its own
rem origin (x [-122.8,101.0] y [-19.1,35.0] z [-45.6,35.5]) across the whole run - local
rem coordinates for the land carrier. BD1C10DF's are world-sized (x [-1397.7,1146.3]).
rem So the first two really do submit absolute world vertices and the override is free;
rem AD7CE9... submits LOCAL vertices and the override deletes the carrier's placement,
rem pinning the vehicle to the world origin while the player rides it away. That is the
rem "it went away from the camera" / "we drove through it" report, and it is NOT a
rem refusal: C1D482DCD1B03ED0 and AD7CE9D672A0BF6B each appear in 'Remix world-refused:'
rem in 1 of ~176 windows in that run.
rem
rem WORLDIDCENSUS: slots on 'Remix worldid-census:', run-cumulative, last line = run total.
rem   Census only, no behaviour. Read t=<=1/<=32/<=128/>128 per program.
set "RPCS3_REMIX_WORLDIDCENSUS=12"
rem WORLDIDMAXT (whole world units) / WORLDIDMAXB (thousandths of 3x3 deviation): when
rem   non-zero, an identity-forced draw whose discarded placement exceeds the threshold
rem   KEEPS its resolved transform instead of being pinned to the origin. 0 = OFF = the
rem   round-21 behaviour byte for byte. THIS IS THE ROUND-23 EXPERIMENT, LEFT UNARMED
rem   deliberately - read the census first, then try WORLDIDMAXT=32.
rem   REVERT: set both back to 0.
rem 2026-08-16 ARMED AT 32 - THIS IS THE LAND-CARRIER FIX, AND IT IS THE ONE
rem KNOB THIS SESSION MOST LIKELY TO MOVE THINGS THAT CURRENTLY LOOK RIGHT.
rem WORLDIDENTITYVP forces transform = identity for named programs. For programs
rem whose vertices are already ABSOLUTE world coordinates that is free. For
rem LOCAL-SPACE models it pins them to the world origin, and that is what has
rem been eating the carrier. Measured over 51,232 flips:
rem   c1d482dcd1b03ed0  98.8% of draws displaced  tmax=355
rem   ad7ce9d672a0bf6b  58.5% displaced           tmax=2123
rem   d0b6a471bb2d463b   100% displaced           tmax=291
rem   bd1c10df5703e559   0.0% displaced   <- absolute coords, identity is free
rem   7f02e76d7369d09e   0.0% displaced   <- same
rem And the clincher: ad7ce9d672a0bf6b and c1d482dcd1b03ed0 sit in ONE FIXED
rem 220x54x81 box centred on their own origin for the WHOLE RUN while the
rem vehicle drives hundreds of units. Local-space models. The translation being
rem discarded IS the carrier's placement.
rem 32 keeps the override for anything translating <= 32 units (the genuine
rem world-identity draws, all measured at t<=1) and hands back the real
rem transform to anything further out.
rem *** BLAST RADIUS, READ THIS *** ad7ce9d672a0bf6b is the MAIN WORLD PROGRAM
rem and 58.5% of its draws are displaced, so this does not only touch the
rem carrier. If static scenery starts moving or landing in the wrong place,
rem that is this knob and nothing else.
rem WATCH: 'Remix worldid-census:' kept= going non-zero; the carrier's deck
rem attaching to the vehicle instead of sitting at the world origin.
rem REVERT: back to 0, which reproduces round 21 byte for byte.
rem ALTERNATIVE if this helps the carrier but breaks scenery: put this back to 0
rem and try WORLDIDMAXB=100 instead, which targets the 1.98 basis rotation
rem rather than the translation.
set "RPCS3_REMIX_WORLDIDMAXT=32"
set "RPCS3_REMIX_WORLDIDMAXB=0"
rem ====================================================================================
rem ROUND 44b - THE ONE KNOB THAT CHANGES PIXELS. Replaces round 44's GAUGEDONORMAXT,
rem which was PLAY-TESTED, REVERTED, and is now REMOVED FROM THE BUILD (the exe contains
rem neither the wide string RPCS3_REMIX_GAUGEDONORMAXT nor gaugedonormaxt=%%u nor the
rem counter gauge_donor_refused). Do not re-add it.
rem
rem WHY ROUND 44 FAILED, measured per flip, round 43 (56,381 flips) vs round 44 (17,583):
rem     anchor_parked    0.0769 -> 0.1542      anchor_recap   0.0766 -> 0.0181
rem     anchor_promoted  0.00032 -> 0.1360     (+42,512%%)
rem     park outcomes    99.6%% recaptured / 0.4%% promoted  ->  11.8%% / 88.2%%
rem     placements on last frame's anchor      19.22%% -> 37.09%%
rem Refusing the off-origin donor parked it; promotion runs at FLIP, so the very donor that
rem was refused got installed ONE FRAME LATE anyway. The gate did not remove the bad gauge,
rem it DELAYED it - which is why the scene warped under camera turn: a one-frame-old gauge
rem displaces everything by distance-from-eye x turn-per-frame.
rem A GAUGE REMEDY MUST NEVER MAKE THE GAUGE LATER OR ABSENT.
rem
rem (My guard was also mis-specified twice, recorded so it is not repeated: I put a threshold
rem on an ABSOLUTE cumulative counter and compared it across sessions of different length
rem - gauge_absent 57,424 vs 295,391 is 3.27/flip vs 5.24/flip, it went DOWN - and even
rem normalised it was the WRONG counter, because park->promote means the gauge was never
rem absent, only late. anchor_promoted moved 425x and I had put no threshold on it.)
rem
rem WHAT GAUGEDONORBEST DOES INSTEAD (whole world units, 0 = OFF = round 43 byte for byte):
rem the frame's FIRST donor installs immediately and unconditionally, exactly as today, so
rem the gauge is never late and never absent. A LATER donor of the same frame may REPLACE it,
rem but only when it is at least TWICE as close to the world origin (measured against the
rem gauge held when the first donor arrived) and only when the installed one is beyond this
rem threshold. First-draw-wins becomes best-draw-wins. Nothing is parked, promoted or refused,
rem so anchor_promoted structurally CANNOT move - and that is the counter that caught round 44.
rem 75.0%% of AD7CE9D672A0BF6B's 3,316,291 draws sit at |t| <= 1, so a good donor is usually
rem in the frame; only the ORDER was wrong.
rem
rem WATCH ON "Remix live:":
rem   gauge_donor_upgrade_avail - a better donor existed. Counted WHETHER OR NOT this knob is
rem                               armed. If 0, the route is dead in the level you played.
rem   gauge_donor_upgraded      - replacements that happened. avail>0 & upgraded=0 = knob off.
rem   anchor_promoted           - MUST STAY NEAR ROUND 43's RATE (0.00032/flip). This knob
rem                               cannot touch it; if it moves, something else did.
rem   gauge_prev                - MUST NOT rise as a SHARE of gauge_used+gauge_prev+gauge_absent.
rem                               Round 43 19.22%%, round 44 (bad) 37.09%%. Divide, do not compare
rem                               raw totals: the sessions are different lengths.
rem REFUTED IF: the scene TEARS within a frame - part of the world offset from the rest, props
rem separating from the floor they stand on. That is the gauge changing mid-frame.
rem REVERT: set "RPCS3_REMIX_GAUGEDONORBEST=0"   <- one line, round 43 bit-exactly.
set "RPCS3_REMIX_GAUGEDONORBEST=32"
rem ====================================================================================
rem ====================================================================================
rem ====================================================================================
rem DEAD LINES - neutralised 2026-08-25 (round 40). Both of these are re-assigned
rem further down (~:708/:709) and in a .cmd the LAST assignment wins, so editing them
rem here does nothing. Exactly the trap the GUESTLIGHTRADIUS/GUESTLIGHTRADIANCE block
rem below already records. EDIT THE LATER LINES; the banner is the arbiter.
rem set "RPCS3_REMIX_GUESTLIGHTVP=830D7D1B9681C475"
rem set "RPCS3_REMIX_GUESTLIGHTFP=BD80201C29B6B01E"
rem 2026-08-17 BLANKED on request: "remove the added lights that are on the doors".
rem This list is what injects synthetic sphere lights at guest fixture geometry,
rem and D7FD9D0C0D7CF184 was its only entry - so the door lights are these.
rem Note that hash is ALSO on RPCS3_REMIX_EMISSIVE below, which is a separate
rem mechanism (the surface glows) and is left alone. If the doors still emit after
rem this, it is the emissive entry and not the injected light - take it off the
rem EMISSIVE line instead, and expect glradiance/glradius to stop mattering.
rem WATCH: guest_lights= dropping to 0 on the live line; Light Statistics in the
rem Remix menu losing its Sphere Lights.
rem REVERT: set "RPCS3_REMIX_GUESTLIGHTALBEDO=D7FD9D0C0D7CF184"
rem ============================ ROUND 40 ============================================
rem BLANKING THIS LINE ON 2026-08-17 SILENTLY KILLED EVERY GUEST LIGHT IN THE GAME,
rem INCLUDING GUESTLIGHTALBEDO2 - and nothing said so. The arming test in
rem maybe_inject_guest_light was `guest_light_albedo_any() && ... && (primary || glow)`,
rem and guest_light_albedo_any() reads ONLY this list. With it blank the leading term
rem was false for every draw in the process, so the glow_match half - i.e. the whole of
rem GUESTLIGHTALBEDO2 below - was unreachable dead code.
rem MEASURED on the round-39 run (64,139 flips, 15,526,016 draws): the live line read
rem   glalbedos=0  guest_lights=0  guest_light_match=0  guest_light_capped=0
rem i.e. NOT ONE analytical light was created from the guest all session. That is why
rem "floor-recessed lights cast no light" and "only some bulbs are lit" - none of them
rem emit. Round 40 fixed the arming test (one line, RemixGSRender.cpp) so ALBEDO2 can
rem reach the rule at all, and arms this list with YOUR OWN light-bulb pick.
rem 71D189E9B559A7F9 = the "light bulb" you Ctrl+Clicked (vtx=124 extent=1.731). It is
rem also on RPCS3_REMIX_EMISSIVE at intensity 30, which makes the bulb GLOW; this line
rem is what makes it LIGHT THE ROOM. Those are two different mechanisms.
rem WATCH: guest_lights= on "Remix live:" must be > 0, and "Remix guest-light:" lines
rem   must appear in bin\remix_dump.log with pos=/radius=/rgb=.
rem REVERT (back to round 39 exactly): set "RPCS3_REMIX_GUESTLIGHTALBEDO="
rem ============================ ROUND 41 - BLANKED AGAIN, ON MEASUREMENT ============
rem Round 40 armed the bulb texture here and it DID create lights - guest_lights=31, the
rem first non-zero in the project. But it created the WRONG lights, and the log says why.
rem MEASURED, all 31 "Remix guest-light:" lines, ONE albedo on ONE (vp,fp) pair:
rem   extent 0.5385 0.5396 0.5399 0.5389 1.731 0.5579 0.5646 0.5607 0.5579
rem   extent 7.893 8.047 8.095 8.453 8.456 8.469 10.02 11.16 12.54 14.52 14.58 20.33
rem          21.59 21.62 21.64 21.6 22.17 30.08 30.11 30.13 58.94 83.18
rem A 154x spread on ONE texture. The 83.18 row produced radius=29.1 at the AABB centre of
rem an 83-unit mesh - that is the "too big and not aligned with the bulbs" disc in your
rem screenshot. 71D189E9B559A7F9 is a texture the bulbs SHARE with a large prop.
rem It is also NOT in the "Remix light-candidate:" census at all, i.e. the backend own
rem fixture classifier never nominated it. We were keying on a texture nothing recommended.
rem The replacement is GUESTLIGHTAUTO=2 further down - "light where a GLOW CARD is drawn" -
rem which needs no list, carries each lamp own colour and its own radius, and is the only
rem rule that can reproduce "only SOME bulbs are lit".
rem REVERT to round 40: set "RPCS3_REMIX_GUESTLIGHTALBEDO=71D189E9B559A7F9"
set "RPCS3_REMIX_GUESTLIGHTALBEDO="
set "RPCS3_REMIX_GUESTLIGHTALBEDO2="
rem DEAD LINES - neutralised 2026-08-16. Both of these are re-assigned further
rem down (GUESTLIGHTRADIUS at ~:240 = 0.6, GUESTLIGHTRADIANCE at ~:229 = 150) and
rem in a .cmd the LAST assignment wins, so editing 0.2 / 30 here did nothing.
rem The banner agrees: glradius=0.6 glradiance=150. Edit the LATER lines.
rem set "RPCS3_REMIX_GUESTLIGHTRADIUS=0.2"
rem set "RPCS3_REMIX_GUESTLIGHTRADIANCE=30"
set "RPCS3_REMIX_SMOOTHNORMALS=1"
set "RPCS3_REMIX_NECTARVP=A41A18E14C782613"
set "RPCS3_REMIX_NECTARFP=E5D8F51451B96165"
set "RPCS3_REMIX_WORLDVP=0214281B9A7A412D,24D1BA819F701E47,BD1C10DF5703E559,7F02E76D7369D09E,AD7CE9D672A0BF6B,33AE0895AE9FEF72,AF06F6D32EC048EE,C1D482DCD1B03ED0"
set "RPCS3_REMIX_STATICINDEXVP=0214281B9A7A412D,24D1BA819F701E47,BD1C10DF5703E559,7F02E76D7369D09E,AD7CE9D672A0BF6B,33AE0895AE9FEF72"
rem ROUND 31, VALUE CHANGED 4 -> 8, and this is the round's headline fix. MEASURED in the
rem round-30 play-test (60,211 flips, bin\remix_dump.log lines 2127518-2211207):
rem   "Remix static-index:" read  peak=4 budget=4  on ALL 811 lines, with deferred=915,419
rem   = 15.2 deferrals per frame against a budget of 4. The budget was the binding constraint
rem   for the entire session.
rem A deferral is NOT free and it is not a slowdown - it is a visual defect. THREE exits, TWO
rem outcomes, at RemixGSRender.cpp:19258-19295 and :19398-19409. With a live handle the draw
rem renders a PREVIOUS FRAME'S triangle subset (the "second mesh falling behind"). That converges
rem when the GEOMETRY stops changing while flips CONTINUE - which is what an in-game pause menu
rem does - and NOT when flips stop: the budget resets only when the frame counter changes, so if
rem flips stop the budget stays spent and the stale copy freezes instead of converging.
rem With no live handle, or on the budgeted-fallback path, the draw RETURNS and is never
rem submitted at all
rem (the "walls and ceiling and floor go invisible at some angles"), and because it returns
rem before the refusal accounting it leaves NO line in "Remix world-refused:" - which is
rem exactly why searching the refusal families for the missing walls comes back empty.
rem COST. The CPU term is measured: "Remix timing:" gives mesh_create=0.11 ms/frame over
rem 20.02 creates/frame = 110/20.02 = 5.49 us per create (a frame-weighted pass over all 840
rem windows gives 5.64 us; the per-window ms field is printed rounded, so the two numerators do
rem not mix). ~4 extra creations/frame is therefore ~22 us = 0.08% of
rem the measured 28.14 ms frame.
rem BUT THE CPU TERM IS NOT THE ONE THAT MATTERS, AND 24 WAS THE WRONG STEP. Review caught it:
rem every rebuild mints a new BLAS handle and the superseded one stays resident for MESHIDLE
rem frames, and RPCS3_REMIX_MESHCAP=0 means there is no ceiling on the resident set. Resident
rem BLAS count is the term this project has ALREADY proved is the FPS driver - the round-30
rem block in this file records FPS sitting at 21-36 and degrading the longer you play until
rem MESHIDLE=600 took mesh_live from 167k to ~5.7k. And peak=4 budget=4 on all 811 lines means
rem the unions never converged, so this churn is STEADY STATE, not a transient.
rem 4 -> 24 would have taken creates/frame from ~20 to ~39, roughly DOUBLING the resident set,
rem i.e. shipping a probable BLAS regression inside a round whose item 3 is performance.
rem 8 is the conservative step: it clears about half of the 15.2 deferrals/frame for ~+4
rem creates/frame (+20%), and the new stale=/dropped= split says whether that is enough.
rem WATCH THESE TOGETHER on the next run, and do not raise this again without them:
rem   "Remix stats:"  meshes_live / created / destroyed      beside the frame rate
rem   "Remix static-index:"  stale= dropped= peak= budget=
rem If mesh_live climbs across the session, set a finite RPCS3_REMIX_MESHCAP before going higher.
rem The source ceiling was ALSO 4, so this knob could only ever be turned DOWN; round 31 raised
rem the clamp to 64 so the range exists at all.
rem REVERT: set "RPCS3_REMIX_STATICINDEXBUDGET=4"
rem
rem ROUND 34 - RAISED 8 -> 32, AND ROUND 31's OBJECTION IS FORMALLY RETIRED.
rem Round 31 blocked this on: "every rebuild mints a new BLAS handle and the
rem superseded one stays resident for MESHIDLE frames, and MESHCAP=0 means
rem there is no ceiling on the resident set." BOTH HALVES ARE FALSE, read from
rem the deployed runtime's own source (dxvk-remix-numos3, rtx_accel_manager.cpp
rem clean since 2026-06-04, and all eight BLAS anchor strings verified present
rem in the deployed d3d9.dll):
rem   * rtx.minPrimsInDynamicBLAS = 1000 with a STRICT '<' against
rem     std::max(1000,100). Haze's unions measure 426.2 / 267.1 / 364.3
rem     triangles across three logged runs, so EVERY union takes the merged
rem     bucket and NONE has ever had its own BLAS. forceMergedBlas overrides
rem     every clause of requestDynamicBlas, including "keep the one you have",
rem     and hands any existing dynamic BLAS back to the pool.
rem   * A new mesh HASH costs no BLAS at all. remixapi_CreateMesh allocates
rem     HOST_VISIBLE|HOST_CACHED buffers and a map insert; a BLAS is only born
rem     on the DRAW path. 86.9 submitted draws/frame against meshes_live=16133
rem     means >99.4% of live handles hold no BLAS on any frame.
rem   * The merged pool is recycled BY BUFFER SIZE, never by mesh hash, and
rem     rtx.numFramesToKeepBLAS resolves to 1 here (enablePreviousTLAS is false:
rem     upscalerType=DLSS + enableRayReconstruction=True, integrateIndirectMode
rem     != ReSTIRGI), so a BLAS is destroyed 2 frames - ~70 ms - after its last
rem     draw. MESHIDLE=600 governs HOST mesh handles, not BLAS memory.
rem There is also no per-frame BLAS build budget, no bucket triangle cap, and
rem scratch memory grows on demand; the only hard cap is RPCS3's own
rem s_max_indices_per_mesh, and at 426 tris/union you are ~800x away from it.
rem
rem THE REAL COST, in order: merged-bucket FULL rebuilds on the GPU (transient,
rem watch frame_ms); CPU 4.54 us/mesh-create = 0.74% of frame; ~20 KB host RAM
rem per resident union. VRAM growth: none on this evidence.
rem
rem WHY 32 AND NOT 64: 32 leaves 'peak' readable. Demand in the plant burst was
rem ~57 deferrals/frame against 8 slots (census lines 43->44: deferred +3202
rem over rebuilds +324 = 9.88/rebuild, ~56 frames at the measured 27.9 fps), so
rem 32 relieves ~56% of it. If the census comes back peak=32 budget=32, go
rem straight to 64 - the whole 8->64 range is bounded at +0.25 ms of frame time.
rem
rem PRE-REGISTERED READING, all DIFFERENCED WITHIN THE NEW RUN (first to last
rem census line - never against another run; the rate varies 1.03-4.34 across
rem runs and raw cross-run values are meaningless):
rem   SUCCESS 1: peak < 32.
rem   SUCCESS 2: dropped/rebuild falls below 3.661 (this run's differenced
rem              value; the widely-quoted "1.17" was an undifferenced 2020/1728
rem              and its correct value is 1.145).
rem   SUCCESS 3: resident/entries on the FINAL line rises above 63.8%
rem              (257/403). resident/evicted/nomesh are POINT-IN-TIME GAUGES,
rem              not cumulative counters - read the last line, never difference
rem              them.
rem   REGRESSION 1: frame_ms rises >1.0 ms above baseline while mesh_create
rem              stays under 1.0 ms. That is the merged-bucket rebuild term.
rem   REGRESSION 2: mesh_create / mesh_creates-per-frame rises above ~6 us.
rem   RETIRED: round 32 registered "if dropped falls while evicted RISES the
rem              raise is a regression". That clause is WRONG and is withdrawn.
rem              'evicted' counts RPCS3-side handles reaped by MESHIDLE, and a
rem              handle carries no BLAS unless drawn that frame. Rising evicted
rem              with falling dropped is the EXPECTED shape. The regression
rem              signal is frame_ms.
rem
rem THE OTHER LEVER, FLAGGED NOT TAKEN: one earlier run ended resident=12
rem evicted=1168 - 1.0% of unions still held a live handle - and every evicted
rem one needs a budget slot to come back, so 1168 unions at 8 slots/frame is
rem >=146 frames and MESHIDLE re-evicts anything out of view for 10 s. That is
rem a treadmill. A longer idle window scoped to static-index UNION meshes only
rem would relieve the budget more cheaply, but it is a code change, not a knob.
rem
rem ===== ROUND 38: 64 WAS THE CLAMP CEILING, NOT A TUNING RESULT ==========================
rem THE CLAMP TRAP, A THIRD TIME (round 31: this same knob, ceiling == default; round 33:
rem SUNSPRITEHOLD armed 277x over its ceiling). RemixTransforms.cpp clamped this to
rem [1, 64] and the launcher armed 64, so "peak=64 budget=64" on every big window was the
rem CLAMP REPORTING ITSELF and the knob could only ever be turned DOWN. Ceiling raised to 512
rem this round so a ladder exists at all.
rem MEASURED, last run in bin\remix_dump.log:
rem   entries=11498 triangles=2252883 rebuilds=12228 deferred=841 stale=10 dropped=831
rem   peak=64 budget=64 resident=115 evicted=11376 nomesh=7
rem Round 37 reported the budget "relieved" from peak=56 budget=64 dropped=0 - that reading
rem came from a 388-ENTRY window. Every 11,498-entry window in the same log reads
rem peak=64 budget=64 with dropped in the hundreds to thousands. Both readings are real; they
rem are different scenes, and the small one is not the one the plant walls are in.
rem THE REAL LIMITER IS NOT THE BUDGET, IT IS THE EVICTION POPULATION. resident=115 against
rem entries=11498 means 98.9% of static-index entries hold a mesh_hash whose mesh the reaper
rem has already freed, and this file's own accounting (RemixGSRender.cpp, above the
rem resident/evicted/nomesh loop) states what that costs: "evicted = mesh_hash != 0 and NOT in
rem m_meshes (the reaper took it -> next draw of this entry takes the `dropped` exit and
rem renders nothing)". Both drop exits return BEFORE the refusal accounting, which is why the
rem missing walls have never appeared in 'Remix world-refused:'.
rem 128 is ONE conservative step (the ladder has been 4 -> 8 -> 32 -> 64), not a jump to the
rem new ceiling: it doubles how many evicted entries can be rebuilt per frame.
rem WATCH TOGETHER: 'Remix static-index:' peak= dropped= resident=, and 'Remix live:'
rem mesh_live= / mesh_created= beside the frame rate. If peak= stops pinning at budget= and
rem dropped= falls, the step worked. If mesh_live climbs across the session, stop here.
rem LADDER if 128 helps but not enough: 256, then 512. If it does NOT help, the answer is the
rem reaper, not the budget: MESHIDLE=600 (~20 s at 30 fps) is what evicts them, and the
rem untaken lever is a longer idle window scoped to static-index UNION meshes only, which is
rem a code change and NOT this knob.
rem REVERT: set "RPCS3_REMIX_STATICINDEXBUDGET=64"
set "RPCS3_REMIX_STATICINDEXBUDGET=512"
rem ======================================================================================
rem 2026-08-15 IDLE-REAPER EXPERIMENT. Textures degrade to flat white after ~30s
rem standing still, and this reproduces with TEXREHASH off, so it is a real
rem residency bug. Both idle thresholds raised 3600 -> 36000 frames (~72s -> ~12min
rem at 50fps). If the degradation stops or moves much later, the idle reaper owns
rem it. RESTORE to 3600 / remove the TEXIDLE line to undo.
rem 2026-08-16 MESHIDLE CUT 36000 -> 600. THIS IS THE PERFORMANCE FIX.
rem Measured: mesh_created=167185 mesh_live=167185 mesh_destroyed=0 over 4439
rem flips - 38 NEW MESHES PER FRAME AND NOT ONE EVER FREED. 38 x 4439 = 167k,
rem i.e. every mesh ever created is still resident. Each one is a BLAS that
rem Remix builds and traverses, which is why FPS sits at 21-36 and degrades the
rem longer you play.
rem 36000 frames is TEN MINUTES at 60fps, so nothing is ever reaped in a normal
rem session. At 600 (10s) the steady state is ~38 x 600 = 23k live meshes
rem instead of 167k - a ~7x cut in BVH with no behaviour change.
rem 'Remix meshchurn:' names the sources, creates/frame:
rem   8eae853c96f5a07e 15-32   f352d7dafa72d0e0 3-17   15ad612980aca110 3-24
rem   2f64c2f8ffd6add1 EXACTLY 5.0 EVERY WINDOW  <- the HUD gauge program
rem That last one is round 12's predicted risk arriving: the mesh content hash
rem includes .color, the gauges recolour with health/nectar, so every colour
rem change mints a brand new mesh for identical geometry. RPCS3_REMIX_VCOLCONST=0
rem is the lever for that specific 5/frame if you want to test it separately.
rem TEXIDLE IS DELIBERATELY LEFT AT 36000 - that is your experiment for the idle
rem TEXTURE degradation, a different cache and a different bug. Meshes and
rem textures reap independently, so this does not disturb it.
rem REVERT: put 36000 back.
set "RPCS3_REMIX_MESHIDLE=600"
set "RPCS3_REMIX_TEXIDLE=36000"
set "RPCS3_REMIX_MESHCAP=0"
set "RPCS3_REMIX_DUMP=0"

rem --- this round's new knobs -------------------------------------------------
rem Mid-frame camera relatch: the wobble fix. Set to 0 to bring the shake back
rem and confirm the attribution (read cam_relatch / world_ref_fresh /
rem world_ref_stale on the "Remix live:" line in bin\remix_dump.log).
set "RPCS3_REMIX_CAMRELATCH=1"
rem Replay every program's own affine UV form instead of the fixed 4096 divisor.
rem Set to 0 if a material's texture shifts or scales for the worse; the
rem "Remix uvscale-fixed:" census names every program still refused.
set "RPCS3_REMIX_UVAFFINEALL=1"
rem Fill from a helmet pick: Ctrl+Click the giant helmet, take vp= from the
rem "Remix picked:" line in bin\remix_dump.log, paste it here (comma-separated
rem for several), relaunch. No rebuild needed.
rem 2026-08-15 REVERTED TO EMPTY. Tagging 2F64C2F8FFD6ADD1 as viewmodel made the
rem NECTAR AND HEALTH GAUGES DISAPPEAR - that program draws the HUD bars, not the
rem visor. With VIEWMODELANCHOR raised it tagged 67,901 draws and the gauges went
rem with them. The visor shell is 830D7D1B9681C475 (vtx=1446 ext=3.2 and vtx=59
rem ext=7.5, ~1.2 units in front of the eye) - but that hash is ALSO GUESTLIGHTVP,
rem so it cannot be tagged program-wide without dragging the lights in. It needs an
rem albedo-level discriminator (1BF8325ADEF3C986 / C61753D31FB96507).
set "RPCS3_REMIX_VIEWMODELVP="
rem How far a VIEWMODELVP-tagged draw may sit from the eye and still count as
rem viewmodel geometry. Raise it if the helmet is tagged but still refused
rem (vm_hash_anchor_refused climbing on the live line).
rem 2026-08-15: raising this to 100000 DID make the guard pass (vm_tagged_hash went
rem 0 -> 67901, vm_hash_anchor_refused 54389 -> 0), so the guard was the only thing
rem blocking the tag. Restored to 4 with VIEWMODELVP empty; raise it again only
rem alongside a hash that is actually the visor.
set "RPCS3_REMIX_VIEWMODELANCHOR=4"

rem --- round 2 -----------------------------------------------------------------
rem Gauge anchor: divide world draws by the main pass's own view*projection,
rem taken from the first WORLDIDENTITYVP draw of each frame, and submit the
rem camera from the same matrix. This is the wobble fix. Set to 0 and relaunch
rem to bring the old behaviour back exactly - that A/B is the whole attribution.
rem Read gauge_used / gauge_prev / gauge_absent / gauge_contested on the
rem "Remix live:" line in bin\remix_dump.log.
set "RPCS3_REMIX_GAUGEANCHOR=1"
rem How many frames of "Remix gauge:" measurement lines to write (0 = off).
rem Re-armed by every successful Ctrl+Click pick. Measures the OLD gauge's error,
rem so it stays meaningful with GAUGEANCHOR=0.
set "RPCS3_REMIX_GAUGETRACE=600"

rem --- round 3 -----------------------------------------------------------------
rem Per-lane UV divisor. Haze's c151 is a VECTOR of per-attribute divisors, not a
rem scalar: program f7f12d5d15bb9c37 divides ATTR8 by c151.x, ATTR9 by c151.y and
rem ATTR10 by c151.z, and 24d1ba819f701e47's live c151 reads
rem [0.00024426 3.05176e-05 0.00024426 3.05176e-05] - two divisors, 8x apart, in
rem one uploaded vector. So a program may divide u and v differently, and
rem bab9af462da74331 does: "MUL o7.xy = ATTR8.xyxx * c68.xyxx". That form used to
rem be refused and the draw fell back to the fixed 1/4096 on BOTH lanes.
rem Inert on every program that already resolved (they name the same component
rem twice). Read uv_affine_lanes on the "Remix live:" line; 0 = it never fired.
rem Set to 0 and relaunch to restore the refusal exactly.
set "RPCS3_REMIX_UVSCALELANES=1"
rem Name every (program, texcoord output, albedo) whose SUBMITTED UVs leave the
rem unit square, with the resolved form and the live value of every constant it
rem reads: "Remix uvrange:" lines in bin\remix_dump.log, capped at 256, plus the
rem uv_tiled counter. Pure logging - this is how a tiling surface is identified
rem without having to put a cursor on it. 0 removes the lines and the counter.
set "RPCS3_REMIX_UVRANGECENSUS=1"
rem Name every program submitted with NO albedo material at all - the tex_none
rem population, which on Haze is larger than the textured one. "Remix notex:"
rem lines carry the surface address, clip size and sampled mask, which is what
rem separates a second untextured pass over the same mesh from geometry that is
rem meant to be untextured. Pure logging. 0 removes the lines.
set "RPCS3_REMIX_NOTEXCENSUS=1"

rem --- round 3 (gauge coverage, aux-pass wash, skip census) ---------------------
rem How many render sources may hold a gauge anchor at once (1..16). Round 2 had
rem four, and a slot is only reusable once its anchor is two frames old - so the
rem shadow pass, the half-res auxiliary pass, the main pass and anything else
rem drawing that frame competed for four entries and the loser fell back to the
rem cross-pass camera. Read gauge_slot_exhausted on the "Remix live:" line, and
rem the "Remix gauge-keys:" census for the real per-frame key count.
rem Set to 4 to restore round-2 sizing exactly.
set "RPCS3_REMIX_GAUGESLOTS=16"
rem When LAST frame's anchor lookup misses on the exact surface key, retry on the
rem pass shape (target + clip size) alone: surface offsets move between frames
rem when the title double-buffers, the pass shape does not. This-frame lookups
rem keep the exact key. Read gauge_prev_exact / gauge_prev_dims. 0 = exact only.
set "RPCS3_REMIX_GAUGEPREVDIMS=1"
rem ===== ROUND 38: THE STALE DIVISOR. This is the round's headline fix. ==================
rem A draw whose render source has not produced its identity-donor draw YET this frame is
rem divided by LAST frame's anchor and then rendered with THIS frame's camera. The residual
rem is D = V(t)*V(t-1)^-1, which for a point p is p -> eye + (p - eye)*dR: a rotation about
rem the eye by ONE FRAME OF CAMERA TURN. So the displacement is
rem     (distance from the eye) x (turn per frame)
rem and the SAME defect reads as a millimetre wobble on the weapon, a hand-width jiggle on a
rem dumpster, and metres on a light fixture across the room. Four separate user reports.
rem
rem MEASURED (round 38, bin\remix_dump.log, round-37 build 56A0A57DA51FF02E):
rem   * 4000 CONSECUTIVE 'Remix vmbasis:' frames (frames 12311..16395, vtx=3649, one line per
rem     frame). Express the recovered viewmodel 3x3 in the camera basis of frame t+s and
rem     measure its frame-to-frame change. s=-1 is a STRICT minimum:
rem         s    median   mean     p95
rem        -3    0.2844   0.4819   1.7876
rem        -2    0.3031   0.4676   1.4879
rem        -1    0.2287   0.3750   1.2348   <- the gun is aligned to LAST frame's camera
rem         0    0.2984   0.4655   1.5531
rem        +1    0.2855   0.4773   1.8047
rem     s=-2 and s=+1 are both worse than s=-1, so this is one frame and not "any
rem     decorrelation helps". Restricted to the 328 frames where the camera turned >1 deg the
rem     same shape holds and steepens (-1: 0.3484/0.4791, 0: 0.3879/0.6472, +1: 0.4882/0.9985).
rem   * All 48 'Remix picked:'/'pick-deep:' lines for the family the user reports as jiggling
rem     (vp=830d7d1b9681c475 fp=c61b0b9586dd67fb - the dumpsters, the lockers AND the
rem     teleporting light fixture) read ref=anchor_prev, anchor_frame == frame-1,
rem     anchor_vp=ad7ce9d672a0bf6b, cam_age=0. Divisor a frame old, camera current. 48 of 48.
rem   * Magnitude: the gun sits 0.73 m from the eye, the camera turns up to 8.86 deg/frame,
rem     0.73 * 8.86 deg = 0.113 m against a measured max |d cpost| of 0.1248 m.
rem
rem The in-tree justification for the prev branch says "never worse than the old path: the
rem elected camera's reference was always a frame old". CAMRELATCH has since falsified that -
rem world_ref_fresh=3598650 vs world_ref_stale=97515 is 97.4% of draws on a CURRENT camera.
rem
rem GAUGECURDIMS=1 tries a THIS-FRAME anchor of the same (target, clip) shape before falling
rem back to last frame's. It is the same relaxation GAUGEPREVDIMS (above, shipped ON) already
rem makes for frame t-1, one frame fresher.
rem
rem *** READ THE COUNTERS BEFORE BELIEVING ANYTHING. On 'Remix live:' (remix_dump.log): ***
rem   gauge_cur_avail  - a this-frame same-shape anchor EXISTED. Counted whether or not this
rem                      knob is armed. IF THIS IS 0 THE ROUTE IS DEAD on this title and no
rem                      value of GAUGECURDIMS changes anything - go to DEFERPREANCHOR=1.
rem   gauge_cur_dims   - draws this route actually rescued. <= gauge_cur_avail always.
rem   gauge_anchor_prev - should FALL by gauge_cur_dims.
rem   gauge_prev_camfresh - prev-branch draws whose ELECTED camera was current: the size of
rem                      the untried alternative route (divide by the fresh camera instead).
rem Visible confirmation: Ctrl+click a jiggling dumpster. 'Remix picked:' read
rem   ref=anchor_prev anchor_frame=<frame-1> in round 37; it must now read
rem   ref=anchor anchor_frame=<this frame>.
rem
rem *** BLAST RADIUS *** this hands a DIFFERENT render source's this-frame anchor to a draw
rem whose own source has not anchored yet. If static scenery lands in the wrong place, or the
rem world shears when you turn, this knob is the first and only suspect.
rem REVERT (no rebuild): set "RPCS3_REMIX_GAUGECURDIMS=0" - reproduces round 37 byte for byte.
set "RPCS3_REMIX_GAUGECURDIMS=1"
rem ======================================================================================
rem Frames for which a flip with NO anchor keeps the last anchor-derived camera
rem split instead of dropping back to the elected half-res gauge. Round 2 rebuilt
rem the camera on only 43% of flips; the other 57% had the scene and the camera in
rem different gauges, which is a whole-scene shake at frame rate. Read gauge_cam /
rem gauge_cam_prev / gauge_cam_held - they should sum to about one per flip.
rem 0 disables holding (do that if a scene cut visibly lags).
set "RPCS3_REMIX_GAUGECAMHOLD=30"
rem
rem ===== ROUND 29: THE "CAMERA X DRIFTS 421 UNITS" ITEM IS SOLVED. IT IS THE ======
rem ===== LAND CARRIER AGAIN, AND IT IS NOT A DRIFT. ===============================
rem
rem ANCHORGAUGECENSUS: pure measurement, no behaviour, 32 bounded lines per stats
rem window. m_active_camera.position has TWO writers in different FRAMES OF
rem REFERENCE and nothing labelled which:
rem   1. the camera election (world space, = the guest's own eye constant c26);
rem   2. apply_gauge_anchor_camera, which every flip re-derives it by splitting the
rem      GAUGE ANCHOR's fused matrix - so it is the eye in the ANCHOR DONOR's frame.
rem The anchor donor for the 1024x576 main clip is AD7CE9D672A0BF6B on 3,516 of
rem 3,901 traced flips, and round 22 (the block up at WORLDIDENTITYVP) already
rem measured that program discarding a real placement: mean |t| 197, max 4,377, and
rem a FIXED carrier-local vertex box. It is the land carrier. Its fused matrix is
rem W*V*P, not V*P, so the anchor's "world" rides with the carrier.
rem
rem MEASURED, round-27 run, session at remix_dump.log line 1906093, 62,315 flips:
rem   * 46 of 681 'Remix particle:' lines disagree by >1 unit; 45 of 46 X-dominant;
rem     max dx 421.44, max dy 2.78, max dz 8.60. X-dominant because the carrier
rem     drives along X - Y and Z are clean to 1e-2 for thousands of frames.
rem   * NOT accumulating error: 0.54 units/frame, dead constant, while the guest's
rem     own eye sits at X=15.2 for 3,000 frames. That is the carrier's speed.
rem   * NOT a reset either: the ~784-unit jumps are the guest re-basing the
rem     carrier's local origin, 4 of 4 coinciding with a main-clip
rem     'Remix anchor-elect:' line (frames 4609, 6139, 7737, 9178). The frames that
rem     "snap back" are the flips where the election latch won instead.
rem   * cam = (guest eye) + (the translation the WORLDIDENTITYVP override is
rem     discarding on that frame's anchor donor), to +-1.01 units on 39 of 41
rem     frames, against a 421-unit signal. The residual is one frame of carrier
rem     travel, i.e. the expected one-frame lag between the two instruments.
rem READ: anchor_cam_offset and anchor_cam_offmax on the "Remix live:" line, and
rem   'Remix anchor-gauge:' in bin\remix_dump.log. offmax in the hundreds with
rem   axis=x and anchor_vp=ad7ce9d672a0bf6b reproduces the above in one grep.
rem   offmax staying under ~2 for a whole session means the level you played has no
rem   moving anchor donor and this mechanism is NOT your wobble.
rem REVERT: set to 0. Removes the lines and the two counters; nothing else changes.
set "RPCS3_REMIX_ANCHORGAUGECENSUS=32"
rem
rem AND HERE IS THE ROOT CAUSE IN ONE SENTENCE, WHICH IS NOT A NEW KNOB AT ALL:
rem ONE declaration (WORLDIDENTITYVP) drives TWO gates, and only one of them ever
rem learned about WORLDIDMAXT=32.
rem   * submit_subdraw (RemixGSRender.cpp ~:18404, `keep_resolved`) reads the draw's
rem     discarded translation, sees 335 units, and correctly says "this is NOT a
rem     world-identity draw, keep its real transform". That is round 24's carrier fix
rem     and it is armed.
rem   * per_draw_transform (~:15398, `capture_gauge_anchor(fused)`) has ALREADY run
rem     for that same draw, earlier in the same submit, and installed that same
rem     fused matrix as the whole frame's world gauge. Its only qualification is the
rem     vp-hash list - it never looks at the translation at all.
rem So the backend says "carrier-local" and "this is the world" about one draw, in
rem one frame, for one reason. WORLDIDMAXT fixed the placement and left the gauge.
rem
rem ROUND 30's FIX, SPECIFIED, NOT SHIPPED: apply the same verdict at the capture
rem site - a WORLDIDENTITYVP draw whose discarded placement exceeds WORLDIDMAXT (and
rem which is not on WORLDIDMAXTEXEMPTVP) may keep its transform but must NOT donate
rem the gauge anchor. 67% of AD7CE9's draws sit at |t|<=1 (1,148,159 of 1,713,442 in
rem the round-25 census), so the slot stays populated by that same program's
rem origin-resident draws and gauge_absent should NOT move. The one thing to think
rem about is the bootstrap: capture_gauge_anchor can only measure a residual against
rem the slot's own held gauge, so the first donor after a scene cut has nothing to
rem be judged against.
rem
rem ==================================================================================
rem ROUND 30 SHIPPED THE FIX, AND IT REFUTED TWO PARTS OF THE PARAGRAPHS ABOVE.
rem ==================================================================================
rem WHAT ROUND 29 GOT RIGHT, now confirmed LIVE by its own census rather than by an
rem offline reconstruction. Newest session in bin\remix_dump.log (starts at line
rem 1998822, the build stamped Aug 17 2026 03:12:57), flips=66508:
rem     anchor_cam_offset=2134    anchor_cam_offmax=429.96    gauge_cam=59008
rem i.e. the two writers of m_active_camera.position disagreed by over a unit on
rem 2,134 flips (3.2%), worst 429.96 units, and the anchor override fires on 88.7%
rem of flips. The mechanism is real and its size matches round 29's 421.44.
rem
rem WHAT ROUND 29 GOT WRONG - AND IT IS WHY ITS PROPOSED FIX (a) WAS NOT SHIPPED.
rem It blamed AD7CE9D672A0BF6B, the land carrier's local-space program, "90% of
rem flips". MEASURED from the census lines of that same session, bucketed by
rem magnitude:
rem     bucket=0 (<=1 unit)     13,213 lines
rem     bucket>=1 (>1 unit)          30 lines
rem   and ALL 30 of the bucket>=1 lines carry anchor_vp=BD1C10DF5703E559.
rem   AD7CE9D672A0BF6B anchored 10,377 lines and 0214281B9A7A412D 2,715 lines, and
rem   on EVERY one of those the two eyes agree to ~1e-6.
rem BD1C10DF5703E559 is the program round 24 documented as submitting ABSOLUTE WORLD
rem vertices (raw box x [-1397.69 .. +1146.32], i.e. the map). So round 29's fix (a)
rem - "refuse a displaced draw as a gauge donor" - would have refused the genuine
rem world-space map and kept the carrier-local gauge instead. NOT SHIPPED.
rem
rem WHAT SHIPPED IS FIX (b), AND IT DOES NOT NEED THE POLARITY QUESTION ANSWERED.
rem Six consumers compute a distance between the camera and a SUBMITTED transform.
rem A submitted transform is fused * anchor->inverse, so it is in the GAUGE ANCHOR's
rem frame by construction - whatever frame that anchor happens to be in. So the
rem right eye for them is the anchor's split, full stop, and there is no need to
rem decide which of the two frames is "the world":
rem     derive_sky_sun's travel        (its verdict is LATCHED PER ALBEDO for the run)
rem     note_sun_card's to_card        (scores the sun-card election)
rem     submit_subdraw's sky inside/anchor tests   (SKYANCHOR is a FEW-UNIT limit, so
rem                                    429.96 units refuses every dome outright)
rem     submit_subdraw's viewmodel distance gate
rem     apply_viewmodel_basis's reflection PIVOT
rem   plus four evidence-gating diagnostics that were quietly biasing this project's
rem   own measurements: SUNCARDMINDIST's floor, FPCENSUSMAXDIST's ceiling, the
rem   viewmodel_far counter and skip_census_dist (on every gate's census line).
rem The eye is published once per flip by apply_gauge_anchor_camera, which already
rem computed it, so this costs ZERO new matrix work. It is one flip stale for a
rem mid-frame consumer = 0.54 units on the carrier = 0.13% of the 429.96 it removes.
rem NOT changed: what m_active_camera.position means. .position and
rem .reference_inverse are always written together and the viewmodel probe divides by
rem .reference_inverse, so decoupling them would break a gate that is correct today.
rem READ: camframe_served / camframe_corrected / camframe_absent / camframe_max on
rem   the "Remix live:" line. camframe_max should be the same order as
rem   anchor_cam_offmax - they measure one disagreement at two sites. corrected=0
rem   with served large is a RESULT: that level had no frame disagreement.
rem   Also NEW on 'Remix picked:': camanchor=[..]. cam= is unchanged so old lines
rem   stay comparable. origin= is anchor-frame, so origin-camanchor is a real
rem   distance and origin-cam is the one that was off by up to 429.96 units on X.
rem REVERT: set to 0. Every consumer falls back to m_active_camera.position, which
rem   is the round-29 value exactly. Full revert: copy
rem   bin\rpcs3-next-prev-round30.exe over bin\rpcs3-next.exe (B84E9D91BA22C65B).
set "RPCS3_REMIX_CAMANCHOREYE=1"
rem ==================================================================================
rem
rem A NO-REBUILD A/B EXISTS but it is BLUNT and it is NOT recommended as a fix:
rem taking AD7CE9D672A0BF6B out of WORLDIDENTITYVP (the commented line at :292
rem already spells it out) stops the carrier defining the gauge, but it also throws
rem away round 24's placement work for 4.5M draws and BD1C10DF only drew 142,901
rem times against AD7CE9's 4.56M, so the main slot would go empty on most frames.
rem Expect gauge_absent to explode. Run it only as an attribution test, on the
rem carrier level, for one session, watching gauge_absent and anchor_cam_offmax.
rem Refuse the second, untextured half-resolution pass Haze draws over the same
rem meshes - the greyscale wash in the nectar-disruption room. Only draws with no
rem material AND no albedo unit AND a render surface strictly smaller than the
rem main pass. Read skip_auxuntex and the "Remix aux-untex:" census lines.
rem Set to 0 and relaunch to put the wash back - that A/B is the attribution.
rem 2026-08-14: TURNED OFF. In round 4 this gate went from refusing 0 draws to
rem refusing 99,695, in the same build where doors went invisible, a black void
rem showed through, and the ground kept disappearing. Set back to 1 to re-test it.
set "RPCS3_REMIX_SKIPAUXUNTEX=0"
rem Fill from a soldier pick: Ctrl+Click a soldier up close, take albedo= from the
rem "Remix picked:" line, paste it here (comma-separated for several), relaunch,
rem then walk backwards until the outfit pops. "Remix watch:" lines then name the
rem verdict - a gate name, or "submitted" right up to the pop, which would mean
rem the game's own LOD swapped the draw instead. No rebuild needed.
rem DEAD LINE - neutralised 2026-08-16. Re-assigned further down (~:382) to
rem EF4700267F08C7F3, and the LAST assignment wins - so a watch IS armed even
rem though this line reads empty. Edit the later line.
rem set "RPCS3_REMIX_WATCHALBEDO="

rem --- round 4 (foliage cutout, f64 gauge division) -----------------------------
rem THE FOLIAGE FIX. Replay the fragment program's per-pixel discard (KIL) and the
rem texture unit's alpha-kill bit as a per-draw alpha test, so cutout foliage stops
rem rendering as a solid wall. 99.6% of this title's materials are created with the
rem RSX alpha test disabled, so the cutout cannot be coming from the material or the
rem blend state - it is in the ucode, which nothing here ever read.
rem Read kil_ucode / kil_ctrl / texkill_seen / kil_applied on the "Remix live:" line
rem in bin\remix_dump.log, and Ctrl+Click a leaf wall for kil= / src= on the pick
rem line. All of kil_ucode=0 texkill_seen=0 means the title cuts out on NEITHER
rem channel and its foliage is pure alpha blend - a real answer, not a failed fix.
rem Set to 0 and relaunch to restore the previous always-pass behaviour exactly.
set "RPCS3_REMIX_FPKIL=1"
rem Alpha reference (0-255) for cutout draws whose own threshold constant the ucode
rem walk could not recover ("Remix kil:" reads ref=-1). Try 64 or 192 if holes come
rem out inverted or the cutout eats too much.
set "RPCS3_REMIX_FPKILREF=128"
rem THE WOBBLE FIX. Invert the gauge anchor and multiply the divide in double
rem precision. The wobble is arithmetic, not reference choice: dividing a fused
rem view*projection by its own SINGLE-precision cofactor inverse leaves 3.2e-3 of
rem basis error and 1.07 units of translation in a room with large coordinates
rem (cargo plane), and almost nothing in a small one (Selva) - which is exactly the
rem pattern reported. Judge it on "Remix gauge-selfcheck:" IN THE CARGO PLANE:
rem err32 ~3e-3, err64 <=1e-8, tmag ~2000. Selva cannot show the defect at all.
rem Set to 0 and relaunch to restore the single-precision divide bit-exactly.
set "RPCS3_REMIX_GAUGEF64=1"

rem --- round 5 (light the level from its own fixtures) --------------------------
rem Nothing above this line was edited. Everything below re-sets a few of the
rem knobs above; cmd's "set" is last-wins, so deleting a line here restores the
rem original value with no other edit.
rem
rem THE LIGHTING FIX, in three parts.
rem
rem 1) GUESTLIGHTVP / GUESTLIGHTFP are now OPTIONAL narrowing - blank means "any
rem    program". They used to be REQUIRED, which is why exactly one lamp type in
rem    the dark cargo area ever produced a light (four bulbs total, and their log
rem    lines went to RPCS3.log where you could not read them mid-session). The
rem    program that was configured, 830D7D1B9681C475, is a shared world program
rem    that also draws non-lamp geometry, so it added no selectivity while
rem    excluding every fixture drawn by anything else. The albedo hash IS the
rem    fixture identity. Restore the old narrowing by deleting these two lines.
rem 2026-08-15 REVERTED: blanking these put lights on DOORS and sheet-metal covers
rem instead of bulbs - the albedo hash is NOT the fixture identity on Haze, it is
rem shared with other props, so the vp+fp narrowing WAS load-bearing. Blank them
rem again only if a fixture drawn by a different program is confirmed by a pick.
rem ============================ ROUND 40 ============================================
rem THAT PRECONDITION IS NOW MET, BY YOUR OWN PICK. The light bulb you Ctrl+Clicked is
rem   vp=c1d482dcd1b03ed0 fp=796bc90574f89ca1 albedo=71D189E9B559A7F9 vtx=124 ext=1.731
rem which is a DIFFERENT program from the 830d7d1b/bd80201c pair armed here - so with
rem the old pair the bulb could never produce a light no matter what was on the albedo
rem list. Re-pointed at the bulb's own pair. The 830d7d1b family loses nothing: it was
rem producing zero lights anyway (guest_lights=0 all last run).
rem KNOWN LIMIT, and it is the next round's job: this vp/fp narrowing is a single
rem GLOBAL pair ANDed against BOTH albedo rules, so only one fixture family can be lit
rem per run. The right shape is parallel comma lists (the UIFORCEPAIRVP2/FP2 idiom),
rem which pairs entry i of VP with entry i of FP and would let the bulbs and the
rem floor-recessed fittings be lit at the same time.
rem DO NOT simply blank these two: 71D189E9B559A7F9 is bound by 8 distinct (vp,fp)
rem pairs in the log, one of which is the SMOKE program ad7ce9d6/aa0fe222 - blanking
rem would put a sphere light inside every smoke puff. That is the same failure the
rem 2026-08-15 note above records for the door texture.
rem REVERT: set them back to 830D7D1B9681C475 / BD80201C29B6B01E.
set "RPCS3_REMIX_GUESTLIGHTVP=C1D482DCD1B03ED0"
set "RPCS3_REMIX_GUESTLIGHTFP=796BC90574F89CA1"
rem 2) Radiance. 30 on a 0.2-unit sphere is a nightlight. THIS IS THE BRIGHTNESS
rem    KNOB - sweep it 50 / 150 / 400 with a relaunch, no rebuild. Too bright:
rem    drop to 30 and set GUESTLIGHTCOLOR=0 for round-4 behaviour.
rem ROUND 41: raised 150 -> 1200 because the EMITTER SHRANK. Radiance is per unit area, so
rem a sphere light total power goes as radius squared (INFERRED from the sphere-light model,
rem not measured on screen). Round 40 ran radius 0.6; round 41 runs max(0.1, extent x 0.5),
rem which on the measured glow cards (extent 0.09 .. 0.33) is about 0.10 .. 0.17.
rem (0.6/0.15)^2 = 16, so equal power would want ~2400 and 1200 is a deliberate half-step -
rem expect it slightly DIMMER than round 40 rather than blown out.
rem THIS IS THE ONLY BRIGHTNESS KNOB. Too dark: 2400, then 4800. Too bright: 600, then 300.
rem REVERT: set "RPCS3_REMIX_GUESTLIGHTRADIANCE=150"
set "RPCS3_REMIX_GUESTLIGHTRADIANCE=1200"
rem 3) Radius. The old fixed 0.2 sphere sat at the CENTRE of a 2.6-unit lamp
rem    housing, i.e. inside its own shade, where the path tracer occluded it with
rem    the lamp mesh itself. The sphere now scales with the draw's own extent.
rem    Raise to 0.5 if fixtures still swallow their bulbs; 0 restores the fixed
rem    radius (GUESTLIGHTRADIUS above stays the floor either way).
rem 2026-08-15 REVERTED to a fixed radius: 0.35 x extent gave ~7-8 units on the
rem draws it matched (one sphere filling half the screen in the debug view).
rem 0 = use GUESTLIGHTRADIUS below, which is now 0.6 - big enough to clear a
rem 2.6-unit lamp housing, small enough to read as a bulb.
rem ROUND 41: THE EMITTER IS NOW THE SIZE OF THE MESH IT CAME FROM.
rem Two problems, both measured. (a) SCALE=0 does NOT mean "use the fixed radius" - env_float
rem REJECTS 0 and falls back to the accessor default, which is 0.35, and the banner has been
rem echoing glradiusscale=0.35 all along. The accessor own comment claims otherwise and is
rem wrong. Written explicitly now so the file and the behaviour agree.
rem (b) RADIUS=0.6 is a FLOOR, and at world scale ~1 unit = 1 m that is a 60 cm emitter
rem forced onto every fixture however small. On the measured 0.54-extent bulbs the floor won
rem outright: the light sphere was LARGER THAN THE BULB. That is the other half of "too big".
rem 0.5 x extent is exactly the mesh own half-extent, i.e. a sphere that fills the bulb and
rem no more; 0.1 is a floor low enough never to bind on a real fixture.
rem REVERT: SCALE=0 and RADIUS=0.6 (note SCALE=0 really means 0.35).
set "RPCS3_REMIX_GUESTLIGHTRADIUSSCALE=0.5"
set "RPCS3_REMIX_GUESTLIGHTRADIUS=0.1"
rem Tint the light by the mean colour of the fixture's own texture, normalised so
rem brightness is unchanged - only hue moves. 0 restores the fixed warm constant.
set "RPCS3_REMIX_GUESTLIGHTCOLOR=1"
rem Destroy a light no draw has re-matched for this many frames. 0 = keep forever,
rem which is what a room lit by its own ceiling fixtures wants: a lamp does not
rem stop existing when you turn around.
rem
rem --- ROUND 48: 0 becomes 4, AND THIS IS THE FLICKER HALF OF THE FEATURE ----------------
rem The round-48 brief assumed a card-keyed light flickers "for free" because a bulb that
rem flickers off stops drawing its card. IT DOES NOT, and this is the line that made the
rem assumption false: at 0 a light is NEVER destroyed, so the card can stop being drawn for
rem any length of time and the light stays lit. Flicker needs a non-zero idle window.
rem
rem 4 frames: a bulb dark for more than ~0.13s goes out; a one- or two-frame blink does not,
rem which is right - that is sub-perceptual and would only add strobing.
rem Re-lighting is immediate and NOT gated by GUESTLIGHTSTABLE, because the cell stays
rem confirmed once it has graduated. That pairing is deliberate and the two knobs have to be
rem read together: STABLE decides what may EVER light, IDLE decides when a light goes out.
rem
rem SIDE EFFECT, and it is wanted: every light also goes out when the player turns away (the
rem fixture stops being drawn) and comes back on the first frame it is drawn again. That is
rem free culling. The only visible cost is that re-entering a room with more than 4 fixtures
rem takes a few frames to fully light, because creation is capped at 4 per frame.
rem WATCH guest_light_reaped= - it should now be LARGE and climbing. 0 means this is inert.
rem REVERT (round 41 exactly): set "RPCS3_REMIX_GUESTLIGHTIDLE=0"
set "RPCS3_REMIX_GUESTLIGHTIDLE=4"
rem Live guest-light cap (was hardcoded at 64). Raise if guest_light_capped starts
rem climbing on the "Remix live:" line AND the extra lights are wanted.
rem ROUND 41: 64 -> 128. GUESTLIGHTAUTO=2 lights every glow card in a level rather than one
rem listed texture, and GUESTLIGHTIDLE=0 keeps them forever, so the cap is now reachable.
rem WATCH guest_light_capped= on "Remix live:" - if it climbs, either the rule over-matches
rem (that is the pre-registered refutation of the glow-card hypothesis) or raise this again.
rem Clamp is 4096, so 128 is nowhere near a ceiling.
rem REVERT: set "RPCS3_REMIX_GUESTLIGHTMAX=64"
set "RPCS3_REMIX_GUESTLIGHTMAX=128"
rem MORE LIGHTS, NO REBUILD: Ctrl+Click any unlit lamp/fixture, copy albedo= from
rem the "Remix picked:" line in bin\remix_dump.log, and append it here and to
rem EMISSIVE below (comma-separated, up to 16 each). Relaunch.
rem The list above is unchanged: D7FD9D0C0D7CF184 is the ceiling fixture already
rem proven to work in the dark area, C2F33F7E5105DAE7 its glow card.
rem
rem Make the fixture's own visible surface glow, from exactly where the game says
rem the light comes from. Cosmetic partner to the sphere light above; blank this
rem list if a non-lamp use of the same texture starts glowing.
rem 2026-08-15: added 71D189E9B559A7F9 - the actual bulb texture, found by a pick.
rem It was drawn by vp=ad7ce9d672a0bf6b (the shared world program), so the
rem GUESTLIGHT vp/fp narrowing can never match it; EMISSIVE is albedo-only and is
rem the right channel. It was ALSO in rtx.worldSpaceUiTextures (removed there),
rem which made it self-emissive as UI: bright to look at, casting no real light.
rem 2026-08-16: added 3ED07EDE1C03A651 - the smelting-plant windows. You
rem identified it against an original-game reference showing them glowing amber;
rem in our render they are flat and dark. Emissive is keyed per albedo, so this
rem is exactly the mechanism for it.
rem 2026-08-17 ROUND 30: THE PER-ALBEDO INTENSITY NOW EXISTS, and the note below
rem ("which does not exist yet") is what this round removed. Syntax, following the
rem SUNMAP=<hash>:<x>,<y>,<z> precedent:
rem     RPCS3_REMIX_EMISSIVE=<hash>[:<intensity>][,<hash>[:<intensity>]...]
rem A hash with NO colon keeps using EMISSIVEINTENSITY below and is parsed exactly as
rem before, so the only behaviour change on this line is the one hash that carries a
rem colon. Bound 16 entries; an intensity must parse, be finite and be > 0 (clamped
rem to 1e6); a colon with junk after it leaves that hash on the global and skips to
rem the next comma rather than eating the rest of the list.
rem
rem *** VALUE CHANGED THIS ROUND, AND HERE IS WHY *** you asked: "Can we increase the
rem brightness by like 30 for this texture hash, it's the light bulbs in the plant
rem 71D189E9B559A7F9". Under the old single global that would ALSO have multiplied
rem the smelting-plant windows (3ED07EDE1C03A651) and the two door/lamp fixtures by
rem 30. So 71D189E9B559A7F9 now carries :30 and the other three are untouched at the
rem global 1. EMISSIVEINTENSITY below is NOT changed.
rem WATCH: emissiveper=1 on the run-start banner and the live line - that is the
rem   count of entries carrying their own intensity. emissiveper=0 with emissive=4
rem   means the exe is stale (round 29 or older) and the colon did nothing.
rem   mat_emissive should not move: the same four hashes are still listed.
rem REVERT the brightness alone: drop ":30" and it is byte-identical to round 29.
rem REVERT: drop 3ED07EDE1C03A651 back off this line.
rem 2026-08-17: D7FD9D0C0D7CF184 REMOVED - "found the door is emissive too".
rem That hash was also the sole GUESTLIGHTALBEDO entry (now blanked), so the doors
rem had BOTH an injected sphere light and a glowing surface. The light went last
rem turn; this is the surface. Removing it should leave the doors as plain
rem geometry lit by the room.
rem The bulbs keep their per-hash 30 - you confirmed "the plants lights look much
rem nicer", so that mechanism is landed and only this extra entry was unwanted.
rem WATCH: mat_emissive falling; doors dark; bulbs UNCHANGED (if the bulbs dim
rem too, the per-hash parse regressed - check emissiveper=1 on the banner).
rem REVERT: put D7FD9D0C0D7CF184 back at the front of this list.
set "RPCS3_REMIX_EMISSIVE=C2F33F7E5105DAE7,71D189E9B559A7F9:30,3ED07EDE1C03A651"
set "RPCS3_REMIX_EMISSIVEINTENSITY=1"
rem TEMPORARY - a light stuck to the camera so you can play while tuning the real
rem fixture lights. SET THIS BACK TO 0 once the room is lit by its own lamps; it
rem flattens shadows and is not what the level should look like.
rem ============================ ROUND 40 ============================================
rem THIS KNOB HAS DONE NOTHING SINCE ROUND 5 AND THE COMMENT ABOVE IS FALSE.
rem place_debug_light used DestroyLight-then-CreateLight on the same hash 0x3 every
rem frame. MEASURED against the deployed runtime's own tree (dxvk-remix-numos3
rem @6476faea): remixapi_DestroyLight only QUEUES the handle (rtx_remix_api.cpp:1613);
rem the queue drains in Present and the erase lands in prepareSceneData's flush loop
rem (rtx_fork_light.cpp:46-60) - i.e. AFTER the create, deleting the light that had
rem just been made, before it could ever be linearized. The DrawLightInstance
rem activation went with it. RemixGSRender.cpp has said so in a comment since round 23
rem and left it deliberately.
rem ROUND 40 FIXED THE MECHANISM (destroy removed, same-hash create, isDynamic=1 -
rem exactly what the sun already does and what 'Remix sun-submit: destroys=0
rem draw=SUCCESS' proves works), and ARMS IT AT 0 so the newly-working light stays off
rem until you ask for it. At 12 it would now really behave the way this file's own
rem source comment warns: "it blew out everything near the player and crushed
rem everything far from them".
rem TO TRY IT: 2 or 3, not 12. It is a real light now.
set "RPCS3_REMIX_CAMLIGHT=0"
rem
rem === THE SUN'S DIRECTION - hand-tune here until round 14 derives it ==========
rem This is the light you are actually seeing. It is NOT Remix's fallback light:
rem the backend creates its own distant sun through the API (RemixGSRender.cpp
rem ~:3470, remixapi_LightInfoDistantEXT, light_info.hash = 0x4, stored as
rem m_sun_light), aimed by remix_rsx::sun_direction() in RemixTransforms.cpp
rem ~:11135. Its built-in default is:
rem       -0.35, -0.9, -0.25      "down and slightly across"
rem which is a generic mid-morning key light that has NOTHING to do with where
rem Haze actually puts its sun - hence the sun being off-screen left while the
rem game's own sun card sits in view. Round 14 is deriving this from the sun
rem card's world position so it tracks each level instead of being tuned by eye.
rem
rem MEANWHILE: un-rem ONE line below and relaunch. y stays at -0.9 and the
rem horizontal magnitude stays at 0.43, so only the COMPASS BEARING changes -
rem the sun keeps the same height and only swings around you. They are listed in
rem order, so just walk down the list until the sun sits where the yellow card
rem is; wrap back to the top if you run off the end.
rem
rem 2026-08-16: YES, THE LIGHT YOU SEE IS THE ONE THIS BACKEND ADDS. Confirmed:
rem RPCS3_REMIX_NOSUN is unset so ensure_sun_light() runs, and rtx.conf now has
rem fallbackLightMode=1 (NoLightsPresent), so Remix's own fallback is suppressed
rem while ours is registered. One distant light, and it is ours.
rem And the reason rotating it did nothing: EVERY LINE BELOW WAS STILL REM'D.
rem The knob was never armed, so sun_direction() kept returning its built-in
rem default. Arming one now so the control is demonstrably live.
rem The armed line is ~180 degrees from the default ON PURPOSE - it is a proof
rem that the knob works, NOT an aim. Walk the list to place it, and remember the
rem sign convention is "direction the light TRAVELS", so if it lands opposite
rem what you want, negate all three terms.
rem 2026-08-16: you said the 180-degree proof line "rotated too far" and asked
rem for the middle ground. Both halfway lines are below - one each way round,
rem because "halfway" between two bearings 180 deg apart is ambiguous. Height
rem and horizontal magnitude are unchanged, so only the BEARING moves.
rem If SUNTRACK above is working (sun_retargeted climbing) this line is
rem OVERRIDDEN and tuning it does nothing - read that counter first.
rem 2026-08-16: "rotated back some to align with this sun alpha card". Bearing
rem history, all at identical height and horizontal magnitude:
rem   215.5 deg  -0.35,-0.9,-0.25    the built-in default
rem    35.5 deg   0.350,-0.9,0.250   the 180-deg proof line - you said TOO FAR
rem   125.5 deg  -0.250,-0.9,0.350   halfway - the one you just judged
rem Now stepping BACK from 125.5 toward the default in ~22.5 deg increments.
rem The armed line is the first step; the two after it go further if that is not
rem enough. Only the BEARING moves.
set "RPCS3_REMIX_SUNDIR=0.1223,-0.9444,-0.3051"
rem set "RPCS3_REMIX_SUNDIR=-0.424,-0.9,0.071"   <- 170.5 deg, further back
rem set "RPCS3_REMIX_SUNDIR=-0.419,-0.9,-0.097"  <- 193 deg, nearly the default
rem set "RPCS3_REMIX_SUNDIR=-0.250,-0.9,0.350"   <- 125.5 deg, what you just saw
rem set "RPCS3_REMIX_SUNDIR=0.250,-0.9,-0.350"   <- the other halfway point
rem set "RPCS3_REMIX_SUNDIR=0.350,-0.9,0.250"    <- the 180-deg proof line
rem set "RPCS3_REMIX_SUNDIR=-0.35,-0.9,-0.25"    <- the built-in default
rem set "RPCS3_REMIX_SUNDIR=-0.071,-0.9,-0.424"
rem set "RPCS3_REMIX_SUNDIR=0.250,-0.9,-0.350"
rem set "RPCS3_REMIX_SUNDIR=0.424,-0.9,-0.071"
rem set "RPCS3_REMIX_SUNDIR=0.350,-0.9,0.250"
rem set "RPCS3_REMIX_SUNDIR=0.071,-0.9,0.424"
rem set "RPCS3_REMIX_SUNDIR=-0.250,-0.9,0.350"
rem set "RPCS3_REMIX_SUNDIR=-0.424,-0.9,0.071"
rem
rem HEIGHT, separately: -0.9 against a 0.43 horizontal puts the sun ~64 degrees
rem up, which is nearly overhead and gives short shadows. For a low sun with long
rem raking shadows shrink the Y term - e.g. ...,-0.30,... is about 35 degrees.
rem If the sun ends up exactly OPPOSITE where you want it, negate all three
rem terms: the sign convention here is "direction the light TRAVELS", and that is
rem the single easiest thing to get backwards.
rem Companions, also at their defaults and also absent from this file:
rem RPCS3_REMIX_SUNRADIANCE (brightness), RPCS3_REMIX_SUNANGLE (angular diameter
rem in degrees - bigger = softer shadow edges), RPCS3_REMIX_NOSUN (1 = no sun).
rem
rem THE REMAINING WOBBLE. Draws that arrive before their pass's own camera matrix
rem does were placed by LAST frame's - one frame of camera motion stale, which is
rem the ~0.5-unit drift measured on the wobbling ceiling fixture. They are now
rem held until this frame's matrix arrives, then placed with it. Read
rem defer_fresh / defer_flip on the "Remix live:" line: fresh >> flip is the fix
rem working. Set to 0 and relaunch to restore the old immediate submit exactly -
rem that A/B is the whole attribution, and this is the FIRST thing to try if
rem geometry starts flickering or landing in the wrong place.
rem 2026-08-15 TURNED OFF pending investigation: measured defer_buffered=774606
rem with defer_flip=367629 (47% missing their frame's anchor entirely), alongside
rem an FPS drop from ~50 to 17-24 in the 02:41 clip. Set to 1 to re-test.
rem
rem 2026-08-15 LATER - RE-TESTING, and here is why the old numbers are stale.
rem You Ctrl+Clicked the ship because it "still sort of follows the camera as I
rem turn". Two picks of it, 43 frames apart, settle what is happening:
rem   frame 2762  origin=[10.504 15.661 -23.294]  cam=[-5.7256 1.5429 -42.1899]
rem   frame 2805  origin=[11.178 15.099 -22.766]  cam=[-5.7245 1.5443 -42.1885]
rem The CAMERA MOVED 0.002 UNITS. The SHIP MOVED 1.02. You were turning, not
rem walking, so a pure rotation translated the object - and both picks read
rem ref=anchor_prev, i.e. placed with the PREVIOUS frame's matrix. That is
rem precisely the population this knob defers.
rem It is a 10% population, not everything, which is why only "some" things
rem slide: gauge_used=282422 with gauge_prev=28285.
rem The 47%/FPS measurement above predates ANCHORSTICKY. Since then
rem gauge_contested went 550394 -> 0 and world_ref_stale is now 3194 against
rem world_ref_fresh=393091 (0.8%), so the condition that made deferral expensive
rem is measurably gone. This is a re-test of a documented risk, not a new idea.
rem WATCH, in this order:
rem   1. defer_fresh >> defer_flip  = working. If defer_flip is again ~half of
rem      defer_buffered, revert immediately - that was the old failure.
rem   2. FPS. If it falls from ~48 toward the 17-24 band, revert.
rem   3. The ship: turn on the spot and see whether it stops sliding.
rem REVERT: set this back to 0. That restores immediate submit bit-exactly.
rem 2026-08-16 BACK TO 0 - THE TRIPWIRE I WROTE ABOVE HAS TRIPPED.
rem The re-test condition was "if defer_flip is again ~half of defer_buffered,
rem revert immediately". Measured this run:
rem   defer_buffered=543244  defer_fresh=149761  defer_flip=393483   = 72%
rem That is WORSE than the 47% that got this switched off the first time. Only
rem 28% of the buffering does any work; the rest is pure cost, and FPS this run
rem swings 15.5-39.0 against ~48 with deferral off.
rem What you lose: the ship no longer slides as you turn - that fix was real and
rem you confirmed it. What you get back: roughly 14 fps.
rem This is a genuine trade, not a defect. Set it back to 1 if you would rather
rem have the stable props than the frames.
rem
rem ============================================================================
rem ROUND 45, 2026-08-29: ARMED. THIS IS THE ONE PIXEL CHANGE OF THE ROUND.
rem ============================================================================
rem Round 44b shipped GAUGEDONORBEST=32 to close the teleporting light fixture.
rem It is MEASURED INERT: gauge_donor_upgrade_avail=0 over 21630 flips, and that
rem counter is incremented whether or not the knob is armed, so the route never
rem had a candidate. gauge_contested=0 in the same run says why - within a frame
rem every later gauge donor AGREES with the first one. There is no better donor
rem to elect, so no within-frame election (round 44's refusal or round 44b's
rem best-of-frame) can ever change anything. Both rounds assumed a choice exists.
rem It does not. THE FRAME'S ONLY DONOR IS ITSELF SOMETIMES WRONG.
rem
rem The inbox pre-designated this as the next population if that happened, and it
rem is the one remedy that does not touch the gauge at all:
rem   gauge_prev_camfresh = 588035 over 21630 flips = 27.2 draws/frame divided by
rem   LAST frame's anchor while the elected camera is CURRENT.
rem
rem MEASURED THIS RUN, on the teleporting fixture itself (albedo E40BF80AF519848A,
rem 'Remix picked:' lines, 5/5 separation):
rem   frame  7546  ref=anchor       origin=[0 3.5e-10 0]            CORRECT
rem   frame  7646  ref=anchor       origin=[7.5e-09 2.3e-10 0]      CORRECT
rem   frame 10443  ref=anchor_prev  origin=[2.277 0.306 2.257]      DISPLACED
rem   frame 10460  ref=anchor_prev  origin=[-0.502 0.556 0.803]     DISPLACED
rem   frame 10525  ref=anchor_prev  origin=[0.349 0.498 -0.377]     DISPLACED
rem Round 4 read the same object 9/9 ref=anchor_prev. Eight of eight displaced
rem placements of this object, across two rounds, are the anchor_prev branch.
rem
rem WHY IT MATCHES "the camera bounces as I walk so props follow the bounce":
rem a one-frame-stale ANCHOR under a pure camera TRANSLATION gives a pure
rem translation error with no rotation - which is exactly a walk-bob, and is a
rem motion round 38's distance x turn-per-frame model cannot produce.
rem
rem WHY THIS IS SAFE UNDER ROUND 44b'S RULE ("a gauge remedy must never make the
rem gauge later or absent"): this is not a gauge remedy. It does not refuse,
rem park, promote or reorder a single donor. It holds the DRAW until this
rem frame's anchor for its own render source lands, then re-divides. Anything
rem still held at flip goes out with the transform it already has, so the worst
rem case is byte-identical to today, one flush later, and no draw is ever lost.
rem anchor_promoted / anchor_parked / anchor_recap structurally cannot move.
rem
rem WHY NOW, when this was switched off on 2026-08-16 for ~14 fps:
rem   1. The two conditions the launcher itself named as making deferral
rem      expensive are measurably gone. gauge_contested 550394 -> 0. world_ref
rem      stale is 76799 against fresh 3495737 = 2.1%.
rem   2. DEFERPREVONLY=1 (below, staged in round 32 for exactly this re-test)
rem      removes the gauge_absent third of the population up front. This run:
rem      defer_absent_declined=114477 against gauge_prev=598158, i.e. 16% of the
rem      old population is gone before a single draw is buffered.
rem   3. THE 14 FPS WAS NEVER INSTRUMENTED. m_timing.deferred_instance did not
rem      exist when that measurement was taken - round 32 added it precisely so
rem      this re-test would not have to guess, and it reads deferred_instance=0.00
rem      on 'Remix timing:' today because the path is dead. The population is
rem      27.2 draws per frame; buffering 27 structs cannot cost 14 fps, so the
rem      old attribution is now checkable rather than believable.
rem
rem READ THESE, IN THIS ORDER, ON 'Remix timing:' AND 'Remix live:':
rem   1. deferred_instance=  on 'Remix timing:' (bin\log\RPCS3.log). It is 0.00
rem      today. THIS IS THE COST, in ms/frame, measured rather than inferred.
rem      Compare it against frame_ms= on the same line.
rem   2. defer_fresh >> defer_flip = the deferral is doing work. PRE-REGISTERED
rem      (round 32's own threshold): defer_flip/defer_buffered must come in
rem      BELOW 72%. If it is still ~half, the absent branch was not the waste,
rem      the narrowing is refuted, and this goes back to 0.
rem   3. anchor_parked / anchor_recap / anchor_promoted MUST NOT MOVE from
rem      8 / 0 / 8 per 21630 flips. This knob cannot touch them. If they move,
rem      something else did.
rem   4. The fixture and the big window pipes: do they stop teleporting.
rem
rem PRE-REGISTERED REFUTATION: a deferred draw is submitted LATER IN THE FRAME
rem than the geometry around it. If anything now renders in front of or behind
rem something it should not - decals sinking into surfaces, blended props
rem sorting wrongly against opaque ones - that is this knob's submit-order
rem change and nothing else in this build can cause it.
rem
rem REVERT: set "RPCS3_REMIX_DEFERPREANCHOR=0"  <- one line, round 44b exactly.
set "RPCS3_REMIX_DEFERPREANCHOR=1"
rem Hard cap on how many draws one frame may hold back; the excess goes out the
rem old way and counts defer_spilled.
set "RPCS3_REMIX_DEFERPREANCHORMAX=4096"
rem ROUND 32 - NARROW THE DEFERRAL POPULATION BEFORE RE-TESTING THE KNOB ABOVE.
rem The 2026-08-16 revert note above is correct: 72% of the buffering did no work
rem (defer_buffered=543244 defer_fresh=149761 defer_flip=393483). But nothing
rem separated the TWO branches that ask to be deferred, and they are not equally
rem likely to pay:
rem   anchor_prev  - last frame's anchor EXISTS for this render source, so this
rem                  frame's is plausibly still to come. Deferring is a good bet.
rem   gauge_absent - NO anchor in this frame or the previous one. Deferring bets
rem                  on a source that has not produced an anchor in two frames.
rem MEASURED, round-31 run: gauge_prev=225700 gauge_absent=125966, so absent is
rem 35.8% of the 351666 deferral population. Reproduced at 35.8% in the earlier
rem session (prev=328430 absent=182985). This drops that third.
rem INERT while DEFERPREANCHOR=0 - it is staged, not active.
rem IF YOU WANT THE STABLE PROPS BACK, this is the run to try:
rem   set "RPCS3_REMIX_DEFERPREANCHOR=1"   (leave DEFERPREVONLY at 1)
rem   then read defer_buffered / defer_fresh / defer_flip / defer_absent_declined.
rem   PRE-REGISTERED: defer_flip/defer_buffered must come in BELOW 72%, and fps
rem   must stay above ~40. If defer_flip is still ~half of defer_buffered, the
rem   absent branch was not the waste and narrowing is refuted - set both back.
rem REVERT: set this to 0 to reproduce round 31's deferral population exactly.
set "RPCS3_REMIX_DEFERPREVONLY=1"
rem ============================================================================
rem ROUND 46 - THE PLAY-TEST CARD. THE ARMS, THE GUN AND THE HELMET.
rem ============================================================================
rem DEFERPREANCHOR=1 above fixed the WORLD ("light fixture is now stable, props
rem are not moving as camera turns"). The viewmodel did NOT move with it, and
rem round 46 found why: the deferral has an explicit exclusion for viewmodel
rem draws, and it is gated on the TAG, not on the PLACEMENT.
rem
rem The exclusion's own comment argues "a viewmodel draw is placed relative to
rem the weapon camera, so no world anchor can make it more correct". That is
rem FALSE at VMTAGONLY=1 (set below, and it is this title's shipped value):
rem the tag is decoupled from the placement, so the arms go down the ordinary
rem WORLD path and are divided by the WORLD gauge. Round 34 already reached this
rem conclusion for the twin gate on the tail-rescue ladder and moved that one to
rem viewmodel_place; this is the sibling it left behind.
rem
rem MEASURED, the round-45 play-test, 44,375 flips:
rem   * all 4 viewmodel picks read  ref=anchor_prev anchor_vp=ad7ce9d672a0bf6b
rem     - divided by the WORLD program's anchor, one frame stale - while all 15
rem     world/prop picks read anchor or identity-bypass. 0 of 15 anchor_prev.
rem     That split IS the user's split: props stable, arms shaking.
rem   * 'Remix albedo-trace:', 9,544 continuous samples, camera FREE:
rem         ref=anchor       5,622 draws   |t| < 0.01 on 5,622 of 5,622 (100%)
rem         ref=anchor_prev  2,099 draws   |t| < 0.01 on     0 of 2,099
rem         ref=camera       1,823 draws   |t| < 0.01 on     0 of 1,823
rem     A fresh anchor is not merely better. It is EXACT, every single time.
rem   * defer_fresh 867,086 / defer_buffered 874,746 = 99.1% of held draws DO
rem     get a fresh anchor later in the same frame. A held viewmodel draw is
rem     overwhelmingly likely to be flushed against one, not to time out.
rem   * the tail rescue is ALREADY armed for these draws and never succeeds:
rem     tail_rescued_cur + tail_rescued_aged = 0 of 131,774, with 99,428 failing
rem     "same_ref". No alternative EXISTING anchor helps. Only waiting does.
rem
rem The operators are preserved, not dropped: the submit site now captures the
rem composite of apply_viewmodel_basis + apply_viewmodel_rotation as
rem Op = post * pre^-1 and the flush replays it. MEASURED what would otherwise
rem be lost: dbasis=0 on all 3,293 census lines (VMBASIS=0, that operator is
rem inert) but drot = 1.46..1.99 units on every one of them.
rem
rem JUDGE IT ON, in order. Both counters are on 'Remix live:', which reaches
rem remix_dump.log - NOT on 'Remix stats:', which is in RPCS3.log and does not
rem survive the next launch of rpcs3:
rem   1. vm_op=<captured>/<declined>/<singular>/<replayed>
rem        captured  sizes the viewmodel population per frame; counted on ANY
rem                  run, armed or not (~2.3/frame last run - vm_tagged 100,226
rem                  over 44,375 flips).
rem        declined  MUST BE 0 at the shipped VMROTAXIS=1 VMROTPIVOT=0
rem                  VMBASIS=0. Non-zero means one of those three moved and the
rem                  operator stopped being replayable - see the warning below.
rem        singular  a failed 3x3 inverse. Must stay ~0.
rem        replayed  bounded above by defer_viewmodel; the gap is exactly the
rem                  draws that reached flip.
rem   2. defer_viewmodel= - viewmodel draws ACTUALLY BUFFERED, counted at the
rem      buffering site rather than at the election. Structurally 0 unless
rem      DEFERVIEWMODEL=1 AND DEFERPREANCHOR=1. Zero with both armed means the
rem      route has no candidates and the diagnosis is wrong, not the plumbing.
rem   3. THE EYES: does the gun still have parkinsons, does the helmet still
rem      detach and stretch in the nectar room.
rem   4. 'Remix cost:' in remix_dump.log (NEW - see below).
rem
rem PRE-REGISTERED REFUTATION: if replayed is ~0 while defer_viewmodel is large,
rem every held viewmodel draw reached FLIP rather than an anchor
rem (flush_deferred_at_flip does not rebuild the transform, so it has nothing to
rem replay) and the gun will look exactly as it does today. If the gun instead
rem TELEPORTS or doubles, the replay is composing wrongly and this knob goes
rem straight back to 0.
rem
rem DO NOT MOVE VMROTAXIS, VMROTPIVOT OR VMBASISPIVOT WHILE THIS IS ON.
rem The replay is exact only while both viewmodel operators are placement-
rem independent world-space LEFT-multiplies. VMROTAXIS>=4 switches
rem apply_viewmodel_rotation to a MODEL-space RIGHT-multiply (M' = M*R), and
rem VMROTPIVOT/VMBASISPIVOT != 0 derive the pivot from the placement itself. The
rem backend REFUSES the capture at those settings rather than replaying a wrong
rem operator - those draws take the immediate path and land in vm_op declined -
rem so nothing breaks, but the knob quietly stops doing anything for them.
rem
rem REVERT: set "RPCS3_REMIX_DEFERVIEWMODEL=0"  <- one line, round 45 exactly.
set "RPCS3_REMIX_DEFERVIEWMODEL=1"
rem ROUND 32 - THE GEOMETRY AUDITS, AND THE ONLY REMOVABLE PART OF THE FRAME.
rem MEASURED in round 31's worst window that actually drew geometry (frames=38):
rem   draw=50.88 of frame_ms=53.03, and the five named children only account for
rem   ui=2.53 mesh_create=0.14 tex_bind=1.11 uv=5.67 draw_instance=0.48
rem   => rest=40.96 ms/frame = 80.5% of draw, 77% of the whole frame.
rem audit_vertex_extent makes FOUR passes over every sub-draw's decoded positions
rem and audit_world_extent walks the whole index list into world space. Both are
rem diagnostics. Setting this to 0 skips both.
rem DO NOT set 0 as a blind perf fix. Read audit= on the new "Remix timing:" line
rem first - the timer exists so the size of the trade is a number, not a guess.
rem What 0 costs: the vtx-spread census, the streak/wext census, and the wext
rem REFUSAL gate. Losing the gate is safe on Haze only because it is measured -
rem wext_refused=0 for the whole round-31 session, so it never fired. Check that
rem counter on "Remix live:" before using 0 on any other title.
rem REVERT: set "RPCS3_REMIX_DRAWAUDIT=1"
rem
rem ROUND 34 - ARMED AT 0, AND THIS IS ROUND 33's OWN PRE-REGISTERED VERDICT.
rem round33-inbox.md registered: "If 'audit' is the largest new child: the fix
rem is RPCS3_REMIX_DRAWAUDIT=0, not an optimisation." MEASURED from
rem bin\log\RPCS3.log (the run that ended 2026-08-17 09:37:23), last two
rem 'Remix timing:' windows:
rem   frame_ms=35.22 | draw=33.20 (ui=2.28 mesh_create=0.26 tex_bind=0.53
rem     uv=3.48 draw_instance=0.43 decode=6.35 audit=11.35 hash=3.76 xform=0.19
rem     rest=4.58)
rem   frame_ms=33.57 | draw=31.51 (... decode=6.10 audit=11.03 hash=3.64
rem     xform=0.12 rest=3.78)
rem So audit IS the largest new child at 11.35 ms = 32.2% of the whole frame
rem and 34.2% of draw, and 'rest' has fallen to 4.58 ms (13%) - the round-31
rem 40.96 ms residual is fully accounted. 'rest' is NOT 0.00 beside a large
rem 'draw', so the saturation/double-counting tripwire did not trip.
rem
rem LEFT AT 1 THIS ROUND ANYWAY, AND THE REASON IS A REVIEW FINDING THAT KILLS
rem THE "ZERO VISUAL CHANGE" CLAIM. I armed 0, then a review of the round-34
rem diff found that DRAWAUDIT=0 has TWO consumers its own documentation does
rem not list, both via m_streak_measured - which is written ONLY inside
rem audit_world_extent, so with the audit off it stays false all session:
rem   * the SKIPEXTENTVP gate reads '&& m_streak_measured', so it stops firing.
rem     The launcher arms SKIPEXTENTVP=57A12323F22F4988 with SKIPEXTENTMIN=128,
rem     and 57A12323F22F4988 is ONE OF THE THREE NEAR-DEPTH VIEWMODEL PROGRAMS.
rem     Disabling that refusal changes what is submitted for the exact
rem     population this round's priority-1 test is measuring.
rem   * extent_plausible = '!m_streak_measured || ...' becomes unconditionally
rem     true, so GUESTLIGHTEXTENT stops rejecting anything.
rem So "wext_refused=0, therefore dropping the audit cannot change what is
rem submitted" was WRONG - wext is not the only gate the audit feeds. Arming
rem both DRAWAUDIT=0 and the viewmodel change in one run would confound the
rem measurement that matters, so frame time waits a round.
rem
rem ROUND 35: take the ~11 ms then, and either (a) arm DRAWAUDIT=0 with
rem SKIPEXTENTVP blanked so the interaction is explicit, or (b) attack the
rem decode child (6.35 ms) instead, by caching decoded positions per
rem (source pointer, count) as round 33 pre-registered.
rem
rem ================= ROUND 48: ARMED AT 0, AND THE WELD IS GONE ==============
rem Round 34 was right to refuse this and its objection has now been removed IN
rem CODE rather than argued away. The blocker was that DRAWAUDIT=0 also disarmed
rem the SKIPEXTENTVP refusal, because that gate reads 'AND m_streak_measured' and
rem m_streak_measured is written only inside audit_world_extent. Option (a) above
rem was to blank SKIPEXTENTVP; that trades one behaviour change for another.
rem
rem INSTEAD: the audit now ALSO runs whenever the current vertex program IS the
rem SKIPEXTENTVP program, whatever DRAWAUDIT says. The gate keeps its input by
rem construction, for the one program hash it can ever fire on, and every other
rem draw in the scene stops paying. Nothing is traded.
rem
rem WHY IT IS WORTH IT - MEASURED, round-47 play-test, bin\log\RPCS3.log, the two
rem in-play 'Remix timing:' windows (the third is a paused/menu frame, ignore it):
rem   frame_ms=69.78 / draw=67.13 (... decode=14.15 audit=24.76 hash=2.33
rem     xform=0.48 rest=13.93)
rem   frame_ms=59.15 / draw=54.73 (... decode=12.14 audit=20.41 hash=1.02
rem     xform=0.48 rest=9.63)
rem audit is 24.76 ms = 35.5% of the WHOLE FRAME and 36.9% of draw - larger than
rem decode and larger than rest. It has grown since round 34 measured 11.35 ms.
rem
rem WHY IT IS SAFE - the other two consumers, MEASURED over 83,019 flips:
rem   wext_refused=0  - the refusal inside audit_world_extent never fired at all
rem   skipg_extent=0  - nor did SKIPEXTENTVP, though it is armed and its program
rem                     IS drawn, so the gate is live and simply never met its
rem                     128-unit threshold. It is preserved anyway, by the code
rem                     change above - this counter is corroboration, not the
rem                     argument.
rem   extent_plausible in maybe_inject_guest_light becomes unconditionally true.
rem                     That is a COST change, not a behaviour change: it removes
rem                     an early-out, and small_enough - the same transformed AABB
rem                     - still gates the trigger.
rem
rem ATTRIBUTION, because GUESTLIGHTAUTO=2 also ships this round. The two cannot
rem be confused: this one moves 'audit=' on the timing line and NOTHING visual;
rem that one moves guest_lights= and is a pixel test. The ONE confound is frame
rem time, and it has a tell - if fps does not improve and 'rest' has climbed while
rem 'audit' fell, that is the extent_plausible interaction above, i.e. the bbox
rem walk in maybe_inject_guest_light (which sits in no named timer) eating the
rem saving. In that case revert GUESTLIGHTAUTO first, not this.
rem REVERT: set "RPCS3_REMIX_DRAWAUDIT=1"
set "RPCS3_REMIX_DRAWAUDIT=0"
rem ALPHA TO COVERAGE - the third cutout channel, and the last hardware mechanism
rem that could explain foliage/fences rendering as solid cards. rpcs3's own OpenGL
rem backend reads these two bits and this backend never did. Read a2c_ctrl /
rem a2c_reg / a2c_applied on the "Remix live:" line; both detection counters at 0
rem for a whole run is the NEGATIVE verdict - a real answer, not a failed fix.
rem Set to 0 and relaunch if holes appear in geometry that should be solid.
rem 2026-08-15 TURNED OFF pending investigation: measured a2c_ctrl=866637 and
rem a2c_applied=658012 - it is alpha-testing 658k draws at reference 128, and the
rem 02:41 clip shows surfaces going transparent/white. Set to 1 to re-test.
rem DEAD LINE - neutralised 2026-08-16. Re-assigned further down (~:449) to 1,
rem and the LAST assignment wins - so alpha-to-coverage is ON despite this
rem reading 0. The banner agrees: fpa2c=1. Edit the later line.
rem set "RPCS3_REMIX_FPA2C=0"
rem NECTAR GREYSCALE. 0 = never publish the effect (what you asked for), 1 = the
rem old behaviour, which fired whenever you merely looked towards the room because
rem the pass it keys on is a generic half-res lighting pass, not the effect itself.
set "RPCS3_REMIX_NECTARMODE=0"
rem Dump the live blend state and fragment-program constants of the half-res
rem lighting passes ("Remix lightpass:" lines). Pure measurement - this is the
rem input for deciding whether the game's real lights can be intercepted next
rem round. 0 removes the lines.
set "RPCS3_REMIX_LIGHTPASSCENSUS=1"
rem THE INVISIBLE DOOR PROTOCOL. This is the albedo the OPEN door picks with; the
rem closed one is refused before it can be picked at all, so this watch is the only
rem instrument that can name the program refusing it. Face the closed door and copy
rem every "Remix watch:" line with verdict=world_refused out of the dump.
rem Blank it once the door is bound - the watch costs a log line per frame.
rem ROUND 45: EF4700267F08C7F3 MATCHED ZERO LINES IN THE ENTIRE 21630-FLIP RUN.
rem It is retired here rather than kept as a decoration. Retargeted at the two
rem albedos the round-45 audit named as worth watching:
rem   4094F22DB2A8278A - the checkerboard tile, vp c1d482dcd1b03ed0, vtx 2088.
rem     It is the SUBMITTED NEIGHBOUR of the black-void underfloor and it picks
rem     cleanly (ref=identity-bypass, origin exactly [0 0 0]). Its verdict lines
rem     carry the render-source key and the eye distance of the surface the
rem     missing planks sit under, which is what the next attempt on the floor
rem     needs and what a pick on an unclickable pixel can never give.
rem   2722B18EB6EEDDF6 - the ONLY albedo any skip gate eats on the MAIN pass
rem     (skippair, with vp 57A12323F22F4988). Measured by subtraction this run:
rem     skip_albedo 1660629 - notex_refused_skip 1661448 + skip_shadowonly 14422
rem     = 13603 draws. It is the giant-NPC-weapon suppressor, not a floor, and
rem     this watch is what will say so continuously instead of by arithmetic.
rem WALK OVER THE BLACK FLOOR WITH THIS ARMED and copy every 'Remix watch:' line.
set "RPCS3_REMIX_WATCHALBEDO=4094F22DB2A8278A,2722B18EB6EEDDF6"
rem SKIPAUXUNTEX is RETIRED this round: the code default flipped 1 -> 0 on the
rem evidence (99,695 refusals in round 4 with the wash still reported, and still
rem no doors with the gate off). The line above already reads 0, so nothing here
rem changes - set it to 1 if you ever want the old rule back for archaeology.

rem --- round 6 -----------------------------------------------------------------
rem Nothing above this line was edited. cmd's "set" is last-wins, so deleting any
rem line below restores whatever the line above it says, with no other edit.
rem
rem === PRIMARY 1: the untextured 41% (flat white / beige / black surfaces) ======
rem 41% of every draw that reaches Remix has NO albedo material and therefore
rem renders WHITE - warm-lit that reads beige, unlit beside a lit neighbour it
rem reads black. Three fixes ship together; each has its own off switch.
rem
rem 1) Neutral grey instead of white for any draw with no texture. A mitigation,
rem    not a fix: it invents nothing, it just stops the unknown surfaces from
rem    being the brightest thing in the room. Read notex_mat_applied.
rem    0 restores the old material-less (white) submit exactly.
rem    NOTE: a draw that carries the game's OWN vertex colour (the sky dome) gets
rem    that colour halved by the grey. If a vertex-coloured surface looks too
rem    dark, this is the knob.
set "RPCS3_REMIX_NOTEXMAT=1"
rem 2) Stop submitting the half-res shadow/lighting passes as white surfaces.
rem    Only draws whose EVERY texture unit is a depth buffer AND whose render
rem    target is smaller than the main pass. Read skip_shadowonly and the
rem    "Remix shadowonly:" lines (they name every program it touches).
rem    0 restores submission - use it if something legitimate vanishes.
set "RPCS3_REMIX_SKIPSHADOWONLY=1"
rem 3) Let the albedo walk step past a refused unit to one the fragment program
rem    ACTUALLY SAMPLES and that carries a colour format. The sampled-mask term
rem    is what the old RETRYUNSUP widening lacked when it painted character faces
rem    onto tree trunks. Read tex_walk_sampled / tex_colorunit.
rem    0 restores the refusal - use it if wrong textures appear on surfaces.
set "RPCS3_REMIX_WALKSAMPLED=1"
rem
rem === PRIMARY 2: the missing world geometry ===================================
rem THE FIX for invisible doors / walls / ground. 57% of refused world draws are
rem refused by the affinity gate, and the shipped residue histogram says the
rem tolerance is NOT the cause: zero refusals sit near it and 83% sit 50-500x
rem over it. They were divided by the wrong reference - the cross-pass half-res
rem camera - because no anchor covered their own render pass at that instant.
rem Three different world programs refuse with the BIT-IDENTICAL residue 3.40683,
rem which is a property of the reference, not of the programs. This retries the
rem division against an anchor of the draw's own pass shape and re-runs the SAME
rem gate, so a bad retry still refuses: worst case is today (missing), never
rem smeared. Read tail_rescued_cur / tail_rescued_aged / tail_rescue_failed and
rem the "Remix tail-rescue:" lines (residue before -> after).
rem 0 restores every refusal exactly - FIRST thing to try if geometry appears in
rem the wrong place.
set "RPCS3_REMIX_TAILRESCUE=1"
rem How stale the second retry's anchor may be, in frames. Covers cuts and burst
rem frames where both anchor slots went stale. Lower it if rescued geometry lags.
set "RPCS3_REMIX_TAILRESCUEAGE=30"
rem
rem === ALPHA TO COVERAGE - re-enabled with CORRECTED semantics =================
rem FPA2C was turned off in round 5 because it alpha-tested 658,012 draws at
rem reference 128 and made surfaces see-through. The reason is now known and it
rem was a read bug, not a tuning problem: the "shader control" bit this code read
rem (0x01000000) sits INSIDE the used-temp-register count field (0xff000000) of
rem the RSX shader control word. It never meant alpha-to-coverage; it meant "this
rem program uses an odd number of temporaries" - which is half the draws in the
rem scene. rpcs3's own GL backend gates A2C on the anti-aliasing REGISTER, and so
rem does core's software emulation of it. Mode 1 now requires BOTH bits. Haze
rem never sets the register, so the replay is inert and a2c_applied must read 0.
rem 2 = the old broken OR (reproduces the see-through regression on demand).
rem 0 = fully off.
set "RPCS3_REMIX_FPA2C=1"
rem
rem === GOD-RAY FAN - hide the game's own baked light-shaft card ================
rem vp=af06f6d32ec048ee and 15ad612980aca110 draw the same 4,000-vertex blended
rem fan with this albedo, world-anchored over the ceiling fixtures. It is a baked
rem light shaft, and our radiance-150 sphere lights plus CAMLIGHT make the
rem translucent card glow instead of reading as a shaft. Hidden here so Remix's
rem own volumetrics and the guest lights draw real shafts. Read cat_hidden.
rem If the room loses wanted atmosphere, change CAT_HIDE to CAT_PARTICLE below.
rem
rem *** 2026-08-16 ROUND 25: APPENDED 597C5F48E671924F. Changing an existing value; here
rem is the measurement. The user Ctrl+Clicked "the warping light fixture and geometry
rem moving past me on the carrier" and BOTH picks landed on this one albedo:
rem   vp=504d2b3a1ce0fc74 fp=9e16b7f230c89df0 albedo=597C5F48E671924F vtx=357 ext=40.0
rem   vp=b01bfce3fc580e3b fp=e86b00e028250181 albedo=597C5F48E671924F vtx=2132 ext=51.3
rem both blend=1 depth_write=0 clip=512x288, and 'Remix lightpass:' names both programs
rem with blend_rgb=1/1/32774 - ONE/ONE/FUNC_ADD. That is the game's DEFERRED LIGHT
rem ACCUMULATION pass, drawn at half the main 1024x576 target, being submitted to Remix as
rem ray-traced world geometry. 1,841 of the 1,841 log lines carrying this albedo are on the
rem 512x288 pass and NONE on the main pass, so hiding it cannot touch main-pass geometry.
rem Its UVs are also junk (uv_ucode=0, fixed 1/4096 fallback, u=[-8..8] on a 128x128 sheet),
rem which is why it reads as a warping slab a few units off the eye that slides as you move.
rem COST: it is also 198 'Remix suncard:' rows, but suncard_elected=0 and SUNTRACK=0, so it
rem elects nothing today; and 13 'Remix sky-census:' rows, all reject:extent, so no sky tag
rem is lost. REVERT: set "RPCS3_REMIX_CAT_HIDE=B6AE753B64E6F693"
set "RPCS3_REMIX_CAT_HIDE=B6AE753B64E6F693,597C5F48E671924F"
rem
rem === ROUND 35: THE PLAYER'S FULL-BODY SHADOW - a (vp, FRAGMENT program) key ===
rem You asked twice: "how can we do away with the legs and arms shadow instead of
rem a shadow showing a full body" and "i also dont see the shadows for them so
rem could we apply that to the legs too".
rem
rem WHY THE KEY IS THE FRAGMENT PROGRAM AND NOT THE ALBEDO. The round-35 brief
rem proposed the (vp, albedo) pair route. MEASURED over the whole 757 MB
rem bin\remix_dump.log, that route OVER-MATCHES and would take the arms with the
rem legs - your own Ctrl+Click picks show both carrying vp=f39f504649b6f442 AND
rem albedo=0721D150DF278E7D:
rem
rem   legs   vp=f39f504649b6f442 fp=b64dc06f79b8b42b vtx=952  extent=1.145
rem   arms   vp=f39f504649b6f442 fp=d6f00cddfb5c6e0a vtx=2140 viewmodel=1
rem
rem The FRAGMENT program separates them cleanly. Every one of the 1381
rem 'Remix fpcandidate:' rows for b64dc06f79b8b42b is vtx=952 - 12 albedos (the
rem skin variants), not one row anything else. That is the whole player body and
rem nothing else, which is exactly what round 20's albedo-only pin was not.
rem
rem MODE 1 (armed) = HIDDEN. In the deployed runtime this sets the instance mask
rem to 0, so the body leaves the acceleration structure entirely: no full-body
rem shadow, and no legs when you look down. Zero conf edits, works immediately.
rem
rem MODE 2 = THIRD_PERSON_PLAYER_MODEL: legs still VISIBLE, casting no shadow.
rem This is the only "visible but no shadow" route that exists in the runtime -
rem there is no castShadow flag anywhere in it. It needs TWO lines added to
rem bin\rtx.conf by you, and BOTH or it is worse than nothing:
rem     rtx.playerModel.enableInPrimarySpace = True
rem     rtx.playerModel.enablePrimaryShadows = False
rem With only the first, the legs go invisible and still cast. With neither, the
rem legs go invisible. Warning: enableInPrimarySpace = True also masks every
rem VIEW_MODEL candidate to zero inside the runtime's createViewModelInstances,
rem which would take the ARMS with it the moment a view-model camera is valid.
rem
rem PRE-REGISTERED: 'Remix stats:' cat_hidepair climbs into the thousands (the
rem body draws about once a frame). If it reads 0 the key never matched and the
rem hashes are wrong, NOT the mechanism.
rem REVERT: blank either half - set "RPCS3_REMIX_HIDEPAIRVP="
set "RPCS3_REMIX_HIDEPAIRVP=F39F504649B6F442"
set "RPCS3_REMIX_HIDEPAIRFP=B64DC06F79B8B42B"
set "RPCS3_REMIX_HIDEPAIRMODE=1"
rem
rem === GUEST-LIGHT DISCRIMINATOR - measurement only this round =================
rem A census of every submitted draw that LOOKS like a light fixture: bright
rem albedo (mean-RGB luminance >= LUM), small (world extent <= MAXEXT), and in
rem one of the two states a fixture uses - opaque housing or blended glow card.
rem Read the "Remix light-candidate:" lines; their albedo= values are the
rem paste-ready input for GUESTLIGHTALBEDO and EMISSIVE above, gathered from an
rem ordinary session instead of by hunting fixtures with the cursor.
set "RPCS3_REMIX_GUESTLIGHTLUM=0.7"
set "RPCS3_REMIX_GUESTLIGHTMAXEXT=6"
rem 1 turns every censused candidate into a real light through the existing
rem dedup/cap machinery. OFF by default: the last time fixture identity was
rem generalised this way it put lights on doors. Read the census FIRST, then flip
rem this to 1 in the same sitting if the candidates look right.
rem ============================ ROUND 41 - THIS IS THE LIGHT FIX ====================
rem MODE 2 = GLOW CARDS ONLY, and it replaces the albedo list entirely.
rem The "Remix light-candidate:" census separates two populations cleanly (MEASURED, round-40
rem run, grouped by albedo+state):
rem   F613BD83DAF2B4E2 glowcard n=82 vtx=[22,23,26]  lum=0.8155 rgb=[0.8424 0.8314 0.5781]
rem   0F86FCEDC4226D3B glowcard n=64 vtx=[4]         lum=0.9375 rgb=[0.9375 0.9375 0.9375]
rem   A0D0AB03F0BB1D3C glowcard n=59 vtx=[200,36,64] lum=1
rem   9CA366166DF12A90 glowcard n=27 vtx=[116,12,16] lum=0.9813 rgb=[1 1 0.7412]
rem   9E95F66ECE26BE29 fixture  n=23 vtx=[216,8,846] lum=0.7226
rem   16B46EA28EEDFC8E fixture  n=22 vtx=[8]         lum=0.7223
rem Glow cards are small ADDITIVE billboards carrying the game own lamp tints; "fixture" rows
rem are the big opaque housings at a flat metal lum ~0.72. Mode 2 takes only the cards.
rem THE HYPOTHESIS (INFERRED - this run is the test): Haze draws a glow card over a fixture
rem that is LIT and omits it for one that is not. If so this reproduces the raster reference
rem "not every bulb is lit" with NO list, and each light gets the card own measured colour
rem and its own extent-derived radius for free.
rem It also retires the (vp,fp) key here: F613BD83DAF2B4E2 alone is drawn by 5 vertex and 12
rem fragment programs, so no pair can name it. Mode 2 ignores GUESTLIGHTVP/FP entirely.
rem PRE-REGISTERED REFUTATION: if the plant ends up with far more lights than it has visibly
rem lit fixtures, or guest_light_capped= starts climbing, the glow card is NOT the "is lit"
rem signal and this is wrong. guest_lights= and guest_light_capped= size it directly.
rem MODE 1 = the old whole-census population (fixtures AND cards) - that is what put lights
rem on doors in August. Do not use 1 as a fallback; use 0.
rem
rem ================= ROUND 48: ARMED AT 2, WITH THE TWO ROUND-41 FAULTS FIXED =================
rem This is the user's request, both halves of it, and they are ONE mechanism: "fix all the
rem lights being lit to only the ones that are actually lit", and "make a spherical light on
rem that flickering light that turns off when the bulb is flickering off".
rem
rem Haze draws an additive glow card over a fixture that is LIT and omits it for one that is
rem not - which is why only some bulbs glow in the raster original. Keying the light on the
rem CARD rather than on a texture list therefore answers both at once: unlit fixtures never
rem produce a card, so they stay dark, and a bulb that flickers off stops drawing its card on
rem the off frames, so its light disappears those frames. No animation code exists or is
rem needed. EMISSIVE cannot do this at all - it is MATERIAL-scoped, so every surface sharing
rem the bulb texture glows or none does, which is exactly why EMISSIVE=71D189E9B559A7F9:30
rem lights every fixture in the level whether it is on or not.
rem
rem Round 41 shipped this and it had to be turned off for two faults. BOTH are fixed in code
rem this round, and neither fix is a list:
rem   * it lit the player's ARMS AND WEAPON, and scattered spheres a metre in front of the
rem     eye. One fault, not two: a viewmodel draw satisfies every term of the glow-card test,
rem     and the position derived from it is the AABB centre of a VIEWMODEL transform.
rem     FIXED by a world-geometry gate on the AUTO arm - see guest_light_vmref= below.
rem   * it lit SOLDIERS AS THEY WALKED. The viewmodel gate cannot catch that (an NPC is
rem     ordinary world geometry) and no render state separates a suit panel's glow card from
rem     a lamp's. What separates them is that A LAMP DOES NOT MOVE.
rem     FIXED by GUESTLIGHTSTABLE below - see guest_light_unstable=.
rem
rem PRE-REGISTERED REFUTATION, unchanged from round 41 and still the right one: if the plant
rem ends up with far more lights than it has visibly lit fixtures, or guest_light_capped=
rem starts climbing, the glow card is NOT the "is lit" signal and this is wrong.
rem THE PIXEL TEST: does the flickering bulb's light flicker with it, and do unlit fixtures
rem stay dark. Both are visible without any log.
rem MODE 1 = the old whole-census population (fixtures AND cards) - that is what put lights
rem on doors in August. Do not use 1 as a fallback; use 0.
rem REVERT: set "RPCS3_REMIX_GUESTLIGHTAUTO=0"
set "RPCS3_REMIX_GUESTLIGHTAUTO=2"
rem
rem --- ROUND 48: the static-fixture gate, and the reason AUTO=2 is usable at all -------------
rem A candidate must re-appear in the SAME quantised cell (0.25 world units, the same cell the
rem light hash already uses) on this many DISTINCT frames before it may create a light.
rem 30 = one second at 30 fps. The cell is 0.25 units, so an NPC walking even slowly leaves its
rem cell within 2-4 frames and can never graduate in any of them; a bolted-down fixture hits
rem one cell every frame it is drawn and graduates in 30.
rem COST OF 30: a fixture takes ~1s of being on screen to light the FIRST time you ever see it.
rem After that its cell is CONFIRMED FOR THE REST OF THE LEVEL, so re-entering the room - and,
rem critically, a bulb flickering back on - re-lights on the first frame. Without that
rem stickiness any window longer than the flicker period would hold a flickering bulb dark
rem forever, which is the opposite of what was asked for.
rem 0 = OFF = round-41 behaviour bit-exactly (light on first sight). Clamp is 600.
rem TOO SLOW TO LIGHT: 15, then 8. STILL LIGHTS MOVING NPCs: 60, then 120.
rem REVERT: set "RPCS3_REMIX_GUESTLIGHTSTABLE=0"
set "RPCS3_REMIX_GUESTLIGHTSTABLE=30"
rem
rem === DEFERPREANCHOR - SUPERSEDED BY ROUND 45. IT IS NOW SET TO 1 ABOVE. ======
rem This block used to read "deliberately LEFT OFF". It is kept because its last
rem sentence is still the operative one: "the FPS attribution is unproven". Round
rem 32 added m_timing.deferred_instance so that it no longer has to be. There is
rem NO 'set' in this block - cmd is last-wins and the only assignment is above.
rem Round 6's numbers, kept for the A/B: 47% of buffered draws (367,629 of
rem 774,606) never saw their frame's anchor. The tail rescue above shares NO code
rem path with the deferral, so nothing here blocks anything.

set "RPCS3_CODEX_INPUT_FILE=%TEMP%\rpcs3-codex-input.txt"

rem --- Selva tree-top probe, 2026-08-15 ----------------------------------------
rem The streak gate refuses any draw whose post-transform bounding box spans more
rem than N times the FRAME'S OWN median world extent. Selva's median measures
rem 5.78, so the default 128 refuses anything over ~740 units - and the trees
rem there measure 1252 to 24000. That makes this gate a prime suspect for the
rem missing tree tops. 0 disables it entirely.
rem WATCH FOR: this gate exists to catch genuinely exploded geometry, so if huge
rem stretched shards or streaks appear anywhere, that is the gate doing its job
rem and the trees need a different fix - put it back to 128 and tell me.
rem Read wext_refused on the "Remix live:" line: it was 63848 with the gate on.
set "RPCS3_REMIX_STREAKGATE=0"

rem --- round 7 -----------------------------------------------------------------
rem Nothing above this line was edited. cmd's "set" is last-wins, so deleting any
rem line below restores whatever the lines above say, with no other edit.
rem
rem === PRIMARY: the guest UI's UV wrapping (garbled HUD / duplicate glyph rows) ==
rem THE SEAM FIX. Haze's UI is drawn from atlases with sub-rect UVs while the
rem atlas is bound REPEAT, and our CPU sampler implements repeat as
rem "coord -= floor(coord)" with a NEAREST fetch. So a UV a hair below 0 does not
rem land on the glyph's own gutter - it lands on the OPPOSITE EDGE OF THE SHEET
rem and returns a different glyph at full weight. That is the garbling. The dump
rem already contained the proof: one font program authors u=[-0.018555..0.99902],
rem which is ~9.5 texels off a 512-wide sheet.
rem A draw whose authored UV span is at most one texture cannot be trying to tile,
rem so out-of-range there is padding and now clamps to the sheet edge. A draw that
rem genuinely spans more than one texture keeps true repeat, per axis.
rem Read ui_uv=in/wrap/seam/mirror/clip/clamp on the "Remix live:" line: seam
rem climbing while text is on screen is the fix firing. "Remix uiwrap:" lines name
rem each 2D atlas with its real wrap modes and its excursion IN TEXELS.
rem 0 restores the old sampling bit-exactly - that A/B is the whole attribution.
set "RPCS3_REMIX_UICLAMPSUBRECT=1"
rem THE SECOND ROUTE, no rebuild. Haze also draws UI as world geometry sampled on
rem the GPU, where no per-sample rule of ours can run. If the HUD counters are
rem still garbled while menu text is fixed, that is the route they take: take the
rem albedo from a "Remix uiwrap:" line, or from a "Remix uvrange: ... rsxwrap=1/1
rem u=[-..." line, or Ctrl+Click the garbled element, and paste it here
rem (comma-separated, up to 16). Relaunch only. 6575ACE3A42A78E6 is the
rem pre-identified first candidate. Read tex_wrap_forced on the live line.
rem Blank it if a world surface that shares one of these textures goes edge-streaky.
rem 2026-08-16 ROUND 24: ARMED IN PLACE with exactly that pre-identified candidate.
rem This is the "nectar rebooting" overlay text the user Ctrl+Clicked
rem (vp=6f76ab0ad8d926b1 fp=4a1bc009fc6161ef albedo=6575ACE3A42A78E6). Edited HERE
rem rather than re-assigned at the bottom of the file, because a second "set" for
rem one knob is what created the three DEAD LINES already flagged above. The full
rem reasoning is in the round-24 block at the end; this is the only assignment.
rem REVERT: blank it again.
rem
rem *** 2026-08-16 ROUND 25: BLANKED AGAIN. THIS KNOB IS A PROVEN NO-OP HERE. ***
rem I am changing a value round 24 set, and this is the measurement that justifies it.
rem The run's own census answers it in one line:
rem   Remix uvrange: vp=6f76ab0ad8d926b1 ... albedo=6575ACE3A42A78E6 rsxwrap=3/3 dims=512x512
rem rsxwrap=3 is RSX CLAMP_TO_EDGE - the GUEST already binds clamp on both axes, and the
rem decoder already maps that to wrap_u/wrap_v = 0, which is why every one of the 146
rem 'Remix uiwrap:' rows for this texture reads wrap=0/0. CLAMPALBEDO can only WRITE 0/0,
rem so it wrote what was already there: same entry state, same material hash, same sampler
rem on both the 2D and the world route. Zero pixels can have changed.
rem And tex_wrap_forced=1 was never evidence against that - it is a PER-UPLOAD counter
rem (RemixTextures.cpp:985-997 says so: "LISTED-AND-CREATED, not 'the wrap actually moved'"),
rem so 1 means this atlas was created once. It was not "failing to reach the population".
rem The real cause is upstream of sampling entirely - see the round-25 block at the end of
rem this file and RPCS3_REMIX_UIRECTSHRINK. Left blank so a dead lever stops costing rounds.
set "RPCS3_REMIX_CLAMPALBEDO="
rem
rem === WHY TWO ROUND-6 FEATURES BARELY FIRED ====================================
rem SKIPSHADOWONLY classified 437,810 draws and skipped 20,656 of them (4.7%).
rem The term that rejected the rest: "strictly smaller than the main pass"
rem compared against the PREVIOUS FRAME's largest textured clip, which on Haze
rem reads 512x288 on most frames - the exact clip the half-res passes use, so the
rem test was 147456 < 147456 and failed. It now compares against the largest
rem textured clip seen this session. Read mainclip= and maxclip= on the live line,
rem and so_nomain / so_notsmaller for the reject partition.
rem 0 restores the previous-frame value exactly.
set "RPCS3_REMIX_MAINCLIPMAX=1"
rem
rem === THE SELVA CANOPY / lay_other MATCHER =====================================
rem 356,828 world refusals say "no matrix chain into HPOS". Reading the programs
rem offline found why: the LAST full-mask writer to o0 is a MOV from a CONSTANT
rem under a condition code - the particle-cull idiom "if culled, park HPOS
rem off-screen" - and the matcher started its backward walk there, found a MOV
rem with no real source, and threw the whole chain away. It now walks past a
rem predicated writer whose only sources are constants.
rem HONEST EXPECTATION: these programs should move OFF lay_other onto an
rem INNER-CHAIN refusal, not straight to visible geometry. Read the lay_other
rem count on "Remix world-fail:" - if it drops and tail/other rises, that is the
rem matcher getting further, which is the deliverable.
rem 0 restores the old terminal choice exactly - use it if geometry appears in
rem the wrong place or streaks.
set "RPCS3_REMIX_SKIPCCCONST=1"

rem --- round 8 -----------------------------------------------------------------
rem Nothing above this line was edited. cmd's "set" is last-wins, so deleting any
rem line below restores whatever the lines above say, with no other edit.
rem
rem === PRIMARY: the giant geometry (the ship, the "incorrect bones" character, ===
rem === the NPC weapons) - THE SCALE BUG, fixed in the matcher ===================
rem Haze packs positions as four 16-bit integers whose w is that vertex's own
rem divisor, and undoes it in the ucode as "pos.xyz = ATTR0.xyz * RCP(ATTR0.w)".
rem The backend already replays that divide on 25 cached programs (~6M divides a
rem run). Four programs were silently missing it. Why: their compiler emits the
rem divide MUL and then a W-ONLY "MOV rN.w = c[467].z" to complete the homogeneous
rem vector, and the matcher asked for "the LAST instruction that wrote this temp"
rem - which is that MOV, not the MUL. It failed the shape test and gave up before
rem it ever looked for the reciprocal. So has_wdivide stayed false and the RAW
rem QUANTISED 16-bit values went to Remix: extent 9211 on the ship, 669.7 on the
rem bones character, giant NPC weapons - around a perfectly clean instance basis,
rem which is exactly what the picks showed.
rem The matcher now skips w-only writes to find the real producer - the rule its
rem two sibling matchers already state in as many words. Sweep of every cached
rem .vp on this title: 29 carry the shape, 25 pick the IDENTICAL instruction
rem either way (no behaviour change), exactly 4 flip refuse -> match, 0 regress.
rem Read wdiv= and wdiv_shadowed= on the "Remix live:" line, and the
rem "Remix wdiv-shadowed:" lines that name each rescued program once.
rem A Ctrl+Click on the ship or the character should now read areason=wdivide
rem (it read areason=sca-xyz) with a single-digit extent.
rem 0 restores the old single-writer selection bit-exactly - the giants must come
rem back. That A/B is the whole attribution, and it is the FIRST bisect step if
rem anything is now the wrong size.
set "RPCS3_REMIX_WDIVWALK=1"
rem
rem === RETIREMENT PROTOCOL for the three suppression lists ======================
rem The lists below exist ONLY because the scale bug was unfixed. They ship
rem UNCHANGED (still active) so stage 0 is a clean first proof with zero launcher
rem edits. Retire them ONE STAGE AT A TIME by removing the "rem " from the front
rem of the lines in that stage, then relaunching. Put the "rem " back to revert.
rem
rem STAGE 0 - do nothing here. Relaunch as shipped and look at the ship and the
rem   "incorrect bones" character: both should already render at correct scale.
rem   The ship was only being eaten by SKIPEXTENTVP because its extent exceeded
rem   128, and post-fix extents are single digits, so that gate stops matching on
rem   its own. The bones albedo was never in the pair list. The NPC weapons stay
rem   hidden in this stage (their albedo IS the SKIPPAIR albedo).
rem
rem STAGE 1 - retire BOTH 57A1... lists. Un-rem the next four lines, relaunch.
rem   The NPC weapons must appear at correct scale. Nothing else may go giant -
rem   watch wext_refused on the live line and the extent census.
rem   This one relaunch is the retirement verdict for both lists.
rem set "RPCS3_REMIX_SKIPPAIRVP="
rem set "RPCS3_REMIX_SKIPPAIRALBEDO="
rem set "RPCS3_REMIX_SKIPEXTENTVP="
rem set "RPCS3_REMIX_SKIPEXTENTMIN="
rem
rem STAGE 2 - un-drop 4D5A87BFFBCE0717, the fourth member of the same decode
rem   family, which has been dropped WHOLESALE since before the forge record
rem   began. Un-rem the next line, relaunch. Whatever it draws comes back, now
rem   decoded. Do this only after stage 1 looks right.
rem set "RPCS3_REMIX_SKIPVP="
rem
rem LOOK AT THE FIRST-PERSON HELMET IN ALL THREE STAGES. If it is correctly sized
rem now, it was this family and the thread is closed. If it is STILL giant at
rem stage 2, Ctrl+Click it: the pick line names its vp and a real areason, which
rem is the next round's evidence. Both outcomes are progress. (The two round-1
rem helmet suspects, 15AD612980ACA110 and 9F591B6A6B825612, already matched
rem before this fix - so if the helmet draws through either of them, its size
rem problem is NOT this bug.)
rem
rem === THE WRONG-TEXTURE THREAD ("I am Groot" - a soldier rendered in bark) =====
rem MEASUREMENT ONLY this round; nothing is rebuilt, so this cannot regress
rem anything. Two censuses now name the two candidate mechanisms:
rem   "Remix texstale:" - a cache entry whose guest bytes moved under it while the
rem     descriptor key stayed put (a streaming pool recycling an address). Counter
rem     tex_stale_detected. TEXVERIFY is how often, in frames, an entry re-checks.
rem   "Remix texdup:"   - one content hash live under two descriptor keys. This is
rem     what tex_key_dup=11383 has been counting without ever naming anything.
rem Ctrl+Click a wrong-textured surface and join its albedo= against both lists.
rem 0 disables the staleness check entirely (bisect step if frame time regresses).
set "RPCS3_REMIX_TEXVERIFY=120"
rem THE ZERO-CODE PROBE, one relaunch, then set it back. TEXREHASH=1 makes the
rem cache notice changed content and rebuild the entry. If bark-textured soldiers
rem are CURED by it, the mechanism is the stale pool slot. If they are not, it is
rem the content alias. Expect heavy mesh churn while it runs - it re-keys the
rem albedo hash, which is folded into the mesh key, and that is exactly why it is
rem a probe and not the fix. Un-rem, relaunch, look, re-rem.
rem 2026-08-15 probe DISARMED. It was armed for the run where textures degraded
rem after ~30s idle, and it is a known LRU-breaker (re-keying the albedo hash on
rem every bind multiplied mesh creation 32x in an earlier session), so that result
rem is confounded. Re-run the idle test with it OFF to tell probe from real defect.
rem To arm: delete the 'rem ' from the line below.
rem set "RPCS3_REMIX_TEXREHASH=1"
rem
rem === THE HELMET THAT TURNED INTO TREE BARK (round 17 - the actual fix) ========
rem ROOT CAUSE, decided from the bytes rather than guessed. texture_descriptor::
rem key() identifies a texture by WHERE it is (offset, location, format, pitch,
rem dims, wrap, alpha state); content_hash identifies it by WHAT it is. Haze
rem streams into a recycled address pool, so bark decodes at address X, the helmet
rem is later written over X, and the helmet's bind produces the SAME key, HITS the
rem cache, and is handed the material whose albedo path is 0x<bark hash>.
rem Nothing ages it out - tex_destroyed=0 and reap_freed=0 over 18,269 flips - so
rem the swap is permanent for the session. Round 16 run: tex_stale_detected=134
rem over 63+ distinct keys, detected and NEVER acted on.
rem THE 'texdup' CENSUS IS THE OTHER DIRECTION AND IS NOT THIS BUG. Inverting the
rem FNV chain on the sample line (key_a=45949ff9170b194c, key_b=4593a0f9170967ff)
rem gives a round-5 pre-image XOR of exactly 0x101 - the two descriptors agree on
rem every field except the alpha-test word. Over all 1,793 texdup lines the lowest
rem differing bit is 0 in 82 cases (alpha state) and geometric from bit 7 upward in
rem the other 1,711 - the signature of two 128-byte-aligned OFFSETS. Same image at
rem two addresses. Content-benign: both entries hold identical pixels.
rem WHAT THIS KNOB DOES: when the existing cadence verify finds the bytes under a
rem stable key have changed, ERASE the entry so the next bind decodes what is
rem actually there. Round 8 left this undone because it assumed the fix had to
rem rebuild IN PLACE with content_hash pinned, and the pin is what makes destroying
rem the material unsafe. Erasing needs no pin. The old material is ORPHANED, not
rem destroyed, because meshes bake the handle and ~45 entries share a content hash.
rem SHIPPED OFF. It is a change on the hottest path in the backend and it has not
rem been play-tested; 0 is build 4b7bdb4 bit for bit.
rem TO ARM: delete the 'rem ' from the line below and relaunch.
rem WATCH FOR: the bark helmet becoming a helmet. Also watch for any surface
rem   flickering between two textures - that would mean the address is being
rem   rewritten faster than the 120-frame cadence and the entry is thrashing.
rem READ: tex_stale_evicted on the live line should track tex_stale_detected, and
rem   tex_stale_orphaned should EQUAL tex_stale_evicted. A gap means a material was
rem   destroyed while a mesh still had its handle baked in - stop and revert.
rem   Expected blast radius from the round-16 run: 134 entries in 18,269 flips.
rem REVERT: re-rem the line (or set 0). TEXVERIFY=0 also disables it entirely.
rem ARMED 2026-08-16 on your report: "saw rebel soldiers switch to bark textures
rem then walked away from them and came back and they were albedo".
rem That detail is the CONFIRMATION of the diagnosis, not just the symptom. The
rem cache key identifies a texture by WHERE it is, the albedo by WHAT it is.
rem Haze recycles addresses, so bark decodes at X, the soldier overwrites X, and
rem the soldier's bind produces the same key, HITS, and inherits bark's material.
rem Walking away destroys the mesh; coming back re-creates it and rebinds
rem correctly - which is exactly why it heals itself. A pure content bug would
rem not heal on re-entry.
rem THIS IS THE HOTTEST PATH IN THE BACKEND AND IT HAS NEVER BEEN RUN. Watch:
rem   tex_stale_orphaned MUST stay EQUAL to tex_stale_evicted. If they diverge,
rem   a live mesh lost its material - re-rem this line immediately.
rem   tex_stale_detected was 77 last session with evicted=0; evicted should now
rem   track it.
rem REVERT: re-rem this line. Fallback binary: bin\rpcs3-next-prev-round17.exe
set "RPCS3_REMIX_TEXSTALEEVICT=1"


rem =============================================================================
rem --- round 9 ---
rem cmd 'set' is last-wins, so everything below overrides anything above it.
rem Every line here is a DEFAULT restated: deleting the whole block changes
rem nothing. The comments say what each 0 gives back.
rem =============================================================================
rem
rem === THE EFFECTS FIX (the visible test for this round) =======================
rem The yellow nectar danger pulse's fragment program is ONE instruction:
rem   MOV r0.xyzw, COL0.xyzw
rem decoded from the cached ucode this round. Its colour IS the vertex colour,
rem which this backend already decodes and already submits - and the per-draw
rem blend extension was telling the runtime to take colour from the Texture
rem argument only, i.e. from round 6's neutral grey 2x2. That is the flat grey.
rem FPVCOL=0 restores the old parity fill for every draw, bit-exactly, and the
rem pulse must go back to grey with it. That A/B is the whole attribution.
set "RPCS3_REMIX_FPVCOL=1"
rem ============================ ROUND 41 - THE WHITE WALLS AND THE GREY SMOKE =======
rem RPCS3_REMIX_FPVCOLHOP lets the fragment classifier step BACKWARDS through identity temp
rem copies before it decides. Default 0 in code (round-40 behaviour bit-exactly); armed at 4
rem here so this run tests it.
rem WHY: MEASURED, every one of 192 "Remix alphastate:" rows reads tcolor=1/0/3 - "colour =
rem albedo texture, vertex colour DISCARDED" - while 22 of 47 vertex programs, the wall
rem program ad7ce9d672a0bf6b among them, carry a fully replayable route=scaled
rem slots=[c[18].x..w]. The vertex side is ready; the FRAGMENT classifier is the gate, and it
rem recognises exactly TWO shapes in the whole title (fpclass=1/1/0, fpvcol_applied=698 of
rem 5,783,237 draws = 0.012%%). If Haze paints a neutral base texture and carries the copper
rem tint per-vertex, "walls white not albedo" IS this, and so is the uncoloured lava smoke.
rem WHY IT IS SAFE: an identity copy is an unconditional MOV of a temp with identity swizzle
rem and no negate/abs - the identity function. Hopping one cannot admit a shape the
rem classifier did not already recognise; it only reaches programs that build the recognised
rem shape in a temp and export it. It CANNOT mis-classify.
rem WHY IT MAY NOT BE ENOUGH, pre-registered: a program doing real arithmetic between the
rem modulate and the export (a fog lerp, a specular add, a MAD) is not reached and stays
rem "other". THE READING: fpclass= on "Remix live:" must move off 1/1/0, and "Remix fpvcol:"
rem lines must show hops= greater than 0. If fpclass stays 1/1/0 the hop is not the gate -
rem and the NEW "Remix fpother:" census in bin\remix_dump.log then names the terminal
rem instruction of every unclassified program, which is the data the real widening needs.
rem WATCH ALSO: vcol_applied= should rise well above its 1.47%% of placed draws.
rem REVERT: set "RPCS3_REMIX_FPVCOLHOP=0"
set "RPCS3_REMIX_FPVCOLHOP=4"
rem The mesh-hash half, severable on its own: let the ATTR3 decode run on a
rem TEXTURED draw when its fragment program proves 'MUL out, <TEX>, COL0'.
rem Vertex colour is hashed into the mesh key, so an ANIMATED vertex colour makes
rem a new mesh per step - watch mesh_created on the live line. VCOLMOD=0 restores
rem the '!material' gate exactly and leaves FPVCOL's blend-state half alone.
rem 2026-08-15 TURNED OFF: the HUD gauges render BLACK with this on, even after the
rem BGRA pack fix (vcolbgra=1). The gauge quads are textured, so VCOLMOD multiplies
rem their texture by a vertex colour that is evidently near-zero - most likely those
rem meshes carry no COL0 attribute at all and the replay is feeding zeros. Off =
rem textured draws sample the texture only, which is how the gauges looked before
rem round 9. Set to 1 to reproduce.
rem ============================ ROUND 41 - TURNED BACK ON, WITH THE BLACK GUARD =====
rem VCOLMOD=0 is what made the FPVCOLHOP work above a NO-OP: fp_wants_vcol ANDs this knob
rem in, so with it at 0 no textured draw ever reaches apply_vertex_colour no matter what
rem the fragment classifier decides. Arming the hop without arming this would have shipped
rem a change that could not alter one pixel.
rem THE 2026-08-15 BLACK-HUD NOTE ABOVE IS NOW ROOT-CAUSED, not guessed. "Remix vcolroute:"
rem names exactly two constant-route programs on this title and one of them resolves to
rem   vp=b01bfce3fc580e3b route=constant cval=[0 0 0 1]
rem i.e. a constant vertex colour of PURE BLACK. On an untextured draw that constant IS the
rem colour and round 12 is right to replay it. On a TEXTURED draw it reaches Remix as a
rem Modulate factor, and a Modulate by zero cannot make a surface more correct - it deletes
rem it. That is the black gauge, exactly.
rem Round 41 adds RPCS3_REMIX_VCOLCONSTBLACK (default 1): refuse a constant-route colour
rem that resolves under 1/255 on a TEXTURED draw, leaving the albedo unmodified. The
rem untextured path is bit-exact. Counter vcol_const_black= on "Remix live:".
rem IF THE HUD GAUGES GO BLACK ANYWAY: set "RPCS3_REMIX_VCOLMOD=0" - one line, done - and
rem   report vcol_const_black=. If that counter is 0 the black came from somewhere else and
rem   the guard is aimed at the wrong mechanism, which is the pre-registered refutation.
rem WATCH: mesh_created= on the live line. Vertex colour is hashed into the mesh key, so an
rem   ANIMATED vertex colour mints a mesh per step. It was 582,659 with this off.
rem REVERT: set "RPCS3_REMIX_VCOLMOD=0"
set "RPCS3_REMIX_VCOLMOD=1"
rem ============ ROUND 42 - THE WHITE WALLS, FOUND IN THE UCODE ======================
rem RPCS3_REMIX_FPVCOLDEEP=1. Round 41's hop was the right idea aimed one instruction too
rem late. Haze's fragment programs DO modulate the albedo by the vertex colour - the
rem modulate is just THIRTY INSTRUCTIONS upstream of the export, behind the whole per-pixel
rem lighting composite, and a classifier that only reads the terminal instruction cannot
rem see it. MEASURED by disassembling the four Haze fragment programs whose raw ucode is on
rem disk in bin\remix_ucode (two of them - aa0fe222771ff5c0 and 65a91390aaf6bef3 - are
rem programs YOUR OWN Ctrl+Clicks landed on walls). Every one reads:
rem     MOV  H1,     ATTR1                ; COL0
rem     MUL  R1.xyz, H1, {2,0,0,0}.xxxx   ; the 0..2 lighting expansion
rem     MOV  R1.w,   H1                   ; alpha copied UNSCALED
rem     TEX  R0,     ATTR5, tex0          ; the albedo
rem     MUL  R1,     R0, R1               ; *** albedo x (COL0 x 2) ***
rem     ... 30 more instructions of normal map, specular, lighting composite ...
rem     MUL  R0.xyz, R0, {0.999,...}.xxxx  END   <- all the old classifier ever saw
rem That last near-identity scale is why 39 of the 64 "Remix fpother:" rows in the round-41
rem run share one terminal shape. Round 11 independently transcribed the same thing into a
rem source comment: "rgb = texRGB * (2 * COL0.rgb) * 0.944243".
rem WHY IT IS SAFE: the search is four terms, all required - an unconditional MUL writing
rem rgb, one operand a temp whose writer SAMPLED a texture, the other a clean COL0 read or a
rem temp reached from one through nothing but BROADCAST constant scales, and the product
rem provably live into COL0.rgb. COL1/ATTR2 is EXCLUDED by measurement, not caution: these
rem same programs carry the packed tangent-space NORMAL there.
rem PRE-REGISTERED, offline, before this build ever ran: the rule was reimplemented in
rem Python and run over all 338 stored .fp files - 22 match, 316 do not, and of the 8
rem programs on this run's own fpother census that have ucode on disk, 7 match and the
rem 8th is a 1-instruction program that samples nothing. Every match reports scale 2.0.
rem THE READING on "Remix live:":
rem   fpdeep=P/D    P = programs the deep search named, D = draws they account for.
rem                 EXPECT P around 20-40 and D in the MILLIONS. P>0 D=0 means the
rem                 programs classify but never reach a textured draw.
rem   fpclass=a/b/c b (the modulate program count) must jump; it was 2/1/0 last run.
rem   vcol_mod=     was 10,280 of 5,392,256 submitted (0.19%%). This is the number that
rem                 has to move by orders of magnitude.
rem   mesh_created= was 402,758. Vertex colour is in the mesh key, so expect ONE bounded
rem                 re-creation of every world mesh. If it climbs without limit, an
rem                 ANIMATED vertex colour is minting a mesh per step - report it.
rem PRE-REGISTERED REFUTATION: if surfaces come out visibly WRONG-COLOURED rather than
rem   merely darker, this is round 8's RETRYUNSUP repeating and the knob goes back to 0.
rem   "Darker" is expected and is the fix; "wrong hue" is the failure.
rem REVERT: set "RPCS3_REMIX_FPVCOLDEEP=0" - one line, restores round 41 bit-exactly.
set "RPCS3_REMIX_FPVCOLDEEP=1"
rem The brightness lever, and the ONE thing most likely to need a second pass.
rem Remix's Modulate factor is an 8-bit unorm, so the ucode's x2 cannot be represented
rem above 0.5 and the fold saturates there. Folding it is still the faithful reading: under
rem the x2 convention 0.5 is the UNLIT-NEUTRAL value, so replaying COL0 raw would land every
rem surface at half the guest's shading.
rem   MIND THE DIRECTION: 1 is the BRIGHTER setting and 0 is the DARKER one. The submitted
rem     factor is min(1, COL0 x 2) at 1 and plain COL0 at 0, and COL0 is at most 1, so BOTH
rem     settings can only darken relative to today's no-modulate factor of 1.0.
rem   STILL WHITE / washed out, or flat white patches where the x2 clips -> set
rem     "RPCS3_REMIX_FPVCOLDEEPSCALE=0" to replay COL0 raw, which halves the factor.
rem   TOO DARK everywhere -> you are already at the brightest this knob offers. The ceiling is
rem     the 8-bit unorm Modulate factor, not this knob; report it and revert FPVCOLDEEP.
rem Only ever applied to RGB: the ucode copies the alpha lane across UNSCALED ("MOV R1.w,
rem H1"), so folding it into alpha would invent an opacity the guest never computes.
set "RPCS3_REMIX_FPVCOLDEEPSCALE=1"
rem
rem === ROUND 43b: THE WHITE WALLS ARE A HEIGHT MAP BOUND AS THE ALBEDO ========
rem Round 42's vertex-colour work was correct AND could never have fixed these walls.
rem MEASURED on the round-42 build: every draw of all five hashes reports
rem class=vcol_modulate replay=1 with vcol0=FEFEFE - the replayed factor is NEUTRAL
rem WHITE, because the guest's COL0 byte is 0x7F, the unlit-neutral value in the 0..2
rem convention. Multiplying by 1.0 cannot darken anything.
rem
rem The fault is one texture unit up. From that run's stats line:
rem   tex_albedo_ucode = 0        tex_albedo_guess = 7,383,877
rem The ucode albedo discriminator did not resolve ONE draw in 41,802 flips. It
rem declines whenever colour_mask == referenced and the backend then takes the LOWEST
rem referenced unit. On fp=65a91390aaf6bef3 - the program on the picked white wall
rem 9AAA430414B3D49D - that is tex0, a PARALLAX HEIGHT MAP sampled into ONE channel
rem and used only to perturb a UV. The real diffuse is tex1, sampled into all four:
rem
rem    9: TEX R2.x   TEX2, tex0     one channel   - elected today
rem   18: ADD R4.xy  TEX2, R2                     - used as a UV perturbation
rem   24: TEX R2     R4,   tex1     four channels - the real diffuse
rem
rem A near-white greyscale height map bound as albedo IS a white wall.
rem
rem ARMED: drop units that CANNOT carry RGB - every sample of them writes fewer than
rem three destination channels. Structural, not a heuristic: the channels are not there.
rem
rem *** THIS REPLACES RPCS3_REMIX_FPALBEDOKILL, WHICH IS GONE FROM THE BUILD. *** That
rem knob reached the same answer on this wall but re-elected 36 programs, 16 of them onto
rem a unit 8 or higher, and the play-test came back as full-screen coloured static at 32
rem fps. It also set unit_from_ucode, which silently unblocked a retry walk that had never
rem once been able to run on this title - two large changes at once. This rule does neither.
rem
rem PRE-REGISTERED OFFLINE over all 338 .fp files in bin\remix_ucode\ BEFORE the build:
rem   323 sampled programs; 202 saturate today; this rule narrows 178 of them, but
rem   changes the ELECTED UNIT of only THREE - and all three go unit 0 to unit 1.
rem   NONE elects a unit above 1.   (the kill: 36 changed, 16 of them to unit 8+)
rem   fp=65a91390aaf6bef3: mask 0x1f to 0x16, unit 0 to 1 - identical to the kill's
rem   answer on the one case that was actually verified.
rem
rem CONTAINED TWICE in albedo_unit_mask(): the narrowed mask must name a unit the backend
rem can BIND, and it must elect a DIFFERENT unit from the one taken anyway - otherwise
rem 'referenced' is returned untouched. So the 175 programs that narrow without moving
rem their election are bit-exact, and the retry walk is never confined for them.
rem
rem READ ON 'Remix live:' / 'Remix stats:'
rem   tex_albedo_narrow above 0        - it moved that many draws. It is a subset of
rem                                      tex_albedo_GUESS, not of tex_albedo_ucode:
rem                                      unit_from_ucode is deliberately NOT set.
rem   tex_albedo_narrow = 0            - it moved nothing. Check fpalbedonarrow=1 on the
rem                                      banner before assuming it is broken.
rem   tex_none / notex_mat_applied climbing hard - surfaces losing texture. Revert.
rem   Ctrl+Click a wall: 'Remix picked:' albedo_unit= should read 1, not 0.
rem
rem PRE-REGISTERED REFUTATION: full-screen noise like last time, or an obviously wrong
rem image (a normal map's blue, a lightmap), means the re-election is still wrong even at
rem three programs. Report which surface and revert.
rem
rem REVERT: set "RPCS3_REMIX_FPALBEDONARROW=0" - one line, round 42 bit-exactly.
set "RPCS3_REMIX_FPALBEDONARROW=1"
rem
rem === THE SELVA CANOPY ========================================================
rem A third matcher pass, tried LAST, for a fused group whose row scalars read
rem different temps/lanes but each chase back through additive hops to one base
rem temp with lanes {x,y,z}. Named from a full 47-slot decode of DF46F03B1B7AB8A4.
rem The additive terms it steps over are the WIND, so the canopy replays STATIC -
rem that is the designed trade, not a bug. Offline sweep over all 64 cached .vp:
rem exactly one program flips, zero regress. 0 restores the refusal bit-exactly
rem and is the bisect if tree canopies land in the wrong place.
set "RPCS3_REMIX_MADCHAINMIX=1"
rem
rem === THE SELVA TREE TOPS (round 17) ==========================================
rem MADCHAINMIX above assumes the per-vertex divide put x,y,z into LANES x,y,z,
rem because DF46F03B1B7AB8A4 does. UCODESTORE finally captured the two canopy
rem programs and neither does:
rem   f7f12d5d15bb9c37  18: MUL r4.xyw = v0.xyxz * r4.wwww   (z parked in lane w)
rem   c1f88035801f88de  14: MUL r0.xzw = v0.xxyz * r1.yyyy   (y lane z, z lane w)
rem Both then build HPOS = c0*x + c1*y + c2*z + c3 exactly like every other world
rem matrix, so the ONLY thing refusing them was the lane assumption. This arm
rem reads the permutation out of the divide instruction's own writemask and
rem swizzle - it proves the mapping rather than relaxing the rule - and lets the
rem same permuted divide satisfy the wdivide step, without which the rescued mesh
rem would replay the raw quantised attribute and render blown apart.
rem OFFLINE SWEEP over all 111 cached .vp (raw cache + remix_ucode): 5 programs
rem move, every one of them currently refused with groups=0, so nothing that
rem renders today can change - f7f12d5d15bb9c37, c1f88035801f88de,
rem 61ed1d272c0653aa, 1d22398b18a0e97d, 5888b152531b2d91. Gauges, sky dome, haze
rem card, effects, flower, first-person and round 9's own canopy are untouched.
rem f7f12d5d15bb9c37 has NO sway at all, so it replays exactly; the other four
rem carry SIN/COS wind that the chase drops, so they replay STATIC - the same
rem designed trade rounds 9 and 10 took.
rem WATCH FOR: the top halves of the Selva trees appearing. Also watch that
rem   nothing new is stretched or exploded - that would mean a rescued group got
rem   its matrix without its divide.
rem READ: 'Remix madlanemap:' in bin\remix_dump.log - one line per program, and
rem   wdiv=1 on every one of them is the field that says the divide was accepted.
rem   Live line: madlanemap=<programs>/<draws>. Banner: madlanemap=1.
rem REVERT: 0. That restores round 9's identity-lane rule bit-exactly.
set "RPCS3_REMIX_MADLANEMAP=1"
rem
rem === THE UCODE CAPTURE (infrastructure - read this once) =====================
rem bin\cache\...\shaders_cache\raw is written ONLY when the GL/Vulkan backend
rem compiles a pipeline. The Remix backend never does, so its newest files are
rem from Aug 10 while Remix sessions have run daily - which is why the giant
rem flower's program and the Selva canopy pair have never been readable offline.
rem This writes the raw ucode of every REFUSED vertex program to bin\remix_ucode\
rem once per program per run. No behaviour change; worst case is dead files.
rem After a session, that directory is the input for the next round's decode.
set "RPCS3_REMIX_UCODESTORE=1"
rem
rem === THE IDLE DEGRADATION (stand still ~30 s, room goes flat white) ==========
rem reap() used to destroy an idle texture's MATERIAL without asking whether a
rem live mesh still referenced it - and the mesh cache bakes the material handle
rem in at CreateMesh while keying the mesh on its CONTENT, so an unchanged mesh
rem is reused and keeps the DESTROYED handle forever. Nothing can heal that: no
rem code path re-points a live mesh's material. Permanent white, and on handle
rem reuse a WRONG texture - which is the first mechanism that explains the idle
rem bug and the bark-soldier family together.
rem TEXREAPSAFE=0 restores the old reap exactly. It is the CONTROL: if the
rem degradation comes back with 0 and not with 1, this was the mechanism.
set "RPCS3_REMIX_TEXREAPSAFE=1"
rem
rem CORRECTION TO THE ROUND-9 PLAN, from this launcher's own lines.
rem The plan's anatomy argued the reaper was the prime suspect because "no
rem TEXIDLE line exists, so the config default of 300 frames applies" - 6-10 s at
rem this title's frame rates, the right order for a ~30 s onset. That is NOT what
rem this launcher does: TEXIDLE=36000 is set above (raised from 3600 by the
rem idle-reaper experiment), i.e. 12-20 MINUTES. At 3600 it was still 72-120 s.
rem So under BOTH values the TEXIDLE reaper is arithmetically the wrong order for
rem a 30-second onset, and the threshold experiment has in effect already been
rem run - twice - with the degradation surviving it.
rem
rem What follows from that, and what to read instead:
rem   * Expect reap_kept=0 and reap_freed=0 on the live line with TEXIDLE=36000.
rem     That is the CORRECT reading, not a broken fix: nothing goes idle inside a
rem     normal session, so the reap-safety fix has nothing to do. It still closes
rem     a real lifetime hole for long sessions and for the default 300.
rem   * The discriminating evidence is now the tex= group on 'Remix live:':
rem       tex_live dropping at the cliff        -> something IS evicting
rem       tex_created + tex_deferred climbing   -> the per-frame budget loop
rem       ALL of them flat while the screen goes -> the defect is downstream of
rem         this cache, in the runtime, and round 10 starts there
rem     Write down those five numbers just before the cliff and just after.
rem   * Only if tex_live DOES drop is the threshold worth pushing further:
rem     un-rem the line below (999999 ~ never), relaunch, repeat, re-rem.
rem set "RPCS3_REMIX_TEXIDLE=999999"
rem
rem === THE DUMP WINDOW (one relaunch, then turn it back off) ===================
rem DUMP=1 prints 'Remix vp=... slice:' - the full 96-instruction position chain -
rem for every program whose innermost operand is not an attribute, which is all
rem four members of the giant-flower family. Walk to the giant flower (and to
rem Selva if convenient), then copy every 'Remix vp=' line for
rem   c97cd1531ac480d8  c6eae522652f742e  5a278d19120f3e30  392df4233aaefbd1
rem out of bin\remix_dump.log. Costs frame time - one window, then re-rem it.
rem (UCODESTORE above gets the same bytes with no frame cost, so this is the
rem belt-and-braces route, not the only one.)
rem set "RPCS3_REMIX_DUMP=1"
rem
rem === THE FIRST-PERSON HELMET - one click, no rebuild =========================
rem The helmet is deliberately ~10 feet in front of the player: it is a HUD
rem element drawn to look like a visor, and on real hardware it does not clip
rem world geometry. So the right handling is to TAG it as a viewmodel, not to
rem "correct" its scale. The machinery is complete and has never been used
rem because nobody has pasted a hash. Ctrl+Click the helmet, take vp= off the
rem 'Remix picked:' line, paste it below (up to 8, comma-separated), relaunch.
rem Then read on 'Remix live:': vm_tagged_hash should climb, and vmcam_applied
rem should climb while vmcam_refused / vm_hash_anchor_refused stay put.
rem set "RPCS3_REMIX_VIEWMODELVP=<paste vp here>"

rem --- round 10 ---------------------------------------------------------------
rem Appended, never edited above. cmd 'set' is last-wins, so anything here that
rem repeated an earlier name would silently override it - nothing here does.
rem All seven active lines are the shipped DEFAULTS written out explicitly, so
rem the bisect is editing a value in front of you rather than adding a line.
rem
rem === LEAD 2 - THE EFFECTS FAMILY (read this one first) ======================
rem VCOLBGRA: the HUD gauges stop being blue. apply_vertex_colour packed
rem R,G,B,A while the runtime binds that field as VK_FORMAT_B8G8R8A8_UNORM -
rem bytes B,G,R,A - so red and blue were swapped in EVERY replayed vertex
rem colour. Amber (R >> B) read back as cyan. That is the whole bug.
rem EXPECTED, NOT A DEFECT: mesh content hashes cover these bytes, so the first
rem run after this build re-creates every coloured mesh ONCE. mesh_created will
rem bump early and then settle. Sustained growth is something else.
rem 0 restores the blue (the attribution A/B).
set "RPCS3_REMIX_VCOLBGRA=1"
rem
rem FPVCOLALPHAGATE: closes a ROUND-9 REGRESSION. Round 9 replayed vertex alpha
rem into opacity even on draws where the guest reads no fragment alpha at all
rem (blend off AND alpha test off). For the Selva flower rows - measured
rem 'blend=0 dw=1 vcol=[alpha=00..00]' - that made opacity 0, and the runtime's
rem blending-disabled arm turns 0 into "invisible". Opaque vegetation with
rem 0-alpha vertex colours has been INVISIBLE since the round-9 build.
rem Watch fpvcol_alpha_skip climb. If something that WAS correctly transparent
rem turns solid, 0 is the sever.
set "RPCS3_REMIX_FPVCOLALPHAGATE=1"
rem
rem FPVCOLEMISSIVE: the nectar pulse stops being grey. It was never
rem mis-coloured after round 9 - it was UNLIT. Albedo is reflectance, and in a
rem dark room reflectance reads grey. The fork applies the SAME fixed-function
rem stage a second time to emissiveColor, so the arg sources round 9 already
rem ships reach emissive radiance the moment a material carries an intensity.
rem This is that material. Value is the intensity; 0 = off (back to grey).
set "RPCS3_REMIX_FPVCOLEMISSIVE=1.0"
rem
rem FPVCOLADDITIVE: the same effects, composited rather than solid. ONE/ONE ADD
rem is classified BlendType::kEmissive by the runtime - the surface occludes
rem nothing and the black parts of the gradient vanish for free. Interpretive
rem (the PS3 composites this pass offscreen; we approximate it in world), so it
rem is its own knob. Bisect order if the pulse looks wrong:
rem   FPVCOLADDITIVE=0  -> rings solid but correctly coloured
rem   FPVCOLEMISSIVE=0  -> back to dark/grey albedo
rem   FPVCOL=0          -> round-8 grey
set "RPCS3_REMIX_FPVCOLADDITIVE=1"
rem
rem === LEAD 1 - THE CAMERA, AT BOTH ELECTION SITES ============================
rem CAMCLIPGATE: the "everything very far away, like looking down into another
rem portal" frames, made structurally impossible. The LOCK PROGRAM ITSELF draws
rem this title's 2048x2048 shadow pass, so the shadow variant was a legitimate
rem PRIMARY candidate - caught winning at frame 23022 with candidates=17 and the
rem camera 30 units above the player, held ~1400 frames. Same same/double/half
rem rule the world-draw path already trusts, applied to the vote.
rem Watch cam_clipgate_refused climb and 'Remix cam-clipgate:' name
rem clip=2048x2048. 0 brings the portal frames back (the attribution).
set "RPCS3_REMIX_CAMCLIPGATE=1"
rem
rem CAMFBRELATCH: the dev-menu camera-type flicker. The lock program stops
rem drawing for stretches near the vertical view, the fallback wins, and because
rem the two draw on different surfaces every handover read as a full identity
rem change. Now the identity stays the lock and only the MATRICES come from the
rem candidate that actually drew. The visible test is 'Remix cam-elect:' GOING
rem QUIET while cam_fb_relatch climbs. 0 restores the flicker.
set "RPCS3_REMIX_CAMFBRELATCH=1"
rem
rem ANCHORSTICKY: Selva's "geometry follows the camera" and the land carrier
rem moving in a different frame from its cargo. The gauge anchor was
rem first-draw-wins; on frames where the good identity-world donor (BD1C) is
rem culled, AD7C's NON-identity draw seized the slot and the whole frame divided
rem by a prop-relative frame. Now a first donor that disagrees with the key's
rem own previous frame is PARKED, a continuous one installs, and anything still
rem parked at flip is promoted - so a real cut converges in one frame.
rem Steady play should read anchor_parked climbing with anchor_promoted ~0, and
rem 'Remix anchor-elect:' lines only at real scene cuts.
rem 0 restores first-draw-wins - and the follow-the-camera with it.
set "RPCS3_REMIX_ANCHORSTICKY=1"
rem
rem === SECONDARY - THE VISOR ==================================================
rem VMANCHORGEO: the repair that makes VIEWMODELANCHOR usable at all. The guard
rem was measuring the draw's instance ORIGIN; the visor draws carry identity-ish
rem transforms with WORLD-SPACE vertices, so it measured ~2100 units to a point
rem no geometry occupies and refused all 54,389 matched draws. It now measures
rem the geometry, which is what the picks were reporting all along (~1.5 units).
rem Your pinned VIEWMODELANCHOR=4 becomes CORRECT with this on.
rem vm_anchor_unmeasured on the live line must stay 0.
set "RPCS3_REMIX_VMANCHORGEO=1"
rem
rem VMALBEDOCAM: submits the VIEW_MODEL camera the runtime requires before the
rem VIEW_MODEL tag does anything at all.
rem ROUND 21 FIXED THE GATE. Up to round 20 this fired ONLY when the albedo list
rem below was armed - and that list is deliberately BLANK on this title, so the
rem camera was never submitted and every tag was inert. MEASURED in the round-20
rem run: vm_tagged_pair=123 with vmcam_twin=0 vmcam_real=0. The runtime's
rem createViewModelInstances returns early on !isCameraValid(ViewModel)
rem (rtx_instance_manager.cpp:1504), so those 123 draws rendered as ordinary
rem world geometry - with the VMBASIS flip still applied, because that runs at
rem the tag site, not the camera site.
rem It now fires when ANY route that can set the tag is armed: VIEWMODELALBEDO,
rem the VMPAIRVP+VMPAIRALBEDO pair, VIEWMODELVP, or a raised VMDEPTHOFFSET.
rem CONFIRM: vmcam_twin on 'Remix live:' must now be NON-ZERO.
rem THIS IS THE ROUND'S ONE BEHAVIOUR CHANGE. Revert it with =0 if anything that
rem rendered in round 20 stops rendering - a VIEW_MODEL-tagged instance gets
rem mask=0 and leaves the world pass once the camera is valid, so this knob is
rem what decides whether the 123 tagged draws are in the world or in the
rem viewmodel pass.
set "RPCS3_REMIX_VMALBEDOCAM=1"
rem
rem === THE GIANT FLOWER ======================================================
rem MADACCUM: the Selva flower that renders at extent 2956-6383. Its position
rem decode was hidden behind three things at once - a billboard-offset
rem decoration on top, a per-lane assembly beneath it, and a scale operand that
rem reached its MAD through a MOV. All three are now crossed, and the rebuilt
rem decode is pos = ATTR0 * c[467].y + c[61].xyz.
rem Ctrl+Click the flower: areason should no longer read 'mad-src0-not-input',
rem extent should collapse to sane single/double digits, and madaccum should
rem read 1/<draws> on the live line with a 'Remix madaccum:' line in the dump.
rem BY DESIGN it is now STATIC - no sway, no billboarding. That is the same
rem trade round 9 made for the tree canopy: the decorations are dropped, not
rem folded. 0 restores the giant/refused state.
rem NOTE the plan for this round predicted a simpler fix and the offline sweep
rem refuted it; what shipped is what the bytes proved. Four MORE programs look
rem rescuable by a wider rule and are deliberately NOT shipped - that rule would
rem have DELETED their geometry downstream. Round 11 has the design.
set "RPCS3_REMIX_MADACCUM=1"
rem
rem === ONE-CLICK ITEM: ARM THE VISOR (optional, your call) ====================
rem The visor CANNOT be tagged by program: vp=830d7d1b9681c475 is also
rem GUESTLIGHTVP, so tagging the program drags the ceiling lights into the
rem viewmodel camera and undoes round 5's lighting. These are its two ALBEDO
rem hashes (opaque shell vtx=1446 ext=3.23, blended layer vtx=59 ext=7.46, both
rem ~1.2 units in front of the eye). Un-rem the line and relaunch to arm it.
rem Read after: vm_tagged_albedo climbing, vmcam_twin climbing, and
rem vm_hash_anchor_refused NO LONGER equal to vmcam_applied.
rem The gauges must NOT vanish - none of their albedos are in this list.
rem ARMED 2026-08-15 on your "fix the weapon model" ask. VMANCHORGEO=1 and
rem VMALBEDOCAM=1 above are the two prerequisites and both are already on, so
rem this line is the last one that was missing.
rem NARROWED 2026-08-16 - THIS PIN WAS HIDING YOUR SMOKE.
rem C61753D31FB96507 is drawn by FOUR programs, not one, and round 19's census
rem (deduped per program, so its 4 lines ARE the complete list) named them:
rem   830d7d1b9681c475   the real visor / arms
rem   9f591b6a6b825612   the head-locked lens-flare card
rem   af06f6d32ec048ee   THE SKY DOME
rem   f39f504649b6f442   THE EFFECTS FAMILY - the missiles and smoke
rem The runtime sets a VIEW_MODEL-tagged instance's mask to 0 and removes it
rem from the world pass. So tagging that albedo did not just tag the visor - it
rem DELETED the smoke and the sky dome from the world. Round 18's PROJSPLIT was
rem placing those draws correctly all along (residue 3.21904 -> 3.47e-08, and
rem 'Remix fxref:' fell from 744 lines to 0); they were then masked out by this
rem line. Two fixes in series, and only the second one was visible.
rem 1BF8325ADEF3C986 alone still tags the opaque visor/arms body (vtx=1446).
rem What is dropped is the 59-vertex blended layer, which we have since measured
rem to be the head-locked flare card, NOT part of the visor.
rem WATCH: smoke/explosions appearing in the Selva opening, and vm_tagged_albedo
rem FALLING sharply (it was 18611 - most of that was not the viewmodel at all).
rem REVERT: put ,C61753D31FB96507 back on the end of the line below.
rem !!! BLANKED 2026-08-16 - BOTH VISOR ALBEDOS ARE SHARED. MEASURED. !!!
rem Narrowing to 1BF8325ADEF3C986 did NOT stop the over-match. The vmbasis
rem census, which prints one line per program, shows that albedo tagged
rem byalbedo=1 on THREE programs at vtx=1446 each:
rem   vp=830d7d1b9681c475   the real viewmodel
rem   vp=af06f6d32ec048ee   the sky-dome program
rem   vp=f39f504649b6f442   THE EFFECTS FAMILY - the missiles and smoke
rem The runtime sets a VIEW_MODEL-tagged instance's mask to 0 and drops it from
rem the world pass, so this pin has been DELETING the effects family. That is
rem why round 18's PROJSPLIT placed them (residue 3.21904 -> 3.47e-08, fxref
rem 744 -> 0 lines) and you still saw nothing.
rem Both visor albedos are shared across the same three programs, so ALBEDO
rem ALONE CANNOT IDENTIFY THE VIEWMODEL ON THIS TITLE - the same lesson the sun
rem card taught. Blanked so the world stops losing geometry; the viewmodel was
rem rendering mis-oriented anyway, so nothing working is being given up.
rem The replacement is a (vp, albedo) PAIR gate, the way SUNCARDVP works, using
rem vp=830d7d1b9681c475 - which must NOT be tagged program-wide, because it is
rem also GUESTLIGHTVP and that undoes round 5's lighting.
rem WATCH: smoke/explosions in the Selva opening, vm_tagged_albedo falling to 0.
rem REVERT: put 1BF8325ADEF3C986 back on this line.
set "RPCS3_REMIX_VIEWMODELALBEDO="
rem
rem --- WEAPON CANDIDATES, deliberately NOT armed yet --------------------------
rem I inventoried every albedo vp=830d7d1b9681c475 has ever drawn across the
rem logged runs. It is NOT a first-person-only program - it also draws whole
rem soldiers (5A55210D7739C716 vtx=24227 ext=11.42, 0DA919D97B091B6E vtx=9758
rem ext=11.34). Against an 11-unit-tall soldier, a 2-3 unit item with a few
rem hundred verts is weapon-sized, and three of those exist:
rem   38C858E6DC48E488  vtx=278  ext=2.30..2.72
rem   3B93DE13339598E0  vtx=314  ext=2.19..2.25
rem   2FF44C92754B151E  vtx=278  ext=2.478
rem I am NOT adding them blind: I cannot yet tell the PLAYER's weapon from an
rem NPC's, and tagging an NPC weapon would fly it to the viewmodel camera and
rem stick it to your face. The discriminator is distance-to-eye, which needs one
rem measurement (round 13) or one Ctrl+Click on your own weapon if it renders.
rem To try one anyway, append it to the VIEWMODELALBEDO line above - ONE at a
rem time, so the result is attributable.
rem Revert everything here with: set "RPCS3_REMIX_VIEWMODELALBEDO="
rem
rem === PASTE-READY: THE HUD GAUGES, SELF-LIT (your list, your call) ===========
rem The gauges are TEXTURED draws with shared per-texture materials, so the
rem self-lit material above cannot reach them - the lever for them is the
rem EMISSIVE albedo list, applied at material creation. That is what stops them
rem being re-lit by the path tracer every frame, i.e. the flicker.
rem RPCS3_REMIX_EMISSIVE is YOUR list and this would append to it, so it ships
rem commented: copy your existing EMISSIVE line, append these seven, relaunch.
rem The list caps at 16 and you currently use three, so all seven fit.
rem   099FCDACD6FDA024 A47E2136CC3785B3 EF2A8E2D0547AD1A D4EC78903E48A162
rem   4B1F4BA19202CFCB D6FB4AC0556FE7E4 4D3334785D8A7E44
rem WATCH: an emissive albedo glows EVERYWHERE its texture appears. If a non-HUD
rem surface starts glowing, take that one hash back off the list.
rem set "RPCS3_REMIX_EMISSIVE=<your three>,099FCDACD6FDA024,A47E2136CC3785B3,EF2A8E2D0547AD1A,D4EC78903E48A162,4B1F4BA19202CFCB,D6FB4AC0556FE7E4,4D3334785D8A7E44"

rem --- round 11 ---
rem
rem === THE BLACK BACKDROP PLANE - IT WAS NEVER MISPLACED ======================
rem HAZEFADE: the black wall standing in the jungle is an atmospheric haze /
rem god-ray card, and it is EXACTLY where the game put it. pick-deep proved that
rem three ways: its fused matrix equals the live camera's V x P to ~1e-5, its
rem vertices stand in world space at z~35 while the camera is at z~-42 (the
rem "at the world origin" reading was the pick's origin= field, which is the
rem INSTANCE TRANSFORM's translation - identity, so zero - not the geometry's
rem place), and its w-divide is a no-op. What was missing is the FADE.
rem Its whole translucency lives in fragment-program maths this backend never
rem replayed, on a texture whose alpha channel is fully opaque (alpha_range
rem 255..255), so it arrived at alpha 255 under SRC_ALPHA/ONE_MINUS_SRC_ALPHA -
rem an opaque plane, path-lit in a dark jungle.
rem Decoded from the two cached programs and now replayed per vertex:
rem   COL0  = ATTR3 * c[18]
rem   rgb   = COL0.rgb * 2 * 0.944243
rem   alpha = COL0.a * dist_ramp * angle_ramp, angle from a per-vertex
rem           quaternion in ATTR9 rotated into view space
rem NOTE for this program the DISTANCE ramp's own constants are 0/0, so it is
rem identically 1 and the ANGLE ramp is the entire fade. Both are implemented.
rem WATCH: hazefade=<n> climbing on the live line, and 'Remix hazefade:' in the
rem dump with a_range= that is NOT 255..255 and that MOVES as you walk and turn.
rem 0 restores the opaque black wall - that A/B is the whole attribution.
set "RPCS3_REMIX_HAZEFADE=1"
rem
rem === THE VERTEX-COLOUR ROUTE - WHY VCOLMOD=1 WENT BLACK =====================
rem FPVCOLROUTE: rounds 9/10 replayed the mesh's ATTR3 as the vertex colour
rem whenever the FRAGMENT program named COL0. But COL0 is not an attribute - it
rem is the VERTEX program's output o1, and the offline sweep over all 82 cached
rem programs says the two are only the same thing for 16 of them:
rem   passthrough 16   scaled 22 (all c[18])   scaled_chain 1   computed 36   none 7
rem The giant flower is the proof case: its rgb IS a scaled chain of ATTR3, but
rem its ALPHA is a computed distance ramp that never touches the mesh attribute
rem at all - and replaying the mesh's 00 alpha there is what made the foliage
rem invisible. No consumption gate can repair a wrong source.
rem Now: passthrough replays as before, scaled/scaled_chain replay with the
rem ucode's own constants folded, computed/none do not replay (white, i.e. the
rem pre-round-9 look), and every ALPHA half additionally needs the route to
rem prove ATTR3.w reaches COL0.w.
rem WATCH: 'Remix vcolroute:' lines in the dump, vcol_route_blocked and
rem vcol_fold on the live line. 0 restores round 10's route-blind replay.
set "RPCS3_REMIX_FPVCOLROUTE=1"
rem VCOLFOLD severs only the constant fold, so "is the route right" and "is the
rem fold right" are separately answerable. 0 = scaled routes replay the RAW
rem attribute (round 10's value) while still being route-gated.
set "RPCS3_REMIX_VCOLFOLD=1"
rem
rem === RE-TEST VCOLMOD (your call, ships commented) ===========================
rem Your VCOLMOD=0 line above is NOT touched. With the route gate live the
rem mechanism that made foliage invisible is structurally closed, so flipping
rem this on for ONE session is now a safe experiment.
rem READ IT LIKE THIS:
rem   foliage and the giant flower VISIBLE, vegetation tinted  -> the fix landed
rem   gauges AMBER                                             -> done, leave it on
rem   gauges still BLACK -> ONE Ctrl+Click on a black gauge and read pick-deep's
rem     new attr3= block:
rem       first=[0 0 0 1] with a sane place/type/size  = the data really IS
rem         (0,0,0,255): a dark depletion/backing layer rendering faithfully
rem         over the amber bars, and the amber comes from albedos the dedupe
rem         never censused. Report it and stand down - nothing to fix.
rem       nonsense place/type/stride                   = a decode misread, and
rem         the printed shape is the repro for round 12.
rem   anything you liked before turns invisible -> VCOLFOLD=0 first, then
rem     FPVCOLROUTE=0, then put VCOLMOD back to 0.
rem set "RPCS3_REMIX_VCOLMOD=1"
rem
rem === THE SKY MAY NEVER GO EMISSIVE =========================================
rem FPVCOLSKYGATE: round 10's self-lit verdict excluded "the flat dome variant"
rem on colour variance and calibrated that on the WRONG variant. The 32-vertex
rem dome is the flat one; the 82-vertex one carries the horizon gradient and
rem passes every term - its own census rows print selflit=1 additive=1. It only
rem escaped becoming an emissive ONE/ONE additive shell because it draws at the
rem MENU, where there is no camera and the world gate refuses it first. With
rem rtx.skyMode=0 the rasterized sky is the visible sky, so the first open-sky
rem visit with a camera would have made it a light the size of the level.
rem Now excluded by MEASUREMENT: extent >= 2000 (the dome spans 10000; no
rem room-scale effect approaches it).
rem WATCH: fpvcol_skygate climbing in open sky = the gate doing its job.
rem 0 is the regression repro. Do not leave it off.
set "RPCS3_REMIX_FPVCOLSKYGATE=1"
rem
rem === INSTRUMENTS (read-only, no behaviour change) ===========================
rem FXREFPROBE: the Selva smoke/explosions are re-attributed. They are NOT the
rem emissive family - no new untextured vcol_pass pair ever appears in the
rem census. They are a refused TEXTURED effects family (f39f504649b6f442 and
rem siblings) whose refusals read persp_residue ~3.71 against a 0.02 tolerance
rem against every reference tried. This probe divides those draws against every
rem live gauge slot and prints the best three, which decides between "the wrong
rem reference was selected" and "these draws carry their own projection".
rem NOTHING IS FIXED FOR THE SMOKE THIS ROUND - this measures which fix is next.
rem WATCH: 'Remix fxref:' lines in the dump during/after the Selva opening.
set "RPCS3_REMIX_FXREFPROBE=1"
rem
rem --- ROUND 18: the fxref census was SATURATED, and 641d6432 is not an effect -
rem MEASURED: the census emitted EXACTLY its 8-line cap in every window it spoke
rem in (744 lines / 93 windows = 8.00 in the round-17 run), so the "320 fxref
rem lines" were never a sample of the family - they are the first eight refusals
rem of one frame per window, repeated. Two of those eight are not effects:
rem   641d6432efd6add4  a SIX-INSTRUCTION two-tap blur quad on the 512x288
rem     half-res buffer (o7.xy = v8.xy - c67.xy ; o7.zw = v8.xy + c67.xy).
rem     Its residue is bit-identical 0.999988 in every frame of every run
rem     because it equals 1/Q of the projection and depends on nothing in the
rem     scene. It is a post-process pass. It is correctly refused. Do NOT give
rem     it a camera - that would put a screen-covering quad into the world.
rem   af06f6d32ec048ee  a 4-vertex sky backdrop quad on surface 01120000.
rem FXREFVP restricts the census to the family actually under investigation.
rem EMPTY restores the round-17 census exactly.
rem WATCH: 'Remix fxref:' lines now all carry vp=f39f504649b6f442, and each one
rem carries world=[...] (the recovered matrix) which the probe never printed.
set "RPCS3_REMIX_FXREFVP=f39f504649b6f442"
rem FXREFMAX: lines per 120-frame stats window. 8 was the hardcoded round-11
rem constant and is what saturated. Clamped to 64 in code.
rem REVERT: 8 restores the round-17 cap.
set "RPCS3_REMIX_FXREFMAX=32"
rem
rem === PROJSPLIT - THE SELVA EFFECTS FIX (round 18, NOT play-tested) =========
rem THE ITEM: "there is suppose to be smoke and explosions going on in the
rem beginning but isn't rendered yet", open since round 4.
rem
rem WHY THIS IS THE FIX AND NOT ANOTHER GUESS. The refusal residue
rem |m03|+|m13|+|m23|+|m33-1| of world = fused * reference_inverse is
rem structurally BLIND to any difference in the VIEW: for reference = V_a*P and
rem fused = W*V_d*P the product is W*V_d*P*P^-1*V_a^-1 = W*V_d*V_a^-1, which is
rem affine for any two rigid views, so its residue is 0. A non-zero residue
rem therefore means ONE thing - the draw's PROJECTION is not the reference's.
rem MEASURED for vp=f39f504649b6f442 over three sessions: residue 3.21949 as the
rem first sampled refusal of ALL THREE runs, median 3.409 (p10 3.386, p90 3.763)
rem over 226 samples at unrelated camera positions. A camera-lag or wrong-anchor
rem defect cannot hold a residue that still while the player walks and turns.
rem A second projection can, and round 8's own split=1 ticket has been printing
rem on every one of these refusals ever since it was added.
rem
rem WHAT IT DOES: at the tail-rescue FAILED exit ONLY - the exit where the draw
rem is dropped today - it rebuilds the reference as
rem   cross = split(anchor_fused).view * split(draw_fused).projection = V * P_d
rem   world = fused * cross^-1 = W*V*P_d * P_d^-1*V^-1 = W
rem and re-runs the SAME is_affine gate every other path passes.
rem
rem BLAST RADIUS: it cannot move anything that renders today, because it only
rem runs where the draw is already being dropped. A wrong premise shows up as
rem proj_split_refused climbing, not as garbage in the scene.
rem ACCEPTANCE: proj_split=<applied>/<refused>/<nosplit> on the 'Remix live:'
rem line. applied climbing during the Selva opening IS the fix landing.
rem REVERT: 0 restores the round-17 drop bit-exactly.
set "RPCS3_REMIX_PROJSPLIT=1"
rem PROJSPLITERR: max split reconstruction L1 error x1000, as AFFINETOL is
rem scaled. 50 = 0.05. Lower is stricter. A split whose error is large means the
rem "view" it synthesised is not the draw's, and the cross would be meaningless.
set "RPCS3_REMIX_PROJSPLITERR=50"
rem UCODESTOREFP: writes the raw fragment ucode of untextured programs the
rem classifier cannot name into bin\remix_ucode\<hash>.fp, so round 12 decodes
rem the flat-yellow sun card offline instead of clicking at it.
rem WATCH: ucode_fp=<n>/<fails> on the live line; new .fp files in remix_ucode.
set "RPCS3_REMIX_UCODESTOREFP=1"
rem
rem === round 11 play-test finding - THE SKY WAS BLOCKING THE SUN ==============
rem SKYANCHOR: measured from your own run, not hypothesised. The sky dome is
rem   vp=af06f6d32ec048ee albedo=D1A6D1B27ADE6232 vtx=304 depth_write=0
rem   rawext=26352  (minext=2000, cleared 13x over)
rem   inside=1      (the camera is inside it)
rem   anchor=4.77 and 5.58  against  limit=4   -> reject:anchor
rem The anchor gate asks how far the draw's own origin sits from the eye, and a
rem dome's origin IS the eye. This one's trails the camera by ~5 units (mostly
rem along z: origin=[854.71 -10.82 2238.8] vs cam=[854.52 -10.84 2243.6]), so a
rem 4.0-unit tolerance refuses it by a hair. Refused = never tagged SKY = handed
rem to Remix as ordinary opaque world geometry, i.e. a 26000-unit shell sealed
rem around the player - which is exactly why the distant sun only reaches the
rem ground when you HIDE the sky texture.
rem 16 is safe, not tuned: the nearest draw that clears depth-write and
rem sky_min_extent() WITHOUT being a dome sits at anchor 96.91, so this keeps a
rem 6x margin under it while clearing 5.58 by 3x. The extent, depth-write and
rem camera-inside gates are all still doing their work.
rem WATCH: 'Remix sky-census:' reject:anchor lines for D1A6D1B27ADE6232 should
rem STOP. Then the sun should light the ground with the sky still drawn.
rem 4 (or removing the line) restores the old behaviour - that A/B is the proof.
rem
rem !!! REVERTED TO 4 ON 2026-08-15 - 16 WORKED AND THAT WAS THE PROBLEM !!!
rem The tag landed exactly as designed (pick read sky=1, anchor 0.000, and every
rem reject:anchor line stopped). The sky then went PITCH BLACK, because on this
rem backend a Sky tag is a HIDE. Runtime source, not a guess:
rem   rtx_instance_manager.cpp:1006
rem     // Hide the sky instance since it is not raytraced.
rem     if (drawCall.cameraType == CameraType::Sky) {
rem       currentInstance.m_isHidden = true;   }
rem The visible sky is then supposed to come back from rasterizeSky(), and that
rem path is unreachable from here: tryHandleSky() (rtx_sky.h:145-200) needs
rem 'originalParams' + 'originalDrawCallState' - the ORIGINAL D3D9 RASTER DRAW -
rem and an API-submitted draw has neither. rasterizeSky() has exactly one caller
rem (rtx_sky.h:191) and zero references anywhere in rtx_remix_api.cpp.
rem CONCLUSION: on the remixapi path, tagging the dome Sky can only ever make it
rem INVISIBLE. This knob is the wrong lever for this backend and 16 must not be
rem restored until the dome has another way to be seen.
rem The real fix is a backend change - give the dome an EMISSIVE material so it
rem lights the scene itself instead of standing there as a black shell that
rem absorbs the sun. Queued as round 13.
set "RPCS3_REMIX_SKYANCHOR=4"
rem
rem === ROUND 36: RPCS3_REMIX_SKYANCHORMODE - MEASUREMENT FIX, NOT ARMED =======
rem The knob above is compared against |transform.translation - eye|. For an
rem absolute-world draw - correct identity-ish transform, world-space vertices -
rem the translation IS the world origin, so the quantity it evaluates is |eye|,
rem and the further the player walks from the origin the more certainly it
rem fails. The unlocked levels read anchor=2137.85 against limit=4 with the
rem camera 2137 units out. Same defect round 31 measured on VIEWMODELANCHOR (the
rem guard read ~2100 to a point no geometry occupied); round 32 already listed
rem SKYANCHOR as dead for absolute-world geometry. Now fixed as a MEASUREMENT.
rem
rem SHIPPED: canchor= (|AABB centre - eye|, the quantity it should have been
rem comparing) and anchormode= now print on every "Remix sky-census:" row BESIDE
rem the existing anchor=, so old and new are auditable on the SAME rows in ONE
rem run instead of across two.
rem   0 = |translation - eye| (round 13..35). DEFAULT AND ARMED.
rem   1 = |AABB centre - eye| against the same limit.
rem   2 = the eye is INSIDE the transformed AABB. Scale-free, translation-free.
rem   3 = 2 OR 0.
rem
rem DELIBERATELY LEFT AT 0, for TWO measured reasons, and this is the important
rem half of the round-36 sky work:
rem   (a) Admitting more domes makes them WORSE, not better. is_sky sets
rem       REMIXAPI_INSTANCE_CATEGORY_BIT_SKY and on this path that tag HIDES the
rem       instance. Mode 2 would turn black domes into ABSENT domes.
rem   (b) MEASURED on the 278 reject:anchor rows of the last 250 MB: 146 of them
rem       are HIGH-vtx terrain (1211..12875 vertices) against 69 plausible dome
rem       bands - and inside=1 fires on exactly 146 of the 278. Until somebody
rem       cross-tabs inside= against vtx on those rows, mode 2 may be admitting
rem       precisely the terrain and refusing precisely the domes. SKY hiding
rem       terrain is how geometry vanishes at certain angles.
rem THE BLACK-SKY FIX IS RPCS3_REMIX_SKYEMISSIVE, NOT THIS.
rem WHAT DOES SEPARATE THEM, measured on the same rows: the dome and terrain
rem populations share ZERO vertex programs and ZERO albedos, and backdrop=1
rem fires on 39 dome rows and 0 terrain rows. A future rule should key on the
rem program hash, the albedo, or the already-computed backdrop predicate - never
rem on a looser distance.
set "RPCS3_REMIX_SKYANCHORMODE=0"
rem
rem === REMINDERS (no code this round, both still your call) ===================
rem The round-8 list-retirement stages (SKIPPAIRVP / SKIPEXTENTVP, then SKIPVP)
rem are still commented and still unrun. The madaccum decode may have made them
rem retirable - worth one session now.
rem VIEWMODELALBEDO (the visor) is still commented above. Round 10 shipped the
rem code; arming it is one un-rem away.

rem =============================================================================
rem --- round 12 ---
rem Nothing above this line was edited. Two new knobs, both appended. cmd 'set'
rem is last-wins, so deleting this whole block restores round 11 exactly.
rem =============================================================================
rem
rem === PRIMARY: round 11's four census lines were STRUCTURALLY SILENT ==========
rem This is why round 11 could not be scored. All four of its new emitters -
rem   'Remix vcolroute:'  'Remix hazefade:'  'Remix selflit-miss:'  'Remix fxref:'
rem opened their guard with dump_enabled(), which is RPCS3_REMIX_DUMP (set to 0
rem on line 73 above) OR the "Log Draw Diagnostics" config box (off). So the
rem counters climbed all session while the lines that explain them never
rem printed. Measured across the THREE round-11 runs sitting in remix_dump.log:
rem   run A  flips=13243  hazefade=22365  vcolroute/hazefade/selflit-miss/fxref = 0/0/0/0
rem   run B  flips=14507  hazefade=11220  ................................... = 0/0/0/0
rem   run C  flips=16232  hazefade= 6635  ................................... = 0/0/0/0
rem while the UNGATED 'Remix effect:' printed 11, 10 and 16 and 'Remix live:'
rem printed 180, 174 and 185 in the same three runs. All SEVEN
rem dump_enabled()-gated censuses emitted 0 lines; all ELEVEN ungated ones
rem emitted. 'Remix effect:' - six source lines ABOVE the silent vcolroute call,
rem at the SAME call site, which is what made the adjacency look impossible -
rem simply has no dump_enabled() guard at all.
rem These four are censuses, not dumps: 64 lines PER RUN, one hash-set probe per
rem draw, the same cost as the unconditional 'Remix effect:' beside them. They
rem are now on their own knob. NOTHING ELSE CHANGED - all four are pure
rem reporters and none of them makes a decision.
rem WATCH FOR: nothing visual. This knob cannot change a pixel. If frame time
rem   regresses, set it to 0 - but the four lines are capped and dedup by
rem   program, so that would be surprising.
rem READ: 'Remix vcolroute:' and 'Remix hazefade:' must now APPEAR in
rem   bin\remix_dump.log. hazefade's a_range= must NOT be 255..255 and must MOVE
rem   with the camera - that is round 11's headline fix finally being scoreable.
rem   Also 'diaglines=1' now prints on the startup banner and the live line;
rem   read that field FIRST if these lines are ever missing again.
rem REVERT: set to 0 (restores round 11's silence exactly).
set "RPCS3_REMIX_DIAGLINES=1"
rem
rem === THE 'constant' VERTEX-COLOUR ROUTE (the black-gauge second mechanism) ===
rem Round 11's sweep found programs whose COL0 is a pure constant -
rem   MOV o1.xyzw, c[K]
rem with the mesh's ATTR3 never read. Round 11 filed that shape under 'computed'
rem and refused it, so those draws render WHITE. This reads c[K] live and submits
rem it as a flat vertex colour.
rem BLAST-RADIUS SWEEP over all 82 cached .vp (71 unique), decoder sanity-checked
rem against all five hand-decoded round-11 programs, run BEFORE this shipped:
rem   computed 25 -> 18,  constant 0 -> 7.  none/passthrough/scaled/scaled_chain
rem   UNCHANGED. alpha_from_attr count unchanged (all 7 movers were already 0).
rem   The five known actors DO NOT MOVE: gauges 2F64C2F8FFD6ADD1, sky dome
rem   FC0FAC8AFCCEC49A, backdrop EDC10321BF7CEB8F, effects F39F504649B6F442,
rem   giant flower C97CD1531AC480D8. Zero near-misses.
rem
rem HONEST EXPECTATION - READ THIS BEFORE JUDGING IT.
rem The sweep contradicts the lead this fix came from. Only ONE of the seven
rem movers is HUD-shaped (2F650A38FFE6ADD1: 6 slots, c[18], and 93 'Remix
rem uiwrap:' lines in your log). The other SIX all read c[94] and are plainly
rem WORLD GEOMETRY - 15 to 48 slots, o11..o14 DP4 blocks against c[97..112], a
rem view-vector term. So this is NOT a targeted gauge fix; it is a route the
rem gauges' near-twin happens to sit in.
rem And with your VCOLMOD=0, this can only fire on UNTEXTURED draws, because
rem that is the only arm that calls the colour replay at all. Your run's whole
rem untextured population is four programs - 7f3d3abcefc8b057, 56cc5a962ead7ecd,
rem 33ae0895ae9fef72, fc0fac8afccec49a - and the two that ARE constant-route are
rem both already dropped by existing gates ('Remix skip-census: gate=characterdepth'
rem and 'gate=unboundblend'). So vcol_const may well read 0. THAT WOULD NOT BE A
rem FAILED FIX - it is the statement that no constant-route program drew
rem untextured-and-submitted this session.
rem
rem WATCH FOR: any world surface taking on a flat tint it did not have -
rem   especially in the half-res lighting passes (504D2B3A1CE0FC74,
rem   8DED5C7DB3AB8563, B01BFCE3FC580E3B all print 'Remix lightpass:') and
rem   around 7F02E76D7369D09E, which is one of your WORLDIDENTITYVP programs.
rem   Geometry CANNOT disappear from this: the submitted alpha is pinned at
rem   opaque 255 and the constant's own alpha is deliberately NOT replayed.
rem READ: vcol_const on the "Remix live:" line, and - this is the real prize -
rem   'Remix vcolroute: ... route=constant cval=[r g b a]' in the dump. cval is
rem   the LIVE c[K], and it prints EVEN WITH THIS KNOB AT 0, because the
rem   classifier always runs and only the replay is gated. So one session tells
rem   you what colour these programs actually want.
rem REVERT: set to 0. Round 11's white comes back bit-exactly.
set "RPCS3_REMIX_VCOLCONST=1"
rem
rem === THE VCOLMOD RE-TEST IS STILL THE REAL GAUGE EXPERIMENT (unchanged) =====
rem Your VCOLMOD=0 on line 600 is NOT touched, and round 11's commented
rem 'set "RPCS3_REMIX_VCOLMOD=1"' on line 900 is still commented. That remains
rem the experiment that can actually reach the gauges, because the gauges are
rem TEXTURED and VCOLMOD is the only switch that lets a textured draw take a
rem replayed vertex colour. VCOLCONST above makes that experiment strictly safer
rem for the constant-route family: where round 11 would have replayed a
rem meaningless ATTR3, the ucode's own constant now goes in instead.
rem If you run it: un-rem line 900 for ONE session, look at the gauges, then
rem grep 'route=constant' out of bin\remix_dump.log and read cval=.
rem
rem === PRIORITY-2 FINDING: vcol_fold IS NOT BROKEN - IT IS NARROW ============
rem vcol_fold=0 was raised as a suspected defect. It is not a defect, and it is
rem also not permanently 0 - the "0 for the whole run" reading was one run.
rem The fold lives inside apply_vertex_colour, which is called from exactly two
rem places: 'material && VCOLMOD' (DEAD - your VCOLMOD=0) and '!material'
rem (untextured only). So the fold can only ever count UNTEXTURED draws of
rem scaled / scaled_chain programs.
rem Measured over the three runs in the log:
rem   run A  vcol_fold=0    untextured programs: 7f3d3abc, 56cc5a96, 33ae0895, fc0fac8a
rem   run B  vcol_fold=0    same four
rem   run C  vcol_fold=184  those four PLUS 0214281b, ad7ce9d6, d0b6a471
rem ad7ce9d672a0bf6b is EXACTLY the program your Ctrl+Click pick reported as
rem route=scaled. In runs A/B it only ever drew TEXTURED, so the fold had
rem nothing to work on; in run C it drew untextured twice and the fold moved.
rem So: the picked draw had material=1 and is blocked by the VCOLMOD gate -
rem that is a DESIGN FACT, not a bug - and the fold code itself demonstrably
rem works. NOTHING WAS CHANGED FOR IT. Widening that gate into textured world
rem geometry is how earlier rounds deleted geometry. It opens up on its own the
rem moment VCOLMOD goes to 1, and 'Remix vcolroute:' will now name every
rem program's route so this never has to be inferred again.

rem ==========================================================================
rem === round 13 ==============================================================
rem ==========================================================================
rem NOTHING BELOW HAS BEEN PLAY-TESTED. Round 13 could not run the game.
rem Every claim here is from the runtime SOURCE, the ucode sweep, or the last
rem run's own log - never from looking at the screen.
rem
rem FIRST, THE CORRECTION THAT MATTERS MOST ----------------------------------
rem The Remix runtime this actually loads is bin\remix\d3d9.dll, and it is
rem BYTE-IDENTICAL to dxvk-remix-numos3\_output\d3d9.dll:
rem   sha256 36a5641af4fa848ef9348ca2fffcd6ff9141ac87007264fb16c6f194a34b0de7
rem It is NOT dxvk-remix-ppsspp. The two forks differ on exactly the code that
rem decides both of this round's headline items, so read numos3 or reach the
rem wrong answer. Re-check that hash after any runtime upgrade.
rem
rem THE SKY: WHAT WAS ACTUALLY WRONG ----------------------------------------
rem The theory was "tagging the dome Sky HIDES it". That is FALSE here, and the
rem log already said so: the dome censuses as
rem   'Remix sky-census: vp=af06f6d32ec048ee TAGGED ... albedo=D1A6D1B27ADE6232'
rem while being plainly visible on screen. The runtime's hide is
rem   if (drawCall.cameraType == CameraType::Sky) m_isHidden = true;   (:1006)
rem keyed on the CAMERA TYPE, and numos3 maps Sky back to Main for API draws on
rem purpose (rtx_remix_api.cpp:916). So RPCS3_REMIX_CAT_SKY and
rem rtx.skyBoxTextures are both no-ops on this backend.
rem
rem The dome is ALREADY emissive: it is in rtx.worldSpaceUiTextures, and WorldUI
rem force-feeds emission at intensity 2.0 with the albedo as the emissive
rem texture (rtx_instance_manager.cpp:1103-1107).
rem
rem What actually eats the sun is OCCLUSION. The dome is a closed OPAQUE shell
rem around the camera, so it takes OBJECT_MASK_OPAQUE (:1283), and the direct
rem shadow ray traces against exactly that mask (integrator_direct.slangh:101).
rem The fallback distant sun is therefore blocked 100% of the time and the only
rem light left in the scene is the dome's own - which is precisely the reported
rem symptom, "the sky lights up the environment".
rem
rem === RPCS3_REMIX_SKYEMISSIVE - the sky dome, by albedo hash ================
rem Gives the listed texture an emissive material AND (via SKYEMISSIVEBLEND
rem below) a BlendType::kEmissive, which puts the instance in the unordered
rem TLAS with OBJECT_MASK_UNORDERED_ALL_EMISSIVE (:1216, :1270). Those bits are
rem deliberately absent from OBJECT_MASK_ALL_STANDARD, so the shadow ray misses
rem the dome entirely - the sun comes back - while primary rays still see it.
rem The hash is the dome's albedo, measured: it is the ONLY texture
rem vp=af06f6d32ec048ee ever draws (421 occurrences across all logged runs).
rem WATCH FOR: the sun returning - real directional shading and cast shadows on
rem   the ground, instead of the flat everywhere-light the dome was providing.
rem   The sky itself should look UNCHANGED (see SKYEMISSIVEINT).
rem   If the sky goes black, the emissive material did not attach - read
rem   mat_skyemissive on the live line before changing anything else.
rem READ: 'Remix skyemissive:' in bin\remix_dump.log names vp/albedo/vtx/ext,
rem   whether the sky classifier tagged it, whether the list matched, and
rem   whether the material was created. On the live line:
rem   mat_skyemissive=1 mat_skyunordered=1 is the fully-armed state.
rem   mat_skyemissive=1 with mat_skyunordered=0 means SKYEMISSIVEBLEND is off.
rem   mat_skyemissive=0 with skyemissive=1 on the banner means the hash never
rem   reached CreateMaterial - wrong hash, or the texture never decoded.
rem REVERT: blank it (set "RPCS3_REMIX_SKYEMISSIVE="). Empty list restores
rem   today's behaviour bit for bit.
rem 2026-08-16 SECOND SKY ADDED - THE SKY TEXTURE IS PER-AREA.
rem The land-carrier / Mantel-base area renders a BLACK sky. Cause, measured:
rem   Remix sky-census: TAGGED vp=af06f6d32ec048ee albedo=CDFE11B12552EA2D
rem                     rawext=27194
rem   D1A6D1B27ADE6232 (the only pinned albedo): 0 occurrences in that run
rem   mat_skyemissive=0  mat_skyunordered=0
rem Same sky-dome PROGRAM, different TEXTURE. The dome is correctly tagged as
rem sky, but the emissive treatment is pinned per-albedo, so it never applies
rem here - a lit dome in Selva and an unlit black one everywhere else.
rem Expect MORE of these: every level with its own sky texture needs its hash
rem on this line. Read 'Remix sky-census:' for a TAGGED line with a large
rem rawext and add whatever albedo it names.
rem WATCH: mat_skyemissive / mat_skyunordered going non-zero in this area.
rem REVERT: drop CDFE11B12552EA2D back off the list.
rem
rem === ROUND 36: FOUR MORE DOMES - THIS IS THE BLACK-SKY FIX ==================
rem The chapter-select unlock made every campaign level reachable and the sky is
rem BLACK in the ones that were never played. The cause is THIS LIST, not the
rem sky classifier. On the remixapi path a SKY tag can only ever HIDE the dome
rem (rtx_instance_manager.cpp:1006 sets m_isHidden, and rasterizeSky() is
rem unreachable from an API draw - the block at ~:1775 above worked that out
rem from the runtime source and it still holds). What makes a dome VISIBLE on
rem this backend is the EMISSIVE material, and that is keyed on the texture
rem content hash HERE, in RemixTextures.cpp, with no reference to the sky
rem classifier at all. Two domes were listed; the other levels use different
rem textures and were therefore never lit.
rem
rem The four added below are the largest anchor-rejected dome candidates in the
rem "Remix sky-census:" rows of the unlocked levels, by world extent. All four
rem recur across six separate runs in the log, which is what makes them a
rem cohort rather than four one-off draws:
rem   35C2353F6B3CE2A8  max wext 2.14e6  vtx 54..79
rem   174F4F689CF2A3D8  max wext 1.83e6  vtx 42..66
rem   3213E0CC136ED294  max wext 1.44e6  vtx 45..90
rem   32AE81D64BEA29CD  max wext 1.29e6  vtx 147..266
rem Low vertex counts against million-unit extents: dome latitude bands. The
rem terrain in the same reject:anchor pile runs vtx 1211..12875 at wext under
rem 79k, and shares ZERO albedos and ZERO vertex programs with these four.
rem
rem LIST BOUND IS 8 AND THIS MAKES SIX - verified against the parser
rem (RemixTransforms.cpp, sky_emissive_albedos(), std::array<u64,8>, buffer 400
rem wchar against 102 used). The count prints on the knobs line and MUST read 6.
rem If it reads 2 the list did not parse at all; if it reads 8 the bound
rem truncated it.
rem PRE-REGISTERED, per level: the sky stops being black and starts glowing at
rem SKYEMISSIVEINT=2.0. If a level goes from black to BLINDING, lower
rem SKYEMISSIVEINT rather than removing the hash. If a level is STILL black, its
rem dome is a fifth texture - Ctrl+Click the sky and read albedo= off
rem "Remix picked:", or take the largest-wext row of "Remix sky-census:".
rem REVERT to the round-35 list (no rebuild):
rem   set "RPCS3_REMIX_SKYEMISSIVE=D1A6D1B27ADE6232,CDFE11B12552EA2D"
set "RPCS3_REMIX_SKYEMISSIVE=D1A6D1B27ADE6232,CDFE11B12552EA2D,35C2353F6B3CE2A8,174F4F689CF2A3D8,3213E0CC136ED294,32AE81D64BEA29CD"
rem
rem === RPCS3_REMIX_SKYEMISSIVEINT - the dome's emissive intensity ===========
rem DEFAULT 2.0, and that number is not a guess: rtx_instance_manager.cpp:1105
rem sets exactly 2.0f on every WorldUI instance, which is what the dome renders
rem at TODAY. Matching it makes this a no-op in brightness, so the first thing
rem you judge is the SUN coming back rather than a brightness change nobody
rem asked for. Tune by eye afterwards - the correct value is unknown.
rem NOTE it is inert while 0xD1A6D1B27ADE6232 is still in
rem rtx.worldSpaceUiTextures: WorldUI's override runs after material creation
rem and re-forces 2.0. Removing the conf entry is what hands control to this
rem knob - see the rtx.conf note at the bottom. The OCCLUSION fix does NOT
rem depend on that; it works with the conf entry left exactly as it is.
rem WATCH FOR: sky too bright/washed out -> lower toward 1.0. Sky too dim or
rem   grey -> raise toward 4. Only meaningful once the conf entry is gone.
rem REVERT: 2.0 (or blank SKYEMISSIVE, which disables the whole path).
set "RPCS3_REMIX_SKYEMISSIVEINT=2.0"
rem
rem === RPCS3_REMIX_SKYEMISSIVEBLEND - the half that gives the sun back ======
rem 1 (default) declares BlendType::kEmissive on the dome's material, which is
rem what moves it off OBJECT_MASK_OPAQUE and out of the sun's shadow ray.
rem Declared on the MATERIAL (useDrawCallAlphaState=0 + blendType_hasvalue=1 +
rem blendType_value=6), so the runtime takes calculateAlphaState's
rem '!useLegacyAlphaState' arm and reads it directly - no dependence on
rem blend-factor pattern matching or on rtx.enableEmissiveBlendModeTranslation.
rem WATCH FOR: this is the A/B that attributes any lighting change. 0 keeps the
rem   emissive look and keeps the occlusion, i.e. keeps today's sunless scene.
rem REVERT: 0.
set "RPCS3_REMIX_SKYEMISSIVEBLEND=1"
rem
rem ==========================================================================
rem === ROUND 39: RPCS3_REMIX_SKYCLASSIFY - the sky WITHOUT a hash list ======
rem ==========================================================================
rem THE DEFECT. The six hashes above are the whole reason any sky renders. A
rem dome whose texture hash is not on that list gets an ordinary opaque
rem material, no emission, and renders BLACK. haze_domes.csv - mined from the
rem game's own archives - names SIXTEEN distinct dome resources, so about ten
rem of them are black and the only cure has been to visit the level and
rem Ctrl+Click the sky. MEASURED in bin\remix_dump.log: 14,726 stats lines read
rem "mat_skyemissive=0 mat_skyunordered=0" - entire sessions in which the
rem sky-emissive material was never created once - while sessions on a listed
rem level reach 43.
rem
rem THE ARCHIVES CANNOT FIX IT: the mining round recovered the dome NAMES but
rem name -> texture hash is not recoverable (assets are keyed by an unrecovered
rem name hash; all 28,278 cached.pak members were scanned for every dome name,
rem zero hits). So the domes must be identified at RUNTIME, by shape.
rem
rem WHAT IT DOES. Per albedo hash: a draw is dome-shaped when it writes no
rem depth, the camera is INSIDE its transformed AABB, its world extent is in
rem [SKYEXTENT, SKYCLASSIFYMAXEXT], its vertex count is at or below
rem SKYCLASSIFYMAXVTX and its extent-per-vertex is at or above SKYCLASSIFYUPV.
rem A hash that has been dome-shaped SKYCLASSIFYMIN times, has never been seen
rem on a non-dome draw, and has been known for SKYCLASSIFYSETTLE frames is
rem PROMOTED into the sky-emissive set - exactly as if you had typed it into
rem RPCS3_REMIX_SKYEMISSIVE above. Same material, same intensity, same blend
rem type, and it also unlocks the per-level SUN (the peak_uv walk that feeds
rem derive_sky_sun is gated on the same predicate).
rem
rem WHY NOT THE EXISTING SKYHASH RULE, WHICH ALREADY DOES THIS SHAPE. Because
rem it REJECTS the two domes we have ground truth for. MEASURED, every
rem "Remix sky-hash-census:" line in the log:
rem   albedo=D1A6D1B27ADE6232 reject:mixed vtx=304 wext=26351.6 upv=86.68
rem   albedo=CDFE11B12552EA2D reject:mixed vtx=372 wext=27194.1 upv=73.10
rem Both are on YOUR list above. Its units-per-vertex floor is 100 and those
rem domes' own latitude bands measure 86.68 and 73.10, so the coarse bands of a
rem tessellated dome disqualify their own texture. Structural, not tuning.
rem
rem MODE 2 = PROMOTE (armed here). 1 = census only, image-identical. 0 = off.
rem 3 = promote and ignore the disqualification. The ceiling is 3, i.e. ABOVE
rem the armed 2, deliberately - a clamp equal to the armed value is the trap
rem that has now cost five rounds.
rem
rem READ, on "Remix live:" in bin\remix_dump.log (also on "Remix stats:" in
rem bin\log\RPCS3.log after exit):
rem   skyclassify_armed     hashes the rule accepted. haze_domes.csv holds 16
rem                         distinct dome resources, but only 12 outside
rem                         multiplayer - so 8..20 over a full single-player
rem                         pass is expected. ABOVE ~28 = over-matching -> raise
rem                         SKYCLASSIFYMAXEXT or SKYCLASSIFYMINVTX first - upv
rem                         is nearly redundant with the vertex ceiling.
rem   skyclassify_promoted  of those, the ones NOT already on your list.
rem   armed MINUS promoted  = how many ground-truth domes the rule agreed with.
rem                         IF THIS IS 0 THE RULE FOUND NOTHING YOU HAD ALREADY
rem                         FOUND BY HAND, which is a reason to distrust it.
rem   skyclassify_entries   material rebuilds performed. 0 with promoted > 0
rem                         means the rebuild reached no resident texture and
rem                         the dome will stay black.
rem   skyclassify_failed    every resident entry refused the rebuild. The
rem                         promotion was WITHDRAWN and will retry. Expect 0.
rem   skyclassify_overflow  the 64-entry promoted set is full. Expect 0; if
rem                         not, the title has more dome textures than the
rem                         array holds - raise the array, not a threshold.
rem
rem READ THIS BEFORE ANY COUNTER ABOVE. In bin\log\RPCS3.log:
rem   Remix skypromote: content=<albedo> mat AAAA -> BBBB ok
rem The two hashes MUST differ. A promotion rebuilds the material for a texture
rem that already has one, and material_hash is derived from the content hash and
rem the wrap/alpha state - none of which a promotion changes. Without the fold
rem this round added, CreateMaterial would be handed two different definitions
rem under ONE hash, and this file already records (round 7) that aliasing them
rem makes the winner DRAW-ORDER DEPENDENT: the dome could stay black while
rem skyclassify_promoted, skyclassify_entries AND mat_skyemissive all report
rem success. If that line ever reads IDENTICAL - THE FOLD DID NOT FIRE, stop and
rem set SKYCLASSIFY=1; no counter on the live line can see that failure.
rem NOTE mat_skyemissive is no longer a 'did the dome attach' test: a promotion
rem rebuilds every entry aliasing the content hash (~45 on this title), so one
rem dome moves it by tens. Use skyclassify_entries and Remix skypromote:.
rem   skyclassify_settling  arm attempts held by the settle window. Non-zero
rem                         then falling is the window working.
rem   skyclassify_rejected  DRAWS carrying a disqualified hash, not hashes - it
rem                         climbs with traffic. The count of distinct refused
rem                         textures is the number of "reject:mixed" lines.
rem REPLAYED BEFORE SHIPPING: applying every gate above to every
rem "Remix sky-census:" row in the 969 MB log admits 18 distinct albedo hashes,
rem and ALL SIX of the hand-listed dome hashes are among them - the ground-truth
rem check passing on historical data before this play-test sees it. The other 12
rem are candidates, not confirmations: the census carries no draw counts, so it
rem cannot evaluate the 8-draw / no-disqualification / settle-window rule that
rem actually arms a hash. EXPECT THE LIVE NUMBER TO BE LOWER THAN 18.
rem
rem And "Remix skyclassify:" names each hash, with vp=, vtx=, wext=, upv=,
rem listed= and the CAMERA POSITION at the moment it armed - that last field is
rem how a dome hash gets attributed to a level, because nothing in the guest
rem signal carries a level name.
rem
rem PRE-REGISTERED, and check them in this order:
rem   1. D1A6D1B27ADE6232 and CDFE11B12552EA2D must both appear on
rem      "Remix skyclassify:" as ARMED:listed. They are ground truth and the
rem      OLD rule rejects both. If they do not arm, the upv floor is still
rem      wrong and nothing else this round claims is worth reading.
rem   2. RAVINE MUST ARM NOTHING NEW. haze_domes.csv: all six jungle_ravine
rem      backgrounds read "(no skyModel authored)". A BLACK SKY IN RAVINE IS
rem      CORRECT. Any hash arming there is a false positive and it is the
rem      cheapest place in the game to see one.
rem   3. On the other levels the sky should stop being black without the sun
rem      changing - the sun is a separate path and this round did not move it.
rem
rem BLAST RADIUS: a promoted hash becomes EMISSIVE and stops occluding (blend
rem type kEmissive). If a wall, a water plane or a fog card starts glowing and
rem stops casting shadow, this knob did it - read "Remix skyclassify:" for the
rem albedo and check it against what is glowing.
rem REVERT (no rebuild): set "RPCS3_REMIX_SKYCLASSIFY=1" - census only, keeps
rem   every number above and changes no pixel. 0 turns even the census off.
rem ============================ ROUND 40 - DROPPED TO 1 ==========================
rem THE ROUND-39 RUN FAILED ITS OWN PRE-REGISTERED CHECK. MEASURED, 64,139 flips:
rem   skyclassify_armed=1  skyclassify_promoted=1  skyclassify_entries=1
rem   Remix skyclassify: albedo=23A3978F1405B16E ARMED dome=61 other=0 agree=1 |
rem     vp=af06f6d32ec048ee vtx=152 wext=24728.6 upv=162.688 inside=1 |
rem     listed=0 promoted=1 | cam=[1746.6 -71.676 1049] frame=59921
rem Round 39 pre-registered "armed - promoted > 0, and if it is 0 the rule found
rem nothing the user had already found by hand - distrust it". It is 0, and
rem listed=0 says the one hash it did arm is NOT on your SKYEMISSIVE list.
rem It also armed at cam=[1746.6 -71.676 1049], which is INSIDE the copper-plant
rem region your other picks came from (X 1746..1806, Z 1049..1228) - i.e. the
rem exact level you report as having white walls that vanish at angles. A
rem promoted hash becomes emissive AND STOPS OCCLUDING, which is what "white"
rem plus "disappears at angles" looks like from a path tracer.
rem HONEST CAVEAT: the census argues it IS a real backdrop - 2048x512, raw box
rem [-12370..12370], 61 of 61 draws dome-shaped. So this may be a correct
rem promotion. That is exactly why it is an A/B and not a deletion.
rem TEST: with this at 1 nothing is rebuilt and no pixel changes from the
rem classifier. If the plant walls stop being white / stop vanishing, this was
rem it. If they are unchanged, put it back to 2 and the classifier is cleared.
set "RPCS3_REMIX_SKYCLASSIFY=1"
rem
rem --- the four thresholds, each with the measurement behind it -------------
rem MAXEXT 4.0e6. MEASURED: the candidate set contains a family at wext 1.22e9
rem   .. 1.12e18 which cannot be geometry - the mined scene descriptor sets
rem   farPlane 14000, so the world fits in ~1.4e4 units. The largest row
rem   carrying a hash YOU listed as a dome is 2.14e6 (35C2353F6B3CE2A8). The
rem   gap 2.14e6 -> 1.22e9 is 570x wide, so this is a round number in a wide
rem   gap, not a tuned one. Do NOT lower it below ~3e6: three of your six
rem   listed domes live in the million-unit family and a "sensible" ceiling
rem   would delete them.
set "RPCS3_REMIX_SKYCLASSIFYMAXEXT=4000000"
rem MAXVTX 1024. MEASURED terrain in the candidate set: 2714..12875 vertices.
rem   MEASURED domes: 33..747. 1024 sits 2.65x below the lowest terrain row.
set "RPCS3_REMIX_SKYCLASSIFYMAXVTX=1024"
rem MINVTX 16 - a FLOOR, and it exists because replaying the gates over the log
rem   named one concrete false positive: albedo AC936E2F25F147B0 on
rem   vp=3c9186d8e026cec5 is a FOUR-VERTEX quad spanning 32,331 units with the
rem   camera inside it. That is a full-screen backdrop card, not a dome. The
rem   smallest vertex count on any hand-listed dome row is 33, so 16 drops that
rem   quad and nothing else (admitted set 19 hashes -> 18).
set "RPCS3_REMIX_SKYCLASSIFYMINVTX=16"
rem UPV 60. MEASURED lowest units-per-vertex on any row carrying a hand-listed
rem   dome hash, after the extent and vertex gates: 73.09. MEASURED highest on
rem   any row the vertex ceiling excludes: 23.57. 60 sits between, a 3.1x gap.
rem   HONEST LIMIT: on this data upv is nearly REDUNDANT with the vertex
rem   ceiling - every row the ceiling excludes is also under 60 - so most of the
rem   separating power is the extent bounds, the vertex bounds and "inside".
rem   Raise toward 73 if something that is not sky starts glowing; lower toward
rem   50 if a level's sky stays black while its dome is clearly being drawn.
set "RPCS3_REMIX_SKYCLASSIFYUPV=60"
rem MIN 8 dome-shaped draws to arm - the same count the older SKYHASH rule
rem   uses, and for the same reason: one dome-shaped draw is what a large flat
rem   effect card looks like for a single frame.
set "RPCS3_REMIX_SKYCLASSIFYMIN=8"
rem SETTLE 60 frames (~2 s at 30 fps) between first sight and arming. This is
rem   the guard on the one failure the rule cannot undo: a material rebuilt
rem   emissive cannot be rebuilt back, so a texture that is SHARED with world
rem   geometry must get the chance to disqualify itself first. Raise it if a
rem   shared texture still slips through; lower it if a level is left too
rem   briefly for its dome to arm at all (watch skyclassify_settling).
set "RPCS3_REMIX_SKYCLASSIFYSETTLE=60"
rem
rem === THE VIEWMODEL CAMERA - why the tag has never done anything ===========
rem Round 12's reading, that vmcam_applied=0 meant the camera was missing, was
rem a misread of the counter. vmcam_applied counts the DIVISOR latch, not the
rem camera. The camera has been submitted every frame all along:
rem   vmcam_twin = 13869 = cam_resolved = one per frame, SetupCamera SUCCESS.
rem The problem is that it was an exact COPY of the world camera, and the
rem runtime's correction matrix
rem   mainViewToWorld * (mainProjectionToView * vmProjection * scale) * vmWorldToView
rem cancels to the IDENTITY when both cameras are the same. rtx.viewModel.scale
rem defaults to 1.0, so nothing moved. The pass ran and did nothing.
rem
rem === RPCS3_REMIX_VMCAMFOVX / VMCAMFOVY - a real viewmodel projection ======
rem Give the VIEW_MODEL camera its own field of view. These write m[0][0] and
rem m[1][1] of the projection (= 1/tan(fov/2)) and nothing else, which is the
rem complete payload: the runtime OVERWRITES the depth row from the main camera
rem (:1526-1528), so the measured near plane of 0.090 is irrelevant and is
rem deliberately not reproduced.
rem The values below are the measured ones - recovered G0 over the tagged draws
rem gives the viewmodel fovx 60.001 / fovy 36.132 against the world's
rem 72.000 / 44.634. BOTH must be set; one alone is ignored.
rem ALSO CONFIRMED, so nobody re-checks it: rtx.playerModel.enableInPrimarySpace
rem defaults to FALSE (rtx_options.h:447) and is absent from rtx.conf, so it
rem does NOT block the viewmodel pass. Do not add it.
rem WATCH FOR: the helmet/visor should stop clipping into world geometry and
rem   should sit at the size the raster original gives it. If the visor jumps,
rem   grows, or smears, these two numbers are the cause - revert them FIRST.
rem READ: vmcam_real on the live line must equal vmcam_twin (one per frame).
rem   vmcam_twin climbing with vmcam_real=0 means the FOV knobs are unset or
rem   were refused as degenerate, and the camera is still an inert twin.
rem REVERT: set both to 0. That restores the exact twin, i.e. round 12.
set "RPCS3_REMIX_VMCAMFOVX=60.001"
set "RPCS3_REMIX_VMCAMFOVY=36.132"
rem
rem === RPCS3_REMIX_FPCENSUSVP - the player's weapon, CENSUS ONLY ============
rem vp=830d7d1b9681c475 draws BOTH the player's first-person weapon AND whole
rem NPC soldiers (5A55210D7739C716 vtx=24227 ext=11.42, 0DA919D97B091B6E
rem vtx=9758 ext=11.34), so extent cannot separate them. Three albedos are
rem weapon-sized against an ~11-unit soldier - 38C858E6DC48E488 (vtx=278,
rem ext=2.30..2.72), 3B93DE13339598E0 (vtx=314, ext=2.19..2.25) and
rem 2FF44C92754B151E (vtx=278, ext=2.478) - and NONE of them may be added to
rem VIEWMODELALBEDO on that evidence. Tagging an NPC's rifle sticks it to the
rem player's face. The discriminator is distance from the EYE measured on the
rem GEOMETRY, which is what this census reports.
rem WATCH FOR: nothing visual. This knob tags nothing and refuses nothing.
rem READ: 'Remix fpcandidate:' lines in bin\remix_dump.log, one per
rem   (vp, albedo), capped at 48. The PLAYER's weapon is the albedo whose
rem   eye_dist stays small (order 1-3 units) across the whole run; an NPC's
rem   weapon wanders with the NPC. Feed the winner to VIEWMODELALBEDO in
rem   round 14 - not before.
rem REVERT: blank it.
set "RPCS3_REMIX_FPCENSUSVP=830D7D1B9681C475"
rem
rem === WHY THAT CENSUS HAS NEVER FOUND THE WEAPON (round 17) ================
rem MEASURED over the whole log: all 242 'Remix fpcandidate:' lines carry
rem vp=830d7d1b9681c475 - the single hash on the line above - because the census
rem returns at the list test before it measures anything. The first-person rig is
rem NOT one program. The 1446-vertex head-locked mesh is drawn by
rem 830d7d1b9681c475, af06f6d32ec048ee AND f39f504649b6f442, and
rem f39f504649b6f442 also draws a 952-vertex mesh whose instance origin sits
rem 1.8194..1.8256 BELOW the eye in three different runs at three different world
rem positions (from viewmodel-census, which is not program-gated). The census
rem meant to find the arms has never measured a single draw from it.
rem Second defect: the seen-set is never cleared, so a (vp, albedo) is sampled
rem ONCE per process. "Stays near the eye all run" was never answerable from it.
rem This knob fixes both. Non-zero admits an UNLISTED program whose geometry
rem centre is measured and within this many world units of the eye, and re-arms
rem the census once per stats window so pairs report repeatedly. Listed programs
rem are exempt from the ceiling so the tagged visor rows always print.
rem CENSUS ONLY. Tags nothing, refuses nothing, moves no counter.
rem READ: 'Remix fpcandidate:' lines, now carrying listedvp= and maxdist=. The
rem   arms/weapon is the (vp, albedo) whose eye_dist MIN and MAX are both small
rem   and close together across the run, on a program other than
rem   830d7d1b9681c475. Use ranges, not snapshots - that is what settled the
rem   visor question in round 16.
rem REVERT: 0. That restores round 13's census bit-exactly.
set "RPCS3_REMIX_FPCENSUSMAXDIST=4"
rem
rem === DO NOT TAG THESE (round 17 refutation, MEASURED) =====================
rem Round 15 recommended CB1677B87EDD72F5 as an untagged twin of the visor. It is
rem a shared WORLD texture: 14,858 lines in the log, 8,755 on ad7ce9d672a0bf6b and
rem 6,082 on c1d482dcd1b03ed0, both world-geometry programs, one sample 2,157
rem units from the eye. Tagging it would fly thousands of world draws onto the
rem player's face. 0BB90155DC1718BF is the same story (1,931 lines, mostly world).
rem The two that ARE safe - 4A7A9D455955A77F (33 lines, only ever vtx=1446) and
rem 1E86FFE43E435272 (26 lines, only ever vtx=59) - are twins of the VISOR, not
rem the arms or the weapon, so they are deliberately NOT added to
rem VIEWMODELALBEDO here. Add them only if the visor itself half-clips.
rem
rem === TWO REVIEW DEFECTS FIXED (no knob, no new behaviour at defaults) =====
rem 1. VCOLCONST was BYPASSABLE. The flat-constant replay branch tested only the
rem    route kind, and vcol_route_replayable() returns an unconditional true
rem    when FPVCOLROUTE=0 - so with FPVCOLROUTE=0 the constant replay ran even
rem    with VCOLCONST=0, and neither knob restored what it documents. Neither
rem    fires at the shipped defaults (both 1), so this never reached the game;
rem    it broke the BISECT knobs, which is how a regression gets attributed.
rem 2. The classifier now requires the 'MOV o1.xyzw, c[K]' to be UNCONDITIONAL.
rem    A conditional write replayed on every vertex is a WRONG colour, not a
rem    refusal, and this route's whole safety argument is that it fails to
rem    white. BLAST-RADIUS SWEEP over all 73 cached .vp: ZERO programs move -
rem    none writes COL0 conditionally on its last write, none saturates it. The
rem    known actors are all unmoved (gauges 2F64C2F8FFD6ADD1 passthrough, sky
rem    dome AF06F6D32EC048EE scaled, haze card EDC10321BF7CEB8F scaled, effects
rem    F39F504649B6F442 scaled, flower C97CD1531AC480D8 scaled_chain,
rem    first-person 830D7D1B9681C475 scaled).
rem Also renamed: the 'Remix vcolroute:' line now prints slots=[...] rather
rem than scale_slots=[...], because the constant route reuses that array for a
rem SOURCE slot, not a scale factor. Update any grep you have.
rem
rem === WATCH ITEM CARRIED FROM THE ROUND-12 REVIEW (VCOLCONST, unfixed) =====
rem The constant colour is read LIVE from the constant file per draw, and the
rem mesh content hash includes .color on every vertex. A constant-route program
rem whose c[K] ANIMATES - a HUD gauge that recolours with health or ammo is
rem exactly that shape - therefore produces a new mesh key on every colour
rem change while its geometry is unchanged, i.e. a CreateMesh per frame for
rem that draw. Round 11's routes were immune because their colour came from the
rem static vertex buffer. NOT pre-optimised, deliberately.
rem WATCH: mesh_created vs mesh_reused on the live line, and 'Remix meshchurn:'.
rem   If mesh_created climbs roughly with frame count once VCOLMOD=1 is tried,
rem   this is why. REVERT: set "RPCS3_REMIX_VCOLCONST=0".
rem
rem === rtx.conf: A RECOMMENDATION ONLY - ROUND 13 DID NOT EDIT IT ===========
rem NOT REQUIRED for the sun fix. Do the code-side test FIRST and on its own.
rem THEN, only if you want SKYEMISSIVEINT to actually control the sky's
rem brightness, remove this ONE hash from the rtx.worldSpaceUiTextures list on
rem line 2 of bin\rtx.conf (leave every other hash alone):
rem   0xD1A6D1B27ADE6232
rem WHY it is inert until then: WorldUI re-forces emissiveIntensity to 2.0 and
rem repoints the emissive texture after the material is created, so the conf
rem entry always wins over the knob.
rem WHY it is safe NOW when it was catastrophic before: removing it previously
rem gave a PITCH BLACK sky, because with no emissive path the dome became an
rem ordinary unlit shell with nothing inside it to light its inner face. With
rem SKYEMISSIVE armed the backend's own material supplies both the emission and
rem the per-texel emissive texture, so the dome keeps its appearance.
rem RISK: if you remove it BEFORE confirming mat_skyemissive=1 on the live line,
rem   you get the pitch-black sky again. Confirm the counter first.
rem NOTE 0xD1A6D1B27ADE6232 is ALSO on rtx.skyBoxTextures (line 18). That entry
rem   is a measured no-op on this backend and can be left alone.
rem
rem =============================================================================
rem --- round 14 ---
rem cmd 'set' is last-wins, so everything below overrides anything above it.
rem Every ACTIVE line here is a DEFAULT restated: deleting the whole block
rem changes nothing. Round 14 ships one behaviour change at its defaults, and it
rem is a LOG LINE ONLY - the 'Remix suncard:' census. Nothing it measures aims
rem anything until you paste a hash.
rem =============================================================================
rem
rem === THE CORRECTION THAT ROUND 14 IS BUILT ON ==============================
rem The sun you are looking at is NOT Remix's fallback light. The backend
rem creates its OWN distant light through the API - RemixGSRender.cpp
rem ensure_sun_light(), remixapi_LightInfoDistantEXT, light_info.hash = 0x4 -
rem aimed by remix_rsx::sun_direction(), default -0.35,-0.9,-0.25. The
rem RPCS3_REMIX_SUNDIR block further up this file is the hand-tune for it.
rem
rem NEW AND UNCOMFORTABLE, verified in the deployed runtime this round:
rem bin\rtx.conf sets rtx.fallbackLightMode = 2 (Always), and at Always the
rem runtime creates its fallback distant light REGARDLESS of how many lights the
rem client supplied - rtx_light_manager.cpp:247-254 short-circuits on the mode
rem and never consults noLightsPresent. So this scene has TWO distant suns right
rem now: ours, and the runtime's at its own default -0.2,-1.0,0.4. Aiming ours
rem does not move the other one, and two suns 25 degrees apart is a large part
rem of "the sun is off to the left".
rem   RECOMMENDED CONF EDIT (round 14 did NOT make it):  in bin\rtx.conf change
rem       rtx.fallbackLightMode = 2      ->      rtx.fallbackLightMode = 0
rem   0 = Never. 1 = NoLightsPresent would also work and would suppress it
rem   automatically, because our API light counts as a light. Do this FIRST -
rem   it needs no rebuild and it is the single biggest change to what you see.
rem
rem SIGN CONVENTION, settled: remixapi_LightInfoDistantEXT::direction is the
rem direction the light TRAVELS (sun -> scene). Traced end to end this round:
rem rtx_remix_api.cpp:714-721 -> RtDistantLight::tryCreate, rtx_lights.cpp
rem :808-825 maps +Z onto it, distant_light.slangh:90 then samples the light at
rem position + (-direction) * 100000. An overhead sun is 0,-1,0. If the sun ends
rem up exactly opposite where you want it, negate all three terms.
rem
rem === STEP 1: FIND THE SUN CARD - census only, no rebuild ===================
rem ROUND 14 COULD NOT IDENTIFY THE SUN CARD OFFLINE, and says so rather than
rem guessing. What was checked and what it showed:
rem   * vp=FC10C996FD0EC49A, named "the sun card" in round 10/11, is UNTEXTURED
rem     (albedo=0000000000000000, had_unit=0 in all 85 log occurrences) and has
rem     not appeared in the last FOUR runs. It cannot be a yellow card with a
rem     white blob in the middle, because it has no texture at all.
rem   * The six bin\remix_ucode\*.fp captures belong to three ordinary WORLD
rem     GEOMETRY vertex programs (0214281B9A7A412D, AD7CE9D672A0BF6B,
rem     D0B6A471BB2D463B) - they draw depth-writing walls, not a sun sprite.
rem     The brief's "the sun's fp should be among them" is refuted.
rem   * No sky-census / worldid-draw / kil / light-candidate / pick row in the
rem     round-13 run has the shape.
rem So this round ships the instrument instead of a fabricated hash.
rem WATCH FOR: nothing visual. This census tags nothing and refuses nothing.
rem READ: 'Remix suncard:' lines in bin\remix_dump.log. One per (vp, albedo) per
rem   window, capped at 32. Each line carries centre=, cam=, dist=, elev=,
rem   azim=, mean_rgb=, alpha=, and BOTH direction senses:
rem       travel=[...]  <- paste this into RPCS3_REMIX_SUNDIR
rem       toeye=[...]   <- the same vector negated, for reading only
rem   The sun is the row with a LARGE dist=, a positive elev=, a yellow/white
rem   mean_rgb, and an albedo that does not change as you walk.
rem   Measured population on the round-13 Selva run: 27 of 110 (vp, albedo)
rem   pairs match the shape before the elevation gate, so one window's 32 lines
rem   covers it.
rem REVERT: set "RPCS3_REMIX_SUNCARDCENSUS=0" (removes the lines and the cost).
set "RPCS3_REMIX_SUNCARDCENSUS=1"
rem Shape bounds for the census AND for the election below.
set "RPCS3_REMIX_SUNCARDMAXVTX=64"
set "RPCS3_REMIX_SUNCARDMINELEV=2"
rem
rem === STEP 2: PIN IT - then the sun aims itself, per level ==================
rem Paste the albedo from the census row you picked, then set SUNTRACK=1. The
rem distant light is then re-created each time the card moves more than
rem SUNTRACKDEG degrees, so the sun follows whatever the level does with it
rem instead of being tuned by eye once and being wrong in the next area.
rem WATCH FOR: the sun swinging to where the yellow card is. Shadows should
rem   point away from the card.
rem READ on the 'Remix live:' line:
rem   suncard_seen=      the SHAPE census population. Climbs with SUNTRACK=0.
rem   suncard_pinned=    the subset on SUNCARDALBEDO. If this stays 0 after you
rem                      paste a hash, THE HASH IS WRONG - stop here.
rem   suncard_elected=   frames that published a direction.
rem   sun_retargeted=    destroy+create pairs actually performed. This is the
rem                      COST. If it tracks the flip count, raise SUNTRACKDEG.
rem   Also 'Remix sun: created travel=[...]' at startup and 'Remix sun-retarget:'
rem   for the first 8 retargets and every 256th after.
rem REVERT: SUNTRACK=0. That restores the static SUNDIR aim bit-exactly.
rem PINNED 2026-08-15: you identified the sun in the Remix texture grid as
rem A61A3CBECA257FE0. Note it is ALSO listed in rtx.skyBoxTextures (as the
rem negative form -0xA61A3CBECA257FE0) - harmless, because on this fork's
rem remixapi path the Sky category is held back at Main and that list is a
rem measured no-op, but worth knowing if it ever behaves oddly.
rem CAVEAT, from the code: pinning does NOT bypass the shape gate.
rem   sun_wanted = sun_shape && (sun_pinned || ...)          (:4192)
rem   sun_shape  = albedo!=0 && state_glow && vtx <= SUNCARDMAXVTX   (:4179)
rem plus the SUNCARDMINELEV floor. So if suncard_pinned STAYS 0 after a
rem session where you looked at the sun, the card is failing shape, not
rem identity - raise SUNCARDMAXVTX (64 -> 256) and drop SUNCARDMINELEV
rem (2 -> 0), ONE at a time so the answer stays attributable.
rem
rem !!! UNPINNED 2026-08-15 - A61A3CBECA257FE0 IS NOT THE SUN. MEASURED. !!!
rem It ran with SUNTRACK=1 for a full session and suncard_pinned stayed 0. All
rem five occurrences of that hash in the run are UI:
rem   Remix uiwrap: vp=2f64c2f8ffd6add1 albedo=A61A3CBECA257FE0 tex=64x64 route=2d
rem   Remix uiwrap: vp=2f650a38ffe6add1 albedo=A61A3CBECA257FE0 tex=64x64 route=2d
rem 2f64c2f8ffd6add1 is the HUD NECTAR/HEALTH GAUGE program and 2f650a38ffe6add1
rem is its constant-colour twin. It is a 64x64 UI sprite - most likely the sun
rem ICON on the HUD, or a glow texture the HUD shares with the sky.
rem The shape gate is the only reason nothing bad happened: sun_shape refused it,
rem so the sun was never aimed at a HUD quad 2 units from the eye.
rem *** THEREFORE DO NOT FOLLOW THE "raise SUNCARDMAXVTX / drop SUNCARDMINELEV"
rem *** ADVICE ABOVE WHILE THIS HASH IS PINNED - that would defeat the very gate
rem *** that caught it. Find the real card first.
rem The real sun card must be a WORLD draw: it will show up in 'Remix suncard:'
rem with a large dist= and a saturated yellow mean_rgb, NOT in 'Remix uiwrap:'.
rem Identify it from a census row, not from the texture grid - the grid cannot
rem tell a world sprite from a HUD sprite that shares a texture.
rem
rem NOTE 2026-08-16: the two 'set' lines that used to live here are now REM'd,
rem because round 16 added its OWN SUNCARDALBEDO / SUNTRACK lines further down
rem and in a .cmd the LAST assignment wins. Two live copies of one knob is how
rem you edit the first one, see no change, and lose an hour. The round-16 block
rem below is the ONLY place these two are set - edit them THERE.
rem set "RPCS3_REMIX_SUNCARDALBEDO="
rem set "RPCS3_REMIX_SUNTRACK=0"
set "RPCS3_REMIX_SUNTRACKDEG=1"
rem 2 = let the census's highest-in-sky candidate drive the light with no pin.
rem NOT recommended blind: 12 of the 27 shape matches in the last run are HUD
rem quads from vp=2F64C2F8FFD6ADD1, and a HUD quad's "world" position is not a
rem place. Read the census first.
rem
rem === STEP 3: THE HARD YELLOW SQUARE (ITEM 2) ===============================
rem The card renders as a flat opaque rectangle with visible edges because its
rem softness is not in anything this backend replays. Fix, using EXACTLY the
rem machinery round 13 built for the sky dome: give the card's albedo an
rem emissive material with the albedo as its per-texel emissive texture, plus
rem BlendType::kEmissive so the instance goes to the unordered TLAS. For a
rem glare card that is the whole fix and not an approximation of one -
rem calcOpaqueSurfaceMaterialOpacity's kEmissive arm drives opacity to 0 with
rem emissive influence 1, so the dark texels stop being drawn (black added is
rem nothing), the rectangle stops occluding, and only the bright core emits.
rem
rem WHY NOT the per-vertex alpha fold the brief asked for: a fold replays a
rem SPECIFIC fragment program's arithmetic, the way apply_haze_fade replays
rem fp=A20A7A99872B615A. This round could not identify the sun card's fragment
rem program AT ALL (see step 1). Inventing the arithmetic of a program nobody
rem has read is exactly the failure round 11 documented, so it was not done.
rem The census names the fp, and round 15 can decode it if this is wrong.
rem
rem NOTE THE ZERO-REBUILD ALTERNATIVE: adding the same hash to
rem RPCS3_REMIX_SKYEMISSIVE gets the identical treatment today, at the DOME's
rem intensity. This knob exists so the card and the dome can be tuned apart.
rem WATCH FOR: soft round glow instead of a square. If the card VANISHES, its
rem   texture is dark and the intensity is too low - raise SUNCARDINT.
rem READ: mat_suncard= on the live line. It MUST be non-zero once a hash is
rem   pinned; 0 with a non-empty list means the hash never reached
rem   CreateMaterial (wrong hash, or the texture never decoded).
rem REVERT: SUNCARDEMISSIVE=0. Restores the card bit-for-bit.
set "RPCS3_REMIX_SUNCARDEMISSIVE=0"
set "RPCS3_REMIX_SUNCARDINT=2.0"
rem
rem === THE FRAMERATE (ITEM 3) - MEASURED, and it is REAL =====================
rem The two screenshots (42.76 vs 14.29) are NOT a controlled A/B - different
rem vantage, and DLSS/frame-gen settings may differ. But the round-13 run's OWN
rem log contains a controlled one, and it says the drop is real:
rem   * 'Remix live:' is emitted on a 2-second wall clock (the 120-flip bound
rem     never wins), so FPS = (flips delta) / 2 directly from the log.
rem   * The round-13 build hits 43.0 FPS at cam=[53.11 -6.65 177.4] - the SAME
rem     coordinates where the round-12 build measures 42.5-43.0. It is NOT
rem     globally slow.
rem   * It then STEPS to 15.0 FPS at cam=[65.2 -6.83 163] and holds 15.0-15.5
rem     for 285 consecutive heartbeats (~9.5 minutes) with the camera parked.
rem     65 ms/frame, which is not a 60 Hz vsync divisor - a genuine frame cost.
rem   * Across that step, submitted draws/frame FELL (123 -> 93), mesh creates
rem     /frame is the LOWEST of any run (4.53, churn 4.2%), and CPU use fell.
rem     The frame got cheaper on every CPU axis while taking 2.4x longer.
rem   => GPU-side and PER-PIXEL. That is the signature of round 13's change:
rem     an emissive+unordered dome is removed from the primary TLAS, so a sky
rem     pixel's primary ray no longer terminates on it and instead traverses to
rem     tMax=infinity plus an unordered-resolve loop (resolve.slangh:1227-1261,
rem     up to 128 steps). Cost scales with the fraction of screen showing OPEN
rem     SKY, which is exactly what "walked 1.2 units and it halved" looks like.
rem NOT PROVEN: the log has no camera ORIENTATION, so "walked" and "turned to
rem   face the sky" are indistinguishable, and the fast windows are moving while
rem   the slow one is parked. Also unexplained: ui_draws/flip is 41 in the
rem   round-13 run against 9-11 in every earlier one.
rem
rem THE A/B THAT SETTLES IT, and it is unusually clean because the effect is
rem stable at a known spot: park at [65.2 -6.83 163], then
rem   1) set "RPCS3_REMIX_SKYEMISSIVEBLEND=0"   <- keeps the emissive sky, puts
rem      the occlusion BACK. If the frame rate returns, the unordered dome is
rem      the cost. YOU LOSE: the dome occludes the sun again, i.e. round 12's
rem      sunless scene. This is the sun-vs-framerate choice, and it is yours.
rem   2) set "RPCS3_REMIX_SKYEMISSIVE="          <- disables the whole round-13
rem      path. Only if (1) is not conclusive.
rem CHEAPER FIRST, no rebuild and nothing lost - these are rtx.conf/dev-menu
rem options, REPORTED not edited:
rem   rtx.enableUnorderedResolveInIndirectRays = False   (default True)
rem   rtx.enablePSRR = False / rtx.enablePSTR = False    (removes 2 of the 3
rem     unordered call sites; costs reflection/transmission PSR quality)
rem   rtx.enableUnorderedEmissiveParticlesInIndirectRays is ALREADY False by
rem     default - do NOT turn it on, it is the single most expensive switch here.
rem
rem =============================================================================
rem --- round 16 ---
rem cmd 'set' is last-wins, so everything below overrides anything above it.
rem Round 16 ships ONE mechanism: the sun-card pin is now a (vertex program,
rem albedo) PAIR instead of an albedo alone, the same shape SKIPPAIRVP +
rem SKIPPAIRALBEDO already uses. With SUNCARDVP EMPTY the pin is albedo-only,
rem i.e. round 14's rule bit-exactly, so deleting this whole block changes
rem nothing about how anything renders.
rem =============================================================================
rem
rem === WHY THE PIN HAD TO BECOME A PAIR ======================================
rem The warmest card in the whole census is C61753D31FB96507 - pure gold
rem mean_rgb=[1.00 0.80 0.00], 59 vertices, blended, above the horizon. It is
rem also the VISOR's blended layer (round 10), it is pinned right now in
rem VIEWMODELALBEDO, and it is on five lists in rtx.conf. One albedo, two
rem completely different jobs. An albedo-only SUNCARDALBEDO cannot separate
rem them, so pinning it would have aimed the distant sun from the player's own
rem helmet. Hence the vp half.
rem
rem === AND THE CANDIDATE IS NOT THE SUN - MEASURED, round 16 =================
rem Round 16's brief proposed arming SUNCARDVP=f39f504649b6f442 with that
rem albedo and SUNTRACK=1. Its own log refutes it. Over 6,796 'Remix suncard:'
rem lines in the 2026-08-16 01:57 run, C61753D31FB96507 appears under THREE
rem vertex programs and never leaves the player's face:
rem   vp=9f591b6a6b825612  n=151  vtx=59  dist 1.7..3.7  dirspread 43.0 deg
rem   vp=830d7d1b9681c475  n= 65  vtx=59  dist 2.0..3.8  dirspread 37.3 deg
rem   vp=f39f504649b6f442  n= 56  vtx=59  dist 1.7..3.7  dirspread 45.3 deg
rem 272 samples, MAXIMUM distance from the eye 3.76 units, all run, in three
rem programs. Identical vertex count in all three = one mesh drawn three ways.
rem 830d7d1b9681c475 is the known visor program and 9f591b6a6b825612 is a
rem round-1 helmet suspect (it is this file's UVAFFINEVP). A sun does not come
rem within 3.8 units of your head and stay there.
rem SO SUNTRACK IS LEFT AT 0. Aiming from this card would swing the sun with
rem your head and pay a destroy+create per degree of turn. The pair is pinned
rem below anyway, with SUNTRACK=0, because that EXERCISES the new gate at zero
rem risk: suncard_pinned must climb, and it must count ONLY the f39f... draws.
rem === THE SUN CARD IS IDENTIFIED - 2026-08-16, from VIDEO ====================
rem vp=9f591b6a6b825612 + albedo=C61753D31FB96507 IS the game's sun.
rem Round 16 called this family "the visor" because all three programs that draw
rem that albedo sit within 3.76 units of the eye. That reasoning does not hold:
rem a SKY-SPACE sprite is also a few units out, because sky geometry is centred
rem on the camera. Distance cannot separate sky-space from head-space.
rem The video can, and did. Two frames 1.2 s apart during a camera move
rem (05-09-11.mp4 @ t=100.0 and t=101.2):
rem   the static palm frond at frame-left rose ~31 px
rem   the yellow sun square rose ~31 px
rem A head-locked visor overlay would not have moved at all. It tracks the
rem WORLD, so it is the sun. Its colour agrees: mean_rgb is exactly
rem [1.00 0.80 0.00] on every one of its census rows.
rem Of the three programs drawing this albedo, only 9f591b6a6b825612 appears in
rem the video session - 830d7d1b9681c475 IS the real visor program and the pair
rem gate now excludes it, which is the whole reason the pair gate exists.
set "RPCS3_REMIX_SUNCARDVP=9f591b6a6b825612"
set "RPCS3_REMIX_SUNCARDALBEDO=C61753D31FB96507"
rem ARMED: with the pair correct, tracking can aim the distant light at the
rem game's own sun instead of a hand-tuned SUNDIR. Read suncard_pinned climbing,
rem then suncard_elected, then sun_retargeted. If sun_retargeted climbs, SUNDIR
rem below is being overridden and is no longer what aims the light.
rem REVERT: SUNTRACK=0 restores the static SUNDIR aim exactly.
rem
rem !!! DISARMED 2026-08-16 - THE PINNED CARD IS NOT THE SUN. MEASURED. !!!
rem Tracking WORKED mechanically (suncard_pinned=394 elected=394
rem sun_retargeted=187) and that is exactly how it proved itself wrong. The four
rem pinned census rows, taken at four very different world positions:
rem   cam=[-9.43 1.60 -33.84]  dist=2.089  elev=3.35  azim=19.7
rem   cam=[-6.44 1.59 -39.99]  dist=2.088  elev=2.34  azim=-6.28
rem   cam=[ 0.16 1.71 -40.06]  dist=2.083  elev=49.4  azim=-3.44
rem   cam=[10.78 1.78 -25.84]  dist=1.973  elev=2.8   azim=104
rem DISTANCE PINNED AT ~2.0 UNITS EVERYWHERE, DIRECTION SWINGING 100 DEGREES.
rem A sun cannot be 2 metres from the eye at four different world positions.
rem This is a HEAD-LOCKED lens-flare / glare card - round 16 called it the visor
rem family and was right. The "180 degrees out" you saw is the distant light
rem chasing your visor, which points wherever you happen to be looking.
rem My "it tracks the world" call came from two JPEG frames of the video, where
rem a camera PITCH and a camera TRANSLATION look identical. The log's
rem four-sample distance invariant is what settles it.
rem RULE FOR RE-ARMING: never pin a card whose dist= does NOT change as you walk.
rem The real sun card is still unidentified and may not exist as world geometry
rem at all - plenty of titles draw only a view-locked flare, which is exactly
rem what this is. In that case SUNDIR below is the right answer, not tracking.
set "RPCS3_REMIX_SUNTRACK=0"
rem TO ARM THE TRACKER ANYWAY (one edit, no rebuild): delete the 'rem ' below.
rem Expect the sun to follow your head. REVERT by putting the 'rem ' back.
rem set "RPCS3_REMIX_SUNTRACK=1"
rem
rem WATCH on the 'Remix live:' line:
rem   suncard_pinned=  must climb. It is now the PAIR count, not the albedo
rem                    count, so it should be roughly the f39f... share only.
rem   suncardvps=1     echoes the list size - 0 there means the pair gate is
rem                    off and the pin fell back to albedo-only.
rem READ on each 'Remix suncard:' line, new this round:
rem   pinned=  pin_alb=  pin_vp=
rem   pin_alb=1 pin_vp=0  the albedo you named, drawn by a program you did not
rem                       - this is what the two visor twins must now report.
rem   pin_alb=0 pin_vp=1  the program matched, the texture did not.
rem   no row at all       it failed the SHAPE gate (state_glow, vtx <= MAXVTX,
rem                       elev >= MINELEV), not the pin.
rem The shape gate is UNCHANGED and still wraps the pin -
rem   sun_wanted = sun_shape && (sun_pinned || ...)
rem - so a wrong pin still cannot aim anything at a HUD quad.
rem
rem === DO NOT SET SUNCARDEMISSIVE WHILE THIS ALBEDO IS PINNED ================
rem SUNCARDEMISSIVE is applied in CreateMaterial, which is keyed on TEXTURE
rem CONTENT and has no vertex program in scope. It therefore CANNOT be narrowed
rem by SUNCARDVP, and with this albedo pinned it would make the VISOR emissive
rem and unordered. It is 0 above; leave it 0.
set "RPCS3_REMIX_SUNCARDEMISSIVE=0"
rem
rem === THE CENSUS LINE BUDGET - spend it on things that could be a sun =======
rem Minimum distance from the eye for a census LINE. It trims nothing else: no
rem counter moves, the shape gate is untouched, and a PINNED or ELECTED row
rem always prints whatever this says.
rem MEASURED on the same run: 3,394 of 6,796 lines (49.9%) sit closer than 4
rem units - 2,919 of them are 4-vertex HUD quads from vp=2f64c2f8ffd6add1 at
rem dist~2, plus the 272 visor rows above. Nothing beyond 4 units is lost: 210
rem of the 236 distinct (albedo, vp) pairs survive.
rem WHY DISTANCE AND NOT THE EXTENT OR VERTEX FLOOR THE BRIEF ASKED FOR: both
rem would have deleted the best remaining candidates. 1,360 lines (20%) are at
rem dist >= 5 with ext < 1 - including every far card drawn by f352d7dafa72d0e0
rem - and the far 4- and 3-vertex cards (B394C7A092BF1F59 at dist 57.8,
rem 45910AA793CC3B87 at 38.8) are exactly the shape a sun sprite has.
rem REVERT: 0 restores round 14's full, unfiltered census exactly. Do that if
rem the census stops showing anything plausible - a sky-locked sun sprite CAN
rem be drawn at a small camera-relative distance.
set "RPCS3_REMIX_SUNCARDMINDIST=4"
rem
rem === THE SUN IS STILL NOT IDENTIFIED - the shortlist, ready to paste =======
rem No row in the run has a sun's signature (a direction that barely moves).
rem The three least-implausible, all far and camera-locked in distance:
rem   581BEAB612452B52 / f352d7dafa72d0e0  n=23 vtx=12  dist 123.8 EXACTLY,
rem     every sample - a card drawn at a fixed radius, i.e. on the sky. White.
rem   9CA366166DF12A90 / f352d7dafa72d0e0  n=10 vtx=56  dist 122..132
rem     mean_rgb=[1.00 1.00 0.74] - the most sun-coloured far card in the run.
rem   15510B9E2A244CC1 / f352d7dafa72d0e0  n=59 vtx=8   out to dist 123.8
rem To test one: replace BOTH lines above with the pair, keep SUNTRACK=0, and
rem confirm suncard_pinned climbs before arming anything.
rem
rem ==========================================================================
rem === ROUND 19 ============================================================
rem ==========================================================================
rem
rem --- THE VIEWMODEL AXIS FLIP (the one thing to play-test this round) ------
rem The arms and weapon RENDER now, but backwards, upside down and displaced
rem up-and-to-the-right. Three symptoms, one operator: a sign error on a pair
rem of axes. The runtime is NOT the cause - its viewmodel correction works out
rem to a 1.257x widening about the eye with a POSITIVE determinant, so it
rem cannot reverse an axis. The error is in the instance transform, and this
rem flips it back, in the camera's own frame, about the eye.
rem
rem   bit0 (1) = negate the camera's RIGHT axis   -> mirrors left/right
rem   bit1 (2) = negate the camera's UP axis      -> flips upside down
rem   bit2 (4) = negate the camera's FORWARD axis -> puts it behind the eye
rem
rem 6 = up+forward = a 180 degree rotation about the camera's right axis. It
rem is the first guess because it is the ONLY single operator that produces
rem all three reported symptoms at once: upside down (up negated), backwards /
rem facing away (forward negated), and displaced UP while staying on the same
rem side of the screen (right preserved).
rem
rem The axes come from the orthonormal columns of the worldToView, so this is
rem an exact reflection/rotation: a WRONG value cannot rescale, stretch or
rem shear the viewmodel, only reorient it. Trying another value is one edit
rem here and no rebuild. If 6 lands it upside down but facing the right way,
rem try 4; if it is mirrored left-right instead, try 3; 2 is up-only.
rem REVERT: 0 = exactly the round-18 orientation.
rem
rem ROUND 34 - DISARMED TO 0, DELIBERATELY, AND THIS IS STEP 1 OF TWO. Round
rem 19 armed 6 to fix "mirrored, upside down and displaced". Round 34 measured
rem that all three symptoms had one cause and it was NOT handedness: the
rem viewmodel divide collapses to the identity by construction and parks the
rem mesh at the world origin (see VMTAGONLY below). An orientation guess taken
rem against a mesh that is 2129 units from where it should be tells you
rem nothing, and applying 6 on top threw it a further 2.5 km.
rem
rem So: STEP 1 (this build) = VMTAGONLY=1 + VMBASISPIVOT=1 + VMBASIS=0. That
rem restores the placement the world path already gets right and adds ONLY the
rem VIEW_MODEL tag - ONE new variable, which is the discipline round 19's own
rem note asked for. The census still emits, with pre==post, and cdist_pre= is
rem the number to read.
rem STEP 2 (only after step 1 is judged) = set this back to 6. With
rem VMBASISPIVOT=1 that is an in-place 180-degree rotation about the camera's
rem right axis: dbasis= goes non-zero and dcentre= must stay ~0. If the arms
rem look right after step 1, do NOT do step 2.
rem
rem === ROUND 35 - 6 -> 5. THE OPERATOR WAS NEVER BROKEN; ONLY THE TARGET IS. ===
rem The round-35 brief reported that flip=6 "produced a 120-degree yaw about the
rem up axis" and asked for the reflection composition to be rewritten. It did
rem not. That reading projected the transform's ROWS onto the camera axes;
rem remixapi_Transform is COLUMN-VECTOR, so the object's world-space axes are
rem its COLUMNS. Re-projected by script over all 10 flip=6 lines of the
rem 2026-08-17 12:36 run in bin\remix_dump.log:
rem
rem   vp=830d7d1b9681c475 albedo=0721D150DF278E7D vtx=2140 frame=3945
rem     COLUMNS post = (+0.99945 +0.99946 +0.99891)  <- the brief's own pass mark
rem     ROWS    post = (-0.50091 +0.99984 -0.50101)  <- the "120 degrees"
rem
rem The census now prints dotpre=/dotpost= (the COLUMN direction cosines
rem X.right / Y.up / Z.fwd) so this cannot be mis-read a third time.
rem
rem MEASURED: pre is uniformly (right, -up, -fwd) on all 24 tagonly=1 census
rem rows, so the eight flips have eight KNOWN dotpost readings:
rem
rem   flip 0 -> (+1 -1 -1) det +1     flip 4 -> (+1 -1 +1) det -1  MIRROR
rem   flip 1 -> (-1 -1 -1) det -1     flip 5 -> (-1 -1 +1) det +1
rem   flip 2 -> (+1 +1 -1) det -1     flip 6 -> (+1 +1 +1) det +1  <- round 34
rem   flip 3 -> (-1 +1 -1) det +1     flip 7 -> (-1 +1 +1) det -1
rem
rem Your verdict on 6 was "arms are facing the right way, just upside down".
rem Keep the forward axis (+1 in Z.fwd), negate the up axis, and do NOT add a
rem mirror, and exactly one flip is left: 5.
rem
rem WHY NOT 4, which the round-19 note above recommends for this exact symptom:
rem 4 has det -1, i.e. it MIRRORS the arms. det_pre reads +1.00001 on every
rem census line, and the legs and the whole world come through the same world
rem divide and are not mirrored - so the divide preserves handedness and the
rem correction from pre to truth must be a PROPER rotation. Round 19's "try 4"
rem was written before that measurement existed. Superseded.
rem
rem PRE-REGISTERED: dotpost=[-1 -1 +1] on the census, dcentre < 1e-3,
rem cdist_post within 0.01 of cdist_pre.
rem IF STILL WRONG, sweep with the dots as the key - this is one edit, no
rem rebuild:
rem   still upside down, left/right now swapped -> the error IS a mirror: try 4
rem   right way up but now facing backwards     -> try 0
rem   right way up, facing right, wrong side    -> try 6 and report again
rem REVERT: 6 = the round-34 step-2 build you just played.
rem
rem =============================== ROUND 36 ===================================
rem DISARMED (5 -> 0), and the sweep above is CLOSED. Three flips were played
rem and all three read wrong to the user - 0 "upside down and backwards",
rem 6 "facing the right way, just upside down", 5 "still upside down". At flip
rem 6 all three camera-axis dots are POSITIVE, so if the defect were a sign
rem pattern then (+,+,+) would have been correct. It is not. The sign-flip
rem model is refuted; RPCS3_REMIX_VMROTAXIS below replaces it. Setting this
rem back to any non-zero value composes a flip UNDER the rotation - legal, but
rem the two are then confounded and dbasis= / drot= is the only way to split
rem them again.
set "RPCS3_REMIX_VMBASIS=0"
rem
rem Cap for the new 'Remix vmbasis:' census, one line per program. It prints
rem the recovered world matrix BEFORE and AFTER the flip, the camera's three
rem world-space axes, and - the field to actually read - eye_pre/eye_post,
rem the object origin resolved onto right/up/forward in metres. The sign
rem pattern that changes between those two triples names the wrong axes
rem directly, so round 20 does not have to guess again. Needs DIAGLINES=1.
set "RPCS3_REMIX_VMBASISMAX=4000"
rem
rem === ROUND 37: VMBASISEVERY / VMBASISVTX - THE CENSUS COULD NOT SEE TIME ===
rem THIS IS THE MOST IMPORTANT CHANGE OF THE ROUND even though it renders
rem nothing. The vmbasis census deduplicates on (vp, albedo) into a set that
rem lives for the whole session, so each object emits exactly ONE line per
rem session, ever. Round 36 ran 19,200 frames and produced NINE lines across
rem SIX frames. The complaint is that the weapon "does not follow the camera
rem turning smoothly" - a statement about TIME - and an instrument that samples
rem once per object per session structurally CANNOT observe it. Not did not.
rem Cannot. Every round that asked the log about jitter was asking a question
rem the instrument could not answer, and no play-test would have changed that.
rem
rem VMBASISEVERY=1 drops the dedup and emits every frame; VMBASISVTX pins it to
rem one mesh so the budget buys FRAMES instead of objects. 3649 is the gun body
rem (albedo 86885A0E60751491 - the hash VMPAIRALBEDO has held since round 20,
rem and the same one the gun-screen Ctrl+Click returned). 4000 lines is ~4000
rem consecutive frames, about two minutes at 30 fps.
rem IF THE LOG HAS NO "Remix vmbasis:" LINES AT ALL after this run, that mesh
rem was not drawn: set "RPCS3_REMIX_VMBASISVTX=0" and
rem set "RPCS3_REMIX_VMBASISEVERY=4" and take the whole viewmodel at 1/4 rate.
rem WHILE PLAYING: turn left and right for a few seconds so the series contains
rem the motion the complaint is about. Standing still measures nothing.
rem REVERT (no rebuild): set "RPCS3_REMIX_VMBASISEVERY=0"
set "RPCS3_REMIX_VMBASISEVERY=1"
set "RPCS3_REMIX_VMBASISVTX=3649"
rem
rem === ROUND 36: RPCS3_REMIX_VMROTAXIS / VMROTDEG / VMROTPIVOT ===============
rem A REAL ROTATION - axis and angle - because no sign mask can express this.
rem
rem WHAT WAS MEASURED (287 "Remix vmbasis:" lines, last 400 MB of
rem bin\remix_dump.log, the VMBASIS=5 session):
rem   1. The camera basis is LEFT-HANDED on all 287 lines (right x up . fwd =
rem      +1.00 exactly), so the sign of a forward coordinate is readable and is
rem      not an artefact of the instrument.
rem   2. EVERY near-eye VIEW_MODEL-tagged draw is submitted BEHIND THE EYE.
rem      26 distinct (vp,vtx) groups inside 5 units: fwd -0.06 .. -0.81, up
rem      +0.05 .. +0.94. THE CONTROL: the one near-eye group that is NOT tagged
rem      (vp=aae8e0d5ae292dd4 vtx=91) reads fwd +0.08 .. +0.48 - in front.
rem   3. The object basis relative to the camera basis is a rotation about the
rem      camera RIGHT axis of 118..179 degrees (axis dot right >= 0.996 on every
rem      near-eye line), det = +1.00000, column norms 1.0000. Not a yaw, not a
rem      mirror, not a shear.
rem ONE operator produces all three at once: 180 degrees about the camera right
rem axis, PIVOTED AT THE EYE. It sends up -> -up, fwd -> -fwd, right -> right,
rem which is "upside down and backwards" - the exact words for VMBASIS=0.
rem
rem WHY VMBASIS=6 DID NOT FIX IT. At 180 degrees the Rodrigues form collapses to
rem (-I + 2 a a^T), which is bit-for-bit the 3x3 that flip 6 already builds - so
rem the TURN was right all along. The launcher pivoted it at the CENTROID
rem (VMBASISPIVOT=1), so it turned the mesh in place and left it above and
rem behind the eye. Correctly-oriented arms hanging behind your head read as
rem "still upside down". Pivot 0 was not the alternative, because pivot 0 is the
rem ANCHOR-FRAME eye (CAMANCHOREYE=1 is armed at ~:482) which round 34 measured
rem throwing the mesh 2537..2564 units.
rem
rem So VMROTPIVOT=0 here is the RAW m_active_camera.position, no anchor
rem conversion, and that is justified by a number rather than a preference:
rem cdist_pre on the current build reads 0.43..1.04 on all 287 lines, and
rem cdist_pre IS |centroid - m_active_camera.position|. A centroid half a unit
rem from that point cannot be in a different frame from it.
rem
rem AXIS: 0 off; 1/2/3 = camera right/up/forward (left-multiplied, about the
rem pivot); 4/5/6 = the MODEL own X/Y/Z (right-multiplied, about the object
rem origin - the "authored Z-up in a Y-up renderer" lever, which would be
rem AXIS=4 DEG=270). The pivot knob is inert for 4..6 (census: rotpivsrc=5).
rem DEG is taken mod 360 - write 180, NEVER 360 (360 parses to 0 = OFF).
rem PIVOT: 0 raw eye (armed) / 1 centroid, i.e. turn in place / 2 the transform
rem translation / 3 the anchor-frame eye = round 19..35 behaviour.
rem
rem READ IT ON "Remix vmbasis:" IN bin\remix_dump.log (readable while the game
rem runs). PRE-REGISTERED - every one of these can read otherwise:
rem   cpost=[right up fwd] must have fwd > 0 where cpre had fwd < 0, and up < 0
rem     where cpre had up > 0, with right unchanged to ~1e-3.
rem   relpost must be (180 - relpre) +- 1 deg, i.e. 1..62 instead of 118..179.
rem   cdist_post must equal cdist_pre to within 1%. A rotation about the eye
rem     CANNOT change the distance from the eye - if cdist moves, the pivot is
rem     not the eye and the whole construction is wrong.
rem   dcentre 0.3..1.6. ~0 means a centroid pivot leaked back in; ~2550 means
rem     the anchor-frame eye did.
rem   dbasis=0 (VMBASIS is off) and drot ~2 (this operator ran).
rem   knobs= must read vmrotaxis=1 vmrotdeg=180 vmrotpivot=0.
rem IF THE ARMS ARE NOW RIGHT WAY UP BUT AT THE WRONG PITCH, that residual is
rem the 118..179 spread (the rig own aim pitch) and the answer is a smaller
rem angle on the SAME axis - try DEG=156, then DEG=118. One edit, no rebuild.
rem IF THEY ARE NOW UPSIDE DOWN THE OTHER WAY, the model-space hypothesis is
rem live: set "RPCS3_REMIX_VMROTAXIS=4" and "RPCS3_REMIX_VMROTDEG=270".
rem REVERT (no rebuild): set "RPCS3_REMIX_VMROTAXIS=0"
set "RPCS3_REMIX_VMROTAXIS=1"
set "RPCS3_REMIX_VMROTDEG=180"
set "RPCS3_REMIX_VMROTPIVOT=0"
rem
rem === ROUND 37: VMROTPIVOTFWD / VMROTPIVOTUP / VMROTPIVOTRIGHT =============
rem THE WEAPON IS TOO CLOSE, and no knob that existed before this round could
rem reach it. Round 36 read cdist_post == cdist_pre as a success criterion and
rem it IS one - a rotation about the eye cannot change the distance from the
rem eye - but that is also exactly why the round-36 operator can put the weapon
rem in FRONT of you at the wrong distance and have no lever to correct it.
rem
rem THE MODEL. With the pivot at camera-space (p_r, p_u, p_f) measured from the
rem eye, a 180 degree turn about the camera right axis sends a centroid at
rem (r, u, f) to (r, 2*p_u - u, 2*p_f - f). So the corrected model moves TWICE
rem the offset, and that factor of two is a prediction the census can falsify:
rem cpost forward must change by exactly 2 * VMROTPIVOTFWD. If it moves by 1x,
rem the offset is being applied as a translation and the composition is wrong.
rem
rem WHY 0.15 AND NOT A NUMBER PICKED BY EYE. MEASURED on the round-36 session:
rem cpost forward reads 0.09 .. 0.55 across the nine census lines, cdist_post
rem 0.38 .. 1.04. World scale is ~1 unit = 1 metre, measured from the camera
rem itself - between census frames 10013 and 10014 it moved 0.11 units in one
rem frame, and 0.26 over the four frames 9656..9660, i.e. 2.0 .. 3.3 units per
rem second at 30 fps, which is human walk-to-jog speed and nothing else. At that
rem scale the gun origin sits 9 to 55 CENTIMETRES in front of the eye, which is
rem why the gun own screen cannot be read without free-camming backwards.
rem 0.15 pushes it +0.30 m, to roughly 0.4 .. 0.85 m - arm length.
rem
rem THE LADDER, if 0.15 is wrong. One edit each, no rebuild, and each moves the
rem weapon by TWICE the change:
rem   still too close -> 0.25 (+0.50 m), then 0.35 (+0.70 m)
rem   now too far     -> 0.08 (+0.16 m)
rem   weapon too LOW  -> set "RPCS3_REMIX_VMROTPIVOTUP=0.15" (raises +0.30 m).
rem     Left at 0 deliberately: arming both at once confounds them, which is
rem     the doctrine round 36 used for VMBASIS and it held.
rem READ IT ON "Remix vmbasis:": cpost=[right up fwd], and rotpivot= must NO
rem LONGER equal cam= (it did, bit for bit, on all nine round-36 lines).
rem knobs= must read vmrotpivotf=0.15. A comma decimal separator parses to 0.
rem REVERT (no rebuild): set "RPCS3_REMIX_VMROTPIVOTFWD=0"
set "RPCS3_REMIX_VMROTPIVOTFWD=0.15"
set "RPCS3_REMIX_VMROTPIVOTUP=0"
set "RPCS3_REMIX_VMROTPIVOTRIGHT=0"
rem
rem === ROUND 37: RPCS3_REMIX_VMROTLOCK - the residual A/B, SHIPPED OFF =======
rem THE BRIEF ASKED WHY relpost IS NOT 180-relpre. It is an identity of the
rem operator, MEASURED on the nine round-36 census lines to better than 4e-6:
rem     cos(relpre) + cos(relpost) = dotpre[0] - 1
rem A turn about the camera RIGHT axis cannot change any component along RIGHT,
rem so the object own X-vs-right misalignment survives untouched and IS the
rem whole residual - relpost equals acos(dotpre[0]) to within 1.5 degrees on
rem every line. 180-relpre only holds when dotpre[0] is 1, and it DID hold on
rem SIX of the nine (1.70 vs 1.68, 62.18 vs 61.51 three times, 24.65 vs 23.55,
rem 1.79 vs ~0). The three that missed all read dotpre[0] < 0.92.
rem
rem THE STALE-PIVOT HYPOTHESIS IS REFUTED TWICE. rotpivot= equals cam= bit for
rem bit on all nine lines (pivot 0 is m_active_camera.position, NOT the
rem anchor-frame eye - that is pivot 3), and camage=0 on every one of the six
rem census frames, including the 79-degree one. Nothing was stale.
rem
rem SO THE RESIDUAL IS EITHER THE MESH GENUINE POSE OR A SECOND DEFECT, and
rem this is the one-play-test test that separates them:
rem   1 = replace the object 3x3 with the camera basis (scale preserved)
rem   2 = align only the object X axis to the camera right axis
rem relpost reading ~0 afterwards proves only that it composed. THE EVIDENCE IS
rem WHAT YOU SEE: arms look right and stop swimming -> it was a defect, keep it.
rem Arms freeze rigid, lose their aim pitch, hands detach -> it was the genuine
rem pose, put it back to 0 and the residual is not a bug.
rem ONLY TRY THIS IF THE WEAPON STILL DOES NOT FOLLOW THE CAMERA SMOOTHLY after
rem the distance fix above. Two orientation knobs at once confounds them.
rem REVERT (no rebuild): set "RPCS3_REMIX_VMROTLOCK=0"
set "RPCS3_REMIX_VMROTLOCK=0"
rem
rem --- THE VIEWMODEL SELECTOR (armed only if VMBASIS cannot fix it) --------
rem MEASURED, and it refutes a comment that has stood since round 5
rem (RemixGSRender.cpp:5313 "Haze reports the same scale_z/offset_z for every
rem draw in the scene"). In the round-18 run's own viewmodel census - 38
rem lines, one per program, cap never reached, so this is the WHOLE
rem population - 35 programs report scale_z=0.49875 offset_z=0.50125 and
rem THREE report 0.00125/0.00125: 830D7D1B9681C475 (the arms, vtx=3649),
rem 57A12323F22F4988, and 9F591B6A6B825612 (the visor). Their measured
rem distances from the eye are 0.427 / 0.425 / 0.558 world units against a
rem limit of 4. A separate near depth slice DOES exist and it selects exactly
rem the viewmodel.
rem
rem The built-in threshold is 0.001 and the viewmodel's offset is 0.00125 -
rem it misses by 25%. Setting this to 2 (=0.002) lets the DEPTH rule select
rem the viewmodel instead of the albedo list, which matters because the
rem albedo list provably cannot: all four programs it currently tags share
rem albedo C61753D31FB96507, and two of them are NOT the viewmodel - the sky
rem dome AF06F6D32EC048EE and the effects family F39F504649B6F442. A
rem VIEW_MODEL-tagged instance is REMOVED from the world pass by the runtime
rem (rtx_instance_manager.cpp:1562 sets its mask to 0) and redrawn glued to
rem the camera, so anything mis-tagged disappears from the world.
rem
rem LEFT AT 0 DELIBERATELY: arming it the same round as VMBASIS would change
rem two things at once and neither result could be attributed. Try VMBASIS
rem first. REVERT: 0 = use the built-in constant = round-18 behaviour.
rem
rem ROUND 34 - ARMED AT 2, AND THE ROUND-33 TEST IS NOW EXPLAINED. Round 33
rem set this to 2, the user play-tested it, and the arms and weapon VANISHED
rem entirely. That run's log is bin\log\RPCS3.log (ended 2026-08-17 09:37:23)
rem and it holds the whole answer:
rem   * vm_tagged=7960 of vm_considered=632100, so the tag DID fire and the
rem     draws DID reach the runtime - "the depth route does not work" is wrong.
rem   * The 'Remix vmbasis:' census emitted for the FIRST TIME EVER, six lines.
rem     Every doc block in the tree calling it "the measurement no census has
rem     ever emitted" was stale from that moment and nobody read the log.
rem   * On the arms draw (vp=830d7d1b9681c475 albedo=86885A0E60751491
rem     vtx=3649): pre translation [0.0555 -0.2443 0.00686] - THE WORLD ORIGIN
rem     - with cam=[1774.14 -31.2479 1177.04], and post translation
rem     [158.622 -59.112 2563.8]. All six lines moved 2537..2564 units.
rem
rem So the arms were thrown 2.5 km away, TWICE over: first by the viewmodel
rem divide (which is self-referential - see VMTAGONLY) and then by VMBASIS
rem reflecting about an eye 2129 units from the mesh. Both are fixed below, so
rem this is armed again. REVERT: 0 = the built-in 0.001 constant, which the
rem viewmodel's 0.00125 misses by 25%, i.e. nothing is ever tagged.
set "RPCS3_REMIX_VMDEPTHOFFSET=2"
rem
rem --- ROUND 34: THE PIVOT. This is the fix VMBASIS needed since round 19. --
rem VMBASIS reflects about the EYE, on the premise that first-person geometry
rem sits AT the eye so a reflection there is a pure reorientation. The census
rem above measures that premise false: the arms' instance transform has its
rem translation at the WORLD ORIGIN, 2129 units from the eye, so reflecting
rem about the eye is a 2.5 km translation with a rotation attached.
rem   0 = the eye (round 19..33, bit for bit)
rem   1 = the draw's own geometry centroid -> rotates in place, moves nothing
rem   2 = the transform's translation column (wrong HERE, since that is the
rem       world origin; provided to separate "modelled about the origin" from
rem       "modelled about the centroid" in one run)
rem
rem READ THE CENSUS, NOT THE SCREEN: dbasis= must be NON-ZERO (the operator
rem ran) and dcentre= must be ~0 (it did not displace the mesh). At pivot 0
rem this title reads dcentre ~2550. cdist_pre= is the number that says whether
rem the arms are in front of the player at all - the untagged world path
rem measures 0.40 and 0.44 units on 'Remix picked:' lines for the two
rem first-person albedos, so ~0.4 is right and ~2129 is the relocation.
rem REVERT: 0.
set "RPCS3_REMIX_VMBASISPIVOT=1"
rem
rem --- ROUND 34: TAG THE VIEWMODEL WITHOUT MOVING IT. The bigger half. -----
rem Tagging a draw VIEW_MODEL currently does FOUR things in per_draw_transform
rem and only one of them is the tag. 1 severs the three placement ones:
rem   * the divide by m_active_viewmodel.reference_inverse,
rem   * 'defer_candidate = false',
rem   * the REFUSED:noref / REFUSED:mode1 arm, which DROPS the draw,
rem   * and '&& !viewmodel_draw' disarming the tail-rescue ladder - which is
rem     where PROJSPLIT lives, and is the same gate round 28 caught deleting
rem     the arms through the VMPAIRVP route.
rem
rem WHY THE VIEWMODEL REFERENCE IS WRONG, MEASURED. It is latched as
rem inverse(folded) from the FIRST viewmodel-depth draw of the frame, and the
rem comment beside the latch says the population is expected to share one
rem view-projection. So world = fused x reference_inverse divides a matrix by
rem its own inverse and collapses to the identity BY CONSTRUCTION - the same
rem self-referential trap round 32 caught in worldid-census tmax. Two of the
rem six census lines have a bit-near-exact identity 'pre' and all six sit
rem within 0.4 units of [0 0 0].
rem
rem AND THE WORLD DIVISOR IS MEASURABLY CORRECT FOR THESE DRAWS. Untagged
rem 'Remix picked:' lines for the same program in the same level read
rem origin=[1780.78 -31.2362 1186.65] vs cam=[1780.42 -31.3929 1186.57] and
rem origin=[1780.75 -31.1536 1186.66] vs cam=[1780.42 -31.3948 1186.57] -
rem 0.40 and 0.44 units from the eye, basis=[0.998796 0.99919 1.00065] and
rem [1.00293 1.00239 1.0015]. That RETIRES round 5's justification for the
rem viewmodel reference ("dividing by the world camera collapses the basis to
rem 0.491, 0.002, 1.150") - against current bytes that quantity reads unity,
rem because the f64 gauge, the anchor election and PROJSPLIT all landed after
rem round 5 measured it.
rem
rem PRE-REGISTERED REFUTATION: with 1 set, vm_tagged stays non-zero while
rem vmcam_considered goes to ZERO (the whole block is skipped), and the
rem census's cdist_pre= must read ~0.4 instead of ~2129. If cdist_pre stays at
rem ~2129 then the relocation is NOT the viewmodel divide and this knob is the
rem wrong lever - say so and stop.
rem REVERT: set "RPCS3_REMIX_VMTAGONLY=0"
set "RPCS3_REMIX_VMTAGONLY=1"
rem
rem --- THE PROJ_SPLIT PREMISE TEST (measuring only this round) -------------
rem Round 18 reported proj_split=74552/0/0 and read the zero refusals as the
rem fix being sound. It is not evidence of anything: the is_affine gate on
rem that construction CANNOT refuse. Both splits come out of try_split_once,
rem which builds its view from an orthonormal basis, so the recovered world
rem is V_draw * V_anchor^-1 - rigid, hence affine, hence accepted no matter
rem how wrong the premise. refused=0 is structural, which is also why applied
rem came out bit-identical to tail_split_ok both times.
rem
rem The premise the gate never tested is V_draw == V_anchor. vdelta= on every
rem 'Remix tail-rescue: ... outcome=projsplit' line now measures it, and this
rem knob can refuse on it. LEFT AT 0 = no gate = round-18 behaviour exactly,
rem so this run reports the distribution before anything is refused on it.
rem Read vdelta= from the log, THEN pick a threshold (units are L1 x 1000).
set "RPCS3_REMIX_PROJSPLITVDELTA=0"
rem
rem === ROUND 20: THE (vp, albedo) PAIR GATE FOR THE VIEWMODEL =================
rem This is the replacement for the blanked VIEWMODELALBEDO above. A draw is
rem tagged VIEW_MODEL only if its vertex program is on VMPAIRVP *AND* its
rem albedo is on VMPAIRALBEDO. Same shape as SUNCARDVP + SUNCARDALBEDO.
rem Empty EITHER list = the route is off = exactly the behaviour you have now.
rem
rem WHY NOT JUST USE VIEWMODELVP. Because it does not mean what it looks like.
rem classify_viewmodel_depth returns "tagged" IMMEDIATELY for anything on
rem VIEWMODELVP, ahead of both depth tests, and that verdict is ORed into the
rem tag - so listing 830D7D1B9681C475 there tags EVERY draw of that program
rem whatever its albedo. That program is also GUESTLIGHTVP, so it would drag
rem the ceiling light fixtures into the viewmodel camera and undo round 5's
rem lighting. This route never touches the depth classifier, so it cannot.
rem
rem !!! THIS WATCH INSTRUCTION IS STALE AS OF ROUND 28 - DO NOT FOLLOW IT !!!
rem It says to judge the arms by vm_tagged_pair being non-zero. Since round 28
rem BLANKED VMPAIRVP, viewmodel_pair_matches() returns false unconditionally
rem (RemixTransforms.cpp:12302), so vm_tagged_pair and vm_pair_far are
rem STRUCTURALLY 0 FOREVER. Reading 0 there now means the knob is doing its job,
rem not that the pair failed - the exact inversion of the text below.
rem Blanking VMPAIRVP was the ARMS FIX, not a disarm: the pair listing was
rem gating rescue_valid and thereby refusing the arms before PROJSPLIT could run.
rem JUDGE THE ARMS INSTEAD BY: 'Remix world-refused:' lines with vp=830d7d…/
rem f39f50… and vtx=3649 DISAPPEARING, and a 'Remix tail-rescue:' line with
rem vtx=3649 outcome=projsplit APPEARING. See the round-28 block below (~:2328).
rem REVERT (restores the refusal, no rebuild):
rem   set "RPCS3_REMIX_VMPAIRVP=830D7D1B9681C475,F39F504649B6F442"
rem The line below is the OLD revert and is wrong now - blanking is the fix, so
rem "reverting" to blank is a no-op. Left visible only so the two do not conflict
rem silently.
rem OLD, WRONG: set "RPCS3_REMIX_VMPAIRVP="
rem 2026-08-16: ARMS ADDED. Round 21's per-frame attribution from
rem 'Remix fpcandidate:' is unambiguous: the arms mesh 86885A0E60751491
rem (vtx=3649) is drawn by f39f504649b6f442 in 26 OF 26 SAMPLED FRAMES and by
rem 830d7d1b9681c475 in ZERO. Same for the weapon 0721D150DF278E7D (vtx=2140).
rem The only 830d+arms lines in the whole run are 'world-refused fail=tail'.
rem So the pair gate as first armed tagged a head-locked card and one small
rem weapon mesh and NEVER the arms - which is why tagging them changed nothing.
rem NOTE this also means f39f504649b6f442 is NOT "the effects family": every
rem albedo it draws is arms, weapon, gear or the flare card, largest non-card
rem extent 0.93, all at eye_dist 0.4-4.0. Not one smoke plume. That label rode
rem through four round briefs unchallenged, and FXREFVP still points at it.
rem
rem !!! REVERTED SAME SESSION - IT COST THE NPC SOLDIERS THEIR BODIES !!!
rem User: "sometimes at this distance the soldiers body disappears and only
rem show their heads". Measured cause: f39f504649b6f442 draws the pinned albedo
rem 0721D150DF278E7D at THREE different vertex counts - 2140, 2846 and 952 -
rem i.e. three different meshes sharing one texture. A VIEW_MODEL tag pulls the
rem instance out of the world pass, so the pin was dragging NPC body meshes onto
rem the viewmodel camera and deleting them from the scene.
rem The scale says it too: vm_tagged_pair went 123 -> 20990 when I widened this,
rem with ~10.6k surviving the anchor guard. A pair of hands is not 10,600 draws.
rem Round 21's attribution still STANDS - the arms really are drawn by
rem f39f504649b6f442, in 26 of 26 sampled frames. The lesson is that on this
rem title program+albedo STILL does not isolate the player, because both are
rem shared. The discriminator nobody has used yet is DISTANCE: the arms sit
rem 0.43-0.47 units from the eye, NPC bodies are metres away.
rem 2026-08-16 FINAL NARROWING - the ALBEDO list was the culprit, not the VP
rem list, which is why reverting the VP change did not stop the bodies vanishing.
rem Who draws each pinned albedo, measured from the run:
rem   1BF8325ADEF3C986 -> 830d7d1b9681c475, af06f6d32ec048ee, f39f504649b6f442,
rem                       edc10321bf7ceb8f          SHARED, 4 programs
rem   0721D150DF278E7D -> 830d7d1b9681c475 @ vtx 2846 and 952
rem                       f39f504649b6f442 @ vtx 2140 and 952   SHARED
rem   86885A0E60751491 -> f39f504649b6f442 @ vtx 3649 ONLY      *** UNIQUE ***
rem 830d7d1b9681c475 draws CHARACTER meshes on 0721D150DF278E7D, so with that
rem albedo pinned a soldier is tagged VIEW_MODEL the moment he comes inside the
rem VIEWMODELANCHOR=4 radius - and a tagged instance leaves the world pass.
rem That is exactly "the body disappears when getting close", and it survived
rem reverting the VP list because the albedo half was doing the damage.
rem 86885A0E60751491 is the ONLY unambiguous first-person hash in the dataset:
rem one program, one vertex count, nothing else anywhere. Pinning the unique
rem pair tags the arms with zero collateral. The weapon (0721D150DF278E7D at
rem vtx=2140) is deliberately NOT pinned - that albedo is shared with the
rem character meshes and is what caused this. Add it back only with a distance
rem gate in front of it.
rem *** 2026-08-16 ROUND 25: RETARGETED. Changing an existing value; measurement below and
rem at VMPAIRALBEDO. This restores 830D7D1B9681C475 alongside the round-23 value rather than
rem replacing it, which is exactly the two lines this file already wrote out at ~:2409.
rem
rem ############################################################################
rem ### 2026-08-17 ROUND 28: THIS VALUE IS NOW BLANK. IT IS THE ARMS FIX.     ###
rem ############################################################################
rem I am CHANGING AN EXISTING VALUE and here is the measurement that justifies it.
rem
rem This pair listing was not merely inert - it was the thing REFUSING the arms.
rem   RemixGSRender.cpp:15576   if (tail_rescue_enabled() && gauge_anchor_enabled()
rem                                 && !viewmodel_draw && ...) rescue_valid = true;
rem `viewmodel_draw` is set true by viewmodel_pair_matches(vp, albedo), which is the
rem cross product of THIS line and VMPAIRALBEDO - i.e. exactly (830D7D1B9681C475 |
rem F39F504649B6F442) x 86885A0E60751491, which is exactly the arms. rescue_valid
rem gates the WHOLE tail-rescue ladder, and PROJSPLIT is nested inside it. So the
rem arms were the one population forbidden from the repair that fixes them.
rem
rem MEASURED, newest run (62,221 flips), from bin\remix_dump.log:
rem   * every 'Remix world-refused: fail=tail' line on these two programs at albedo
rem     86885A0E60751491 reads rescue=n/a - the initialiser. The ladder never ran.
rem     830d: 47 lines, f39f: 165 lines, residues 3.133 - 5.755, vtx=3649 only.
rem   * the SAME programs' other draws DO reach the ladder and PROJSPLIT FIXES THEM:
rem       vp=830d7d1b9681c475 vtx=2140 residue 3.29808 -> 1.9736e-06
rem       vp=830d7d1b9681c475        residue 3.48264 -> 2.99074e-08
rem     114 projsplit lines on 830d, 179 on f39f, ZERO outcome=failed on either.
rem   * across the whole log, 1,262 rescue=n/a lines on these two programs and every
rem     single one sits on a VMPAIR albedo. No other albedo ever produces rescue=n/a
rem     on them. The correlation is total.
rem   * what the pair listing was BUYING: vm_tagged=0 in this entire 62,221-flip run
rem     (it was 1 in 72,000 frames in round 26). It tags nothing. It only refuses.
rem
rem PREVIOUS VALUE, restore this line to undo, no rebuild:
rem   set "RPCS3_REMIX_VMPAIRVP=830D7D1B9681C475,F39F504649B6F442"
rem VMPAIRALBEDO below is left at 86885A0E60751491 UNCHANGED - blanking either half
rem disarms the pair, and leaving the albedo makes the revert a one-line edit.
rem
rem WATCH: 'Remix world-refused:' lines with vp=830d7d1b9681c475 / f39f504649b6f442
rem   and vtx=3649 should DISAPPEAR, and 'Remix tail-rescue:' lines with vtx=3649
rem   and outcome=projsplit should appear. proj_split=<applied>/... climbs.
rem   *** DO NOT judge this by vm_tagged_pair= or vm_pair_far=. Both are now structurally
rem   0 forever, by design. Every 'WATCH' line further down this section that names them
rem   is STALE and would have you revert a working change. ***
rem SIDE EFFECT, stated because it is undocumented anywhere else: blanking this also
rem   disarms the VIEW_MODEL CAMERA. vm_tag_route_armed has four terms (VIEWMODELALBEDO,
rem   this pair, VIEWMODELVP, VMDEPTHOFFSET) and this launcher now makes all four false, so
rem   the REMIXAPI_CAMERA_TYPE_VIEW_MODEL submit stops happening. That is harmless HERE only
rem   by coincidence: Haze's depth rule rejects every draw anyway (measured offset_z=0.00125
rem   against a 1e-3 threshold), so no-tag plus no-camera is self-consistent and nothing is
rem   stranded in a pass that does not exist. On any other title, check that before copying
rem   this line.
rem RISK, stated: PROJSPLIT's construction assumes the draw's view equals the
rem   anchor's, and that premise is UNTESTED here - vdelta measured 2072-4878 over
rem   840 projsplit lines with the PROJSPLITVDELTA gate disarmed. is_affine cannot
rem   refuse the result, so a clean residue proves nothing about placement. If the
rem   arms now appear somewhere absurd, that is this, and restoring the line above
rem   puts them back to invisible.
set "RPCS3_REMIX_VMPAIRVP="
rem
rem !!! READ THIS BEFORE TRUSTING THE ALBEDO ON THE NEXT LINE !!!
rem The round-20 brief named 1BF8325ADEF3C986 as "the opaque viewmodel body,
rem vtx=1446". MEASURED against the round-19 run's own 'Remix fpcandidate:'
rem census, that is NOT the arms:
rem   1BF8325ADEF3C986  vtx=1446  ext=3.499  eye_dist=1.684   <- a 3.5-unit
rem                     card 1.7 units in front of the eye, drawn by all
rem                     THREE programs (sky dome, effects, viewmodel)
rem   86885A0E60751491  vtx=3649  ext=0.862  eye_dist=0.465   <- the arms
rem   0721D150DF278E7D  vtx=2140  ext=0.980  eye_dist=0.437   <- the weapon
rem The last two are also the ONLY draws on this program in the near depth
rem band (scale_z=0.00125 offset_z=0.00125); everything else, 1BF8... included,
rem is in a world/full-range band. Both were reported vmlisted=0, i.e. the old
rem albedo pin never selected the actual first-person geometry at all - which
rem is the complete reason VMBASIS=6 changed nothing you could see.
rem
rem So all three are listed. 1BF8325ADEF3C986 is the brief's pair and is kept
rem so its result stays attributable; the other two are the measured arms and
rem weapon. If the arms now render glued to the camera but the 3.5-unit card
rem is in the way, delete 1BF8325ADEF3C986 from this line - no rebuild.
rem REVERT: set "RPCS3_REMIX_VMPAIRALBEDO="
rem 2026-08-16: cut to the ONE albedo that is not shared - see the VMPAIRVP
rem block above for the per-program measurement. Dropped:
rem   1BF8325ADEF3C986   drawn by 4 programs incl. the sky dome program
rem   0721D150DF278E7D   drawn by 830d7d1b9681c475's CHARACTER meshes - this is
rem                      the one that was deleting soldiers' bodies at close range
rem Kept 86885A0E60751491: f39f504649b6f442 @ vtx=3649, and nothing else in the
rem entire run draws it. Pair it with VMPAIRVP=F39F504649B6F442 above and the
rem gate matches the player's arms and NOTHING ELSE.
rem WATCH: vm_tagged_pair should be SMALL and steady - hundreds, not the 20990
rem the widened list produced. NPC bodies must stay solid at any range.
rem *** 2026-08-17 ROUND 28: STALE. VMPAIRVP is blank, so this albedo is inert and
rem vm_tagged_pair is structurally 0. Left armed on purpose so restoring VMPAIRVP is a
rem one-line revert. See the block at the VMPAIRVP set line. ***
rem REVERT (no rebuild): set "RPCS3_REMIX_VMPAIRALBEDO="
rem *** 2026-08-16 ROUND 25: RETARGETED, and this is THE measurement that justifies both
rem halves. VMPAIRVP/VMPAIRALBEDO are comma-separated LISTS matched as a CROSS-PRODUCT
rem (RemixTransforms.cpp:12282/12289, two independent std::find), so two vps x two albedos
rem is four combinations, not two pairs.
rem   * The user Ctrl+Clicked their own weapon and their own arms. BOTH are
rem     vp=830d7d1b9681c475, albedos 0721D150DF278E7D (weapon) and 86885A0E60751491 (arms).
rem   * The round-23 retarget to F39F504649B6F442 alone is why the route died: every line
rem     carrying f39f + 86885A is a 'Remix world-refused: fail=tail persp_residue=3.30..4.42'
rem     (11 of 11), i.e. refused at RemixGSRender.cpp:17040, long before the tag site at
rem     :18543. This file's own note at ~:2382 records the before/after: with
rem     VMPAIRVP=830D7D1B9681C475 round 21 measured vm_tagged_pair=123; rounds 22 and 23,
rem     after the retarget, measured 1 and 0. Nothing was wrong with the route.
rem   * Cross-product exposure, MEASURED over 261 'Remix fpcandidate:' rows of the new lists:
rem       830d7d x 86885A  n=26   eye 0.385..0.686  ext<=0.86   ALL inside 2
rem       830d7d x 0721D1  n=98   eye 0.227..63.45  ext<=20.2   84 inside 2, 9 in (2,4], 5 >4
rem       f39f50 x 86885A  n=0
rem       f39f50 x 0721D1  n=137  eye 0.371..3.97   ext<=1.27   125 inside 2, 12 in (2,4]
rem     So 235 tag and 26 are clipped - by VMPAIRMAXDIST=2 for the (2,4] band and by
rem     VIEWMODELANCHOR=4 beyond that. The pair route is the ONLY viewmodel route
rem     VMPAIRMAXDIST actually bounds (this file, ~:2404), which is why it was chosen over
rem     the depth route below.
rem *** 2026-08-17 ROUND 28: EVERY 'WATCH' INSTRUCTION IN THIS SECTION IS NOW STALE. ***
rem VMPAIRVP is BLANK (see the block at its own `set` line, ~:2300). With either half of
rem the pair empty, viewmodel_pair_matches() returns false unconditionally, so
rem 'vm_tagged_pair=' and 'vm_pair_far=' are STRUCTURALLY 0 FOREVER. The instructions above
rem tell a play-tester to read 0 as failure and "revert immediately" - do NOT. Reading 0 on
rem those two counters is now the EXPECTED, CORRECT state and is not evidence of anything.
rem What to watch instead is at the VMPAIRVP set line: 'Remix world-refused:' lines with
rem vtx=3649 disappearing and 'Remix tail-rescue: ... vtx=3649 outcome=projsplit' appearing.
rem THE ONLY CORRECT REVERT for VMPAIRVP is the round-25 value, which is the one written
rem beside its own set line:  830D7D1B9681C475,F39F504649B6F442
rem The line below offered F39F504649B6F442 alone - that was round 23's value, which round 25
rem measured as the reason the route died (11 of 11 lines refused). Two conflicting revert
rem targets for one knob is how a revert makes things worse; this is the wrong one.
rem SUPERSEDED REVERT, kept only so the round-23 value is not lost:
rem   set "RPCS3_REMIX_VMPAIRVP=F39F504649B6F442"
rem
rem !!! 0721D150DF278E7D REMOVED - SAME REGRESSION, THIRD TIME !!!
rem The warning two lines above called it exactly: "revert immediately if soldier
rem bodies vanish". Reported this run: "Legs are now facing the same orientation
rem as the weapon, but I have no arms" and "npc's body disappears when you get
rem too close". That is character geometry being pulled into the viewmodel pass.
rem The scale gives it away on its own: vm_tagged_pair=35627. A pair of hands and
rem one gun is not 35,000 draws.
rem 0721D150DF278E7D is drawn by 830d7d1b9681c475 on MULTIPLE character meshes
rem (measured at vtx 2846, 2140 and 952) as well as on the player's weapon, so as
rem a bare albedo on that program it can never be safe.
rem 86885A0E60751491 stays - it is the only first-person hash in the dataset that
rem exactly one program draws at exactly one vertex count.
rem THE WEAPON IS THEREFORE UNTAGGED FOR NOW, deliberately: losing the gun's
rem placement costs less than deleting NPC bodies and the player's own legs.
rem To bring it back it needs a discriminator that is NOT the albedo. The
rem fragment program is the candidate, but it is UNRESOLVED: the user's picks of
rem the weapon AND the arms both read fp=0ccd70030837ee85, while round 25
rem measured the arms on fp=8d820cc431042b23 in a different run. Settle that
rem disagreement before re-arming anything here.
rem REVERT: append ,0721D150DF278E7D - and expect the legs back with it.
set "RPCS3_REMIX_VMPAIRALBEDO=86885A0E60751491"
rem
rem === ROUND 21 ==============================================================
rem !!! THE PAIR ABOVE IS AIMED AT THE WRONG PROGRAM. MEASURED THIS ROUND. !!!
rem In the round-20 build's own run, 'Remix fpcandidate:' attributes the arms
rem mesh 86885A0E60751491 (vtx=3649) to vp=f39f504649b6f442 in 26 of 26 sampled
rem frames and to vp=830d7d1b9681c475 in ZERO. The weapon's main mesh
rem 0721D150DF278E7D vtx=2140 is the same: 25 frames on f39f..., 0 on 830d...
rem (830d... draws a DIFFERENT weapon mesh, vtx=952, in 4 frames). So
rem VMPAIRVP=830D7D1B9681C475 tags the head-locked card and one small weapon
rem mesh and never touches the arms. That is why vm_tagged_pair=123 and the
rem arms did not move.
rem THE ONE-LINE FIX, no rebuild - add the program that actually draws them:
rem   set "RPCS3_REMIX_VMPAIRVP=830D7D1B9681C475,F39F504649B6F442"
rem It stays a PAIR gate, so it can only ever tag f39f... draws whose albedo is
rem one of the three above - not the effects or world geometry that program
rem also draws. It is left UNARMED here only because this round was told not to
rem change an existing value.
rem
rem ALPHACENSUS: cap for the new per-albedo 'Remix alphastate:' line, 0 = off.
rem It prints, for each distinct albedo, the exact remixapi_InstanceInfoBlendEXT
rem this backend shipped AND the verdict the runtime derives from it -
rem rt_fullyopaque=1 is "opacity forced to 1.0, hard-edged square",
rem rt_blendingdisabled=1 is "opacity binarised to 0 or 1, hard-edged shape".
rem This is the instrument the sun-card and missing-effects questions needed and
rem no round has had. Read it for C61753D31FB96507 (the yellow sun card) first.
rem Costs one hash-set insert per NEW albedo and nothing thereafter.
rem REVERT: set "RPCS3_REMIX_ALPHACENSUS=0"
set "RPCS3_REMIX_ALPHACENSUS=192"
rem
rem === ROUND 23 ==============================================================
rem
rem --- 1. A PER-LEVEL SUN ----------------------------------------------------
rem The backend has always aimed ONE sun (SUNDIR, above) at every level. The sky
rem is per-AREA and the backend already watches its albedo change, so the dome's
rem albedo hash is a per-level key that needs no signal from the game.
rem
rem MEASURED OFFLINE, and this is what makes the whole thing possible: the sun is
rem PAINTED INTO the dome texture. bin\remix_tex\unit0_D1A6D1B27ADE6232_2048x1024.bmp
rem (the Selva dome, dumped in an earlier round) has its peak Rec.709 luma 249.2
rem - rgb 255,251,214, a warm white - at texel (1340,513); 112 texels sit within
rem 2%% of that peak and their centroid is uv (0.6769, 0.5015); the top half of
rem the image (v < 0.5) is solid black, i.e. the panorama occupies v in [0.5,1].
rem It is a broad glow, not a hard disc (ZERO texels reach luma 250), which is
rem why the code uses the luminance-weighted centroid of the near-peak texels.
rem
rem SUNSKY=1 turns on the automatic derivation: texture_cache::upload measures
rem that centroid once per dome upload, and at the dome's first draw the backend
rem finds the dome VERTEX whose texcoord is nearest it and takes the world ray
rem from the eye to that vertex. Solved ONCE per dome albedo, then republished
rem every time that dome is drawn - so walking into a new area re-aims the sun on
rem the first frame the new dome appears.
rem
rem WHAT TO READ: 'Remix sunmap:' in bin\remix_dump.log (and RPCS3.log). It
rem prints skyalbedo=, the derived travel=[x y z], the peakuv= it used, uverr=
rem (how close the best vertex's texcoord got - large means the dome's UVs do not
rem cover the bright region and the answer is a guess), vtx=, a centroid_travel=
rem cross-check computed from the dome's own centroid instead of the eye, and
rem sundir= (what SUNDIR would have given). Also 'Remix sun-retarget: src=sky'.
rem
rem SUNMAP is the config file the request asked for, and it OVERRIDES the
rem automatic answer. Copy the travel= vector out of 'Remix sunmap:' (or aim it
rem by eye) and paste it in, one entry per level:
rem   set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:-0.365,-0.9,0.228;CDFE11B12552EA2D:-0.2,-0.85,-0.5"
rem Vectors need not be unit length; they are normalised in code. The convention
rem is the same as SUNDIR: the direction the light TRAVELS, sun -> scene.
rem Precedence: SUNMAP > SUNSKY > SUNTRACK(card) > SUNDIR. SUNDIR still covers
rem every level that matches nothing, so a level with no entry is unchanged.
rem
rem ONLY albedos on RPCS3_REMIX_SKYEMISSIVE are measured or matched - that list
rem is the per-level key. Add a new area's dome hash there first.
rem REVERT: set "RPCS3_REMIX_SUNSKY=0"   (and blank SUNMAP)
rem
rem *** 2026-08-16 ROUND 25: ARMED for Selva, and the reason is that the derivation CANNOT
rem succeed on this dome - not at 98, not at 90, not at any threshold. This was empty, so
rem this is an arming, not a value change.
rem
rem MEASURED, offline, from bin\remix_tex\unit0_D1A6D1B27ADE6232_2048x1024.bmp (top-down
rem DIB, biHeight=-1024, no flip anywhere in the chain; my offline 98% centroid reproduces
rem the live peakuv=[0.67693 0.50146] exactly, so the arithmetic below is the shipped code's):
rem   * v is NOT inverted. v=1.0 is the zenith pole (row 1020 is horizontally constant,
rem     std=0.010, RGB exactly 148,170,202); rows 0..511 are the unused black hemisphere.
rem     Flipping v would send the carrier dome from +22.5deg to +58.3deg elevation and would
rem     move Selva nowhere. THE ROUND-24 BRIEF'S "inverted axis" HYPOTHESIS IS REFUTED.
rem   * The 98% window is 112 texels ALL ON ROW 513 - a one-row resampling overshoot on the
rem     band edge (row 512 max 162.75, row 513 max 249.18, row 514 max 230.25). The set stays
rem     confined to that single row for EVERY threshold from 100 down to 93.
rem   * Excluding row 513 entirely and re-measuring at 98 gives v=0.51668 = +3.00deg, still
rem     below the +6.54deg the guard needs. So a "drop the spike row" rule does not fix it.
rem   * THE REAL FINDING: there is no sun in this texture. The luma profile down the glow's
rem     own column falls MONOTONICALLY from 222 at +1.5deg to 170 at +50deg with no local
rem     maximum anywhere. It is a broad hazy horizon glow, not a disc. SUNSKYPEAKFRAC=86 is
rem     the first value that clears the horizon guard, but it clears it by dragging a centroid
rem     through a smooth gradient - that is a guess wearing a measurement's clothes, and the
rem     threshold is SHARED with the carrier dome whose texture was never dumped so the risk
rem     to it cannot be measured. SUNSKYPEAKFRAC IS THEREFORE LEFT AT 98 (see the end block).
rem
rem THE VECTOR. Azimuth is MEASURED: the derivation's own centroid_travel for this dome is
rem [0.6193 -9.13e-09 -0.78515] - y is float-noise zero, i.e. the horizon bearing at the
rem glow's azimuth, and it is the only calibrated u->direction mapping available offline.
rem Elevation is CHOSEN at 25 degrees, because the texture has no elevation to measure.
rem   travel = [0.6193*cos25, -sin25, -0.78515*cos25] = [0.56127, -0.42262, -0.71159], |v|=1.
rem SUNMAP is exempt from the horizon guard by design (RemixGSRender.cpp:3799) and wins over
rem the derivation outright (:3645-3662, source=2). The carrier dome is deliberately NOT
rem listed, so it keeps deriving its own correct answer (uverr=0.035, travel y=-0.383).
rem WATCH: 'Remix sunmap: skyalbedo=D1A6D1B27ADE6232 src=map' with mapped=1, and NO
rem 'Remix sunrefuse:' line for it. If Selva's shadows point the wrong way, the elevation or
rem the azimuth sign is what to turn - change the numbers here, no rebuild.
rem REVERT: set "RPCS3_REMIX_SUNMAP="
rem === 2026-08-17: THE SUN IS NOW READ FROM THE GAME'S OWN LEVEL FILES ========
rem The PBCK archives were cracked (20-byte index entries, per-chunk zlib;
rem 143,203 entries inflated with zero failures across all 35 paks). Each level
rem pak carries a scene-settings block tagged k_scene_sun, and two properties in
rem it move together in all 203 blocks measured:
rem   0x361b  full circle, 18 distinct values 0..354.375   -> AZIMUTH degrees
rem   0x3624  4.75 .. 18.5                                 -> ELEVATION degrees
rem   0x3631  warm/white, R>=G>=B in 173 of 179, never blue -> SUN COLOUR
rem Levels sharing a skydome share (az, el) exactly - three different paks using
rem obs_sky all read 337.5. That is what makes them the sun and not noise.
rem
rem crashed_plane (the level whose location string is TROPICALJUNGLE - "Selva" is
rem our label, it appears nowhere in the game data) authors:
rem   az=129.0  el=16.5  sun rgb (0.880, 0.800, 0.706)  ambient (0.404,0.522,0.624)
rem
rem *** THE TWO INDEPENDENT METHODS AGREE ON AZIMUTH TO 0.8 DEGREES ***
rem   from the level file, az-from-+X convention:  travel = (0.6034,-0.2840,-0.7451)
rem   from the sky texture centroid (was armed):   travel = (0.5613,-0.4226,-0.7116)
rem   horizontal bearing 141.0 deg vs 141.8 deg
rem A texture centroid and an authored level value landing within a degree of one
rem another is real corroboration - neither could have produced the other.
rem
rem WHAT IS STILL UNKNOWN is ELEVATION, and it is a clean binary test:
rem   16.5 deg = angle ABOVE THE HORIZON  (armed below)
rem   73.5 deg = angle FROM THE ZENITH    (first alternate)
rem The property-name table is not shipped in any of the 35 paks and EBOOT.BIN is
rem an encrypted SELF, so the convention cannot be read - only tested.
rem NOTE the tension: you said the sun looks "a little low" at the old 25 deg. If
rem 16.5 looks LOWER still, the answer is the zenith reading, not this one.
rem *** 2026-08-17: THE BINARY TEST RESOLVED - 0x3624 IS MEASURED FROM ZENITH ***
rem Armed 16.5 deg above the horizon; you reported "Sun is still not high enough
rem making the scene dark in the selva region". Lower was the wrong direction, so
rem the property is the ANGLE FROM THE ZENITH: 90 - 16.5 = 73.5 deg elevation.
rem That also makes the morning level consistent - its 4.75-9.0 values become
rem 81-85 deg, i.e. near-overhead, which is wrong for a morning... so watch this:
rem if Selva now looks RIGHT and the morning level looks WRONG, the property is
rem per-level and not a single convention, and the CSV needs re-reading per row
rem rather than one global rule.
rem Same azimuth (129 deg) either way - that half is corroborated to 0.8 deg by
rem the sky-texture centroid, so only the Y term changes here.
rem *** BOTH FILE CONVENTIONS ARE WRONG - MEASURED BY BRACKETING ***
rem   el=16.5 (above horizon)  -> "still not high enough making the scene dark"
rem   el=73.5 (from zenith)    -> "sun light is too high"
rem So 0x3624 is NOT a raw elevation in either sense. The AZIMUTH half is fine -
rem 129 deg, independently corroborated to 0.8 deg by the sky-texture centroid -
rem so only the height term is misread, and we bracket it by eye from here.
rem Reference points on the same azimuth:
rem   el 16.5 too low | el 25 "a little low" (the old sky-texture value) | el 73.5 too high
rem Armed 35 deg: one clear step above the value you called "a little low", well
rem under the one you called too high. Walk DOWN the ladder if 35 overshoots.
rem   el 35 -> 0.5155,-0.5736,-0.6366   (armed)
rem   el 45 -> 0.4450,-0.7071,-0.5495
rem   el 30 -> 0.5450,-0.5000,-0.6730
rem   el 25 -> 0.5703,-0.4226,-0.7042   (the sky-texture answer, "a little low")
set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:0.5155,-0.5736,-0.6366"
rem set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:0.4450,-0.7071,-0.5495"  <- el 45
rem set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:0.5450,-0.5000,-0.6730"  <- el 30
rem set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:0.5703,-0.4226,-0.7042"  <- el 25
rem set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:0.1787,-0.9588,-0.2207"  <- el 73.5 from zenith (too high, tested)
rem set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:0.6034,-0.2840,-0.7451"  <- el 16.5 above horizon (too low, tested)
rem set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:-0.7451,-0.2840,0.6034"  <- az from +Z instead
rem set "RPCS3_REMIX_SUNMAP=D1A6D1B27ADE6232:0.56127,-0.42262,-0.71159" <- the sky-texture value
set "RPCS3_REMIX_SUNSKY=1"
rem Vertex floor for the dome draw the derivation is allowed to use. Stops a
rem 4-vertex card that shares the dome's program from winning the UV match. Every
rem sky-dome draw in the census carries 134..1446 vertices.
set "RPCS3_REMIX_SUNSKYMINVTX=64"
rem
rem --- 2. THE NECTAR GAUGE: WHY IT IS RIGHT IN A VEHICLE AND WRONG ON FOOT ----
rem ROOT CAUSE, MEASURED. is_screen_space_draw() (RemixGSRender.cpp) decides
rem UI-vs-world from exactly TWO live per-draw inputs: the outer constant block
rem read out of RSX constant memory, and depth_write_enabled(). It never looks at
rem the vertex program, the albedo, the clip size or the viewport. So a HUD
rem program that is not statically fingerprinted screen_space FLIPS between the
rem compositor and world geometry from draw to draw.
rem
rem In the round-22 run (bin\log\RPCS3.log, pid 20048) the gauge program
rem 2f64c2f8ffd6add1 is on BOTH paths in ONE session:
rem   2D  : 277 'Remix uiwrap: ... route=2d' lines
rem   3D  : 'Remix sky-census: vp=2f64c2f8ffd6add1 reject:extent ...
rem          raw=[-0.02 -0.765 0]..[0.02 -0.725 0] origin=[-5.2743 -0.001 -40.476]
rem          cam=[-5.2743 0 -41.982]' and 'Remix fpcandidate: ... eye_dist=1.679'
rem i.e. on the 3D path the HUD quad is placed as WORLD GEOMETRY 1.5 units in
rem front of the eye, with raw vertices that are already inside the NDC cube and
rem z = 0. All three reported symptoms follow from that one placement:
rem   clips through geometry  -> it IS geometry, at 1.5 units
rem   resolution changes      -> a world quad is rasterised at whatever the
rem                              camera's projection makes of it
rem   uncoloured              -> the world path only replays vertex colour when
rem                              VCOLMOD is on, and VCOLMOD=0; the 2D path reads
rem                              the colour straight out of ATTR3
rem 'Remix vcolroute:' confirms the two halves: 2f64c2f8ffd6add1 route=passthrough
rem (ATTR3) and its twin 2f650a38ffe6add1 route=constant cval=[1 1 1 1].
rem
rem UIFORCEVP pins a listed program to the compositor whenever depth writes are
rem off. depth_write is the guard because it is the one input that says "this
rem draw wants to occlude", and every gauge draw measured reads depth_write=0.
rem This is NOT the viewmodel tag that made the gauges vanish in an earlier round
rem - that deleted them from the world; this routes them to the overlay the
rem compositor is already drawing 348,501 UI draws through in this same run, and
rem which already accepts this exact program (the 277 route=2d lines above).
rem WATCH: 'ui_forced=' on 'Remix stats:'. Zero means the list is not matching.
rem REVERT: set "RPCS3_REMIX_UIFORCEVP="
set "RPCS3_REMIX_UIFORCEVP=2F64C2F8FFD6ADD1"
rem
rem === ROUND 36: RPCS3_REMIX_UIFORCEPAIRVP / UIFORCEPAIRFP - THE HELMET ======
rem The same override keyed on a (vertex program, FRAGMENT program) PAIR. This
rem is the fourth time the helmet has been asked for and the first time it has
rem been aimed, because the Ctrl+Click finally isolated it:
rem   vp=830d7d1b9681c475 fp=479890ff55f1d96e albedo=C61753D31FB96507 vtx=59
rem   extent=6.465 depth_test=0 depth_write=0 blend=1
rem
rem WHY NOT JUST ADD THE VP TO THE LIST ABOVE. MEASURED over the last 400 MB of
rem bin\remix_dump.log (1,038,135 lines):
rem   vp=830d7d1b9681c475 alone = 47,399 lines, 31 fragment programs, and its
rem     two largest fps carry 230 and 193 distinct vertex counts. It draws most
rem     of the scene. A vp-only pin is a ~26x over-match - round 20 failure mode.
rem   the PAIR                  = 1,833 lines, 1,767 of them (96.4%) at albedo
rem     C61753D31FB96507 / vtx=59, i.e. the helmet. The 3.6% residue is 35 lines
rem     of a vtx=152 family at extent ~2.2 and 9 quads under extent 0.13.
rem
rem WHY THE UI ROUTE AND NOT A CATEGORY FLAG - decided on round 35 reading of
rem the DEPLOYED runtime, not on preference. There is NO per-instance castShadow
rem flag anywhere in it. HIDDEN sets mask=0 and kills primary rays too, so the
rem helmet would vanish (fails "visible"). THIRD_PERSON_PLAYER_MODEL needs
rem rtx.playerModel.enableInPrimarySpace=True, which masks every VIEW_MODEL
rem candidate to zero and would take the ARMS with it - a head-on collision with
rem the VMROT work above. The UI route needs none of that: a forced draw returns
rem BEFORE per_draw_transform and submit_subdraw, so no mesh, no instance, no
rem material and NO BLAS are created. No shadow and no world clipping by
rem construction rather than by flag. The existing depth_write guard still runs
rem and still helps - the helmet reads depth_write=0, and any member of the pair
rem that writes depth stays on the world path untouched.
rem
rem WATCH, on "Remix stats:" in bin\log\RPCS3.log (read it AFTER the session -
rem that file is exclusively locked while the game runs):
rem   ui_forced_pair= should be in the hundreds or thousands. ZERO means the
rem     hashes never matched, NOT that the mechanism failed.
rem   uiforcepairvp= / uiforcepairfp= print the PARSED hashes, so a typo that
rem     parses to 0 - which silently disarms the route - is visible there.
rem THE RISK, stated up front: the compositor can still REFUSE a forced draw
rem (ui_skipped / ui_space_none / ui_render_target) and a refusal DELETES it
rem rather than falling back to world geometry. If the helmet DISAPPEARS instead
rem of flattening, read those three counters - not ui_forced_pair.
rem REVERT (no rebuild): set "RPCS3_REMIX_UIFORCEPAIRVP="
set "RPCS3_REMIX_UIFORCEPAIRVP=830D7D1B9681C475"
set "RPCS3_REMIX_UIFORCEPAIRFP=479890FF55F1D96E"
rem
rem === ROUND 37: UIFORCEPAIRVP2 / UIFORCEPAIRFP2 - THE OTHER TWO COPIES =====
rem THE ROUTE ABOVE WORKS. MEASURED on the round-36 log, not inferred:
rem ui_forced_pair=14664 rising at EXACTLY one per frame over four consecutive
rem "Remix stats:" intervals (101/101, 99/99, 105/105, 104/104 frames against
rem increments), 138 "Remix uiwrap: ... route=2d" lines carrying the helmet
rem albedo C61753D31FB96507, and ZERO compositor refusals - ui_skipped froze at
rem 5077 at frame 9678 and did not move for the next 13,800 frames.
rem
rem SO WHY DOES IT STILL CLIP: that albedo is submitted by THREE (vp, fp) pairs
rem and the round-36 key held ONE. The other two reached DrawInstance as world
rem geometry, which is the clipping, and one of them is dedicated to the helmet
rem - all 33 of its census lines carry that albedo and no other:
rem   830d7d1b9681c475 / 479890ff55f1d96e  138 lines  UI    (round 36 pair)
rem   f39f504649b6f442 / 4afa02b3dbbe9b7e   33 lines  WORLD, all the helmet
rem   830d7d1b9681c475 / 609a4216b89e296a    2 lines  WORLD
rem Both extra pairs read depth_write=0, so the existing guard admits them.
rem Matched POSITIONALLY, slot against slot, never crossed - crossing two lists
rem would force nine combinations of which six are other geometry, which is
rem round 20 failure mode exactly.
rem
rem READ IT: ui_forced_pair should roughly DOUBLE, to ~2 per frame; check it
rem against the frame delta on two consecutive "Remix stats:" lines. The knobs
rem line must read uiforcepairs=2 - a 0 or 1 there means a list did not parse.
rem IF THE HELMET VANISHES the compositor refused the new pairs: read ui_skipped,
rem frozen at 5077 since frame 9678, which would have to MOVE for that to be it.
rem EXPECT IT TO LOOK LIKE THE COMPOSITED COPY, NOT THE WORLD ONE: the existing
rem pair uiwrap lines read u=[2676..30089] v=[3417..28455] on a 512x512 texture,
rem three orders of magnitude outside [0,1], so the atlas region being painted
rem is already not a sane one. That is a separate defect - named, not fixed.
rem REVERT (no rebuild): set "RPCS3_REMIX_UIFORCEPAIRVP2="
rem --- ROUND 38: A FOURTH PAIR DRAWS THE HELMET, and round 37 could not have seen it -------
rem Round 37 asked "either a fourth pair draws it, or the UI route does not prevent clipping".
rem MEASURED, last 200 MB of bin\remix_dump.log, every line carrying albedo C61753D31FB96507:
rem   Remix fpcandidate  vp=830d7d1b9681c475 fp=479890ff55f1d96e   n=424   uiwrap route=2d 331
rem   Remix fpcandidate  vp=9f591b6a6b825612 fp=cbede4eb45f0fd25   n=416   uiwrap            0
rem   Remix fpcandidate  vp=f39f504649b6f442 fp=4afa02b3dbbe9b7e   n=246   uiwrap route=2d  65
rem   Remix fpcandidate  vp=830d7d1b9681c475 fp=609a4216b89e296a   n=6     uiwrap            0
rem The SECOND row is the fourth pair, it is the second-busiest of the four, and it is the only
rem busy one with ZERO uiwrap lines - it never reaches the 2D route, so its copy is submitted
rem as world geometry and clips. Same vtx=59 as the other three, i.e. the same mesh. It also
rem shows up under 'Remix alphastate:' and 'Remix kil:' with the same albedo and vtx.
rem Confirming that the extra-pair route itself works: (f39f..., 4afa...) is UIFORCEPAIRVP2
rem slot 0 and it produced 65 'route=2d' lines, so positional pairing parses and routes.
rem Slot 1 (830d..., 609a...) has only 6 draws in 200 MB - too rare to judge, left armed.
rem The array behind these lists is std::array<u64, 8> (RemixTransforms.cpp, the extra-pair
rem struct), so three of eight slots are used and there is room.
rem
rem PRE-REGISTERED: uiforcepairs= must read 3. THAT SPECIFIER IS ON 'Remix stats:', WHICH GOES
rem ONLY TO bin\log\RPCS3.log - NOT to remix_dump.log. 0 of 13,495 'Remix live:' lines in the
rem last 200 MB carry ui_forced_pair. Round 37 read it correctly in RPCS3.log
rem (ui_forced_pair=27708 -> 27780, uiforcepairs=2) but wrote it up as "the knobs line", which
rem is the 'Remix live:' line in remix_dump.log and does not carry it. Read RPCS3.log AFTER
rem the emulator exits - the file is exclusively locked while it runs.
rem WATCH: 'Remix uiwrap:' gaining route=2d lines for vp=9f591b6a6b825612.
rem If the helmet VANISHES instead: ui_skipped, which is live again (0 -> 7163 in the last run,
rem so round 37's "frozen since frame 9678" no longer holds and it can be read as evidence).
rem NOTE 9F591B6A6B825612 is also armed on RPCS3_REMIX_UVAFFINEVP above - different mechanism,
rem left alone.
rem REVERT (no rebuild): drop the third entry from both lists below.
set "RPCS3_REMIX_UIFORCEPAIRVP2=F39F504649B6F442,830D7D1B9681C475,9F591B6A6B825612"
set "RPCS3_REMIX_UIFORCEPAIRFP2=4AFA02B3DBBE9B7E,609A4216B89E296A,CBEDE4EB45F0FD25"
rem
rem --- 3. THE VIEWMODEL PAIR: IT IS AIMED AT REFUSED DRAWS --------------------
rem MEASURED, round-22 run: vm_tagged_pair=0 over the whole run. The reason is
rem not that the pair is too narrow - it is that the vertex program it names
rem never reaches the tagging site with that albedo. Every line in the run that
rem carries BOTH f39f504649b6f442 AND 86885A0E60751491 is a REFUSAL:
rem   Remix world-refused: vp=f39f504649b6f442 vtx=3649 albedo=86885A0E60751491
rem     fail=tail areason=wdivide persp_residue=4.27 tol=0.02   (x40)
rem and that pair appears on ZERO 'Remix fpcandidate:' lines. The SAME mesh
rem (vtx=3649) drawn by 830d7d1b9681c475 IS world-resolved and does reach
rem fpcandidate, at eye_dist 0.465..0.624. Corroborated across runs: with
rem VMPAIRVP=830D7D1B9681C475 round 21 measured vm_tagged_pair=123; after the
rem retarget to F39F504649B6F442 rounds 22 and 23 measure 1 and 0.
rem
rem So round 21's one-line fix above is right, and VMPAIRMAXDIST is what makes it
rem SAFE - 830d7d1b9681c475 also draws distant character meshes, which is what
rem deleted NPC bodies the last time a shared albedo was listed. The separation
rem is 20x and it is measured, from 'Remix fpcandidate:' eye_dist on that program
rem (listedvp=1, so its rows are NOT truncated by FPCENSUSMAXDIST):
rem   86885A0E60751491 vtx=3649  first-person body   0.465 .. 0.624
rem   0721D150DF278E7D vtx=2140  the weapon          0.429 .. 1.090
rem   EC3C2D7AC0AB2938 vtx=554   character mesh     20.708 .. 38.764
rem   19177730D341388A vtx=92    character mesh     20.871 .. 37.257
rem
rem WHY 2 AND NOT SOMETHING LARGER: the existing anchor guard already refuses
rem anything past VIEWMODELANCHOR=4, so the 20..38 unit meshes above were never
rem the threat - they are already excluded. The band that is NOT protected is
rem 2..4 units, which is exactly what "NPC bodies AT CLOSE RANGE" means. 2 covers
rem that gap while the first-person body (0.465..0.624) and weapon (0.429..1.090)
rem stay comfortably inside. Note the '||' short-circuits, so a draw past 4 is
rem attributed to vm_hash_anchor_refused and vm_pair_far counts only the (2,4]
rem band - one counter per draw, but a smaller number than you might expect.
rem
rem VMPAIRMAXDIST bounds the PAIR route only - not the hash route, not the albedo
rem route, not the depth route - and 0 reproduces round 22 exactly. It is armed
rem at 2 here but is INERT until the two lines below are applied, because the
rem pair it would bound currently tags nothing at all. This round was told not to
rem change an existing value, so they are left commented:
rem   set "RPCS3_REMIX_VMPAIRVP=830D7D1B9681C475,F39F504649B6F442"
rem   set "RPCS3_REMIX_VMPAIRALBEDO=86885A0E60751491,0721D150DF278E7D"
rem WATCH: 'vm_tagged_pair=' should climb off 0, and 'vm_pair_far=' should be
rem non-zero (that is the bound refusing the distant character meshes). If NPC
rem bodies disappear again, vm_pair_far=0 means the bound never fired.
rem REVERT: set "RPCS3_REMIX_VMPAIRMAXDIST=0"
set "RPCS3_REMIX_VMPAIRMAXDIST=2"

rem --- round 24 ----------------------------------------------------------------
rem Nothing above this line was edited. cmd's "set" is last-wins, so deleting any
rem line below restores whatever the lines above say, with no other edit.
rem
rem === THE ONE CHANGE THIS ROUND THAT MOVES PIXELS ==============================
rem WORLDIDMAXT=32 (armed at 09:32 two rounds ago, LEFT AT 32 - not changed) is
rem correct for the three LOCAL-space programs it was built for. It is INVERTED
rem for the two programs that submit ABSOLUTE world vertices: there the resolved
rem translation is not a placement to preserve, so applying it moves geometry
rem that was already in the right place. This list exempts those two and leaves
rem the hatch armed for everyone else. Empty = round 23 byte for byte.
rem
rem MEASURED, this run's own 'Remix worldid-census:' final row:
rem   BD1C10DF5703E559  draws=24760   kept=5584  tmax=847.5
rem   7F02E76D7369D09E  draws=131397  kept=38    tmax=293.3
rem and the clincher, one 'Remix worldid-draw:' row:
rem   vp=bd1c10df5703e559 translation=417.553
rem     raw=[-1397.69 -40.03 -271.72]..[1146.32 -4.57 261.50]
rem The raw vertices already span 2,544 world units - the whole map - so those
rem 417 units are added on top of a correct position. 92 of 140 traced bd1c rows
rem sit above the 32-unit threshold and are therefore kept and displaced.
rem Round 22 measured BOTH of these at 0.0% displaced ("absolute coords, identity
rem is free"), which is exactly why they are the two that must be exempt.
rem
rem THIS IS THE CANDIDATE FIX FOR: the smelting plant losing its floor and
rem ceiling, "some missing walls at angles", "a part moving past the rest of the
rem ship", and geometry that still wobbles after 23b.
rem WATCH: 'Remix worldid-census:' - kept= for these two programs must go to 0
rem while ad7ce9d6/c1d482dc/d0b6a471 keep theirs. 'worldidexempt=2' on
rem 'Remix stats:' confirms the list parsed.
rem IF THE LAND CARRIER REGRESSES instead, that is this line - blank it.
rem REVERT: set "RPCS3_REMIX_WORLDIDMAXTEXEMPTVP="
set "RPCS3_REMIX_WORLDIDMAXTEXEMPTVP=BD1C10DF5703E559,7F02E76D7369D09E"
rem
rem === THE "nectar rebooting" OVERLAY UV WRAP ===================================
rem You Ctrl+Clicked it: vp=6f76ab0ad8d926b1 fp=4a1bc009fc6161ef
rem albedo=6575ACE3A42A78E6, depth_test=0 depth_write=0 blend=1.
rem MEASURED: that program takes BOTH paths in this run, exactly like the gauge
rem program did - 29 'Remix uiwrap: route=2d' lines (composited) AND 12
rem 'Remix fpcandidate:' + 1 'Remix sky-census:' lines, which are only ever
rem emitted on the world path. UICLAMPSUBRECT (the UV-seam fix) runs ONLY on the
rem 2D route; on the world route the UVs go to the GPU and no rule of ours can
rem run. So the text is correctly clamped on 2D frames and wraps on world frames.
rem That is the reported symptom, and it is the same root cause as the gauges.
rem
rem CLAMPALBEDO is armed rather than UIFORCEVP DELIBERATELY. Both would work, but
rem CLAMPALBEDO forces the clamp on the world route without changing routing, so
rem the worst case is "the wrap is unchanged". UIFORCEVP changes which path the
rem draw takes, and its known risk is the element VANISHING if the compositor
rem refuses it. Take the reversible one first. 6575ACE3A42A78E6 is also the
rem pre-identified candidate the round-7 block at ~:646 already names for exactly
rem this knob, which is independent corroboration.
rem WATCH: tex_wrap_forced on the 'Remix live:' line going non-zero.
rem IF IT DOES NOT HELP: blank this and instead append 6F76AB0AD8D926B1 to
rem RPCS3_REMIX_UIFORCEVP at ~:2370 (making it a 2-entry list). Read
rem 'ui_forced=' and 'uiforce_vps=2'. If the text then VANISHES, revert that.
rem NOTE: CLAMPALBEDO is assigned IN PLACE at ~:666, its only "set" line - not
rem here. A second "set" for one knob is what created the three DEAD LINES this
rem file already carries, so this block is comment-only.
rem REVERT: blank the assignment at ~:666.
rem
rem === THE SKY SUN: two guards, both at their defaults =========================
rem SUNSKYDOWN=1 refuses a DERIVED sun whose travel y is >= 0, i.e. a sun at or
rem below the horizon lighting the level from underneath. This is a LATCH GUARD:
rem a solution is taken once per dome albedo and never revised, so one bad
rem derivation aims that level's sun wrongly for the whole session. MEASURED on
rem this build, the Selva dome D1A6D1B27ADE6232 derived
rem   travel=[0.58892 0.11312 -0.80024]     <- POSITIVE y, sun below the horizon
rem against the working dome CDFE11B12552EA2D's [-0.19933 -0.38269 0.90212].
rem With the guard the bad solve is refused and SUNDIR keeps Selva, which is what
rem shipped before the derivation existed. SUNMAP is NOT subject to this - a
rem hand-written vector is never second-guessed.
rem THE REFUSAL IS STICKY: the refused albedo claims its slot with source=0, so
rem the derivation is never re-entered for it and SUNDIR keeps that level for the
rem whole session. Without that it would re-derive every draw against the MOVING
rem camera and latch on the first frame that happened to squeak below zero - a
rem horizon-grazing sun, picked at random. A review caught exactly that.
rem WATCH, and use THIS counter not sun_sky_refused: 'sunsky_up=' on
rem 'Remix stats:'. sun_sky_refused is shared with three other refusal paths that
rem are structurally expected traffic (every 4-vertex card sharing a dome albedo
rem trips the vertex floor), so it can never be evidence this guard fired.
rem You also get ONE line per refused dome:
rem   Remix sunrefuse: skyalbedo=D1A6D1B27ADE6232 reason=horizon travel=[...]
rem     peakuv=[...] uverr=... cam=[...]
rem That line is the 98-threshold BASELINE to compare a re-tuned SUNSKYPEAKFRAC
rem against - without it a refused dome would print nothing at all.
rem REVERT: set "RPCS3_REMIX_SUNSKYDOWN=0"  (restores round 23 exactly)
set "RPCS3_REMIX_SUNSKYDOWN=1"
rem SUNSKYPEAKFRAC is the fix for WHY Selva derived a below-horizon sun. Percent
rem of the dome texture's peak luma a texel must reach to join the centroid.
rem MEASURED offline on bin\remix_tex\unit0_D1A6D1B27ADE6232_2048x1024.bmp:
rem   98 -> 112 texels on ONE row (y=513), peak_uv=[0.67693 0.50146]
rem   90 -> 972 texels over 28 rows,       peak_uv=[0.66404 0.50961]
rem   85 -> 19418 texels over 229 rows,    peak_uv=[0.63368 0.56012]
rem At 98 the window catches a one-row luma spike on the TOP EDGE of the panorama
rem band (row 512 max 162.75, row 513 max 249.18, row 514 max 230.25), so the
rem centroid lands on v=0.5015 - the first row of the band - and the derived ray
rem points at the horizon. LEFT AT 98 (bit-identical to round 23) on purpose:
rem the threshold is SHARED by every dome, and CDFE11B12552EA2D currently solves
rem correctly at uverr=0.035. Do not move it until you have seen Selva refuse.
rem THEN: try 90, relaunch, read 'Remix sunmap:' for D1A6D1B27ADE6232 and check
rem travel= now has a NEGATIVE y. If 90 is not enough, try 85.
rem REVERT: 98.
set "RPCS3_REMIX_SUNSKYPEAKFRAC=98"
rem
rem --- round 25 ----------------------------------------------------------------
rem Nothing above this line was ADDED to. Four existing knobs were edited IN PLACE
rem (CLAMPALBEDO -> blank, CAT_HIDE +1 hash, SUNMAP armed, VMPAIRVP/VMPAIRALBEDO
rem retargeted), each with its measurement beside it. The only NEW knobs are the
rem two below, which have no earlier "set" line anywhere in this file.
rem
rem === THE HUD FONT - what is actually wrong, proved against the user's own pixels =
rem The friend's read was right in substance ("taking some extra boundary from the
rem texture sheet") and this round nailed it to the texel.
rem
rem MEASURED. Take one 'Remix uiwrap:' row for the font atlas and crop the DUMPED
rem atlas at exactly the rectangle it logs:
rem   albedo=6575ACE3A42A78E6 tex=512x512  u=[444.0..476.0] v=[64.5..110.5] texels
rem The crop is, pixel for pixel, the garbled ammo counter in the user's screenshot:
rem the BOTTOM of one glyph row, the wanted digits, and the TOP of the row below -
rem three bands with blank gaps, in the same proportions. So the compositor, the
rem sampler, the wrap mode and the seam rule are all doing exactly what they are
rem told, and the fault is entirely in the AUTHORED/DECODED RECTANGLE upstream.
rem The atlas has NO gutter (rows 29..176 are one continuous ink band, only 6 blank
rem columns in 512), so an oversized rectangle shows neighbours at full strength.
rem
rem THE FACTOR IS EXACTLY 0.5, ABOUT THE RECTANGLE'S OWN CENTRE, AND IT IS MEASURED,
rem NOT FITTED. For four independent single-glyph rectangles taken from the census,
rem scale the rectangle about its centre and score the mean ink on the resulting
rem one-texel border:
rem   scale  1.00  0.90  0.80  0.70  0.60  0.55  0.50  0.45  0.40  0.35
rem   rect0  48.4  45.8  42.1  41.5  19.8   4.3   0.0  45.3  73.1  68.1
rem   rect1  82.2  43.8  58.7  23.7   5.2   4.3   0.0  37.6  50.1  37.2
rem   rect2  62.1  57.5  79.0  86.3  17.4   6.1   0.0  16.1  28.1  48.1
rem   rect3  51.9  37.9  49.3  31.1   5.9   3.1   0.0  12.9  20.9  37.3
rem Exactly 0.0 on four of four at 0.50 and nowhere else; and shrinking toward the
rem low corner, the high corner or the texture origin instead of the centre is
rem non-zero at every one. Halving about the centre yields a cleanly framed glyph
rem ("2" and "1" verified by eye); halving about the origin yields a clipped "C".
rem
rem THIS IS A BRIDGE, AND HERE IS THE DEBT. It corrects the rectangle at the sampler
rem instead of fixing whatever doubles it in the UV decode. A centre-preserving
rem doubling is the signature of a per-vertex OFFSET-FROM-CENTRE term being scaled
rem twice, which points at the MAD/affine recovery (madaccum, madmix) - but the
rem uiwrap census only carries per-DRAW boxes, so the cause cannot be closed without
rem per-VERTEX texcoords. Those already exist behind RPCS3_REMIX_UIDUMPVP, which
rem prints 'Remix ui-quads[n]: ... [i](x,y,u,v)' per vertex plus the uvscale actually
rem used. Arm it next round on 6F76AB0AD8D926B1 and the cause falls out in one run.
rem
rem SCOPE AND SAFETY. Per TRIANGLE, not per draw - a string's draw-level box is the
rem union of many cells, but each glyph quad's triangles carry that quad's box alone.
rem Only for listed hashes. And guarded to authored spans of at most half the sheet
rem on both axes (the unit is normalised UV), so a full-sheet blit of a listed texture
rem can never be collapsed onto its middle quarter. The largest AUTHORED single-glyph
rem span measured is 70/512 = 0.137 in u and 46/512 = 0.09 in v, so the guard sits
rem 2.7x above the population it admits - not the 5x an earlier draft of this note said.
rem
rem TWO RESIDUAL RISKS, named rather than hidden, BOTH SETTLED BY THE UIDUMPVP RUN THIS
rem SHIPS ARMED (see below):
rem  1. "each glyph quad's triangles carry that quad's box" holds for quads / triangles
rem     / indexed lists but NOT for a triangle STRIP - strip_to_list emits every
rem     consecutive triple, so a connecting triangle straddles two cells with a span of
rem     about 0.18, UNDER the 0.5 guard, and would be MOVED rather than resized. This
rem     project's own round-7 note calls these "a 13-quad glyph batch" and the one
rem     primitive field logged for a font-atlas program reads prim=8 (QUADS), so this is
rem     BELIEVED SAFE, NOT PROVEN. The per-vertex dump settles it in one run.
rem  2. The list is keyed on ALBEDO alone. At least two vertex programs share this atlas
rem     (6F76AB0AD8D926B1 draws the "nectar rebooting" overlay from it). If one of them
rem     authors its rectangle CORRECTLY, its text now renders a quarter-area crop.
rem     SIGNATURE: some UI text goes oversized or cropped while the HUD is fixed.
rem
rem AND ONE COUNTER WILL MOVE THE OTHER WAY. The shrunk box lies strictly inside the
rem authored one, so coordinates that used to fall outside [0,1] now fall inside:
rem ui_uv's 'seam' bucket will FALL toward zero for this atlas. The CLAMPALBEDO note
rem near :662 says "seam climbing while text is on screen is the fix firing" - that
rem reading is now inverted for 6575ACE3A42A78E6 specifically.
rem Both rasterizers read the same rule so the triangle and quad paths cannot drift.
rem This is the 2D compositor route ONLY; the 19 world-route draws of this atlas are
rem untouched, by design, because that is the "nectar rebooting" overlay and not the
rem HUD the user asked about.
rem WATCH: 'uirectshrink=1 uirectshrinkpct=50' on the run-start banner and on
rem 'Remix live:', and 'shrink=50' on every 'Remix uiwrap:' row for this albedo.
rem shrink=100 on those rows means the hash did not parse.
rem REVERT: set "RPCS3_REMIX_UIRECTSHRINK="
rem ROUND 31, VALUE CHANGED: 85CC1EB51A38B1E9 ADDED. This REFUTES round 30, which predicted
rem the opposite and warned that adding it would halve correct text. Round 30 could not run
rem its own test (the UIDUMP budget was exhausted 1,670 dumps before the first 128x192 draw)
rem and fell back to a v_lo residue argument. The budget was raised to 4000, the capture
rem LANDED - 136 lines, 837 quads, tex=128x192 - and round 30's own nominated discriminator
rem now has an answer:
rem   extent / adjacent-centre-spacing = 2.058 median (min 2.017, max 2.112, n=136)
rem   positive control 6575ACE3A42A78E6, round 30       = 2.0417 mean (1.3882-2.4848, n=15)
rem The signature is PRESENT, and tighter than on the calibrated control. Second, independent
rem argument: the v extents quantise on 6 texels (6.0 n=136, 18.0 n=204, 24.0 n=497) against a
rem measured row pitch of EXACTLY 16 - a 24-texel glyph cannot sit in a 16-texel row, while the
rem halved 12 can. Both say doubled about the centre, which is what UIRECTSHRINKPCT=50 undoes.
rem The hash-to-sheet binding (128x192 on vp 6F76AB0AD8D926B1 == 85CC1EB51A38B1E9) is round
rem 30's attribution, inherited, not re-measured here - the ui-quads line carries no albedo.
rem REVERT: set "RPCS3_REMIX_UIRECTSHRINK=6575ACE3A42A78E6"
set "RPCS3_REMIX_UIRECTSHRINK=6575ACE3A42A78E6,85CC1EB51A38B1E9"
rem
rem The factor, in percent. 100 is a no-op even with the hash listed and restores
rem today's sampling bit-exactly (it short-circuits before any arithmetic, which
rem matters: c + (x-c)*1.0f differs from x for 35% of random inputs in IEEE-754
rem single, so the early return is load-bearing, not cosmetic). 50 is the measured
rem value above. A value that PARSES is clamped into 10..100, so 200 becomes 100 and
rem is a true no-op; only unparseable input falls back to 50.
rem If the glyphs come out slightly too large or too small, this is the dial - and if
rem it needs to be anything other than 50, say so, because 50 was exact on four of
rem four rectangles and a different best value would mean the model is wrong.
rem REVERT: set "RPCS3_REMIX_UIRECTSHRINKPCT=100"
set "RPCS3_REMIX_UIRECTSHRINKPCT=50"
rem
rem === PAY THE BRIDGE'S DEBT IN THE VERY NEXT RUN - diagnostic, no behaviour change =
rem UIRECTSHRINK corrects the rectangle at the sampler. To DELETE it we need to know
rem why the rectangle is doubled, and that needs per-VERTEX texcoords, which the
rem uiwrap census does not carry (it is per-draw boxes only). Those already exist:
rem UIDUMPVP + UIDUMP make RemixGSRender.cpp:10451-10464 log
rem   'Remix ui-quads[n]: vp=... tex=WxH verts=N [i](x,y,u,v) [i+1](...)'
rem per VERTEX, alongside 'Remix ui-dump[n]: ... uvtype=%u/%u uvscale=%.5f,%.5f' and a
rem one-shot BMP of the atlas. Read them out of bin\log\RPCS3.log, NOT remix_dump.log.
rem
rem WHY 5BB8451BCD6F1FEB AND NOT 6F76AB0AD8D926B1. Three programs draw the font atlas;
rem only this one ever authors a SINGLE-GLYPH box, which is the population that shows
rem the defect. MEASURED over the 146 uiwrap rows for albedo 6575ACE3A42A78E6:
rem   vp                 rows   min du   min dv   rows with du<=60 AND dv<=60
rem   5bb8451bcd6f1feb     57     24.0     46.0     9      <- the HUD counters
rem   6f76ab0ad8d926b1     54    243.0     89.5     0      <- multi-glyph strings only
rem   3f73fa83fa68911f     35    286.0    110.5     0      <- ditto
rem And note min dv = 46.0 against an atlas row pitch of ~23.5: the SMALLEST v extent
rem any glyph draw ever authors is already exactly two rows. That is the 2x factor
rem falling out of the aggregate census independently of the four hand-checked rects.
rem
rem THE QUESTION TO ANSWER: is the doubling in the raw attribute, in uvscale, or in an
rem additive term the affine fit is dropping? Compare a quad's four [i](x,y,u,v) against
rem uvscale and against the glyph cell. Then delete UIRECTSHRINK and fix the decode.
rem COST: ~24 extra log lines and one BMP. No pixels change. UIDUMP=0 turns it off, and
rem note UIDUMPVP alone does nothing - the limit comes from UIDUMP.
rem
rem === ROUND 26: THIS CAPTURE WORKED, AND IT ANSWERED EVERYTHING IT WAS ARMED FOR ===
rem The 24 'Remix ui-quads[..]' lines it produced in bin\log\RPCS3.log were replayed
rem offline against bin\remix_atlas.bmp. Results, all reproduced rather than argued:
rem   * 4 vertices per glyph, and 92 vertices for "11:17 Hours, 18th June 2048" -
rem     exactly 23 non-space glyphs. THESE ARE QUADS, NOT A STRIP. Round 25's open
rem     residual risk (1) is closed.
rem   * Sampling the atlas through the AUTHORED rects reproduces the ROUND-24
rem     screenshot (three bands of partial glyphs).
rem   * Shrinking only the UV reproduces the ROUND-25 screenshot: right glyphs,
rem     double size, ~44% overlap. i.e. "more legible but bigger and squished".
rem   * Shrinking BOTH the UV and the SCREEN rect about each quad's own centre
rem     renders clean, correctly spaced text. That is what round 26 ships.
rem   * The px-per-texel ratio is IDENTICAL between "authored" and "both shrunk", so
rem     the fix leaves each glyph exactly the size it already was and removes only
rem     the padding. The k cancels. That is why it is the right correction.
rem   * ROOT CAUSE, as far as it can be taken from this layer: the expansion is about
rem     EACH QUAD'S OWN CENTRE, and (half-extent)/(centre spacing) is invariant under
rem     any affine map. Every step from the vertex attribute to the compositor - the
rem     recovered VP matrix, build_prescale, the NDC/pixel conversion, the viewport,
rem     uv_scale - is affine and draw-wide. NO global term in this backend can
rem     produce it. It is per-vertex, in the decoded attribute data.
rem
rem RETARGETED TO 6F76AB0AD8D926B1. *** THIS IS A DELIBERATE VALUE CHANGE ***, and it
rem is diagnostic-only - no pixel depends on UIDUMPVP. Justification: the 5BB8451B
rem capture is complete and fully analysed above, so re-running it learns nothing;
rem the one OPEN risk of the round-26 fix is residual risk (2), that another program
rem sharing this atlas authors its rectangle CORRECTLY and is now being halved. Three
rem programs share albedo 6575ACE3A42A78E6 and this is the largest of the other two.
rem Read its 'Remix ui-quads[..]' and check one quad: if its u/v span is ~2x the glyph
rem cell like 5BB8451B's, the fix is right for it too. If it is ~1x, blank
rem UIRECTSHRINK and the rule needs a vp discriminator.
rem
rem ROUND 26 ALSO MAKES UIDUMPVP CAPTURE THAT PROGRAM'S UCODE to
rem bin\remix_ucode\<vphash>.vp, alongside the atlas BMP. Decoding the stored ucode of
rem the six particle programs is exactly what closed the smoke question this round
rem after 22 rounds of guessing, and "what per-vertex term doubles the rectangle" is
rem the same shape of question. UI programs are arch=fused/inner_is_input, i.e.
rem HEALTHY by store_refused_ucode's test, so they had never been captured.
rem NEXT ROUND: flip UIDUMPVP back to 5BB8451BCD6F1FEB to collect the HUD counter
rem program's shader too, then decode both and diff them. Requires UCODESTORE=1,
rem which is already on.
rem ==================================================================================
rem ROUND 30 ANSWERED RISK (2) FOR THE PROGRAM ABOVE, AND FOUND A SECOND ATLAS.
rem ==================================================================================
rem You asked: "ctrl clicked that pulsate warning that is a grey texture and nectar
rem disruption font, perhaps they need to be the same uv wrapping as the font we
rem fixed?" and "UV still not fixed for nectar administering". Good hypothesis. It was
rem tested per albedo rather than by resemblance, because UIRECTSHRINK is a 50% SHRINK
rem and applying it to a correctly-authored quad renders it at HALF SIZE. Results:
rem
rem 1. POSITIVE CONTROL 6575ACE3A42A78E6 (512x512, already listed): SIGNATURE PRESENT.
rem    From the 24 'Remix ui-quads[..]' lines in bin\log\RPCS3.log (lines 34781-34879,
rem    16 quads, all 24 payloads byte-identical):
rem      extent / adjacent-centre-spacing = 2.0417 mean over n=15 (min 1.388 max 2.485)
rem      v extent 45.978-46.029 texels against a MEASURED atlas row pitch of 23
rem        (autocorrelation peak 0.3767 at lag 23) = 1.9990-2.0013
rem      every u extent halves to an integer: 16, 17, 18, 19, 20
rem    So the doubling is confirmed independently of the round-26 replay. The fix is
rem    right for THIS program too (it is one of the three sharing the atlas), which
rem    closes residual risk (2) for 6F76AB0AD8D926B1.
rem
rem 2. THERE IS A SECOND GLYPH ATLAS ON THE SAME TEXT PROGRAM, and it is NOT listed:
rem    85CC1EB51A38B1E9, 128x192, drawn by the same vp+fp pair (6F76AB0AD8D926B1 /
rem    4A1BC009FC6161EF), 4 'Remix uiwrap:' lines, bbox byte-identical in all 4 frames
rem    and in all 24 occurrences across 7 sessions. It is an ASCII glyph sheet.
rem    *** DO NOT ADD IT TO UIRECTSHRINK ON THE PRESENT EVIDENCE. *** MEASURED:
rem      its atlas row pitch is EXACTLY 16.000 texels (9 bands, tops 1,17,33,...,129,
rem        eight deltas all 16; autocorrelation 0.7650 at lag 16)
rem      authored v_lo = -0.500 texels, v_hi = 110.500, u_lo = -5.000, u_hi = 121.500
rem      under round-26 doubling every cell top moves up by half a pitch, so v_lo MUST
rem        be congruent to -8 mod 16 (i.e. -8.0, +8.0, +24.0, ...). It is -0.500.
rem        Discrepancy 7.5 texels, and no row index fits (16k-8 = -0.5 gives k=0.469).
rem      under 1:1 authoring v_lo = 16k - 0.5 gives k = 0 EXACTLY, residual 0.000.
rem      the same test passes on the positive control, so it is calibrated, not invented.
rem    VERDICT: signature ABSENT on v. NOT DETERMINED on u (that sheet is a
rem    PROPORTIONAL cache - measured variable glyph widths, no column grid - so there
rem    is no pitch to compare the -5.000-texel excursion against, and authored left
rem    bearing explains it equally well).
rem
rem 3. "pulsate warning" (grey): NOT DETERMINED, and structurally out of this knob's
rem    reach. No 'Remix picked:' line in ANY of the 82 sessions in remix_dump.log lands
rem    on a full-screen overlay - 1,506 pick lines, 41 of them on UI, and the ONLY
rem    glyph-atlas pick ever recorded is 6575ACE3A42A78E6. Separately: 58 of the 68 UI
rem    albedos in this session author exactly u=[0..1] v=[0..1] with exc=0.00,0.00, so
rem    no full-sheet UI overlay is 2x too large in UV at all; and ui_rect_shrink_for()
rem    returns 100 whenever a span exceeds 0.5 of the sheet, so listing a full-screen
rem    albedo here is a GUARANTEED NO-OP whatever its screen rect does.
rem
rem *** VALUE CHANGED THIS ROUND: UIDUMP 24 -> 4000. *** Diagnostic only - no pixel
rem depends on UIDUMP, it only bounds how many 'Remix ui-quads[..]' lines are written.
rem JUSTIFICATION, measured: the budget of 24 was exhausted at RPCS3.log line 34879,
rem t=34.787 s, and the FIRST 128x192 draw on this same program is at t=89.542 s /
rem frame 3747. At the observed ~1 dump per frame that is roughly 1,670 dumps past the
rem old budget, so item 2's extent/spacing test could not be run at all. 4000 covers it
rem with margin and costs about 6 MB of RPCS3.log.
rem AFTER THE NEXT RUN, run this and it settles item 2 outright:
rem     findstr /C:"ui-quads" bin\log\RPCS3.log | findstr /C:"tex=128x192"
rem   v extent about 32 texels => DOUBLED, add 85CC1EB51A38B1E9 to UIRECTSHRINK.
rem   v extent about 16 texels => correctly authored, and adding it would HALVE
rem     correct text. The v_lo arithmetic above predicts this second outcome.
rem REVERT: set "RPCS3_REMIX_UIDUMP=24" (or 0 to switch the capture off entirely).
rem PREVIOUS: set "RPCS3_REMIX_UIDUMPVP=5BB8451BCD6F1FEB"
rem ROUND 31, VALUE CHANGED 4000 -> 0. The capture it was raised for is COMPLETE - see the
rem UIRECTSHRINK block above - so the budget has served its purpose and is now pure cost.
rem MEASURED: those 4,000 lines are 32,184,151 bytes at a mean 8,046 bytes/line = 40.1% of
rem ALL bytes in bin\log\RPCS3.log, and each one was built with a per-vertex fmt::append
rem loop. The cap is tested before the format, so frames past 4000 cost nothing - but frames
rem 1-4000 built ~8 KB of string each. Set it back to a small number only to re-open a
rem specific UI capture, and name the sheet with UIDUMPVP when you do.
rem REVERT: set "RPCS3_REMIX_UIDUMP=4000"
set "RPCS3_REMIX_UIDUMP=0"
set "RPCS3_REMIX_UIDUMPVP=6F76AB0AD8D926B1"
rem
rem === WHAT ROUND 25 DELIBERATELY DID NOT TOUCH, AND WHY =========================
rem 1. SUNSKYPEAKFRAC stays 98. 86 is the first value that clears Selva's horizon
rem    guard, but the dome has no sun to find at any threshold (monotonic glow, no
rem    local maximum), and the threshold is shared with the carrier dome whose
rem    texture was never dumped, so the risk to the one dome that already works
rem    cannot be measured. SUNMAP does the job per-dome with no shared risk.
rem 2. VMDEPTHOFFSET stays 0. The three first-person programs really do sit in a
rem    compressed depth slice (scale_z=offset_z=0.00125 against 0.50125 for all 40
rem    other programs - a 400x separation) and the built-in limit of 1e-3 misses it
rem    by 25%, so VMDEPTHOFFSET=2 would tag them. It is the CLEANER discriminator.
rem    It is not armed because VMPAIRMAXDIST and the anchor guard bound the PAIR
rem    route only, NOT the depth route (RemixGSRender.cpp:18196 gates the anchor
rem    guard on viewmodel_listed, which a pure depth tag never sets) - so the depth
rem    route is entirely unbounded. Take the bounded route first; if the pair route
rem    tags but misses part of the rig, THIS is the fallback, and its signature is
rem    unique: vm_tagged>0 while vm_tagged_hash/albedo/pair all stay 0.
rem 3. WORLDIDMAXT stays 32 and its exempt list stays as round 24 left it. The
rem    exemption WORKED - the final worldid-census reads kept=0 for both
rem    BD1C10DF5703E559 and 7F02E76D7369D09E while ad7ce9d6/d0b6a471/c1d482dc keep
rem    theirs - but it is only 5,540 draws out of submitted=12,327,801 (0.045%),
rem    which is why the smelting plant did not visibly change. That knob can only
rem    choose between two transforms; it can never make MISSING geometry appear, and
rem    a lost floor and ceiling is a REFUSAL (world_refused=124110: tail 58907,
rem    nocam 34129, lay_other 31074). Attack it there next round, not here.
rem    NEW AND UNACTIONED: ad7ce9d672a0bf6b, c1d482dcd1b03ed0 and d0b6a471bb2d463b
rem    also submit ABSOLUTE world coordinates in this run - their per-draw boxes are
rem    a fixed ~220x54x81 volume but sit at 8,509 DIFFERENT absolute positions
rem    (centres spanning 1,954 units in x). Round 22 read the constant box SIZE as a
rem    constant POSITION and concluded the opposite. If that holds, WORLDIDMAXT is
rem    inverted for them too and is displacing 2,093,174 draws. Too big to ship
rem    blind in the same round as four other changes.
rem
rem ================================================================================
rem === ROUND 26 ===================================================================
rem ================================================================================
rem
rem THIS ROUND CHANGES ONLY ONE VALUE IN THIS FILE: UIDUMPVP, which is diagnostic-only
rem and is justified in its own block above. Everything else round 26 ships is CODE.
rem
rem --- 1. THE HUD FONT. Fixed properly, not bridged further. ----------------------
rem UIRECTSHRINK / UIRECTSHRINKPCT are UNCHANGED (6575ACE3A42A78E6 / 50), but the code
rem behind them now shrinks the primitive's SCREEN rectangle by the same percentage
rem about the same per-primitive centre, not only its UV rectangle. Round 25 shipped
rem the UV half alone, which magnifies the right glyph 1/k times onto an unchanged
rem screen quad - exactly the "more legible but bigger and squished" that came back.
rem CONFIRMS: 'Remix stats:' gains ui_rect=<shrunk>/<declined>. shrunk must climb
rem while HUD text is on screen; declined must stay 0 (it counts primitives that
rem matched but were refused by the axis-aligned-quad guard).
rem REVERT: set "RPCS3_REMIX_UIRECTSHRINK="   (whole feature, one variable)
rem HALF-REVERT to round 25's behaviour is NOT available by knob - it was wrong.
rem
rem --- 2. THE SUN WAS BEING DELETED. This is the big one. ------------------------
rem update_sun_light() called DestroyLight then CreateLight with the same hash. In the
rem deployed runtime (bin\remix\d3d9.dll sha256 16A0B512F33EBB66..., = numos3
rem _output\d3d9.dll) DestroyLight only QUEUES (rtx_remix_api.cpp:1605-1616) while
rem CreateLight is IMMEDIATE (:1520-1552), and Present applies destroys FIRST
rem (:2134-2139) and tombstones any create for the same handle (:2142-2144). So the
rem retarget DELETED the sun, and ensure_sun_light() can never rebuild it because the
rem handle is the hash and stays non-null. MEASURED in the round-25 run: sun created
rem at 0:01:27, retargeted 89.24 deg at 0:01:27, and there was no distant sun in the
rem scene for the remaining 14 minutes.
rem The destroy is gone. CreateLight with the same hash is already an update in place
rem (rtx_light_manager.cpp:722-732). isDynamic=1 is now set on both light fills, which
rem is also required: updateLightStaticSleep (rtx_fork_light.cpp:123-157) stops
rem copying new data for a static external light after N updates and would freeze the
rem direction all over again.
rem CONFIRMS: 'Remix sun-submit:' every 600 frames in bin\remix_dump.log, and
rem sun_destroys= must read 0 on every in-play 'Remix stats:' line.
rem THIS IS ALSO THE MOST LIKELY CAUSE OF "the land carrier's ground is dark" - there
rem has been no directional light in any level since the first retarget fires.
rem
rem --- 3. THE SMOKE AND EXPLOSIONS. Root cause CLOSED, fix is round 27. ----------
rem The effects are SIX vertex programs - 2D5186A8011589B8, 2FC998873A9F54B4,
rem 315E21388632FE3F, 391B10C33305C812, 946A6296D06C4AF8, 4ECE0A28F80FFC72 - and all
rem six were decoded from their stored ucode in bin\remix_ucode\*.vp. They are
rem POINT-SPRITE BILLBOARDS EXPANDED INSIDE THE VERTEX PROGRAM:
rem   ATTR0.xyz = the particle CENTRE, in WORLD space, IDENTICAL on all four corners
rem   ATTR0.w   = per-particle rotation angle
rem   v8 (TC0)  = .xy corner coord 0..1 (remapped to +-1 by c467), .zw sprite SIZE
rem   v10 (TC2) = the atlas sub-rect, so the UV is per-vertex scaled AND biased
rem   c8/c9/c10 = camera basis columns, c26 = eye, c0..c3 = fused view*projection
rem   corner = ATTR0.xyz + A*(size.x*corner.x) + B*(size.y*corner.y), then c0..c3
rem This is why they read arch=unknown / note=no matrix chain into HPOS: the 4x4 is
rem real and complete but its operand is a COMPUTED TEMP, not an attribute. They are
rem REFUSED as fail=lay_other and DROPPED - 78,648 draws this run, never reaching
rem Remix at all. Every earlier attribution (the emissive path, f39f504649b6f442, the
rem VIEW_MODEL mask, blend mapping, the conf lists) is refuted and closed.
rem
rem *** DO NOT ADD THESE SIX HASHES TO WORLDIDENTITYVP. *** It was recommended as a
rem "visually inert" way to print their raw ATTR0 box, and it is NOT inert:
rem RemixGSRender.cpp:1641 gates the GAUGE-ANCHOR capture on world_identity_vp_matches,
rem so listing them feeds the particle pass's own view*projection into the gauge that
rem places the whole world (gauge_used=6,385,543). Nothing is worth that.
rem And it would not show anything either: submitting raw ATTR0 at identity gives FOUR
rem COINCIDENT POINTS per quad - zero-area triangles. Same reason DRAWNOWORLD=1 would
rem place them correctly and render nothing.
rem THE FIX IS CODE: the backend has to replay the billboard expansion, because the
rem quad's size, rotation and shape exist only inside the vertex program. See
rem round27-inbox.md.
rem
rem --- 4. THE SMELTING PLANT FLOOR. Re-attributed, and the round-25 brief is WRONG.
rem It is NOT a refusal. The refusal census is COMPLETE for the round-25 run (max 21
rem of 128 census slots used in any window, so every distinct program+reason that
rem refused is named) and world_refused=220072 splits exactly as
rem nocam=52981 + lay_other=78648 + tail=88443, of which:
rem   lay_other = the six particle programs above,
rem   tail      = 4-to-8-vertex cards plus the player's own arms and weapon,
rem   nocam     = pre-camera frames.
rem NONE of it is floor-shaped world geometry. The floor is DISPLACED, not refused,
rem and WORLDIDMAXT=32 is what displaces it. The final worldid-census of that run:
rem   ad7ce9d672a0bf6b draws=1713442 kept=513111 t=1148159/52172/120116/392995
rem   d0b6a471bb2d463b draws= 195950 kept= 58967 t= 131639/ 5344/ 13896/ 45071
rem   c1d482dcd1b03ed0 draws=  57689 kept=  8063 t=  43230/ 6396/  3517/  4546
rem   bd1c10df5703e559 draws=  69702 kept=0   <- EXEMPT, and this is the FIXED CEILING
rem   7f02e76d7369d09e draws=  37479 kept=0   <- EXEMPT, ditto
rem (t buckets are <=1 / (1,32] / (32,128] / >128; tmax=430.5)
rem That asymmetry IS the reported symptom: the half of the room drawn by the two
rem exempt programs came back, the half drawn by the main world program did not.
rem
rem THE A/B, ARMED BUT NOT ENABLED. Run it in a round of its own, NOT in the same run
rem as the sun fix - a floor with no light on it and a floor in the wrong place look
rem the same from inside the room, and there has been no sun since round 14.
rem Uncomment ONE of these, run, look at the smelting plant, then put it back:
rem   set "RPCS3_REMIX_WORLDIDMAXT=128"   <- returns 120,116 ad7ce9d6 draws (+13,896
rem                                          d0b6a471, +3,517 c1d482dc) to identity
rem                                          while leaving the carrier placed
rem   set "RPCS3_REMIX_WORLDIDMAXT=0"     <- full revert to round-21 placement; the
rem                                          carrier WILL regress, that is the control
rem WATCH: 'Remix worldid-census:' kept= for ad7ce9d672a0bf6b.
rem
rem --- 5. WHAT WAS CHECKED AND DELIBERATELY LEFT ALONE ---------------------------
rem CAMLIGHT stays 12. It is not washing out the sun and never was: the sphere is
rem radius 0.1 (LIGHTRADIUS unset), so E = 12*pi*0.01/d^2 = 0.377/d^2 against the
rem distant sun's pi*3 = 9.42, i.e. the sun is 25x stronger at 1 unit and 225x at 3.
rem It is also inert for the same destroy-ordering reason as the sun, and setting it
rem to 0 would RAISE the radiance (place_debug_light falls back to LIGHTRADIANCE=100
rem when CAMLIGHT is 0), not lower it. Leave it.
rem AFFINETOL stays unset (0.02). ZERO refused draws sit anywhere near it - the
rem residue histogram reads <0.05=0, <0.2=0, <1=24495, <10=63679, >=10=269 - so
rem admitting any of them needs a 50x widening, and the same tolerance is the
rem acceptance test inside both rescue arms (RemixGSRender.cpp:14590, :14826), which
rem currently place 80,979 draws.
rem PROJSPLITERR stays 50. proj_split_refused=0 and nosplit=0: it is rejecting
rem nothing, so tuning it changes nothing.
rem SKYEMISSIVEBLEND stays 1, and the DARK GROUND is not fixable from any conf file.
rem BlendType::kEmissive (6) forces m_isUnordered, which puts the dome in
rem Tlas::Unordered ONLY, and every shadow / NEE / ReSTIR-GI / indirect / volumetric
rem ray traces Tlas::Opaque. The one bridge is the NEE cache, gated on
rem rtx.enableUnorderedEmissiveParticlesInIndirectRays, which is already False in
rem bin\user.conf line 31 - and user.conf beats rtx.conf unconditionally, so putting
rem it in rtx.conf would be a silent no-op. The real fix is a
rem remixapi_LightInfoDomeEXT; the likely-sufficient fix is the sun coming back.
rem
rem =============================================================================
rem === ROUND 27 ================================================================
rem =============================================================================
rem NOTHING ABOVE THIS LINE WAS EDITED. Every knob below is BRAND NEW - no
rem existing value was changed and no knob got a second "set" line. Checked.
rem
rem === ITEM 1: THE SMOKE, THE MISSILE TRAILS AND THE EXPLOSIONS =================
rem Open since round 4, root-caused in round 26, replayed here.
rem
rem WHAT WAS WRONG. Six vertex programs build their quad INSIDE the vertex
rem program, so the 4x4 into HPOS takes a computed temp instead of an attribute
rem and the matcher refuses every one of them with "no matrix chain into HPOS".
rem That refusal is fail=lay_other, and it was 209,904 DROPPED DRAWS in the
rem round-26 run - the whole particle system, never reaching Remix at all.
rem
rem WHAT SHIPS. The backend now replays the expansion on the CPU from the stored
rem ucode's own algebra. It is not a guess: all six programs were disassembled
rem and the closed forms were checked against a numeric RSX vertex-program
rem emulator, agreeing to 3.6e-15.
rem
rem AND ONE MEASUREMENT THAT MADE IT SAFE: these are NOT one-vertex point
rem sprites. The guest already submits FOUR REAL VERTICES per quad - 6,148
rem census lines across all six programs, every vertex count divisible by 4,
rem zero exceptions - and the shader only DISPLACES them. So the replay rewrites
rem positions in place and never changes a vertex count or an index buffer.
rem
rem TWO FAMILIES, TWO LISTS, because they are different geometry and one can be
rem right while the other is wrong. Blank either line on its own.
rem   family A - camera-facing rotating sprite (smoke puffs, explosions)
set "RPCS3_REMIX_PARTICLEBILLBOARDVP=2D5186A8011589B8,2FC998873A9F54B4,4ECE0A28F80FFC72,946A6296D06C4AF8"
rem   family B - the trail ribbon (missile/rocket trails), two endpoints, no roll
set "RPCS3_REMIX_PARTICLERIBBONVP=315E21388632FE3F,391B10C33305C812"
rem Two of the six index an ANIMATION GRID inside the atlas rect instead of using
rem the rect directly. This needs its own list rather than detection, because the
rem other four also feed ATTR9 (they read only .w, as an opaque passthrough), so
rem "ATTR9 exists" is not the test and applying the grid to them would put every
rem particle on the wrong cell. Blank this if the particles are correctly PLACED
rem but showing the wrong frame of their animation.
set "RPCS3_REMIX_PARTICLEFLIPBOOKVP=946A6296D06C4AF8,391B10C33305C812"
rem THE ACCEPTANCE INSTRUMENT. 'Remix particle:' lines in bin\remix_dump.log, one
rem per (program, outcome) per window. It prints the decoded centre, the authored
rem size, and - the field that matters most - the EYE the replay billboarded from
rem (c26) beside the backend's OWN camera position. If those two disagree, the
rem particle pass draws through a different camera than the gauge anchor and the
rem replayed world is not our world. That is the one assumption in this route the
rem ucode alone could not settle, so read it first. 0 removes the lines.
set "RPCS3_REMIX_PARTICLECENSUS=32"
rem Replay the atlas sub-rect UV as well as the position. Set to 0 to isolate
rem "the particles are in the wrong place" from "the particles are in the right
rem place but textured wrong" - with it off they fall back to the fixed 1/4096
rem divisor, which for these programs is junk, so 0 is a DIAGNOSTIC not a fix.
set "RPCS3_REMIX_PARTICLEUV=1"
rem *** 2026-08-17 ROUND 28: NEW KNOB. Re-read the flipbook PHASE per quad. ***
rem Of the three values the animation grid carries, gx and gy are per-effect
rem constants but PHASE (v9.z) is a PER-PARTICLE age. Round 27 read all three once,
rem from vertex 0 of the draw, and applied that one cell to the whole batch - and
rem these draws batch heavily (censused vertex counts up to 1,620, i.e. 405 quads
rem in ONE draw). So every particle of an explosion was pinned to particle 0's
rem animation frame; if that frame is a spent cell of the sheet the whole batch is
rem invisible. That is the exact shape of "gun smoke and impacts render, explosions
rem do not". It cannot regress a genuinely per-effect phase, but by VALUE not by
rem pointer: map_attribute() refuses a register-sourced attribute outright, so such
rem an ATTR9 turns the flipbook path OFF rather than aliasing vertex 0; if the
rem stream really carries one phase per effect, every vertex holds that same value.
rem REVERT: set this to 0. That restores round 27's draw-wide read alone.
set "RPCS3_REMIX_PARTICLEFLIPPHASE=1"
rem WATCH: particle=<draws>/<quads> on the "Remix live:" line climbing while
rem   smoke or a trail is on screen, and particle_declined= (ROUND 28, NEW) as its
rem   partner: particle + particle_declined IS the whole matched population.
rem *** DO NOT use lay_other for this. ROUND 28 REFUTES round 27's own acceptance
rem   test. lay_other is raised inside per_draw_transform() at the !fp.has_outer()
rem   exit, which these six programs take on EVERY draw whether the replay then
rem   succeeds or not - the replay only forces the transform to identity afterwards.
rem   So lay_other counts the POPULATION, not the drop, and cannot fall. MEASURED,
rem   newest run: lay_other=130,728 against particle=125,830/3,907,165.
rem   *** AND DO NOT read the 4,898 difference as particle_declined's expected value.
rem   lay_other fires for ANY !has_outer() program, only inside the has_reference
rem   branch, and double-counts under RPCS3_REMIX_DUMP. What IS measured is that the
rem   'Remix layother:' census named exactly these six programs and no seventh, out
rem   of a 64-line budget it never filled. particle_declined is sound on its own
rem   terms; it is not lay_other-minus-anything.
rem   particle + particle_declined is every matched draw THAT REACHED THE REPLAY -
rem   not every matched draw. submit_subdraw returns early in ~40 places above it.
rem   particle_ref=<attr>/<consts>/<group>/<basis> is a DIAGNOSIS, not a partition.
rem   MEASURED: 100% of particle_ref[basis]=9,039 is zeroseg and 100% of that is
rem   RIBBONS - a trail whose two endpoints coincide. The guest's own RSQ(0) makes
rem   the same quad vanish, so this is faithful, not lost geometry. particle_zeroseg
rem   (ROUND 28, NEW) reports it directly instead of via the census.
rem   particlevps=0/0 on the banner means neither list parsed - check for typos.
rem *** ALSO MEASURED, AND IT IS THE STRONGEST UNACTED LEAD OF ROUND 28. ***
rem   'Remix particle:' prints eye=[...] beside cam=[...] precisely so that "does the
rem   particle pass use a different camera than the world?" is a measurement rather
rem   than an assumption. Over all 681 census lines of the newest run:
rem     five of the six programs agree - max L1 |eye-cam| of 1.34, and four of them
rem     are under 0.34, so their particles really are in our world;
rem     2FC998873A9F54B4 does NOT - 45 of its 58 censused draws read an eye up to
rem     421 UNITS away from the backend's camera, and every one was submitted.
rem   Those particles are being placed hundreds of units from where the guest drew
rem   them. If you see a class of smoke appearing somewhere it should not, remove
rem   2FC998873A9F54B4 from the family-A list above and leave the other three:
rem     set "RPCS3_REMIX_PARTICLEBILLBOARDVP=2D5186A8011589B8,4ECE0A28F80FFC72,946A6296D06C4AF8"
rem   That is a diagnosis, not a fix - the fix is finding out why c26 disagrees.
rem REVERT: blank both VP lines. That restores round 26 byte for byte.
rem RISK, stated honestly: I cannot run the game. If particles appear as
rem   stretched shards, in the wrong place, or as a wall of quads at the world
rem   origin, this is the knob and nothing else - blank the two VP lines and the
rem   scene returns exactly to what you saw in round 26.
rem
rem === ITEM 2: AIM THE SUN FROM THE SUN'S OWN SPRITE ============================
rem You asked "any way we can attach the light to the suns texture? It's not
rem clickable in game, the suns hash id is A61A3CBECA257FE0." You were right, and
rem the reason it is not clickable is the reason it works: it is a 64x64
rem SCREEN-SPACE sprite (route=2d, drawn by 2f64c2f8ffd6add1 and 2f650a38ffe6add1)
rem so it never existed as world geometry to click on. Because the game draws it
rem where its own sun APPEARS, its screen position encodes the sun's DIRECTION:
rem unproject the quad's NDC centre through the live camera and that ray is the
rem ray to the sun - per level, per frame, no texture analysis, no hand tuning.
rem
rem PRECEDENCE NOW SHIPPED:
rem     SUNMAP  >  SPRITE  >  SUNSKY  >  SUNTRACK card  >  SUNDIR
rem SUNMAP STAYS ON TOP. An earlier draft of this round demoted it below the
rem sprite, because it was one hand-tuned vector per sky texture and you reported
rem it aiming the Mantel land carrier wrongly. That premise died before shipping:
rem Haze's PBCK archives were unpacked and every level pak authors its own
rem k_scene_sun block with an azimuth and elevation, and for the jungle level the
rem file-derived direction agrees with the sky-texture centroid to 0.8 degrees in
rem azimuth. SUNMAP is now FILE-DERIVED GROUND TRUTH wherever it has an entry, and
rem ground truth outranks a derivation - including this one.
rem So the sprite's job is what it should always have been: the automatic
rem per-level source for every level SUNMAP does NOT have an entry for.
rem The SUNMAP line above was NOT touched.
set "RPCS3_REMIX_SUNSPRITE=A61A3CBECA257FE0"
rem GUARDED, because a sprite clamped to a screen edge is where the EDGE is, not
rem where the sun is, and using it would swing the light as you turn. The quad
rem must be fully inside the screen and must be small and roughly square. NDC
rem units; 0.5 is a quarter of the screen per axis.
set "RPCS3_REMIX_SUNSPRITEMAXSPAN=0.5"
rem 'Remix sunsprite:' lines carrying the NDC box, the derived direction and the
rem reject reason. This is how "the sun never moves" gets answered without a
rem guess: seen>0 with solved=0 means every sighting was REJECTED and the line
rem says by which guard. 0 removes the lines.
set "RPCS3_REMIX_SUNSPRITECENSUS=16"
rem *** 2026-08-17 ROUND 28: NEW KNOB. Stop the sun flipping when you look down. ***
rem You reported: "if I look up enough the sun light is showing the proper direction
rem but as I look down more, it changes to where it used to be." The log shows it
rem happening, in two consecutive retargets:
rem   Remix sun-retarget: travel=[0.3695 -0.8084 -0.4582] src=sprite  frame=4337
rem   Remix sun-retarget: travel=[-0.1993 -0.3827 0.9021] moved=100.2 deg src=sky
rem                                                                    frame=4339
rem The sprite can only be solved while it is FULLY on screen, so it necessarily
rem stops solving the moment you tilt far enough for the sun to leave the view -
rem and ONE FRAME LATER the precedence chain drops to the sky-texture centroid,
rem which on this level points ~100 degrees away. A sun that swings with head pitch
rem is worse than a sun that is slightly wrong and stays put.
rem This holds the last SOLVED direction for N frames. Nothing is recomputed while
rem held. 300 frames is ~5 seconds - long enough to cover any amount of looking
rem around, far short of a level. 'Remix sun-retarget:' prints src=spritehold rather
rem than src=sprite while it is holding, so the two states are never confused.
rem *** IT APPLIES TO THE SUNMAP RUNG TOO, and the round-28 review caught that before
rem it shipped. m_sky_sun_frame is republished only while that area's DOME IS DRAWN,
rem so holding only the sprite would INVERT the precedence: any two frames without a
rem dome - indoors, looking down, a cutscene - would drop SUNMAP out of the chain and
rem let the held sprite re-aim the light away from the file-derived ground truth that
rem was authoritative one frame earlier. Both rungs now take THIS value, so they can
rem never trade places because of it, and src=sunmaphold names the SUNMAP half.
rem Bounded residue, deliberate: neither latch is cleared on a level change, so for up
rem to 300 frames after a transition the previous level's direction can still be held.
rem REVERT: set this to 0. That restores round 27's one-frame rule on BOTH rungs.
rem 2026-08-17: RAISED 300 -> 999999. The user named the cause without knowing it:
rem "I have to look up for the sun light to be properly aligned on the
rem landcarrier and like ~10 seconds later it returns to its old position."
rem 300 frames IS ~10 seconds at 30 fps. So the sprite solves correctly while it
rem is on screen, the hold keeps that answer for 300 frames, the hold expires,
rem and the light reverts to the stored SUNMAP/SUNDIR value - which on the
rem carrier is wrong. The timing is not a coincidence, it is the knob.
rem 999999 makes the hold effectively permanent: the last SOLVED sprite direction
rem persists until a newer solve replaces it. A stale-but-correct sun beats a
rem fresh-but-wrong one, and there is no scene in this game where the sun should
rem move on its own.
rem WATCH: the carrier's sunlight should stop snapping back after you look away.
rem 'Remix sunsprite:' lastgood= should stay populated; src= should stop
rem alternating between sprite and sunmap on that level.
rem REVERT: 300.
set "RPCS3_REMIX_SUNSPRITEHOLD=216000"
rem WATCH: sunsprite=<seen>/<solved> on the "Remix live:" line, and
rem   "Remix sun-retarget: ... src=sprite" in bin\remix_dump.log. src= is the
rem   field that says which source won; if it still reads sunmap, the sprite is
rem   not solving and the census says why.
rem *** ROUND 28 ALSO FIXED THE DOMINANT REJECTION, IN CODE, NOT HERE. MEASURED:
rem   of 1,258 'Remix sunsprite:' census lines in the newest run, ALL 610 `nocam`
rem   rejections read camvalid=1 vpinv=0 - the camera was present and simply carried
rem   no view*projection inverse. Cause: the archetype-B split path was the only one
rem   of the three consider_camera_candidate() sites that never filled
rem   has_view_proj_inverse, so the field kept its `false` default. Fixed at source.
rem   sunsprite_rej[offscreen/span/nocam/backwards] now has a FOURTH slot: `backwards`
rem   was folded into nocam and it is the one verdict that means the sun would be
rem   aimed 180 degrees the wrong way. It measured ZERO; any non-zero is new.
rem REVERT: blank RPCS3_REMIX_SUNSPRITE. That puts SUNMAP back on top with no
rem   rebuild, and the whole chain returns to round 26.
rem
rem === ITEM 3: THE ARMS - WHAT I FOUND, AND WHY I SHIPPED NO FIX ================
rem The brief said "the arms SHOULD now be the only tagged thing - verify
rem vm_tagged_pair is small and sane, and then VMBASIS finally has a correctly
rem tagged object to act on". I verified it, and the premise does not survive:
rem
rem   MEASURED, round-26 build's own run:  vm_tagged=1   vm_considered=13,631,618
rem
rem The viewmodel tag fires ONCE in a 72,000-frame session. VMBASIS=6 is acting on
rem essentially nothing, so the arms you are looking at are NOT going through the
rem viewmodel path at all - they are ordinary world geometry, and nothing about
rem their orientation can be fixed by tuning VMBASIS until that changes.
rem
rem The brief's proposed fix - a (vp, fp) gate - is also refuted by measurement:
rem   * your picks of BOTH the weapon (vtx=2140) and the arms (vtx=4456) read the
rem     SAME fp=0ccd70030837ee85 on vp=830d7d1b9681c475, so fp does not separate
rem     the weapon from the arms;
rem   * that same fp draws four WORLD textures (1CDD5249E6504F13 alone has 164
rem     fpcandidate rows and 79 world-refused rows), so (vp, fp) alone would tag
rem     world geometry - exactly the class of regression that keeps deleting
rem     bodies.
rem And the real obstacle is upstream of tagging entirely: the arms are REFUSED at
rem the world gate (fail=tail, residue 3.2-6.2 against tol=0.02, tail rescue
rem failed), not mis-tagged.
rem
rem SO THIS ROUND SHIPS THE MISSING MEASUREMENT INSTEAD OF A GUESS. 'Remix
rem fpcandidate:' now carries fp= (and dedups on it), which is the one census
rem large enough to answer "does the fragment program separate the player's rig
rem from the NPC bodies that share its albedo". Until now the only lines carrying
rem (vp, fp, albedo) together were eleven of your own clicks.
rem A (vp, fp, albedo) TRIPLE route exists and is DELIBERATELY LEFT UNARMED so it
rem can be armed next round from this file with no rebuild:
rem   set "RPCS3_REMIX_VMTRIPLEVP=830D7D1B9681C475,F39F504649B6F442"
rem   set "RPCS3_REMIX_VMTRIPLEFP=0CCD70030837EE85"
rem   set "RPCS3_REMIX_VMTRIPLEALBEDO=86885A0E60751491"
rem VMPAIRALBEDO is UNCHANGED at 86885A0E60751491, as instructed.
rem
cd /d "%~dp0bin"
start "" "rpcs3-next.exe" "E:\PS3 Games\Haze [BLUS30094]\PS3_GAME\USRDIR\EBOOT.BIN"

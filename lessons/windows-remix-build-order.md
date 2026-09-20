On this Windows checkout, build `rpcs3/emucore.vcxproj` before relinking `rpcs3/rpcs3.vcxproj` when Remix core sources change.

The standalone `rpcs3.vcxproj` target can report success while reusing an older `emucore.lib`; it does not prove that edits under `rpcs3/Emu/RSX/Remix/` compiled. On 2026-09-09, a direct app build missed a compile error in `RemixTransforms.cpp`, while a fresh Release-x64 `emucore.vcxproj` build exposed it. Verify the core build has zero errors, then relink the app.

Serialize the app and test link steps too. On 2026-09-12, running `rpcs3_test.vcxproj` and `rpcs3.vcxproj` together after the core finished caused the app's pre-link library step to fail with LNK1104 opening `build/lib/Release-x64/rpcs3.lib`: the test linker was still reading that same library. Let the test build finish, then retry the app; no process termination or source change was needed. The serialized retry succeeded.

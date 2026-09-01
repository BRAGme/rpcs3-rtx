**RPCS3 RTX Remix — preview 2**
<https://github.com/BRAGme/rpcs3-rtx/releases/tag/remix-preview-2>

58 commits on from preview 1. The headline isn't a rendering change — **the configuration finally ships**.

🔧 The main thing:
• Preview 1 shipped the backend without the 217 tuned knobs that make it work — they lived in a launcher script that never left my machine
• If it looked worse than the screenshots, that's why: you got the code and none of the tuning
• A `<TITLEID>.conf` beside the exe is now read at startup, and `BLUS30094.conf` ships in the zip with all of them and the comments behind each one
• The camera lock comes from that file now, so launching from the GUI works — before, anything but the launcher left the camera unpinned and the level warped

✅ Also fixed:
• Soldiers left yellow light trails — the old gate assumed "a lamp doesn't move", but an idle NPC doesn't either
• It now keys on cell history: every real fixture occupied 1 quantised cell all scene, the soldier card occupied 26

⚠️ Issues:
• Interior fixtures are over-lit — every bulb in a room lights where the original lights one
• Pre-existing and understood, not new: the emissive list is material-scoped so it can't say "only the lit one", and three attempted fixes are refuted and documented in the shipped config
• Haze is the only title with real work behind it — everything else is unexplored, not broken
• Still a research build; nothing is playable start to finish
• Needs a Remix Plus / extended-API runtime (stock Remix won't connect) plus your own firmware

🤝 The repo is open:
• Issues are on, and a knob set for a **second game** is the most useful thing you can send
• No git needed — paste your launcher into an issue and you're credited as co-author
• Check `camlock=` in the first ten lines of `remix_dump.log` before reporting: all zeros means the config didn't load and that session measured nothing

57 commits since preview 1. The headline is not a rendering change — it's that **the configuration finally ships**.

This is still a **research build**. One title has had real work done on it; everything else is unexplored. Read "What does not work" before downloading.

---

## The reason this build exists

**Preview 1 shipped the backend without the settings that make it work.**

RPCS3 has no per-game config mechanism, so every tuned value in this project arrived through `RPCS3_REMIX_*` environment variables set by a launcher script that never left the developer's machine. That's not a handful of overrides — measured, it's **217 knobs**. Camera lock, guest lights, sky classification, the viewmodel rig, the anchor gauge, all of it.

Anyone who downloaded preview 1 got the code and none of the configuration. If it looked worse than the screenshots, that is why.

Preview 2 adds a loader: a file named `<TITLEID>.conf` beside `rpcs3.exe` is read once at backend init and pushed into the environment before any knob is read. **`BLUS30094.conf` ships in the zip** with all 217 settings and the comments explaining why each is what it is.

It also retires a trap that cost a whole round of work. The launcher used to carry `CAMLOCKVP`, so starting the game any other way left the camera unpinned — `camlock=0000000000000000`, `cam_fallback` at 43.5% against a healthy 3.5%, and a 114.6° FOV with a 6.1-unit far plane that clips the level. A play-test came back as *"the entire area is warping like crazy"* and it was chased as a rendering fault before that line showed nothing had ever been armed. **Launching from the GUI now gets the full configuration.**

Why load-once rather than a live poll: 177 knob accessors in this backend are `static const` and latch on first call. A per-frame loader would have been a silent no-op for three quarters of what it loaded.

---

## Also fixed

**Soldiers left yellow light trails.** Round 48 added a gate reasoning that "a lamp does not move" — true, but an idle NPC doesn't move either, so no threshold on dwell time separates them. Grouping every minted light by its trigger texture made the discriminator obvious:

| trigger | distinct cells occupied |
|---|---|
| suit glow card | **26** |
| ceiling fixture | 1 |
| ceiling fixture | 1 |
| ceiling fixture | 1 |

Every real fixture occupies exactly one quantised cell for a whole scene. The thing on legs occupied 26. `RPCS3_REMIX_GUESTLIGHTCELLS` disqualifies a source once it has been seen in more than N cells — a property an idle NPC cannot fake, where dwell time is one it can.

**A camera sanity census.** `SetupCamera` validates nothing, so a degenerate or ortho matrix that wins the election becomes a live world camera and nothing says so. `RPCS3_REMIX_CAMSANITY` measures the deviation against the title's *own* latched median FOV — 114.6 isn't absurd in the abstract, only against Haze's 72.0. Ships at census-only; it names the problem without changing behaviour.

**Six silent return paths made countable.** The guest-light pipeline could report 31,845 candidates matched and zero lights created, with every existing counter reporting a small honest number for a path that wasn't the problem. Four of its seven returns had no counter at all. They do now, and it immediately refuted two hypotheses that looked right on paper.

---

## What does not work

**Interior fixtures are over-lit.** Every bulb in a room lights, where the original lights one. The cause is understood and documented: `RPCS3_REMIX_EMISSIVE` is *material-scoped*, so every surface sharing the bulb texture emits and the list cannot express "only the lit one". Three candidate fixes were tried and refuted by play-test — the reasoning and measurements are in `BLUS30094.conf` so nobody repeats them. This is pre-existing, not new in preview 2.

**One title.** Haze (`BLUS30094`) is the only game with real work behind it. Anything else is unexplored rather than known-broken — and finding out is the most useful thing you can do with this build.

**Still a research build.** Nothing here is playable start to finish.

---

## You need two things that are not in the zip

**A Remix runtime on the extended API line** — stock NVIDIA RTX Remix will not connect. See `SETUP.txt` inside the zip for where to get it and where to put it.

**A PS3 firmware dump**, same as stock RPCS3.

---

## Contributing

Issues are open, with templates for [a bug](https://github.com/BRAGme/rpcs3-rtx/issues/new?template=remix_bug_report.yaml) and for [settings that worked on a title](https://github.com/BRAGme/rpcs3-rtx/issues/new?template=game_settings.yaml).

The most useful contribution is a knob set for a **second game**. You don't need git — paste your launcher into an issue and you get credited as co-author.

One habit worth adopting before reporting anything: check `camlock=` in the first ten lines of `remix_dump.log`. If it reads all zeros the configuration didn't load, and nothing measured in that session means what it appears to.

AI-assisted work is welcome and needs no disclosure. The one ask, in [CONTRIBUTING.md](https://github.com/BRAGme/rpcs3-rtx/blob/remix-backend/CONTRIBUTING.md), is to distinguish what you measured from what you tuned by eye — "I turned it up until it looked right" is a good answer.

---

The sibling project, the same idea for PlayStation 2, is [pcsx2-rtx-remix](https://github.com/BRAGme/pcsx2-rtx-remix).

---

`rpcs3-rtx-remix-f0759abc-win64.zip`
SHA256 `37a9f9759bf623ccc047c4dc342fc2929f1a7d8f645804f3c590bc3f076b5d8d`

Built from `remix-backend` commit `f0759abc`, tagged `remix-preview-2`.

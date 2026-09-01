# Contributing

This is a research fork of RPCS3 that submits PS3 geometry to the RTX Remix runtime. It is not
finished, and the most valuable help is not code.

If you only read one section, read [The one rule](#the-one-rule).

---

## What helps most

**Try a second game.** Essentially all the work so far has been on one title. Anything you learn on
a different game is new ground — even "it boots and submits nothing" is a data point nobody has.

**A log from a title nobody has tried.** `remix_dump.log` next to the executable often says what a
title needs without anyone else owning the disc: whether a camera was elected, how many draws were
submitted, what was refused.

**The `RPCS3_REMIX_*` set that got a game rendering.** There is no per-game config file in this
fork — settings come through environment variables, usually from a launcher script. Share the
launcher.

**Code**, if you want to.

---

## Read `camlock=` before trusting any run

The first ten lines of `remix_dump.log` carry a `camlock=` value. This is the single most useful
habit in this project:

- `camlock=7f3d3abcefc8b057` — the launcher ran, the knobs are armed, the run means something.
- `camlock=0000000000000000` — the launcher did **not** run. Every knob is at its built-in default,
  the camera pin is absent, and **no measurement from that session means anything.**

This has cost real time more than once. A play-test came back as "the entire area is warping like
crazy" and the warping was chased as a rendering fault before that line showed the emulator had been
started straight from the executable, so nothing was ever armed. Check it first, every time.

---

## The one rule

**Only write down numbers you actually measured on the title you are describing.**

Comments and reports in this repo are written as evidence, and someone else tunes against them. An
unverified value is fine — an unverified value *labelled as unverified* is genuinely useful, because
it says "this is where to start" without claiming more. What is not fine is the two being
indistinguishable.

"I turned it up until it looked right" is a good answer. Write that, rather than something that
reads like a measurement.

---

## AI-assisted work

**Welcome, no disclosure required.** Much of this fork was written with AI assistance and the
commits say so in a `Co-Authored-By` trailer. It will not count against your contribution.

The caveat is the rule above, because it is the specific way this goes wrong: **a model asked to
fill in settings or a report will invent measurements that look exactly like real ones.** Confident,
plausible, correctly formatted, and untrue. Use whatever tools you like, then check the numbers
against the actual game before writing them down as fact — or label them as unchecked.

---

## How to send something

**No git needed.** Open an issue — there are templates for
[a bug](.github/ISSUE_TEMPLATE/remix_bug_report.yaml) and for
[settings that worked](.github/ISSUE_TEMPLATE/game_settings.yaml).

**With git:** fork, branch, commit, open a pull request against `remix-backend`. That is the default
branch, so a fork lands on it automatically. Merging keeps your name as the author of your commits.

**To just ask something:** this project is discussed on the
[RTX Remix Showcase Discord](https://discord.gg/j6sh7JD3v9), the community server for Remix modding
generally. Good for a quick question; open an issue for anything that should be tracked.

---

## Style, if you are writing prose or comments

Not rules, just what the existing files do:

- Measured numbers rather than adjectives.
- Say what was refuted as well as what worked. A large share of the comments in this repo exist only
  to stop the next person re-running a test that already failed.
- Record why a value is what it is, not just what it is.

---

## What belongs upstream

If it reproduces with the Remix backend **off**, it is an upstream RPCS3 issue and they can help far
more than we can. Test that first.

---

The sibling project, the same idea for PlayStation 2, is
[pcsx2-rtx-remix](https://github.com/BRAGme/pcsx2-rtx-remix).

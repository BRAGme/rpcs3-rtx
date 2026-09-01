#!/usr/bin/env bash
# Does the guest-light rule mint lights on MOVING emitters?
#
# This is the measurable half of "soldiers leave yellow light trails". A bolted-down fixture
# mints one light at one position and re-mints there; a walking NPC mints at a new quantised
# cell every 0.25 units it travels, so the same trigger albedo turns up at many positions. That
# spread is the trail, and it is visible in the log without looking at the screen.
#
# HOW TO USE IT: get into gameplay, walk past some soldiers, then run this. It reads the whole
# of bin/remix_dump.log, so run it against a log from a session where soldiers were on screen --
# a menu or an intro mints nothing and the script will correctly say so.
#
#   bash tools/check-light-trails.sh [path-to-remix_dump.log]
set -euo pipefail

LOG="${1:-$(cd "$(dirname "$0")/.." && pwd)/bin/remix_dump.log}"

if [ ! -f "$LOG" ]; then
  echo "no log at $LOG"
  exit 1
fi

TOTAL=$(grep -ac "Remix guest-light:" "$LOG" || true)
echo "guest-light mint lines: $TOTAL"

if [ "$TOTAL" = "0" ]; then
  echo
  echo "Nothing minted. Either the trigger never matched, or the session never reached a scene"
  echo "with the trigger geometry in it. Not evidence either way -- get into gameplay first."
  exit 0
fi

echo
echo "Positions per trigger albedo. ONE position is a fixture. MANY is a moving emitter, and the"
echo "count is roughly how long the trail is."
echo

grep -a "Remix guest-light:" "$LOG" \
  | sed -E 's/.*pos=\[([^]]*)\].*albedo=([0-9A-Fa-f]+).*/\2 [\1]/' \
  | sort \
  | awk '
      { albedo=$1; $1=""; pos=$0; key=albedo SUBSEP pos;
        if (!(key in seen)) { seen[key]=1; distinct[albedo]++ }
        total[albedo]++ }
      END {
        for (a in distinct)
          printf "  albedo=%s  distinct positions=%d  mints=%d%s\n",
                 a, distinct[a], total[a],
                 (distinct[a] > 8 ? "   <-- MOVING EMITTER, this is a trail" : "")
      }' \
  | sort -t= -k3 -rn

echo
echo "If any line is flagged, the static-fixture gate is not catching it. Check whether that"
echo "trigger came from the AUTO rule or from an explicit GUESTLIGHTVP/FP/ALBEDO list: the gate"
echo "in RemixGSRender.cpp reads"
echo
echo "    stable_frames != 0 && auto_trigger && !trigger_accepted"
echo
echo "so an EXPLICIT trigger skips it entirely by design, and the shipped BLUS30094.conf uses one."

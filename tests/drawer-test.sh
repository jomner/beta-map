#!/bin/bash
# beta-map-draw against the Python drawing, and its redraws. WORLD is a Beta world folder (a copy: this adds two
# oddly named region files to it); it takes a few minutes for a handful of regions. Exit code 0 if all passed.
#   bash tests/drawer-test.sh WORLD
# Checks:
#   1  the drawer's site is pixel for pixel the Python drawing's
#   2  built without -std=c99 and with -march=native (fused multiply-add allowed): still the same (the pragma)
#   3  a run with --no-drawer in between (which removes the drawer's tiles), then the drawer: everything comes back
#   4  the site deleted but the cache kept: everything comes back
#   5  a run stopped halfway: the site doesn't change; the next run finishes it
#   6  --views 1, then 4 again: the same as the Python drawing
#   7  region files named r.00.0.mcr and r.+0.0.mcr are ignored by both drawers
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); WORLD=$(cd "$1" && pwd)
W=$(mktemp -d /tmp/bm-test-XXXX); BM="$W/bm"; fails=0
mkdir -p "$BM" && cp "$HERE/beta_map.py" "$HERE/beta-map-draw.c" "$BM/" && cp -r "$HERE/web" "$BM/"
cc -std=c99 -O2 -Wall -o "$BM/beta-map-draw" "$BM/beta-map-draw.c" -lz -lm || exit 1
cc -O2 -march=native -o "$W/bmd-fused" "$BM/beta-map-draw.c" -lz -lm || exit 1
check() { if python3 "$HERE/tests/compare.py" "$W/py" "$2" > "$W/cmp.log" 2>&1; then echo "PASS $1"; else echo "FAIL $1"; tail -5 "$W/cmp.log"; fails=$((fails + 1)); fi; }
draw() { python3 "$BM/beta_map.py" "$WORLD" "$@" > "$W/run.log" 2>&1 || { echo "  (run failed)"; tail -3 "$W/run.log"; }; }
tilesum() { find "$1/tiles/c" -path '*/.next' -prune -o -name '*.png' -print0 | sort -z | xargs -0 cat | md5sum; }

r0=$(ls "$WORLD/region" | head -1)
cp "$WORLD/region/$r0" "$WORLD/region/r.00.0.mcr"; cp "$WORLD/region/$r0" "$WORLD/region/r.+0.0.mcr"
draw "$W/py" --no-drawer; cp "$W/run.log" "$W/py-run.log"
draw "$W/c"; check "1 the same pixels as Python" "$W/c"
draw "$W/f" --drawer "$W/bmd-fused"; check "2 built with fused multiply-add allowed" "$W/f"
draw "$W/c" --no-drawer; draw "$W/c"; check "3 after a Python run in between" "$W/c"
rm -rf "$W/c"; draw "$W/c"; check "4 after the site was deleted (cache kept)" "$W/c"
before=$(tilesum "$W/c")
# a region really changed (another region's contents in its file), and the run made to fail partway: its cache
# can't be written, so it stops after drawing that region's top-down tiles and before any tile is moved into place
set -- $(ls "$WORLD/region" | grep -E '^r\.-?[0-9]+\.-?[0-9]+\.mcr$' | head -2)
cp "$WORLD/region/$1" "$W/keep.mcr"; cp "$WORLD/region/$2" "$WORLD/region/$1"
chmod a-w "$W/c-cache/c"
"$BM/beta-map-draw" "$WORLD" "$W/c-cache/c" "$W/c/tiles/c" > "$W/stop.log" 2>&1; code=$?
chmod u+w "$W/c-cache/c"
if [ $code -ne 0 ] && grep -q "drawing" "$W/stop.log" && [ "$(tilesum "$W/c")" = "$before" ]; then
  echo "PASS 5a a run that failed after drawing changed nothing on the site"
else echo "FAIL 5a exit $code, site changed: $([ "$(tilesum "$W/c")" = "$before" ] && echo no || echo yes)"; tail -3 "$W/stop.log"; fails=$((fails + 1)); fi
cp "$W/keep.mcr" "$WORLD/region/$1"                     # (back as it was, so the Python drawing still applies)
draw "$W/c"; check "5b the next run is right again" "$W/c"
draw "$W/c" --views 1; draw "$W/c"; check "6 --views 1, then all four again" "$W/c"
want=$(ls "$WORLD/region" | grep -cE '^r\.-?(0|[1-9][0-9]*)\.-?(0|[1-9][0-9]*)\.mcr$')
got_py=$(grep -oE '[0-9]+ Overworld' "$W/py-run.log" | head -1 | cut -d' ' -f1)
got_c=$(grep -oE '^[0-9]+ Overworld' "$W/run.log" | head -1 | cut -d' ' -f1)
if [ "$got_py" = "$want" ] && [ "$got_c" = "$want" ]; then echo "PASS 7 odd names ignored ($want regions in both)"
else echo "FAIL 7 regions: expected $want, Python $got_py, drawer $got_c"; fails=$((fails + 1)); fi
rm -f "$WORLD/region/r.00.0.mcr" "$WORLD/region/r.+0.0.mcr"
rm -rf "$W"
echo "$fails failure(s)"; exit $((fails > 0))

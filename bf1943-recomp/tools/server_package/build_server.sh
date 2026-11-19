#!/usr/bin/env bash
# Builds the dedicated server (only the server: no window, no launcher) of
# Battlefield 1943 or, with --bfbc1, Battlefield: Bad Company, from your own
# game files, on Linux or macOS. Run it once per game; then start_server.sh.
#
#   ./build_server.sh --game <game package | disc image .iso | default.xex | game folder>
#                     [--bfbc1] [--jobs N] [--recompile]
#
# Battlefield 1943: the Xbox 360 package or folder of version 1.06.
# Bad Company: the USA disc image (.iso) or a folder / default.xex of it.
# The game files are unpacked (or linked, for a folder) into
# bf1943-recomp/original/v106 or bfbc1-recomp/original/bfbc1, where start_server.sh
# finds them. The real build is bf1943-recomp/build_linux.sh --server (its
# packages are listed if missing; macOS: see build_macos.sh).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd -P)
GAME= BC=0 EXTRA=()
while [ $# -gt 0 ]; do
  case $1 in
    --game) [ $# -ge 2 ] || { echo "error: --game needs a value"; exit 1; }; GAME=$2; shift 2 ;;
    --bfbc1) BC=1; shift ;;
    --jobs) [ $# -ge 2 ] || { echo "error: --jobs needs a value"; exit 1; }; EXTRA+=(--jobs "$2"); shift 2 ;;
    --recompile) EXTRA+=(--recompile); shift ;;
    -h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown option $1 (see --help)"; exit 1 ;;
  esac
done
[ -n "$GAME" ] || { sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; echo "error: --game is required"; exit 1; }
[ -e "$GAME" ] || { echo "error: no such file or folder: $GAME"; exit 1; }
GAME=$(cd "$(dirname "$GAME")" && pwd -P)/$(basename "$GAME")

PLAT=linux; [ "$(uname -s)" = Darwin ] && PLAT=macos
if [ $BC = 1 ]; then
  TREE=$HERE/bfbc1-recomp; DATA=$TREE/original/bfbc1; UNPACK=xiso_extract.py; OUT=$TREE/build/$PLAT-bfbc1-server
else
  TREE=$HERE/bf1943-recomp; DATA=$TREE/original/v106; UNPACK=stfs_extract.py; OUT=$TREE/build/$PLAT-server
fi
if [ ! -f "$DATA/default.xex" ]; then
  mkdir -p "$(dirname "$DATA")"
  if [ -d "$GAME" ]; then
    # a folder: linked, not copied (the game is large)
    rm -rf "$DATA"; ln -s "$GAME" "$DATA"
  elif [ "$(basename "$GAME" | tr A-Z a-z)" = default.xex ]; then
    rm -rf "$DATA"; ln -s "$(dirname "$GAME")" "$DATA"
  else
    echo "Unpacking $GAME into $DATA"
    python3 -I "$HERE/bf1943-recomp/tools/$UNPACK" "$GAME" "$DATA"
  fi
fi
[ -f "$DATA/default.xex" ] || { echo "error: no default.xex found in $DATA"; exit 1; }

BCARGS=()
[ $BC = 1 ] && BCARGS=(--bfbc1 --bc-tree "$TREE")
"$HERE/bf1943-recomp/build_linux.sh" --server --game "$DATA" --out "$OUT" ${BCARGS[@]+"${BCARGS[@]}"} ${EXTRA[@]+"${EXTRA[@]}"}
echo
echo "Done. Start the server with: ${TREE#$HERE/}/start_server.sh"

#!/usr/bin/env bash
# Builds Battlefield 1943 for Linux (bf1943: Vulkan, SDL3) or its dedicated
# server (bf1943_server) from this tree and your own copy of the game, in one
# command: what the Windows launcher's setup (launcher/setup.js) does. On
# macOS (build_macos.sh runs this) the same, as an x86-64 program that Apple
# Silicon Macs run through Rosetta (macOS 15 or newer: AVX2), its Vulkan
# through MoltenVK (the LunarG Vulkan SDK).
#
#   ./build_linux.sh --game <game folder | default.xex | STFS package>
#                    [--out <build dir>] [--server] [--jobs N] [--recompile]
#                    [--bfbc1 [--bc-tree <dir>]]
#
#   --game       the game files: a folder with the v1.06 default.xex in it (and
#                the rest of the game), that default.xex, or the Xbox 360
#                package (unpacked into <out>/gamefiles)
#   --out        build folder (default: build/linux-client, --server: build/linux-server)
#   --server     the dedicated server instead of the game
#   --jobs N     parallel compile jobs (default: from cores and free memory)
#   --recompile  run XenonRecomp again even if generated/ppc is there
#   --bfbc1      Battlefield: Bad Company instead (bfbc1 / bfbc1_server; the
#                game files: its folder, default.xex or the disc image .iso,
#                unpacked into <out>/gamefiles); also --title bfbc1
#   --bc-tree    Bad Company's tree (default: ../src-bfbc1 or ../bfbc1-recomp)
#
# Steps: check the packages (prints the install command, never runs sudo),
# check the game files, build XenonRecomp (thirdparty/XenonRecomp), make the
# recompiler config if the tree has none (tools/make_recomp_config.py), run
# XenonRecomp into generated/ppc, compile Xenia's shaders (build/xenia_shaders,
# with glslangValidator / spirv-opt), configure and build with clang, then put
# the exe and the files it needs in <out>/game (Bad Company: and its main
# menu's PC SETTINGS row in <out>/userdata, tools/bc_menu_patch.py), with the
# bots' navigation tool (tools/navgen; tools/make_navs.py makes the maps'
# meshes with it, into <out>/game/nav).
#
# Output for a launcher: "progress <step> <done> <total>" lines (steps: check
# game tools config recompile shaders configure build install, and "all" for the
# whole run). The last line is "ok <exe>" on success, "error: <why>" (exit 1)
# on failure. Full logs: <out>/logs.
set -u
set -o pipefail

TREE=$(cd "$(dirname "$0")" && pwd -P)
XEX_SHA1=b5aaa325ff358d66a116000931b7204da3f14534    # default.xex v1.06
XEX_SHA1_105=1c1f22606fa1eff15bf0409214279118dc805c28 # v1.05

GAME= OUT= SERVER=0 JOBS= RECOMPILE=0 BC=0 BC_TREE=
OS=$(uname -s)
PLAT=linux; [ "$OS" = Darwin ] && PLAT=macos
usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; }
die() { printf 'error: %s\n' "$*"; exit 1; }
need_arg() { [ $# -ge 2 ] && [ -n "$2" ] || die "$1 needs a value"; }
while [ $# -gt 0 ]; do
  case $1 in
    --game) need_arg "$@"; GAME=$2; shift 2 ;;
    --out) need_arg "$@"; OUT=$2; shift 2 ;;
    --server) SERVER=1; shift ;;
    --jobs) need_arg "$@"; JOBS=$2; shift 2 ;;
    --recompile) RECOMPILE=1; shift ;;
    --bfbc1) BC=1; shift ;;
    --title) need_arg "$@"
      case $2 in bfbc1) BC=1 ;; bf1943) BC=0 ;; *) die "--title: bf1943 or bfbc1, not $2" ;; esac
      shift 2 ;;
    --bc-tree) need_arg "$@"; BC_TREE=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; die "unknown option: $1" ;;
  esac
done
[ -n "$GAME" ] || { usage; die "--game <folder with the game files> is required"; }
[ -e "$GAME" ] || die "no such game folder or file: $GAME"
# The game's own tree (Bad Company: its sources and recompiler config; the
# shared code is this tree's) and its name.
GTREE=$TREE NAME=bf1943 TITLE="Battlefield 1943 1.06"
if [ $BC = 1 ]; then
  if [ -z "$BC_TREE" ]; then
    for d in "$TREE/../src-bfbc1" "$TREE/../bfbc1-recomp"; do
      [ -f "$d/game/linux/CMakeLists.txt" ] && { BC_TREE=$d; break; }
    done
  fi
  [ -n "$BC_TREE" ] && [ -f "$BC_TREE/game/linux/CMakeLists.txt" ] ||
    die "Bad Company's tree (src-bfbc1 or bfbc1-recomp) is missing"
  GTREE=$(cd "$BC_TREE" && pwd -P) NAME=bfbc1 TITLE="Battlefield: Bad Company (USA disc)"
  XEX_SHA1=c1ad8ac5c18b1b0b23a4c3476721462852af1a93 XEX_SHA1_105=
fi
if [ $SERVER = 1 ]; then TARGET=${NAME}_server; else TARGET=$NAME; fi
if [ -z "$OUT" ]; then
  if [ $BC = 1 ]; then OUT=$TREE/build/$PLAT-bfbc1; [ $SERVER = 1 ] && OUT=$OUT-server
  elif [ $SERVER = 1 ]; then OUT=$TREE/build/$PLAT-server; else OUT=$TREE/build/$PLAT-client; fi
fi
mkdir -p "$OUT/logs" || die "cannot create $OUT"
OUT=$(cd "$OUT" && pwd -P)
LOGS=$OUT/logs

if [ $SERVER = 1 ]; then STEPS=(check game tools config recompile configure build install)
else STEPS=(check game tools config recompile shaders configure build install); fi
STEP_N=0
progress() { printf 'progress %s %s %s\n' "$1" "$2" "$3"; }
begin() {  # begin <step> <text>
  STEP=$1
  STEP_N=$((STEP_N + 1))
  printf '==> [%d/%d] %s\n' "$STEP_N" "${#STEPS[@]}" "$2"
  progress "$STEP" 0 1
}
finish() {
  progress "$STEP" 1 1
  progress all "$STEP_N" "${#STEPS[@]}"
}
# fail <log> <message>: the log's tail, then the error line.
fail() {
  if [ -s "$1" ]; then echo "---- last lines of $1:"; tail -n 25 "$1"; echo "----"; fi
  die "$2"
}
# run_log <log> <command...>: runs it with all output to the log.
run_log() {
  local log=$1; shift
  "$@" > "$log" 2>&1
}
# run_ninja <log> <command...>: a CMake/Ninja build, "[n/m]" lines turned into
# progress lines (at most one per percent).
run_ninja() {
  local log=$1; shift
  : > "$log"
  local last=-1 line n m pct
  "$@" 2>&1 | while IFS= read -r line; do
    printf '%s\n' "$line" >> "$log"
    case $line in
      \[*/*\]*)
        n=${line#\[}; m=${n#*/}; n=${n%%/*}; m=${m%%\]*}
        case $n$m in *[!0-9]*|'') continue ;; esac
        [ "$m" -gt 0 ] || continue
        pct=$((n * 100 / m))
        if [ $pct != $last ] || [ "$n" = "$m" ]; then progress "$STEP" "$n" "$m"; last=$pct; fi ;;
    esac
  done
  return "${PIPESTATUS[0]}"
}
if [ "$OS" = Darwin ]; then
  sha1() { shasum -a 1 "$1" | cut -d' ' -f1; }
  sha1sum() { shasum -a 1 "$@"; }
else
  sha1() { sha1sum "$1" | cut -d' ' -f1; }
fi
ver_ge() {  # ver_ge <have> <want>: dotted versions
  # (awk, not sort -V: macOS's sort may not have it)
  awk -v a="$1" -v b="$2" 'BEGIN {
    n = split(a, x, "."); m = split(b, y, "."); if (m > n) n = m
    for (i = 1; i <= n; i++) { if (x[i] + 0 > y[i] + 0) exit 0; if (x[i] + 0 < y[i] + 0) exit 1 }
    exit 0 }'
}

# ---- 1. packages ---------------------------------------------------------------
begin check "Checking the build tools and libraries"
CMAKE_PLAT_OPTS=()
if [ "$OS" = Darwin ]; then
  # x86-64 code either way: Apple Silicon runs it through Rosetta, which has
  # AVX2 (x86-64-v3) from macOS 15; os_sync_wait_on_address needs 14.4.
  macver=$(sw_vers -productVersion 2>/dev/null)
  if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ]; then
    ver_ge "$macver" 15 || die "Apple Silicon: macOS 15 or newer is needed (Rosetta runs AVX2 from 15 on); this is $macver"
    pgrep -q oahd 2>/dev/null || /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null ||
      die "Rosetta is not installed: softwareupdate --install-rosetta --agree-to-license"
  else
    ver_ge "$macver" 14.4 || die "macOS 14.4 or newer is needed; this is $macver"
    sysctl -n machdep.cpu.leaf7_features 2>/dev/null | grep -qw AVX2 ||
      echo "warning: this CPU has no AVX2: the build works, but the game needs an x86-64-v3 (AVX2) CPU to run"
  fi
  CMAKE_PLAT_OPTS=(-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4)
elif [ "$OS" = Linux ]; then
  [ "$(uname -m)" = x86_64 ] || die "x86-64 only: the recompiled game code is built for x86-64-v3 (AVX2); this machine is $(uname -m)"
  if ! grep -qw avx2 /proc/cpuinfo 2>/dev/null; then
    echo "warning: this CPU has no AVX2: the build works, but the game needs an x86-64-v3 (AVX2) CPU to run"
  fi
else
  die "this script is for Linux and macOS (on Windows use the launcher)"
fi

DISTRO=
if [ -r /etc/os-release ]; then
  DISTRO=$( . /etc/os-release; echo "${ID:-} ${ID_LIKE:-}")
fi
APT_BASE="clang cmake ninja-build python3 pkg-config build-essential"
DNF_BASE="clang cmake ninja-build python3 pkgconf-pkg-config gcc-c++"
PAC_BASE="clang cmake ninja python pkgconf base-devel"
APT_CLIENT="glslang-tools spirv-tools libvulkan-dev libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev"
DNF_CLIENT="glslang spirv-tools vulkan-headers vulkan-loader-devel libX11-devel libxcb-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXScrnSaver-devel libXfixes-devel libxkbcommon-devel wayland-devel wayland-protocols-devel libdecor-devel alsa-lib-devel pulseaudio-libs-devel pipewire-devel systemd-devel dbus-devel"
PAC_CLIENT="glslang spirv-tools vulkan-headers vulkan-icd-loader libx11 libxcb libxext libxrandr libxcursor libxi libxss libxfixes libxkbcommon wayland wayland-protocols libdecor alsa-lib libpulse pipewire systemd-libs dbus"
if [ $SERVER = 1 ]; then APT_CLIENT= DNF_CLIENT= PAC_CLIENT=; fi
install_hint() {
  if [ "$OS" = Darwin ]; then
    echo "Install them with:"
    echo "  xcode-select --install"
    echo "  brew install cmake ninja python glslang spirv-tools"
    [ $SERVER = 1 ] || echo "  and the Vulkan SDK (MoltenVK, universal): https://vulkan.lunarg.com/sdk/home#mac"
    return
  fi
  local apt="sudo apt install $APT_BASE $APT_CLIENT"
  local dnf="sudo dnf install $DNF_BASE $DNF_CLIENT"
  local pac="sudo pacman -S --needed $PAC_BASE $PAC_CLIENT"
  case " $DISTRO " in
    *" debian "*|*" ubuntu "*) echo "Install them with:"; echo "  ${apt% }" ;;
    *" fedora "*|*" rhel "*|*" centos "*) echo "Install them with:"; echo "  ${dnf% }" ;;
    *" arch "*) echo "Install them with:"; echo "  ${pac% }" ;;
    *) echo "Install them with one of:"; echo "  Debian/Ubuntu: ${apt% }"; echo "  Fedora:        ${dnf% }"
       echo "  Arch:          ${pac% }" ;;
  esac
}

MISSING=()
BASE_CMDS="clang clang++ cmake ninja python3 sha1sum"; [ "$OS" = Darwin ] && BASE_CMDS="clang clang++ cmake ninja python3 shasum"
for c in $BASE_CMDS; do
  command -v "$c" > /dev/null 2>&1 || MISSING+=("command $c")
done
if [ $SERVER = 0 ]; then
  for c in pkg-config glslangValidator spirv-opt; do
    command -v "$c" > /dev/null 2>&1 || MISSING+=("command $c")
  done
fi
if command -v cmake > /dev/null 2>&1; then
  v=$(cmake --version | sed -n '1s/[^0-9]*\([0-9][0-9.]*\).*/\1/p')
  ver_ge "$v" 3.25 || MISSING+=("CMake 3.25 or newer (have $v; also: pip install cmake)")
fi
if command -v clang > /dev/null 2>&1; then
  v=$(clang -dumpversion 2>/dev/null)
  ver_ge "$v" 15 || MISSING+=("clang 15 or newer (have $v)")
  # clang uses GCC's C++ library on Linux: its headers must be there. (Apple's
  # clang reports its own version numbers: 15 is Xcode 15.)
  arch_flag=; [ "$OS" = Darwin ] && arch_flag="-arch x86_64"
  printf '#include <vector>\nint main() { std::vector<int> v; return int(v.size()); }\n' |
    clang++ $arch_flag -std=c++20 -x c++ - -o /dev/null > "$LOGS/check_cxx.log" 2>&1 ||
    MISSING+=("a C++ standard library for clang (Linux: libstdc++ headers; macOS: Xcode's command line tools; see $LOGS/check_cxx.log)")
fi
if command -v python3 > /dev/null 2>&1; then
  python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' || MISSING+=("Python 3.8 or newer")
fi
if [ "$OS" = Darwin ] && [ $SERVER = 0 ]; then
  # The game loads the Vulkan loader (or MoltenVK) at run time: an x86-64 one,
  # so the LunarG SDK's universal libraries, not Homebrew's arm64 ones.
  VK_LIB=
  for d in "${VULKAN_SDK:-}/lib" /usr/local/lib; do
    for l in libvulkan.1.dylib libMoltenVK.dylib; do
      [ -f "$d/$l" ] && lipo -archs "$d/$l" 2>/dev/null | grep -qw x86_64 && { VK_LIB=$d; break 2; }
    done
  done
  [ -n "$VK_LIB" ] || MISSING+=("an x86-64 (universal) Vulkan loader or MoltenVK: the LunarG Vulkan SDK (set VULKAN_SDK, or its system-wide install in /usr/local/lib)")
elif [ $SERVER = 0 ] && command -v pkg-config > /dev/null 2>&1; then
  for p in x11 x11-xcb xcb xext vulkan; do
    pkg-config --exists "$p" || MISSING+=("library $p (pkg-config)")
  done
  pkg-config --exists alsa || pkg-config --exists libpulse || pkg-config --exists libpipewire-0.3 ||
    MISSING+=("a sound library: alsa, libpulse or libpipewire-0.3 (pkg-config)")
  OPTIONAL=()
  for p in xrandr xcursor xi xscrnsaver xfixes xkbcommon wayland-client libudev dbus-1 libdecor-0 alsa libpulse libpipewire-0.3; do
    pkg-config --exists "$p" || OPTIONAL+=("$p")
  done
  if [ ${#OPTIONAL[@]} -gt 0 ]; then
    echo "note: not found (SDL builds without them: less input, display or sound support): ${OPTIONAL[*]}"
  fi
fi
if [ ${#MISSING[@]} -gt 0 ]; then
  echo "Missing:"
  for m in "${MISSING[@]}"; do echo "  - $m"; done
  install_hint
  die "missing build tools or libraries (see above)"
fi
echo "clang $(clang -dumpversion), $(cmake --version | head -n1), ninja $(ninja --version), $(python3 --version)"
finish

# ---- 2. game files ---------------------------------------------------------------
begin game "Checking your game files"
GAME=$(cd "$(dirname "$GAME")" && pwd -P)/$(basename "$GAME")
if [ -d "$GAME" ]; then
  [ -f "$GAME/default.xex" ] || die "no default.xex in $GAME (the folder with the game files, or the package)"
  GAME_DIR=$GAME
elif [ "$(basename "$GAME")" = default.xex ]; then
  GAME_DIR=$(dirname "$GAME")
else
  # An Xbox 360 package: unpacked once into <out>/gamefiles.
  GAME_DIR=$OUT/gamefiles
  if [ ! -f "$GAME_DIR/default.xex" ] || [ "$(sha1 "$GAME_DIR/default.xex")" != $XEX_SHA1 ]; then
    echo "unpacking $GAME"
    rm -rf "$GAME_DIR"
    unpacker=stfs_extract.py what="an Xbox 360 package of Battlefield 1943 1.06"
    [ $BC = 1 ] && unpacker=xiso_extract.py what="a disc image (.iso) of Battlefield: Bad Company"
    run_log "$LOGS/unpack.log" python3 -I "$TREE/tools/$unpacker" "$GAME" "$GAME_DIR" ||
      fail "$LOGS/unpack.log" "could not unpack $GAME ($what is expected)"
  fi
  [ -f "$GAME_DIR/default.xex" ] || die "no default.xex in the package $GAME"
fi
XEX=$GAME_DIR/default.xex
h=$(sha1 "$XEX")
[ -z "$XEX_SHA1_105" ] || [ "$h" != $XEX_SHA1_105 ] ||
  die "this is version 1.05 of the game: use the 1.06 package (the game updated on the console)"
if [ "$h" != $XEX_SHA1 ]; then
  if [ $BC = 1 ]; then
    xinfo=$(python3 -I "$TREE/tools/xex_info.py" "$XEX" 2>/dev/null || true)
    if printf '%s\n' "$xinfo" | grep -q "title_id 454107F9"; then
      bver=$(printf '%s\n' "$xinfo" | sed -n 's/.* version \([^ ]*\) base_version.*/\1/p')
      die "this is Battlefield: Bad Company${bver:+ version $bver}, but not the USA disc this port is built for: the Gold Edition, Platinum Hits and other regions have different code and cannot be used (SHA-1 of the USA disc default.xex: $XEX_SHA1)"
    fi
  fi
  die "$XEX is not $TITLE (SHA-1 $h)"
fi
echo "$TITLE: $GAME_DIR"
finish

# ---- 3. XenonRecomp ----------------------------------------------------------------
begin tools "Building the recompiler (XenonRecomp)"
TOOLS=$OUT/recomp-tools
run_log "$LOGS/tools_configure.log" cmake -S "$TREE/tools/xex_dump" -B "$TOOLS" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ||
  fail "$LOGS/tools_configure.log" "configuring XenonRecomp failed"
run_ninja "$LOGS/tools_build.log" cmake --build "$TOOLS" --target XenonAnalyse XenonRecomp xex_dump ||
  fail "$LOGS/tools_build.log" "building XenonRecomp failed"
XENONRECOMP=$TOOLS/XenonRecomp/XenonRecomp/XenonRecomp
XENONANALYSE=$TOOLS/XenonRecomp/XenonAnalyse/XenonAnalyse
XEXDUMP=$TOOLS/xex_dump
for b in "$XENONRECOMP" "$XENONANALYSE" "$XEXDUMP"; do [ -x "$b" ] || die "missing after the build: $b"; done
finish

# ---- 4. the recompiler config ------------------------------------------------------
begin config "The recompiler config"
GEN=$GTREE/generated
CFG=$GEN/${NAME}_config.toml
PPC_CONTEXT=$TREE/thirdparty/XenonRecomp/XenonUtils/ppc_context.h
mkdir -p "$GEN" || die "cannot create $GEN"
if [ -f "$CFG" ]; then
  tables=$(sed -n 's/^switch_table_file_path *= *"\([^"]*\)".*/\1/p' "$CFG")
  [ -z "$tables" ] || [ -f "$GEN/$tables" ] || die "$CFG names $tables, which is missing in $GEN"
  echo "using $CFG"
else
  # A fresh clone has none (generated/ is not in the repo): made from default.xex.
  echo "making $CFG from your default.xex (tools/make_recomp_config.py)"
  python3 -I "$TREE/tools/make_recomp_config.py" --title $NAME --xex "$XEX" --gen "$GEN" --analyse "$XENONANALYSE" \
    --recomp "$XENONRECOMP" --xex-dump "$XEXDUMP" --ppc-context "$PPC_CONTEXT" 2>&1 |
    tee "$LOGS/config.log" | grep --line-buffered -E '^(progress|pass|config|helpers)'
  [ "${PIPESTATUS[0]}" = 0 ] || { rm -f "$CFG"; fail "$LOGS/config.log" "making the recompiler config failed"; }
  RECOMPILE=1
fi
finish

# ---- 5. XenonRecomp -> generated/ppc -----------------------------------------------
begin recompile "Recompiling the game code (XenonRecomp)"
LOCAL=$GEN/${NAME}_config.linux.toml
xex_toml=$(printf '%s' "$XEX" | sed 's/[\\"]/\\&/g')
sed "s|^file_path = .*|file_path = \"$(printf '%s' "$xex_toml" | sed 's/[|&\\]/\\&/g')\"|" "$CFG" > "$LOCAL" ||
  die "cannot write $LOCAL"
PPC=$GEN/ppc
STAMP=$PPC/.linux-recomp
# Made from this config (but for the XEX's path) and this XEX: nothing to do.
want=$( { grep -v '^file_path' "$CFG"; echo "$h"; } | sha1sum | cut -d' ' -f1)
if [ $RECOMPILE = 0 ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ] && [ -f "$PPC/ppc_recomp_shared.h" ]; then
  echo "generated/ppc is up to date"
elif [ $RECOMPILE = 0 ] && [ ! -f "$STAMP" ] && [ -f "$PPC/ppc_recomp_shared.h" ]; then
  echo "using the generated/ppc already there (made by another build; --recompile makes it again)"
else
  mkdir -p "$PPC" && rm -f "$PPC"/*.cpp "$PPC"/*.h "$STAMP"
  (cd "$GEN" && "$XENONRECOMP" "$(basename "$LOCAL")" "$PPC_CONTEXT") > "$LOGS/recompile.log" 2>&1 ||
    fail "$LOGS/recompile.log" "XenonRecomp failed"
  [ -f "$PPC/ppc_recomp_shared.h" ] || fail "$LOGS/recompile.log" "XenonRecomp wrote no code into $PPC"
  ! grep -q '// ERROR' "$PPC"/ppc_recomp.*.cpp || echo "warning: the recompiled code has // ERROR markers (functions cut short)"
  echo "$want" > "$STAMP"
  echo "$(ls "$PPC" | grep -c '^ppc_recomp\..*\.cpp$') source files in $PPC"
fi
finish

# ---- 6. Xenia's shaders -------------------------------------------------------------
if [ $SERVER = 0 ]; then
  begin shaders "Compiling the GPU shaders"
  run_log "$LOGS/shaders.log" python3 -I "$TREE/tools/build_xenia_shaders.py" "$TREE/thirdparty/xenia/src" \
    "$TREE/build/xenia_shaders" || fail "$LOGS/shaders.log" "compiling the shaders failed"
  tail -n 1 "$LOGS/shaders.log"
  finish
fi

# ---- 7. configure ------------------------------------------------------------------
begin configure "Configuring the game build (CMake; downloads SDL3 the first time)"
BUILD=$OUT/cmake
CLIENT=ON; [ $SERVER = 1 ] && CLIENT=OFF
if [ $BC = 1 ]; then GAME_OPTS=(-DBFBC1_CLIENT=$CLIENT "-DROOT=$TREE"); else GAME_OPTS=(-DBF1943_CLIENT=$CLIENT); fi
run_log "$LOGS/configure.log" cmake -S "$GTREE/game/linux" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "${CMAKE_PLAT_OPTS[@]}" "${GAME_OPTS[@]}" ||
  fail "$LOGS/configure.log" "CMake configure failed"
finish

# ---- 8. build ------------------------------------------------------------------------
if [ -z "$JOBS" ]; then
  # The recompiled sources are big: about 2 GB of memory per compile job.
  if [ "$OS" = Darwin ]; then
    cores=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
    mem=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 8589934592) / 2147483648 - 2 ))  # (all but 4 GB)
  else
    cores=$(nproc 2>/dev/null || echo 4)
    mem=$(awk '/^MemAvailable:/ { print int($2 / 2097152) }' /proc/meminfo 2>/dev/null)
  fi
  JOBS=$cores
  [ -n "$mem" ] && [ "$mem" -ge 1 ] && [ "$mem" -lt "$JOBS" ] && JOBS=$mem
  [ -n "$mem" ] && [ "$mem" -lt 1 ] && JOBS=1
fi
begin build "Building $TARGET with $JOBS jobs (20-60 minutes)"
run_ninja "$LOGS/build.log" cmake --build "$BUILD" --target "$TARGET" -j "$JOBS" ||
  fail "$LOGS/build.log" "the build failed"
[ -x "$BUILD/$TARGET" ] || fail "$LOGS/build.log" "the build made no $BUILD/$TARGET"
finish

# ---- 9. install --------------------------------------------------------------------------
begin install "Putting the game together in $OUT/game"
DEST=$OUT/game
mkdir -p "$DEST" || die "cannot create $DEST"
cp -f "$BUILD/$TARGET" "$DEST/$TARGET" || die "cannot copy $TARGET to $DEST"
# Next to the exe: the bots' shipped knowledge (both), the PC menus' fonts and
# the PBR shader table (the game).
# (Bad Company: its own PBR table; its bots learn in nav/, no shipped knowledge.)
if [ $BC = 0 ]; then
  rm -rf "$DEST/bot_knowledge" && cp -R "$TREE/data/bot_knowledge" "$DEST/" || die "cannot copy data/bot_knowledge"
fi
if [ $SERVER = 0 ]; then
  rm -rf "$DEST/fonts" && cp -R "$TREE/data/fonts" "$DEST/" || die "cannot copy data/fonts"
  cp -f "$GTREE/data/pbr_shaders.txt" "$DEST/" || die "cannot copy data/pbr_shaders.txt"
fi
# macOS: MoltenVK beside the exe (the game loads it when no Vulkan loader is
# installed), so the folder runs on a Mac without the SDK.
if [ "$OS" = Darwin ] && [ $SERVER = 0 ] && [ -f "$VK_LIB/libMoltenVK.dylib" ]; then
  cp -f "$VK_LIB/libMoltenVK.dylib" "$DEST/" || echo "warning: cannot copy libMoltenVK.dylib"
fi
# Bad Company: its main menu's PC SETTINGS row, made from the player's game
# files into the --user folder of the command below (the launcher makes its
# own; without it the game has the stock menu).
if [ $BC = 1 ] && [ $SERVER = 0 ] &&
   [ ! -f "$OUT/userdata/override/dist/xenon/menu/main/mainmenu-00.fbrb" ]; then
  run_log "$LOGS/menu_patch.log" python3 -I "$GTREE/tools/bc_menu_patch.py" --game "$GAME_DIR" --user "$OUT/userdata" ||
    echo "warning: the PC SETTINGS menu patch failed (see $LOGS/menu_patch.log): the game has the stock main menu"
fi
# The bots' navigation tool (tools/navgen), next to the exe: tools/make_navs.py
# makes each map's meshes with it on this PC (the game files' own, never shipped).
if run_log "$LOGS/navgen_configure.log" cmake -S "$TREE/tools/navgen" -B "$OUT/navgen" -G Ninja \
     -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ "${CMAKE_PLAT_OPTS[@]}" &&
   run_log "$LOGS/navgen_build.log" cmake --build "$OUT/navgen" &&
   cp -f "$OUT/navgen/navgen" "$DEST/navgen"; then
  echo "navgen (the bots' navigation tool): $DEST/navgen"
else
  echo "warning: the bots' navigation tool (navgen) did not build (see $LOGS/navgen_*.log): the bots play without routes"
fi
printf 'tree %s\ncommit %s\ngame %s\n' "$GTREE" \
  "$(git -C "$GTREE" rev-parse --short HEAD 2>/dev/null || echo unknown)" "$GAME_DIR" > "$DEST/.build-info"
finish

echo
echo "Built $DEST/$TARGET ($(du -h "$DEST/$TARGET" | cut -f1))."
if [ $SERVER = 1 ]; then
  echo "Run the server:"
  maps=" --maps Wake_island_s"; [ $BC = 1 ] && maps=  # (Bad Company: its own rotation)
  echo "  \"$DEST/$TARGET\" --game \"$GAME_DIR\" --user \"$OUT/server_data\" --dedicated --name \"My Server\"$maps"
else
  echo "Run the game (needs a Vulkan 1.3 driver; settings and saves go in --user):"
  echo "  \"$DEST/$TARGET\" --game \"$GAME_DIR\" --user \"$OUT/userdata\""
fi
case $GAME_DIR in /mnt/*) echo "(The game files are on a Windows drive: copied to the Linux disk they load much faster.)" ;; esac
echo "ok $DEST/$TARGET"

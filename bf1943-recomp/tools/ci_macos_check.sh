#!/usr/bin/env bash
# Compile check for the macOS port, with no game files: the platform-specific
# sources are compiled (syntax only) as x86-64 for macOS 14.4 against a stand-in
# ppc_config.h. It proves these files compile, not that the game runs or links.
# Run on a Mac (or by .github/workflows/macos-check.yml): needs Xcode's command
# line tools and SDL3 (brew install sdl3).
set -u
TREE=$(cd "$(dirname "$0")/.." && pwd -P)
[ "$(uname -s)" = Darwin ] || { echo "error: this check is for macOS"; exit 1; }
STUB=$(mktemp -d)
cat > "$STUB/ppc_config.h" <<'H'
#pragma once
#define PPC_CONFIG_SKIP_LR
#define PPC_CONFIG_CTR_AS_LOCAL
#define PPC_CONFIG_XER_AS_LOCAL
#define PPC_CONFIG_RESERVED_AS_LOCAL
#define PPC_CONFIG_SKIP_MSR
#define PPC_CONFIG_CR_AS_LOCAL
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x1000000ull
#define PPC_CODE_BASE 0x82160000ull
#define PPC_CODE_SIZE 0x900000ull
H
XE=$TREE/thirdparty/xenia
SDL_INC=$(brew --prefix sdl3 2>/dev/null)/include
FLAGS=(-arch x86_64 -mmacosx-version-min=14.4 -march=x86-64-v3 -std=c++20 -fsyntax-only -w -fno-strict-aliasing
  -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DVK_NO_PROTOTYPES -DBF1943_CLIENT
  -I"$STUB" -I"$TREE/thirdparty/XenonRecomp/XenonUtils" -I"$TREE/thirdparty/XenonRecomp/thirdparty/simde"
  -I"$TREE/runtime" -I"$TREE/runtime/xenia_shims" -I"$XE/src" -I"$XE" -I"$XE/third_party/fmt/include"
  -I"$TREE/thirdparty/VulkanMemoryAllocator/include" -I"$TREE/thirdparty/fidelityfx/include" -I"$TREE/thirdparty/stb"
  -I"$TREE/thirdparty/ffmpeg/xenia-config" -I"$TREE/thirdparty/ffmpeg" -I"$SDL_INC")
FILES=(
  runtime/memory.cpp
  runtime/threads.cpp
  runtime/xenia_shims/window_sdl.cc
  thirdparty/xenia/src/xenia/base/threading_posix.cc
  thirdparty/xenia/src/xenia/base/memory_posix.cc
  thirdparty/xenia/src/xenia/base/mutex.cc
  thirdparty/xenia/src/xenia/base/clock_posix.cc
  thirdparty/xenia/src/xenia/ui/vulkan/vulkan_instance.cc
  thirdparty/xenia/src/xenia/ui/vulkan/vulkan_presenter.cc
  thirdparty/xenia/src/xenia/ui/vulkan/spirv_tools_context.cc
)
fail=0
for f in "${FILES[@]}"; do
  lang=c++; case $f in *.cc|*.cpp) ;; esac
  if clang++ "${FLAGS[@]}" -x $lang "$TREE/$f" > "$STUB/out.txt" 2>&1; then
    echo "ok    $f"
  else
    echo "FAIL  $f"; grep -E "error" "$STUB/out.txt" | head -8; fail=1
  fi
done
rm -rf "$STUB"
exit $fail

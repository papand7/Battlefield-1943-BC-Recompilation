#!/usr/bin/env bash
# Builds Battlefield 1943 (or Bad Company, --bfbc1) for macOS: the same steps
# and options as build_linux.sh, which knows both. An x86-64 program (Apple
# Silicon: through Rosetta, macOS 15 or newer); needs Xcode's command line
# tools, Homebrew's cmake ninja python glslang spirv-tools, and the LunarG
# Vulkan SDK (MoltenVK). See build_linux.sh --help.
[ "$(uname -s)" = Darwin ] || { echo "error: this script is for macOS (Linux: build_linux.sh)"; exit 1; }
exec "$(cd "$(dirname "$0")" && pwd -P)/build_linux.sh" "$@"

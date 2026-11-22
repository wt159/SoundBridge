#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# SoundBridge WSL/Linux environment setup (idempotent)
#
# Mirrors CI sdk-coverage.yml (ubuntu-24.04):
#   apt deps -> 3rdparty source build -> dist/linux_x86_64_gcc_debug
#
# This script only prepares the environment. Main configure/build/test stay
# native CMake commands documented in AGENTS.md (printed at the end).
#
# Usage (inside WSL Ubuntu-24.04):
#   bash scripts/setup_linux_env.sh
#
# Overridable env vars:
#   SB_REPO_DIR            target working tree (default ~/soundbridge)
#   SB_REPO_URL            git remote (default GitHub HTTPS, anonymous fetch)
#   SB_LOCAL_REPO          fallback clone source when network clone fails
#                          (default /mnt/d/workspace/SoundBridge if present)
#
# Idempotent: every stage is skipped when its output already exists.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_DIR="${SB_REPO_DIR:-$HOME/soundbridge}"
REPO_URL="${SB_REPO_URL:-https://github.com/wt159/SoundBridge.git}"
LOCAL_REPO_FALLBACK="${SB_LOCAL_REPO:-/mnt/d/workspace/SoundBridge}"
DIST_DIR="$REPO_DIR/sdk/3rdparty/dist/linux_x86_64_gcc_debug"

# --- 1. apt dependencies (CI list + nasm/yasm insurance) --------------------
PKGS=(build-essential cmake lcov qtbase5-dev libpulse-dev pkg-config nasm yasm)
MISSING=()
for p in "${PKGS[@]}"; do
    dpkg -s "$p" >/dev/null 2>&1 || MISSING+=("$p")
done
if [ ${#MISSING[@]} -gt 0 ]; then
    SUDO=""
    [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    echo "[1/3] apt install: ${MISSING[*]}"
    $SUDO apt-get update -qq
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq "${MISSING[@]}"
else
    echo "[1/3] apt deps already present, skip"
fi

# --- 2. repo working tree ----------------------------------------------------
if [ -d "$REPO_DIR/.git" ]; then
    echo "[2/3] repo already at $REPO_DIR, skip clone"
else
    echo "[2/3] clone into $REPO_DIR"
    if ! git clone "$REPO_URL" "$REPO_DIR"; then
        if [ -d "$LOCAL_REPO_FALLBACK/.git" ]; then
            echo "    network clone failed, fallback to local $LOCAL_REPO_FALLBACK"
            git clone "$LOCAL_REPO_FALLBACK" "$REPO_DIR"
            git -C "$REPO_DIR" remote set-url origin "$REPO_URL"
        else
            echo "ERROR: clone failed and no local fallback at $LOCAL_REPO_FALLBACK" >&2
            exit 1
        fi
    fi
fi

# --- 3. 3rdparty -> dist/linux_x86_64_gcc_debug ------------------------------
if [ -d "$DIST_DIR/lib" ]; then
    echo "[3/3] dist already exists at $DIST_DIR, skip 3rdparty build"
else
    echo "[3/3] build 3rdparty (boost+ffmpeg take the bulk, ~10-20 min)"
    # ffmpeg build.sh compiles serially by default; MAKEFLAGS parallelizes it
    # without touching 3rdparty sources. b2 (boost) picks its own -j.
    export MAKEFLAGS="-j$(nproc)"
    export CMAKE_BUILD_PARALLEL_LEVEL="$(nproc)"
    cmake -S "$REPO_DIR/sdk/3rdparty" -B "$REPO_DIR/sdk/3rdparty/build" \
        -Wno-unused-cli \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_TOOLCHAIN_FILE="$REPO_DIR/cmake/toolchain/toolchain.linux_x86_64_gcc.cmake" \
        -G "Unix Makefiles" \
        -DBUILD_OPTION_BOOST=ON \
        -DBUILD_OPTION_FFMPEG=ON \
        -DBUILD_OPTION_FLAC=ON \
        -DBUILD_OPTION_OGG=ON \
        -DBUILD_OPTION_VORBIS=ON \
        -DBUILD_OPTION_SDL2=ON \
        -DBUILD_OPTION_ZLIB=ON
    cmake --build "$REPO_DIR/sdk/3rdparty/build"
    cmake --install "$REPO_DIR/sdk/3rdparty/build"
fi

echo ""
echo "Environment ready. Next steps (native CMake, see AGENTS.md):"
echo "  cd $REPO_DIR"
echo "  # plain Debug variant (daily iterate):"
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \\"
echo "        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -G 'Unix Makefiles'"
echo "  cmake --build build"
echo "  SDL_AUDIODRIVER=dummy ctest --test-dir build -R sdk_ -j4 --output-on-failure"
echo "  # coverage variant (mirrors CI, 33% gate + HTML report):"
echo "  cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug \\"
echo "        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \\"
echo "        -DENABLE_COVERAGE=ON -G 'Unix Makefiles'"
echo "  SDL_AUDIODRIVER=dummy cmake --build build-cov --target coverage"

#!/usr/bin/env bash
set -euo pipefail
prefix=${1:?dependency prefix required}
minimum=${2:-16.0}
work=${RUNNER_TEMP:-/tmp}/fp-audio-deps
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$work"
git clone -q --depth 1 -b "${SRC_TAG:-0.2.2}" https://github.com/libsndfile/libsamplerate.git "$work/src"
git clone -q --depth 1 -b "${SDL_TAG:-release-2.32.10}" https://github.com/libsdl-org/SDL.git "$work/sdl"
git -C "$work/sdl" apply --whitespace=nowarn "$root/patches/sdl2-queue-probe.diff"
for platform in ios host; do
    destination=$prefix
    target=()
    sdl_extra=()
    if [[ $platform == ios ]]; then
        target=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$minimum")
    else
        destination=$prefix/host-audio
        sdl_extra=(-DSDL_VIDEO=OFF -DSDL_RENDER=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF -DSDL_COREAUDIO=OFF)
    fi
    cmake -S "$work/src" -B "$work/src-$platform" -G Ninja "${target[@]}" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_INSTALL_PREFIX="$destination" -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DLIBSAMPLERATE_EXAMPLES=OFF
    cmake --build "$work/src-$platform" --parallel 4
    cmake --install "$work/src-$platform"
    cmake -S "$work/sdl" -B "$work/sdl-$platform" -G Ninja "${target[@]}" "${sdl_extra[@]}" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$destination" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$destination" -DSampleRate_DIR="$destination/lib/cmake/SampleRate" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_LIBSAMPLERATE=ON -DSDL_LIBSAMPLERATE_SHARED=OFF
    python3 - "$work/sdl-$platform/include-config-release/SDL2/SDL_config.h" <<'PY'
from pathlib import Path
import sys
config=Path(sys.argv[1]).read_text()
assert '#define HAVE_LIBSAMPLERATE_H 1' in config
assert '#define SDL_LIBSAMPLERATE_DYNAMIC' not in config
PY
    cmake --build "$work/sdl-$platform" --parallel 4
    cmake --install "$work/sdl-$platform"
done
mkdir -p "$prefix/licenses"
cp "$work/src/COPYING" "$prefix/licenses/libsamplerate.txt"

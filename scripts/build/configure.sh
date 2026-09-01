#!/usr/bin/env bash
# configure.sh — Run CMake configure step using the resolved preset.
# Expects: ROOT_DIR, PRESET, BUILD_DIR, CLEAN (from build.sh)

# Guard: only source common.sh if not already loaded
[ -z "${C_RESET:-}" ] && source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

if [ "${CLEAN:-0}" = "1" ] && [ -d "$BUILD_DIR" ]; then
    info "Cleaning build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

info "Configuring preset: ${C_BOLD}${PRESET}${C_RESET}"
info "CMake: $(cmake --version | sed -n '1s/^cmake version //p')"
if [ -n "${CMAKE_EXTRA_ARGS:-}" ]; then
    info "Custom CMake options: enabled (see command output below)"
fi

# CMake presets have platform conditions that prevent cross-platform use.
# We call cmake --preset which reads CMakePresets.json directly.
if ! cmake --preset "$PRESET" ${CMAKE_EXTRA_ARGS:-}; then
    if [ "${PLATFORM:-}" = "linux" ]; then
        warn "Linux SDL3 source builds require development headers for" \
            "every enabled backend."
        warn "Use the consolidated dependency report above; CMake does" \
            "not install OS packages."
    fi
    die "CMake configure failed"
fi

ok "Configure done  (build dir: $BUILD_DIR)"

# Report the resolved state from CMakeCache.txt instead of guessing from the
# command line. This remains accurate when a cache, preset, or toolchain file
# changes a default.
cmake_cache_value() {
    sed -n "s/^$1:[^=]*=//p" "$BUILD_DIR/CMakeCache.txt" | tail -n 1
}

if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    sdl_provider="$(cmake_cache_value ALTIRRA_SDL3_PROVIDER)"
    sdl_version="$(cmake_cache_value ALTIRRA_SDL3_RESOLVED_VERSION)"
    sdl_linkage="$(cmake_cache_value ALTIRRA_SDL3_LINKAGE)"
    cxx_compiler="$(cmake_cache_value CMAKE_CXX_COMPILER)"
    cmake_generator="$(cmake_cache_value CMAKE_GENERATOR)"

    if [ -n "$cmake_generator" ]; then
        info "Generator: ${C_BOLD}${cmake_generator}${C_RESET}"
    fi
    if [ -n "$cxx_compiler" ]; then
        info "Compiler:  ${C_BOLD}${cxx_compiler}${C_RESET}"
    fi
    if [ -n "$sdl_provider" ]; then
        info "SDL3:      ${C_BOLD}${sdl_provider}, ${sdl_linkage}," \
            "${sdl_version}${C_RESET}"
    fi

    if [ "${PLATFORM:-}" = "linux" ] && [ "$sdl_provider" = "bundled" ]; then
        x11_enabled="$(cmake_cache_value SDL_X11)"
        wayland_enabled="$(cmake_cache_value SDL_WAYLAND)"
        info "SDL video: X11=${x11_enabled:-unknown}," \
            "Wayland=${wayland_enabled:-unknown}"
    fi
fi

#!/usr/bin/env bash

# Rebuild the platform-native SDL_GPU fragment shader headers. This follows
# SDL's own shader build process and requires the SDL_shadercross CLI and xxd.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
shader="$script_dir/sdlgpu_screenfx.frag.hlsl"
generated_dir="$script_dir/generated"
temp_dir=$(mktemp -d)

cleanup() {
    rm -rf -- "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

command -v shadercross >/dev/null 2>&1 || {
    echo "shadercross is required (https://github.com/libsdl-org/SDL_shadercross)" >&2
    exit 1
}
command -v xxd >/dev/null 2>&1 || {
    echo "xxd is required" >&2
    exit 1
}

make_header() {
    input=$1
    symbol=$2
    output=$3

    xxd -i -n "$symbol" "$input" \
        | sed -e 's/^unsigned char /static const unsigned char /' \
            -e 's/^unsigned int /static constexpr unsigned int /' \
        > "$output"
}

mkdir -p "$generated_dir"

shadercross "$shader" -o "$temp_dir/screenfx.spv"
shadercross "$shader" -o "$temp_dir/screenfx.msl"
shadercross "$shader" -o "$temp_dir/screenfx.dxil"

make_header "$temp_dir/screenfx.spv" kATSDLGPU_ScreenFX_SPIRV \
    "$generated_dir/sdlgpu_screenfx_frag_spv.h"
make_header "$temp_dir/screenfx.msl" kATSDLGPU_ScreenFX_MSL \
    "$generated_dir/sdlgpu_screenfx_frag_msl.h"
make_header "$temp_dir/screenfx.dxil" kATSDLGPU_ScreenFX_DXIL \
    "$generated_dir/sdlgpu_screenfx_frag_dxil.h"

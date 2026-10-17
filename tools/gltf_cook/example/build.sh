#!/bin/sh
# build.sh <build dir> <out dir>: builds the example's library and model into
# <out dir>/mod/effects/ and <out dir>/mod/models/panel.glb, the
# layout scene_capture's anim-example presets read.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=$1
out=$2
mkdir -p "$out/mod/effects" "$out/mod/models"
"$build/aeron_gltf_cook" --library "$here/examplepanel.yaml" --out "$out/mod/effects"
"$build/aeron_gltf_cook" "$here/panel.gltf" --out "$out/mod/models/panel.glb" \
	--effects-dir "$out/mod/effects"

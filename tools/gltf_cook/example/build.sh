#!/bin/sh
# build.sh <build dir> <out dir>: builds the example's library and model into
# <out dir>/remaster/effects/ and <out dir>/remaster/models/panel.glb, the
# layout scene_capture's anim-example presets read.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=$1
out=$2
mkdir -p "$out/remaster/effects" "$out/remaster/models"
"$build/aeron_gltf_cook" --library "$here/examplepanel.yaml" --out "$out/remaster/effects"
"$build/aeron_gltf_cook" "$here/panel.gltf" --out "$out/remaster/models/panel.glb" \
	--effects-dir "$out/remaster/effects"

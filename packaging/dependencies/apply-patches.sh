#!/bin/sh
# Usage: apply-patches.sh <dependency folder> <source checkout>
#
# Applies the dependency's patches (*.patch in the folder, in name order) to
# a Git checkout of its source at the version in the folder's `version` file.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <dependency folder> <source checkout>" >&2
    exit 2
fi
for patch in "$1"/*.patch; do
    [ -e "${patch}" ] || exit 0
    git -C "$2" apply "${patch}"
done

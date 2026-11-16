#!/bin/sh
# Usage: build-sdl.sh [--source <dir>] [--build <dir>] <install prefix> [CMake options...]
#
# Builds the SDL that aeron is pinned to and installs it into the prefix:
# clones SDL at the version in sdl/version (a commit or a release), applies
# aeron's patches, then configures, builds and installs a shared library.
# Extra options are passed to SDL's CMake configure step, for example a
# toolchain file.
#
# Point aeron's CMake at the result with -DCMAKE_PREFIX_PATH=<install prefix>.
#
#   --source <dir>  The SDL checkout. Default: ${TMPDIR:-/tmp}/aeron-sdl/source.
#   --build <dir>   The build tree. Default: ${TMPDIR:-/tmp}/aeron-sdl/build.
#
# Both folders are kept, so a second run only rebuilds what changed. The
# checkout is reset and patched again when the version or a patch changes.
# SDL_VERSION in the environment builds another version, and
# CMAKE_BUILD_PARALLEL_LEVEL sets the number of build jobs.
set -eu

script_dir="$(cd "$(dirname "$0")" && pwd)"
work_dir="${TMPDIR:-/tmp}/aeron-sdl"
source_dir="${work_dir}/source"
build_dir="${work_dir}/build"

usage() {
    echo "usage: $0 [--source <dir>] [--build <dir>] <install prefix> [CMake options...]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --source) [ "$#" -ge 2 ] || usage; source_dir="$2"; shift 2 ;;
        --build) [ "$#" -ge 2 ] || usage; build_dir="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) break ;;
    esac
done
[ "$#" -ge 1 ] || usage
prefix="$1"
shift

version="${SDL_VERSION:-$(cat "${script_dir}/sdl/version")}"
if echo "${version}" | grep -Eq '^[0-9a-f]{40}$'; then
    ref="${version}"
    is_commit=1
else
    ref="refs/tags/release-${version}"
    is_commit=0
fi

# The stamp records the version and the patches of the checkout.
stamp="${source_dir}.aeron-stamp"
key="${version} $(cat "${script_dir}/sdl"/*.patch 2>/dev/null | git hash-object --stdin)"
if [ ! -f "${stamp}" ] || [ "$(cat "${stamp}")" != "${key}" ]; then
    if [ ! -d "${source_dir}/.git" ]; then
        if [ -e "${source_dir}" ]; then
            echo "Source path exists but is not a Git checkout: ${source_dir}" >&2
            exit 1
        fi
        mkdir -p "${source_dir}"
        git -C "${source_dir}" init --quiet
    fi
    rm -f "${stamp}"
    git -C "${source_dir}" fetch --quiet --depth 1 \
        https://github.com/libsdl-org/SDL.git "${ref}"
    git -C "${source_dir}" reset --hard --quiet
    git -C "${source_dir}" clean -fd --quiet
    git -C "${source_dir}" checkout --quiet --detach FETCH_HEAD
    "${script_dir}/apply-patches.sh" "${script_dir}/sdl" "${source_dir}"
    echo "${key}" > "${stamp}"
fi

# A shallow clone has no tags, so SDL would name its revision after a short
# commit hash. aeron's CMake checks the revision, so give it the full commit.
revision_option=""
if [ "${is_commit}" -eq 1 ]; then
    header="${source_dir}/include/SDL3/SDL_version.h"
    major="$(sed -n 's/^#define SDL_MAJOR_VERSION  *\([0-9]*\).*/\1/p' "${header}")"
    minor="$(sed -n 's/^#define SDL_MINOR_VERSION  *\([0-9]*\).*/\1/p' "${header}")"
    micro="$(sed -n 's/^#define SDL_MICRO_VERSION  *\([0-9]*\).*/\1/p' "${header}")"
    revision_option="-DSDL_REVISION=SDL-${major}.${minor}.${micro}-${version}"
fi

generator_option=""
if command -v ninja >/dev/null 2>&1; then
    generator_option="-GNinja"
fi

cmake -S "${source_dir}" -B "${build_dir}" \
    ${generator_option} \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_INSTALL_PREFIX=${prefix}" \
    -DSDL_INSTALL_DOCS=OFF \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF \
    -DSDL_TESTS=OFF \
    ${revision_option} \
    "$@"
cmake --build "${build_dir}" --parallel ${CMAKE_BUILD_PARALLEL_LEVEL:-}
cmake --install "${build_dir}"

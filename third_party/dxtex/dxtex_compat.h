/*
 * dxtex_compat.h — minimal compatibility shim replacing
 * `DirectXTexP.h` for the vendored BC6HBC7.cpp build on macOS/Linux.
 *
 * DirectXTexP.h is the project's "include everything" header. On
 * Windows it drags in <Windows.h>, COM, d3d12.h, dxgiformat.h, etc.
 * BC6HBC7.cpp itself only needs DirectXMath + a few standard C++
 * headers + `UNREFERENCED_PARAMETER`. This shim provides exactly
 * that surface area; everything else from DirectXTexP is unused.
 */
#ifndef DXTEX_COMPAT_H
#define DXTEX_COMPAT_H

/* DirectXMath.h references unqualified `size_t` but does not include a
 * header that declares it at global scope. Recent libc++ (Xcode SDKs)
 * no longer leaks ::size_t transitively, so pull in <cstddef> first. */
#include <cstddef>

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#endif

/* DirectXTex public TEX_COMPRESS_FLAGS values referenced by the BC7
 * encode path inside BC6HBC7.cpp. We don't expose the BC7 entry
 * point; the symbol just needs to parse so the file compiles. The
 * numeric value matches the DirectXTex.h enum so behaviour is
 * preserved should anyone link the BC7 path in the future. */
#ifndef TEX_COMPRESS_BC7_QUICK
#define TEX_COMPRESS_BC7_QUICK 0x20000
#endif

#endif /* DXTEX_COMPAT_H */

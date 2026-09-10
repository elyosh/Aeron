#ifndef AERON_COMPAT_WIN_TYPES_H
#define AERON_COMPAT_WIN_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared primitives for the DirectDraw/Direct3D compatibility shim.
 *
 * The codebase uses stdint types with DirectX field names (see
 * assets/model_texture.h), so the curated ddraw.h / d3d.h do the same rather than
 * introducing DWORD/WORD/etc. Only HRESULT, the GUID layout, and the COM calling
 * convention need dedicated definitions. The guards match the Windows SDK so
 * host dependencies can include its headers after this one. */

/* COM method result. Original success tests are `hr >= 0`. */
#ifndef _HRESULT_DEFINED
#define _HRESULT_DEFINED
typedef int32_t HRESULT;
#endif

/* Legacy spelling used by DirectPlay callers. */
#ifndef GUID_DEFINED
#define GUID_DEFINED

typedef struct _GUID {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t  Data4[8];
} GUID;
#endif

/* COM interface id (GUID layout); same shape as the audio shim's DSCompatGuid. */
typedef struct DxGuid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t  data4[8];
} DxGuid;

typedef const DxGuid* DxRefIid;

/* Interface-id equality for QueryInterface dispatch. */
static inline int DxGuidEqual(DxRefIid a, const DxGuid* b) {
	int i;

	if (!a || !b) {
		return 0;
	}
	if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3) {
		return 0;
	}
	for (i = 0; i < 8; ++i) {
		if (a->data4[i] != b->data4[i]) {
			return 0;
		}
	}
	return 1;
}

/* Calling convention. The original DirectX COM methods are STDMETHODCALLTYPE
 * (__stdcall, callee-cleans-stack). For the recovered caller code to byte-match
 * under the x86 MSVC matching build the shim vtable method pointers must be
 * __stdcall there. On the 64-bit host port build __stdcall is ignored (platform
 * default), so this is a no-op. */
#if defined(_WIN32) && defined(_M_IX86)
#define AERON_DXAPI __stdcall
#else
#define AERON_DXAPI
#endif

/* HRESULT values the recovered retry loops actually test. */
#define DX_S_OK ((HRESULT)0)
#define DX_DD_OK ((HRESULT)0)
#define DX_D3D_OK ((HRESULT)0)
#define DX_E_NOTIMPL ((HRESULT)0x80004001)
#define DX_E_INVALIDARG ((HRESULT)0x80070057)
#define DX_E_FAIL ((HRESULT)0x80004005)
#define DX_DDERR_SURFACELOST ((HRESULT)0x887601C2u)
#define DX_DDERR_NOEXCLUSIVEMODE ((HRESULT)0x887600E1u)
#define DX_DDERR_WASSTILLDRAWING ((HRESULT)0x8876021Cu)
#define DX_DDERR_OUTOFVIDEOMEMORY ((HRESULT)0x8876017Cu)

#define DX_SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define DX_FAILED(hr) ((HRESULT)(hr) < 0)

#ifdef __cplusplus
}
#endif

#endif

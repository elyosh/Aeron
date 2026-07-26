#pragma once

#include "aeron/render.h"

#include <FidelityFX/host/ffx_interface.h>

struct AeronFsr3Backend;

AeronFsr3Backend* AeronFsr3Backend_Create(uint32_t profile_index, bool direct_history);
/* Submit all resource initialization recorded by the FFX context-creation
 * callbacks. Must succeed before the context is used. */
bool              AeronFsr3Backend_FinishInitialization(AeronFsr3Backend* backend);
void              AeronFsr3Backend_Destroy(AeronFsr3Backend* backend);
FfxInterface      AeronFsr3Backend_GetInterface(AeronFsr3Backend* backend);

uint32_t    AeronFsr3Backend_ProfileCount(void);
const char* AeronFsr3Backend_ProfileName(const AeronFsr3Backend* backend);
uint32_t    AeronFsr3Backend_ManifestSchema(void);
const char* AeronFsr3Backend_ManifestHash(void);
const char* AeronFsr3Backend_FallbackReason(const AeronFsr3Backend* backend);
const char* AeronFsr3Backend_LastFailedPipeline(const AeronFsr3Backend* backend);
void        AeronFsr3Backend_AppendFallbackReason(AeronFsr3Backend* backend, const char* reason);
bool        AeronFsr3Backend_UsesFp16(const AeronFsr3Backend* backend);
bool        AeronFsr3Backend_UsesWaveSpd(const AeronFsr3Backend* backend);
bool        AeronFsr3Backend_UsesLanczosLut(const AeronFsr3Backend* backend);
bool        AeronFsr3Backend_UsesDirectHistory(const AeronFsr3Backend* backend);
const char* AeronFsr3Backend_AtomicLayout(const AeronFsr3Backend* backend);

void               AeronFsr3Backend_BeginDispatch(AeronFsr3Backend* backend);
bool               AeronFsr3Backend_LastExecutionSucceeded(const AeronFsr3Backend* backend);
AeronTexture*      AeronFsr3Backend_OutputAlias(const AeronFsr3Backend* backend);
AeronRenderTarget* AeronFsr3Backend_BorrowedOutputTarget(const AeronFsr3Backend* backend);
void AeronFsr3Backend_SetReconstructedDepthInitialized(AeronFsr3Backend* backend, AeronBuffer* buffer);

FfxResource AeronFsr3Backend_TextureResource(AeronTexture* texture, const wchar_t* name,
											 FfxResourceStates state);
FfxResource AeronFsr3Backend_BufferResource(AeronBuffer* buffer, uint32_t stride, const wchar_t* name,
											FfxResourceStates state);

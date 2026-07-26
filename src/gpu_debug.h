/* SDL_GPU debug helpers. Keep SDL types inside Aeron. */
#ifndef AERON_GPU_DEBUG_H
#define AERON_GPU_DEBUG_H

#include <SDL3/SDL_gpu.h>

#ifndef AERON_GPU_DEBUG_LABELS
#if defined(NDEBUG)
#define AERON_GPU_DEBUG_LABELS 0
#else
#define AERON_GPU_DEBUG_LABELS 1
#endif
#endif

#if AERON_GPU_DEBUG_LABELS

static inline void AeronGpuDebug_Push(SDL_GPUCommandBuffer* command_buffer, const char* name) {
	if (command_buffer && name) {
		SDL_PushGPUDebugGroup(command_buffer, name);
	}
}

static inline void AeronGpuDebug_Pop(SDL_GPUCommandBuffer* command_buffer) {
	if (command_buffer) {
		SDL_PopGPUDebugGroup(command_buffer);
	}
}

static inline void AeronGpuDebug_Marker(SDL_GPUCommandBuffer* command_buffer, const char* name) {
	if (command_buffer && name) {
		SDL_InsertGPUDebugLabel(command_buffer, name);
	}
}

static inline void AeronGpuDebug_NameTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture,
											 const char* name) {
	if (device && texture && name) {
		SDL_SetGPUTextureName(device, texture, name);
	}
}

static inline void AeronGpuDebug_NameBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer,
										  const char* name) {
	if (device && buffer && name) {
		SDL_SetGPUBufferName(device, buffer, name);
	}
}

#else

static inline void AeronGpuDebug_Push(SDL_GPUCommandBuffer* command_buffer, const char* name) {
	(void)command_buffer;
	(void)name;
}
static inline void AeronGpuDebug_Pop(SDL_GPUCommandBuffer* command_buffer) { (void)command_buffer; }
static inline void AeronGpuDebug_Marker(SDL_GPUCommandBuffer* command_buffer, const char* name) {
	(void)command_buffer;
	(void)name;
}
static inline void AeronGpuDebug_NameTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture,
											 const char* name) {
	(void)device;
	(void)texture;
	(void)name;
}
static inline void AeronGpuDebug_NameBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer,
										  const char* name) {
	(void)device;
	(void)buffer;
	(void)name;
}

#endif

#endif

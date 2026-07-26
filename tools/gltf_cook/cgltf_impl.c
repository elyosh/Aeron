/*
 * Single TU that materializes cgltf + cgltf_write implementations.
 * Keeping the IMPLEMENTATION macros isolated from gltf_cook.c lets that
 * file stay header-only as far as cgltf is concerned and avoids
 * pulling cgltf's strict-aliasing-sensitive guts through other TUs.
 */

#define CGLTF_IMPLEMENTATION
#define CGLTF_WRITE_IMPLEMENTATION
#include "cgltf_write.h" /* transitively includes cgltf.h */

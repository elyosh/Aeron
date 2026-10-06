#ifndef AERON_XWAU_MAT_H
#define AERON_XWAU_MAT_H

/*
 * Material files of the XWAU (X-Wing Alliance Upgrade) "effects" renderer
 * (Materials/<OptName>.mat and Materials/DefaultGlobalMaterial.mat), parsed
 * the way that renderer (xwa_ddraw_d3d11) parses them (Materials.cpp LoadIndividualMATParams /
 * ReadMaterialLine / FindMaterial), restricted to the keys that drive
 * surface shading. Shipped files rely on the parser's quirks, which are
 * reproduced:
 *   - comments are ';' or '#' in column 0 only; a value is the first
 *     whitespace-delimited token after " = ", so trailing text is ignored;
 *   - a key line is sscanf("%s = %s"): "Key=1" (no space) never matches;
 *   - a section header is a line starting with '[' in column 0; its names
 *     are split on ',' with no trimming, so "[A, B]" never matches "B";
 *   - lines before the first section are ignored;
 *   - slot 0 starts as the global default material; every section starts
 *     as a copy of slot 0 as it is at that point, and a section named
 *     "Default" replaces slot 0 when it closes;
 *   - lookup strips everything from "_fg_" (skin clones) and compares
 *     names case-insensitively; the first matching section wins and an
 *     unmatched texture gets slot 0.
 */

#include <stdbool.h>
#include <stddef.h>

#define XWAU_MAT_NAME_MAX 64
#define XWAU_MAT_PATH_MAX 256

typedef struct XwauMatParams {
	float metallic;     /* Metallic */
	float intensity;    /* Intensity */
	float glossiness;   /* Glossiness */
	float nm_intensity; /* NMIntensity */
	float specular_val; /* SpecularVal */
	float ambient;      /* Ambient */
	bool  shadeless;    /* Shadeless */
	bool  alpha_isnt_glass;
	bool  alpha_to_bloom;
	bool  no_color_alpha;
	char  normal_map[XWAU_MAT_PATH_MAX]; /* NormalMap value, "" when unset */
} XwauMatParams;

typedef struct XwauMatFile XwauMatFile;

/* The renderer's built-in defaults (Materials.h Material(), material_defs.h),
 * used when DefaultGlobalMaterial.mat is absent. */
void XwauMat_CodeDefaults(XwauMatParams* out);

/* Apply every key line of DefaultGlobalMaterial.mat to `io` (the file has no
 * sections). Returns false when the file cannot be read. */
bool XwauMat_ReadGlobalDefault(const char* path, XwauMatParams* io);

/* Parse an OPT's material file on top of `global`. Returns NULL when the file
 * cannot be read; callers then use `global` for every texture, as the
 * renderer does for an OPT without a material file. */
XwauMatFile* XwauMat_Load(const char* path, const XwauMatParams* global);

/* Material for an OPT texture name (slot 0 when unmatched). */
const XwauMatParams* XwauMat_Find(const XwauMatFile* file, const char* texture_name);

void XwauMat_Free(XwauMatFile* file);

#endif /* AERON_XWAU_MAT_H */

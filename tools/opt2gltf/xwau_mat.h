#ifndef AERON_XWAU_MAT_H
#define AERON_XWAU_MAT_H

/*
 * Material files of the XWAU (X-Wing Alliance Upgrade) "effects" renderer
 * (Materials/<OptName>.mat and Materials/DefaultGlobalMaterial.mat), parsed
 * the way that renderer (xwa_ddraw_d3d11) parses them (Materials.cpp LoadIndividualMATParams /
 * ReadMaterialLine / FindMaterial), restricted to the keys that drive
 * surface shading and to the image sequences (below). Shipped files rely on
 * the parser's quirks, which are reproduced:
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

/*
 * Image sequences (Materials.cpp LoadTextureSequence / LoadFrameSequence and
 * the anim_*, lightmap_*, frame_* branches of ReadMaterialLine), parsed with
 * the renderer's quirks:
 *   - parentheses are removed before parsing; a malformed element ends the
 *     line and keeps the elements read so far;
 *   - "G-a, .., G-b" lists every image from a to b, interpolating seconds;
 *   - an unknown event name plays as EVT_NONE (flagged `event_unknown`);
 *   - lightmap_once plays on the colour layer (IsLightMap omits it);
 *   - a section starts without sequences, even when it copies [Default]'s
 *     shading values.
 * Frame groups (frame_*) keep their group; the caller lists its images.
 */
#define XWAU_SEQ_EVENT_MAX 40

typedef enum XwauSeqLayer { XWAU_SEQ_COLOUR = 0, XWAU_SEQ_ILLUMINATION } XwauSeqLayer;

typedef enum XwauSeqMode { XWAU_SEQ_LOOP = 0, XWAU_SEQ_RANDOM, XWAU_SEQ_ONCE } XwauSeqMode;

typedef enum XwauSeqOverlay { XWAU_SEQ_REPLACE = 0, XWAU_SEQ_MULT, XWAU_SEQ_SCREEN } XwauSeqOverlay;

/* Placement offset: none (FIXED_LOC), RAND_LOC (inside the texture), or a
 * RAND_LOC=(x0,y0,x1,y1) range (also the default, -0.2..0.2). */
typedef enum XwauSeqLoc {
	XWAU_SEQ_LOC_FIXED  = 0,
	XWAU_SEQ_LOC_INSIDE = 1,
	XWAU_SEQ_LOC_RANGE  = 2
} XwauSeqLoc;

typedef struct XwauSeqFrame {
	int   group, image;
	float seconds;
} XwauSeqFrame;

typedef struct XwauSequence {
	char           key[16]; /* the .mat key, lower case */
	XwauSeqLayer   layer;
	XwauSeqMode    mode;
	XwauSeqOverlay overlay;
	char           event[XWAU_SEQ_EVENT_MAX]; /* EVT_/IEVT_ name in upper case; "" = EVT_NONE */
	bool           instance;                  /* IEVT_ */
	bool           event_unknown;
	char           dat[XWAU_MAT_PATH_MAX]; /* DAT path as written */
	int            alpha_mode;             /* 1 black to alpha, 2 alpha is bloom mask, else 0 */
	float          tint[3];
	float          aspect, offset[2]; /* frame UV = uv * (aspect, 1) + offset */
	bool           clamp;
	bool           rand_scale; /* RAND_SCALE=min, max */
	float          scale[2];
	XwauSeqLoc     loc;
	float          loc_range[4];
	/* frame_*: a DAT group at `fps` (image -1: every image of the group). */
	bool          group_frames;
	int           group, image;
	float         fps;
	XwauSeqFrame* frames; /* listed frames */
	size_t        frame_count;
	int           section; /* 0 for [Default], else the section's 1-based order */
} XwauSequence;

typedef struct XwauMatSequences {
	const XwauSequence* items;
	size_t              count;
	int                 section; /* the sequences' section (0 for [Default]) */
} XwauMatSequences;

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

/* Sequences of an OPT texture: `own` are its section's, or [Default]'s when it
 * has no section; `inherited` are [Default]'s, which a texture with its own
 * section inherits for the instance events it does not define. Returns
 * whether the texture has its own section. */
bool XwauMat_FindSequences(const XwauMatFile* file, const char* texture_name, XwauMatSequences* own,
						   XwauMatSequences* inherited);

void XwauMat_Free(XwauMatFile* file);

#endif /* AERON_XWAU_MAT_H */

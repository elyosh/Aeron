#ifndef AERON_XWAU_MAT_H
#define AERON_XWAU_MAT_H

/*
 * Reads the material files of the XWAU (X-Wing Alliance Upgrade) "effects"
 * renderer: Materials/<OptName>.mat and Materials/DefaultGlobalMaterial.mat.
 * The files are parsed the same way as in that renderer, xwa_ddraw_d3d11
 * (LoadIndividualMATParams, ReadMaterialLine and FindMaterial in
 * Materials.cpp). Only the keys that drive surface shading, the light of a
 * light-emitting texture and the image sequences (below) are read. The files that XWAU ships rely on quirks
 * of the parser, so these quirks are reproduced:
 *   - A comment starts with ';' or '#' in column 0 only. A value is the first
 *     token after " = " and ends at white space, so any text after it is
 *     ignored.
 *   - A key line is read with sscanf("%s = %s"), so "Key=1" (no space) never
 *     matches.
 *   - A section header is a line that starts with '[' in column 0. Its names
 *     are split on ',' without trimming, so "[A, B]" never matches "B".
 *   - Lines before the first section are ignored.
 *   - Slot 0 starts as the global default material. Each section starts as a
 *     copy of slot 0 as it is at that point. A section named "Default"
 *     replaces slot 0 when it closes.
 *   - The lookup removes everything from "_fg_" onwards (skin clones) and
 *     compares names in any case. The first matching section is used, and a
 *     texture without a match gets slot 0.
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
	/* Light = r, g, b (LoadLightColor): the colour of the light the renderer
	 * puts on the texture's faces, scaled by its brightness. A line with
	 * fewer than three numbers sets no light. */
	bool  has_light;
	float light[3];
} XwauMatParams;

/*
 * Image sequences are parsed like LoadTextureSequence, LoadFrameSequence and
 * the anim_*, lightmap_* and frame_* branches of ReadMaterialLine
 * (Materials.cpp), with the same quirks:
 *   - Parentheses are removed before parsing. A malformed element ends the
 *     line, and the elements read before it are kept.
 *   - "G-a, .., G-b" lists every image from a to b and interpolates their
 *     durations.
 *   - An unknown event name plays as EVT_NONE and sets `event_unknown`.
 *   - lightmap_once plays on the colour layer, because IsLightMap does not
 *     include it.
 *   - A section starts without sequences, even when it copies the shading
 *     values of [Default].
 * A frame group (frame_*) keeps its group number, and the caller lists its
 * images.
 */
#define XWAU_SEQ_EVENT_MAX 40

typedef enum XwauSeqLayer { XWAU_SEQ_COLOUR = 0, XWAU_SEQ_ILLUMINATION } XwauSeqLayer;

typedef enum XwauSeqMode { XWAU_SEQ_LOOP = 0, XWAU_SEQ_RANDOM, XWAU_SEQ_ONCE } XwauSeqMode;

typedef enum XwauSeqOverlay { XWAU_SEQ_REPLACE = 0, XWAU_SEQ_MULT, XWAU_SEQ_SCREEN } XwauSeqOverlay;

/* The placement offset is none (FIXED_LOC), random inside the texture
 * (RAND_LOC), or random within the range RAND_LOC=(x0,y0,x1,y1). The default
 * is the range -0.2..0.2. */
typedef enum XwauSeqLoc {
	XWAU_SEQ_LOC_FIXED  = 0,
	XWAU_SEQ_LOC_INSIDE = 1,
	XWAU_SEQ_LOC_RANGE  = 2
} XwauSeqLoc;

typedef struct XwauSeqFrame {
	int   group, image;
	float seconds;
	float intensity; /* XWAU bloom multiplier */
} XwauSeqFrame;

typedef struct XwauSequence {
	char           key[16]; /* the .mat key, lower case */
	XwauSeqLayer   layer;
	XwauSeqMode    mode;
	XwauSeqOverlay overlay;
	char           event[XWAU_SEQ_EVENT_MAX]; /* EVT_/IEVT_ name in upper case, "" for EVT_NONE */
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
	/* frame_*: a DAT group played at `fps`. Image -1 means every image of the
	 * group. */
	bool          group_frames;
	int           group, image;
	float         fps;
	float         intensity; /* used by all the group's frames */
	XwauSeqFrame* frames; /* listed frames */
	size_t        frame_count;
	int           section; /* 0 for [Default], otherwise the 1-based order of the section */
} XwauSequence;

typedef struct XwauMatSequences {
	const XwauSequence* items;
	size_t              count;
	int                 section; /* section of the sequences (0 for [Default]) */
} XwauMatSequences;

typedef struct XwauMatFile XwauMatFile;

/* Sets the built-in defaults of the renderer (Material() in Materials.h,
 * material_defs.h). They are used when DefaultGlobalMaterial.mat is missing. */
void XwauMat_CodeDefaults(XwauMatParams* out);

/* Applies every key line of DefaultGlobalMaterial.mat to `io`. The file has no
 * sections. Returns false when the file cannot be read. */
bool XwauMat_ReadGlobalDefault(const char* path, XwauMatParams* io);

/* Parses the material file of an OPT on top of `global`. Returns NULL when the
 * file cannot be read. Callers then use `global` for every texture, as the
 * renderer does for an OPT without a material file. */
XwauMatFile* XwauMat_Load(const char* path, const XwauMatParams* global);

/* Returns the material for an OPT texture name, or slot 0 when no section
 * matches. */
const XwauMatParams* XwauMat_Find(const XwauMatFile* file, const char* texture_name);

/* Finds the sequences of an OPT texture. `own` gets the sequences of its
 * section, or those of [Default] when the texture has no section. When the
 * texture has its own section, `inherited` gets the sequences of [Default],
 * which the texture inherits for the instance events that it does not define.
 * Otherwise `inherited` is empty. Returns whether the texture has its own
 * section. */
bool XwauMat_FindSequences(const XwauMatFile* file, const char* texture_name, XwauMatSequences* own,
						   XwauMatSequences* inherited);

void XwauMat_Free(XwauMatFile* file);

#endif /* AERON_XWAU_MAT_H */

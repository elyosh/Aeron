/*
 * Reads XWAU Dynamic Cockpit files into opt2gltf screen surfaces
 * (xwau_dynamic_cockpit.h).
 */

#include "xwau_dynamic_cockpit.h"

#include "xwau_gltf_material.h"

#include "util.h"
#include "xwau_dat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Limits from DynamicCockpit.h in xwa_ddraw_d3d11: MAX_DC_SRC_ELEMENTS
 * sections per cockpit, and MAX_DC_COORDS_PER_TEXTURE rectangles and erase
 * regions per section. */
#define DC_MAX_SECTIONS 57
#define DC_MAX_COORDS 12

/* g_DCElemSrcNames, in index order. */
static const char* const dc_source_names[] = {
	"LEFT_SENSOR_SRC",
	"RIGHT_SENSOR_SRC",
	"LASER_RECHARGE_SRC",
	"SHIELD_RECHARGE_SRC",
	"ENGINE_POWER_SRC",
	"BEAM_RECHARGE_SRC",
	"SHIELDS_SRC",
	"BEAM_LEVEL_SRC",
	"TARGETING_COMPUTER_SRC",
	"QUAD_LASERS_LEFT_SRC",
	"QUAD_LASERS_RIGHT_SRC",
	"LEFT_TEXT_BOX_SRC",
	"RIGHT_TEXT_BOX_SRC",
	"SPEED_THROTTLE_SRC",
	"MISSILES_SRC",
	"NAME_TIME_SRC",
	"NUM_SHIPS_SRC",
	"QUAD_LASERS_BOTH_SRC",
	"DUAL_LASERS_L_SRC",
	"DUAL_LASERS_R_SRC",
	"DUAL_LASERS_BOTH_SRC",
	"B_WING_LASERS_SRC",
	"SIX_LASERS_BOTH_SRC",
	"SIX_LASERS_L_SRC",
	"SIX_LASERS_R_SRC",
	"SHIELDS_FRONT_SRC",
	"SHIELDS_BACK_SRC",
	"KW_TEXT_CMD_SRC",
	"KW_TEXT_TOP_SRC",
	"KW_TEXT_RADIOSYS_SRC",
	"TEXT_RADIO_SRC",
	"TEXT_SYSTEM_SRC",
	"TEXT_CMD_SRC",
	"TARGETED_OBJ_NAME_SRC",
	"TARGETED_OBJ_SHD_SRC",
	"TARGETED_OBJ_HULL_SRC",
	"TARGETED_OBJ_CARGO_SRC",
	"TARGETED_OBJ_SYS_SRC",
	"TARGETED_OBJ_DIST_SRC",
	"TARGETED_OBJ_SUBCMP_SRC",
	"EIGHT_LASERS_BOTH_SRC",
	"THROTTLE_BAR_SRC",
	"AUTOSIZE_MISSILES_L_SRC",
	"AUTOSIZE_MISSILES_R_SRC",
	"AUTOSIZE_SPEED_SRC",
	"AUTOSIZE_CHAFF_SRC",
	"AUTOSIZE_NAME_SRC",
	"AUTOSIZE_TIME_SRC",
	"AUTOSIZE_MISSILES_B_SRC",
	"AUTOSIZE_MISSILES_MIS_SRC",
	"DUAL_LASER_ENERGY_BAR_L_SRC",
	"DUAL_LASER_ENERGY_BAR_R_SRC",
	"QUAD_LASER_ENERGY_BAR_LU_SRC",
	"QUAD_LASER_ENERGY_BAR_LD_SRC",
	"QUAD_LASER_ENERGY_BAR_RU_SRC",
	"QUAD_LASER_ENERGY_BAR_RD_SRC",
	"ALL_LASER_ENERGY_BAR_SRC",
};
#define DC_SOURCE_COUNT (sizeof dc_source_names / sizeof dc_source_names[0])
#define DC_THROTTLE_BAR_SOURCE 41

/* g_HUDRegionNames, in index order. */
static const char* const dc_region_names[] = {
	"LEFT_SENSOR_REGION",       "RIGHT_SENSOR_REGION",  "SHIELDS_REGION",        "BEAM_REGION",
	"TARGET_AND_LASERS_REGION", "LEFT_TEXT_BOX_REGION", "RIGHT_TEXT_BOX_REGION", "TOP_LEFT_REGION",
	"TOP_RIGHT_REGION",         "TEXT_RADIOSYS_REGION", "TEXT_CMD_REGION",
};
#define DC_REGION_COUNT (sizeof dc_region_names / sizeof dc_region_names[0])

/* One uv_coords line as LoadDCUVCoords stores it. */
typedef struct DcCoord {
	int      source;
	float    dst[4];
	uint32_t bg, highlight, lock; /* 0xRRGGBB | flags << 24 */
} DcCoord;

typedef struct DcSection {
	char    name[128];
	char    cover[128];
	bool    hologram, noisy, transparent, always_visible;
	int     coord_count;
	DcCoord coords[DC_MAX_COORDS];
	int     erase_count;
	int     erase[DC_MAX_COORDS];
} DcSection;

struct XwauDynamicCockpitCover {
	char         key[1100]; /* resolved path + group + image */
	char         name[160]; /* <datbase>_<group>_<image> */
	XwauDatImage image;
	uint8_t*     base;
	uint8_t*     emissive; /* NULL when no texel is lit */
	bool         ok;
};

static int lookup_name(const char* const* names, size_t count, const char* name) {
	for (size_t i = 0; i < count; ++i)
		if (strcasecmp(names[i], name) == 0)
			return (int)i;
	return -1;
}

/* ReadNameFromLine: reads the first token and removes a trailing ';' or ','.
 * Returns the token length before the removal, or 0 when there is no token. */
static int read_name_from_line(const char* buf, char name[50]) {
	if (sscanf(buf, "%49s", name) < 1)
		return 0;
	const int len = (int)strlen(name);
	if (len > 0 && (name[len - 1] == ';' || name[len - 1] == ','))
		name[len - 1] = 0;
	return len;
}

/* LoadDCUVCoords. */
static void load_uv_coords(DcSection* section, const char* buf, float width, float height, int line,
						   const char* label) {
	if (section->coord_count >= DC_MAX_COORDS) {
		Report(NULL, "%s:%d: more than %d uv_coords in [%s]; skipped", label, line, DC_MAX_COORDS,
			   section->name);
		return;
	}
	const char* s = strchr(buf, '=');
	if (!s)
		return;
	s++;
	while (*s == ' ' || *s == '\t')
		s++;
	char      slot_name[50];
	const int len = read_name_from_line(s, slot_name);
	if (len == 0)
		return;
	const int source =
		strstr(slot_name, "SRC") ? lookup_name(dc_source_names, DC_SOURCE_COUNT, slot_name) : -1;
	if (source < 0) {
		Report(NULL, "%s:%d: unknown source '%s'; skipped", label, line, slot_name);
		return;
	}
	s += len + 1;
	float        x0, y0, x1, y1, intensity = 1.0f;
	unsigned int color = 0x121233u, highlight = 0, lock = 0;
	int          text_layer = 1, obj_layer = 1, bloom = 0;
	const int res = sscanf(s, "%f, %f, %f, %f; 0x%x; %f; %d; %d; 0x%x; 0x%x; %d", &x0, &y0, &x1, &y1, &color,
						   &intensity, &text_layer, &obj_layer, &highlight, &lock, &bloom);
	if (res < 4) {
		Report(NULL, "%s:%d: expected at least 4 coordinates; skipped", label, line);
		return;
	}
	uint32_t bits = 0x04u | 0x08u; /* text and graphics layers */
	if (res >= 6) {
		if (intensity < 0.0f)
			intensity = 0.0f;
		uint32_t temp = (uint32_t)intensity - 1u;
		bits |= temp > 3u ? 3u : temp;
	}
	if (res >= 7)
		bits = text_layer ? (bits | 0x04u) : (bits & 0xFBu);
	if (res >= 8)
		bits = obj_layer ? (bits | 0x08u) : (bits & 0xF7u);
	if (res < 9)
		highlight = color;
	if (res < 10)
		lock = color;
	if (res >= 11)
		bits = bloom ? (bits | 0x10u) : (bits & 0xE7u);
	DcCoord* c   = &section->coords[section->coord_count++];
	c->source    = source;
	c->dst[0]    = x0 / width;
	c->dst[1]    = y0 / height;
	c->dst[2]    = x1 / width;
	c->dst[3]    = y1 / height;
	c->bg        = color | (bits << 24);
	c->highlight = highlight | (bits << 24);
	c->lock      = lock | (bits << 24);
}

/* LoadGeneric3DCoords. */
static bool load_3d(const char* buf, float out[3]) {
	const char* s = strchr(buf, '=');
	float       v[3];
	if (!s || sscanf(s + 1, "%f, %f, %f", &v[0], &v[1], &v[2]) < 3)
		return false;
	memcpy(out, v, sizeof v);
	return true;
}

static float srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void color_to_linear(uint32_t color, float out[3]) {
	out[0] = srgb_to_linear((float)((color >> 16) & 0xffu) / 255.0f);
	out[1] = srgb_to_linear((float)((color >> 8) & 0xffu) / 255.0f);
	out[2] = srgb_to_linear((float)(color & 0xffu) / 255.0f);
}

/* Bakes the base image of the cover (premultiplied colour, cover alpha) and
 * its lit texels. In PixelShaderDC, a texel with V * a >= 0.8 is shadeless and
 * 20% brighter. It shows no HUD, so its base alpha is 1. */
static bool bake_cover(XwauDynamicCockpitCover* cover) {
	const size_t n = (size_t)cover->image.width * cover->image.height;
	cover->base    = (uint8_t*)malloc(n * 4);
	uint8_t* glow  = (uint8_t*)calloc(n * 4, 1);
	if (!cover->base || !glow) {
		free(glow);
		return false;
	}
	bool lit = false;
	for (size_t i = 0; i < n; ++i) {
		const uint8_t* p = &cover->image.rgba[i * 4];
		uint8_t*       b = &cover->base[i * 4];
		const float    a = p[3] / 255.0f;
		uint8_t        v = p[0] > p[1] ? p[0] : p[1];
		v                = v > p[2] ? v : p[2];
		if ((v / 255.0f) * a >= 0.8f) {
			b[0] = b[1] = b[2] = 0;
			b[3]               = 255;
			for (int k = 0; k < 3; ++k) {
				const float bright = p[k] * 1.2f;
				glow[i * 4 + k]    = (uint8_t)(bright > 255.0f ? 255.0f : bright + 0.5f);
			}
			glow[i * 4 + 3] = 255;
			lit             = true;
		} else {
			for (int k = 0; k < 3; ++k)
				b[k] = (uint8_t)(p[k] * a + 0.5f);
			b[3] = p[3];
		}
	}
	if (lit)
		cover->emissive = glow;
	else
		free(glow);
	return true;
}

/* Loads the cover that a cover_texture value names, once per cockpit. */
static const XwauDynamicCockpitCover* load_cover(XwauDynamicCockpit* dc, const char* value,
												 XwauDynamicCockpitFindFile find, void* find_context,
												 const char* label) {
	char spec[512];
	if (!PathFormat(spec, sizeof spec, "%s", value))
		return NULL;
	char* image_sep = strrchr(spec, '-');
	char* group_sep = NULL;
	if (image_sep) {
		*image_sep = 0;
		group_sep  = strrchr(spec, '-');
	}
	if (!group_sep) {
		Report(label, "unreadable cover_texture '%s'; no cover", value);
		return NULL;
	}
	*group_sep         = 0;
	const int    group = atoi(group_sep + 1), image = atoi(image_sep + 1);
	const size_t spec_len = strlen(spec);
	if (spec_len < 4 || strcasecmp(spec + spec_len - 4, ".dat") != 0) {
		Report(label, "cover_texture '%s' is not a DAT image; no cover", value);
		return NULL;
	}
	char path[1024];
	if (!find(find_context, spec, path, sizeof path)) {
		Report(label, "cover file '%s' not found; no cover", spec);
		return NULL;
	}
	char key[1100];
	snprintf(key, sizeof key, "%s|%d|%d", path, group, image);
	for (size_t i = 0; i < dc->cover_count; ++i)
		if (strcmp(dc->covers[i]->key, key) == 0)
			return dc->covers[i]->ok ? dc->covers[i] : NULL;
	XwauDynamicCockpitCover*  cover = (XwauDynamicCockpitCover*)calloc(1, sizeof *cover);
	XwauDynamicCockpitCover** slot  = cover ? ARRAY_PUSH(dc->covers, dc->cover_count) : NULL;
	if (!slot) {
		free(cover);
		return NULL;
	}
	*slot = cover;
	snprintf(cover->key, sizeof cover->key, "%s", key);
	const char* base = strrchr(path, '/');
	char        stem[128];
	snprintf(stem, sizeof stem, "%s", base ? base + 1 : path);
	char* dot = strrchr(stem, '.');
	if (dot)
		*dot = 0;
	snprintf(cover->name, sizeof cover->name, "%s_%d_%d", stem, group, image);
	char err[512];
	if (!XwauDat_LoadImage(path, group, image, &cover->image, err, sizeof err)) {
		Report(label, "cover %s; no cover", err);
		return NULL;
	}
	if (!bake_cover(cover)) {
		Report(label, "out of memory baking cover %s", cover->name);
		return NULL;
	}
	cover->ok = true;
	return cover;
}

/* LoadHoloOffsetFromIniFile. */
static bool load_hologram_offset(const char* path, float out[3]) {
	FILE* f = fopen(path, "rt");
	if (!f)
		return false;

	enum { OUT_OF_TAG, IN_TAG } fsm = OUT_OF_TAG;

	char buf[256], param[128], svalue[128];
	out[0] = out[1] = out[2] = 0.0f;
	while (fgets(buf, sizeof buf, f)) {
		if (buf[0] == ';' || buf[0] == '#' || !buf[0])
			continue;
		if (buf[0] == '[')
			fsm = strstr(buf, "CockpitHoloOffsets") || strstr(buf, "GunnerTurretHoloOffsets") ? IN_TAG
																							  : OUT_OF_TAG;
		if (fsm != IN_TAG)
			continue;
		svalue[0] = 0;
		if (sscanf(buf, "%127s = %127s", param, svalue) > 0) {
			const float value = (float)atof(svalue);
			if (strcasecmp(param, "OffsetX") == 0)
				out[0] = value;
			if (strcasecmp(param, "OffsetY") == 0)
				out[1] = value;
			if (strcasecmp(param, "OffsetZ") == 0)
				out[2] = value;
		}
	}
	fclose(f);
	return true;
}

static DcSection* find_section(DcSection* sections, int count, const char* name) {
	for (int i = 0; i < count; ++i)
		if (strcasecmp(sections[i].name, name) == 0)
			return &sections[i];
	return NULL;
}

bool XwauDynamicCockpit_Load(const char* dc_path, const opt_file_t* opt, const char* basename,
							 XwauDynamicCockpitFindFile find, void* find_context, XwauDynamicCockpit* out,
							 const char* label) {
	memset(out, 0, sizeof *out);
	FILE* f = fopen(dc_path, "rt");
	if (!f) {
		Report(label, "cannot read %s", dc_path);
		return false;
	}
	DcSection* sections = (DcSection*)calloc(DC_MAX_SECTIONS, sizeof *sections);
	out->texture_count  = opt->texture_count;
	out->screens =
		(OptGltfScreen*)calloc(opt->texture_count ? (size_t)opt->texture_count : 1, sizeof *out->screens);
	if (!sections || !out->screens) {
		fclose(f);
		free(sections);
		XwauDynamicCockpit_Free(out);
		return false;
	}

	/* ---- Parse (LoadIndividualDCParams) ---------------------------- */
	int        section_count = 0;
	DcSection* current       = NULL;
	float      cover_width = 1.0f, cover_height = 1.0f;
	bool       render_throttle   = false;
	float      throttle_color[3] = { 0.0f, 0.0f, 1.0f }; /* g_DCThrottleColor */
	char       buf[256], param[128], svalue[128];
	int        line = 0;
	while (fgets(buf, sizeof buf, f)) {
		line++;
		if (buf[0] == ';' || buf[0] == '#' || !buf[0])
			continue;
		svalue[0] = 0;
		if (sscanf(buf, "%127s = %127s", param, svalue) <= 0)
			continue;
		const float value = (float)atof(svalue);
		if (buf[0] == '[') {
			char name[128];
			snprintf(name, sizeof name, "%s", buf + 1);
			char* end = strstr(name, "]");
			if (end)
				*end = 0;
			current = find_section(sections, section_count, name);
			if (current) {
				current->coord_count = 0;
				current->erase_count = 0;
			} else if (section_count < DC_MAX_SECTIONS) {
				current = &sections[section_count++];
				snprintf(current->name, sizeof current->name, "%s", name);
			} else {
				Report(NULL, "%s:%d: more than %d sections; [%s] ignored", label, line, DC_MAX_SECTIONS,
					   name);
			}
		} else if (strcasecmp(param, "uv_coords") == 0) {
			if (current)
				load_uv_coords(current, buf, cover_width, cover_height, line, label);
			else
				Report(NULL, "%s:%d: uv_coords outside a section; skipped", label, line);
		} else if (strcasecmp(param, "erase_region") == 0) {
			const int region =
				strstr(svalue, "REGION") ? lookup_name(dc_region_names, DC_REGION_COUNT, svalue) : -1;
			if (!current || region < 0)
				Report(NULL, "%s:%d: erase_region '%s' skipped", label, line, svalue);
			else if (current->erase_count < DC_MAX_COORDS)
				current->erase[current->erase_count++] = region;
		} else if (strcasecmp(param, "cover_texture") == 0) {
			if (current)
				snprintf(current->cover, sizeof current->cover, "%s", svalue);
		} else if (strcasecmp(param, "cover_texture_size") == 0) {
			const char* s = strchr(buf, '=');
			if (s && sscanf(s + 1, "%f, %f", &cover_width, &cover_height) < 2)
				Report(NULL, "%s:%d: expected 'width, height'", label, line);
		} else if (strcasecmp(param, "hologram") == 0) {
			if (current)
				current->hologram = value != 0.0f;
		} else if (strcasecmp(param, "noisy_hologram") == 0) {
			if (current)
				current->noisy = value != 0.0f;
		} else if (strcasecmp(param, "transparent") == 0) {
			if (current)
				current->transparent = value != 0.0f;
		} else if (strcasecmp(param, "always_visible") == 0) {
			if (current)
				current->always_visible = value != 0.0f;
		} else if (strcasecmp(param, "render_throttle") == 0) {
			render_throttle = value != 0.0f;
		} else if (strcasecmp(param, "throttle_color") == 0) {
			load_3d(buf, throttle_color);
		} else if (strcasecmp(param, "xwahacker_fov") != 0 && strcasecmp(param, "xwahacker_large_fov") != 0 &&
				   strcasecmp(param, "wireframe_mesh_color") != 0 &&
				   strcasecmp(param, "render_laser_ions") != 0 &&
				   strcasecmp(param, "laser_energy_color") != 0 &&
				   strcasecmp(param, "ion_energy_color") != 0 &&
				   strcasecmp(param, "fix_missles_countermeasures_text") != 0 &&
				   strcasecmp(param, "fix_missiles_countermeasures_text") != 0) {
			Report(NULL, "%s:%d: unknown key '%s' ignored", label, line, param);
		}
	}
	fclose(f);

	/* ---- Bind sections to the OPT's textures ---------------------- */
	float bar[3];
	for (int k = 0; k < 3; ++k)
		bar[k] = srgb_to_linear(
			throttle_color[k] < 0.0f ? 0.0f : (throttle_color[k] > 1.0f ? 1.0f : throttle_color[k]));
	int bound = 0, rects = 0, holograms = 0;
	for (int si = 0; si < section_count; ++si) {
		const DcSection* section = &sections[si];
		char             opt_name[128];
		snprintf(opt_name, sizeof opt_name, "%s", section->name);
		char* comma = strchr(opt_name, ',');
		if (!comma) {
			Report(label, "section [%s] names no texture; skipped", section->name);
			continue;
		}
		*comma                  = 0;
		const char*  texture    = comma + 1;
		const char*  opt_ext    = strrchr(opt_name, '.');
		const size_t stem_len   = opt_ext ? (size_t)(opt_ext - opt_name) : strlen(opt_name);
		const bool   same_model = strlen(basename) == stem_len &&
								  strncasecmp(opt_name, basename, stem_len) == 0 &&
								  (!opt_ext || strcasecmp(opt_ext, ".opt") == 0);
		int32_t      ti         = -1;
		for (int32_t i = 0; same_model && i < opt->texture_count && ti < 0; ++i)
			if (strcasecmp(opt->textures[i].name, texture) == 0)
				ti = i;
		if (ti < 0) {
			Report(label, "section [%s] matches no texture of %s; skipped", section->name, basename);
			continue;
		}
		OptGltfScreen* screen = &out->screens[ti];
		if (screen->present) {
			Report(label, "section [%s]: texture %s is already used; skipped", section->name, texture);
			continue;
		}
		screen->present        = true;
		screen->transparent    = section->transparent;
		screen->always_visible = section->always_visible;
		screen->hologram       = section->hologram;
		screen->noisy          = section->noisy;
		for (int e = 0; e < section->erase_count; ++e)
			screen->hide |= 1u << section->erase[e];
		for (int c = 0; c < section->coord_count; ++c) {
			const DcCoord*     coord = &section->coords[c];
			OptGltfScreenRect* r     = &screen->rects[screen->rect_count++];
			const uint32_t     bits  = coord->bg >> 24;
			r->source                = (uint32_t)coord->source;
			memcpy(r->dst, coord->dst, sizeof r->dst);
			color_to_linear(coord->bg, r->bg);
			color_to_linear(coord->highlight, r->highlight);
			color_to_linear(coord->lock, r->lock);
			r->gain   = (float)((bits & 3u) + 1u) * ((bits & 0x10u) ? 2.0f : 1.0f);
			r->layers = ((bits & 0x08u) ? OPT_GLTF_SCREEN_LAYER_GRAPHICS : 0u) |
						((bits & 0x04u) ? OPT_GLTF_SCREEN_LAYER_TEXT : 0u);
			if (render_throttle && coord->source == DC_THROTTLE_BAR_SOURCE) {
				r->layers |= OPT_GLTF_SCREEN_LAYER_BAR;
				memcpy(r->bar, bar, sizeof bar);
			}
		}
		/* Without rectangles, XWAU draws the texture as it is, and the
		 * section only erases HUD regions. */
		if (screen->rect_count == 0) {
			screen->transparent = screen->hologram = screen->noisy = false;
		} else if (section->cover[0] && !section->hologram) {
			const XwauDynamicCockpitCover* cover = load_cover(out, section->cover, find, find_context, label);
			if (cover) {
				screen->cover_base =
					(OptGltfImageView) { cover->base, cover->image.width, cover->image.height };
				screen->cover_emissive =
					(OptGltfImageView) { cover->emissive, cover->emissive ? cover->image.width : 0,
										 cover->emissive ? cover->image.height : 0 };
				screen->cover_name = cover->name;
				/* Lit texels glow with the same strength as illumination. */
				screen->cover_emissive_strength = XWAU_GLTF_ILLUM_GAIN;
			}
		}
		holograms += screen->hologram ? 1 : 0;
		rects += (int)screen->rect_count;
		bound++;
	}
	free(sections);

	/* ---- Hologram offset ------------------------------------------- */
	if (holograms > 0) {
		const size_t len = strlen(basename);
		char         craft[256], relative[300], path[1024];
		snprintf(craft, sizeof craft, "%.*s", (int)(len > 7 ? len - 7 : 0), basename);
		if (PathFormat(relative, sizeof relative, "FlightModels/%s.ini", craft) &&
			find(find_context, relative, path, sizeof path) &&
			load_hologram_offset(path, out->hologram_offset))
			out->has_hologram_offset = out->hologram_offset[0] != 0.0f || out->hologram_offset[1] != 0.0f ||
									   out->hologram_offset[2] != 0.0f;
	}
	Report(label,
		   "dynamic cockpit %s: %d of %d sections bound, %d rectangles, %zu covers, %d holograms"
		   "%s",
		   dc_path, bound, section_count, rects, out->cover_count, holograms,
		   out->has_hologram_offset ? " (offset)" : "");
	return true;
}

bool XwauDynamicCockpit_ApplyHologramOffset(opt_file_t* opt, const XwauDynamicCockpit* dc) {
	if (!dc->has_hologram_offset)
		return true;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		opt_mesh_t* m = &opt->meshes[mi];
		if (m->lod_count < 1)
			continue;
		const int32_t original = m->vertex_count;
		int32_t*      moved    = NULL; /* original vertex -> its moved copy */
		for (int32_t g = 0; g < m->lods[0].group_count; ++g) {
			opt_face_group_t* grp      = &m->lods[0].groups[g];
			const int         states   = (grp->state_count > 1 && grp->state_textures) ? grp->state_count : 1;
			bool              hologram = false;
			for (int s = 0; s < states && !hologram; ++s) {
				const int32_t ti = s == 0 ? grp->texture_index : grp->state_textures[s];
				hologram =
					ti >= 0 && ti < dc->texture_count && dc->screens[ti].present && dc->screens[ti].hologram;
			}
			if (!hologram)
				continue;
			if (!moved) {
				moved = (int32_t*)malloc((size_t)(original ? original : 1) * sizeof *moved);
				if (!moved)
					return false;
				for (int32_t v = 0; v < original; ++v)
					moved[v] = -1;
			}
			for (int32_t fi = 0; fi < grp->face_count; ++fi) {
				opt_face_t* face = &grp->faces[fi];
				for (int k = 0; k < 4; ++k) {
					const int32_t vi = face->verts[k];
					if (vi < 0 || vi >= original)
						continue;
					if (moved[vi] < 0) {
						const int32_t copy = m->vertex_count;
						if (!ARRAY_PUSH(m->vertices, m->vertex_count)) {
							free(moved);
							return false;
						}
						m->vertices[copy] = m->vertices[vi];
						m->vertices[copy].x += dc->hologram_offset[0];
						m->vertices[copy].y += dc->hologram_offset[1];
						m->vertices[copy].z += dc->hologram_offset[2];
						moved[vi] = copy;
					}
					face->verts[k] = moved[vi];
				}
			}
		}
		free(moved);
	}
	return true;
}

void XwauDynamicCockpit_Free(XwauDynamicCockpit* dc) {
	if (!dc)
		return;
	for (size_t i = 0; i < dc->cover_count; ++i) {
		XwauDat_FreeImage(&dc->covers[i]->image);
		free(dc->covers[i]->base);
		free(dc->covers[i]->emissive);
		free(dc->covers[i]);
	}
	free(dc->covers);
	free(dc->screens);
	memset(dc, 0, sizeof *dc);
}

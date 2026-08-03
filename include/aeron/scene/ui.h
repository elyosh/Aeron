#ifndef AERON_SCENE_UI_H
#define AERON_SCENE_UI_H

/*
 * AeronUi — immediate-mode settings-menu toolkit.
 *
 * A themable widget layer for remaster configuration screens: windows,
 * columns, labels, buttons, toggles, sliders, selectors, tabs, scroll
 * regions, and modals, driven per frame by game code over game-owned
 * values. Rendering is flat themed surfaces (optional bevel/gradient)
 * via AeronDrawList2D rounded-rects + AeronText glyphs, presented at
 * native swapchain resolution.
 *
 * Contract per frame, between Aeron_BeginFrame and Aeron_Present:
 *
 *   AeronUi_BeginFrame(ui, &(AeronUiFrameDesc){ .input = in, .dt_seconds = dt });
 *   if (AeronUi_BeginWindow(ui, "VIDEO", &wd)) {
 *       AeronUi_Toggle(ui, "Fullscreen", &cfg.fullscreen);
 *       ...
 *       AeronUi_EndWindow(ui);
 *   }
 *   AeronUiOutput out = AeronUi_EndFrame(ui);
 *   AeronUi_Submit(ui);   -- draws above previously submitted layers
 *
 * The game reacts to `out`: suppress game input while wants_* are set,
 * pop its screen state on cancel_pressed. Values are game-owned
 * pointers; widget calls return nonzero the frame the value changes.
 * Navigation: Up/Down move focus, Left/Right adjust the focused value
 * (or hop columns), Enter/Space/pad-South accept, Esc/pad-East cancel,
 * Q/E or shoulder buttons switch tabs; mouse hover moves focus.
 *
 * No steady-state allocation: all capacity is fixed at Create; over-
 * capacity submissions warn once per frame and drop.
 */

#include "aeron/input.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronUiContext AeronUiContext;

/* Straight-alpha linear-space RGBA color role. */
typedef float AeronUiColor[4];

/* Every metric is in REFERENCE PIXELS at 1080p output height; the
 * context scales by output_height / 1080 so screens keep proportions at
 * any resolution. */
typedef struct AeronUiTheme {
	/* Color roles. */
	AeronUiColor window_shadow;  /* drop shadow under windows */
	AeronUiColor surface;        /* window body */
	AeronUiColor surface_border; /* window border ring */
	AeronUiColor title_bar;      /* title bar fill (gradient top) */
	AeronUiColor title_bar_low;  /* title bar gradient bottom */
	AeronUiColor title_text;
	AeronUiColor text;
	AeronUiColor text_dim; /* help text, disabled */
	AeronUiColor accent;   /* value highlights, slider fill */
	AeronUiColor focus_outline;
	AeronUiColor widget_bg;     /* button/slider/selector body */
	AeronUiColor widget_bg_hot; /* focused/hovered body */
	AeronUiColor widget_bg_low; /* widget gradient bottom */
	AeronUiColor widget_border;
	AeronUiColor slider_track;
	AeronUiColor scrollbar;
	AeronUiColor scrollbar_thumb;
	AeronUiColor separator;
	AeronUiColor row_highlight; /* focused form-row background bar */
	AeronUiColor scrim;    /* modal backdrop / menu backdrop dim */
	AeronUiColor bevel_hi; /* bevel band inside top edges */
	AeronUiColor bevel_lo; /* bevel band inside bottom edges */

	/* Metrics (reference px @1080p). */
	float window_pad;
	float window_border_px;
	float title_height;
	float item_spacing; /* vertical gap between rows */
	float column_spacing;
	float row_height; /* minimum widget row height */
	float widget_border_px;
	float corner_radius; /* 0 = square */
	float focus_px;      /* focus outline thickness */
	float slider_track_px;
	float scrollbar_px;
	float label_fraction; /* row split: label portion (0..1) */
	float text_px;        /* body text size */
	float title_px;
	float help_px;
	float bevel_px; /* 0 disables bevel bands */

	/* Effects. */
	int   widget_gradient; /* nonzero: widget_bg -> widget_bg_low */
	int   title_gradient;  /* nonzero: title_bar -> title_bar_low */
	float shadow_soft_px;  /* window drop-shadow blur; 0 disables */

	/* Interaction timing (seconds). */
	float repeat_delay_s;
	float repeat_interval_s;
	float open_fade_s; /* whole-UI fade on first frame(s); 0 = off */
} AeronUiTheme;

/* Compiled-in default dark theme — zero assets required. */
const AeronUiTheme* AeronUi_DefaultTheme(void);

/* Overlays theme values from a YAML file (colors.*, metrics.*,
 * effects.*, timing.*) onto `out`, starting from the default theme.
 * Colors are authored as sRGB [r, g, b, a] 0..1 lists and converted to
 * linear. Missing keys keep their defaults; unknown keys warn. Returns
 * nonzero on success (a missing file is a failure; a partial file is
 * not). */
int AeronUi_ThemeLoadYaml(AeronUiTheme* out, AeronVfs* vfs, AeronVfsRoot root, const char* path);

/* Font roles. Text draws at theme sizes: glyph scale =
 * theme.*_px * ui_scale / atlas->cell_h, so bake atlases at or above
 * the largest on-screen pixel size (4K-equivalent recommended: only
 * ever downscaled, stays crisp). `title` NULL = use `regular`.
 *
 * `*_tracking_atlas` is extra advance between glyphs in ATLAS pixels,
 * scaled with the glyph scale at draw time. Leave 0 for TTF-baked
 * atlases (their advances carry side bearings); classic-extracted
 * game fonts store bare ink widths and need the source engine's
 * inter-glyph gap here (e.g. TIE's font8: 1 classic px = 9 atlas px). */
typedef struct AeronUiFontSet {
	const AeronFontAtlas* regular;
	const AeronFontAtlas* title;
	float                 regular_tracking_atlas;
	float                 title_tracking_atlas;
} AeronUiFontSet;

/* UI interaction sounds — the game owns audio; the toolkit reports
 * events through the optional callback. */
typedef enum AeronUiSound {
	AERON_UI_SOUND_FOCUS  = 0, /* focus moved */
	AERON_UI_SOUND_ACCEPT = 1, /* button/toggle activated */
	AERON_UI_SOUND_ADJUST = 2, /* slider/selector value changed */
	AERON_UI_SOUND_CANCEL = 3,
} AeronUiSound;
typedef void (*AeronUiSoundFn)(AeronUiSound sound, void* user);

typedef struct AeronUiDesc {
	int            max_widgets;     /* per frame; 0 = 256 */
	int            draw_record_cap; /* internal draw list; 0 = 8192 */
	AeronUiSoundFn sound_fn;        /* optional */
	void*          sound_user;
} AeronUiDesc;

AeronUiContext* AeronUi_Create(const AeronUiDesc* desc); /* NULL on failure */
void            AeronUi_Destroy(AeronUiContext* ctx);

void AeronUi_SetTheme(AeronUiContext* ctx, const AeronUiTheme* theme); /* copied */
void AeronUi_SetFonts(AeronUiContext* ctx, const AeronUiFontSet* fonts);

typedef struct AeronUiFrameDesc {
	const AeronInputSnapshot* input; /* required */
	float                     dt_seconds;
	/* Output dimensions in pixels. Zero = query the engine content rect
	 * (the normal game path); explicit values support offline tests. */
	int output_w, output_h;
} AeronUiFrameDesc;

/* Input-capture report for the game's suppression logic. */
typedef struct AeronUiOutput {
	int wants_keyboard;
	int wants_mouse;
	int wants_gamepad;
	int capture_all;    /* rebind capture active: suppress everything */
	int cancel_pressed; /* unconsumed cancel — pop the game screen */
} AeronUiOutput;

void          AeronUi_BeginFrame(AeronUiContext* ctx, const AeronUiFrameDesc* frame);
AeronUiOutput AeronUi_EndFrame(AeronUiContext* ctx);

/* Renders this frame's UI as a presentation layer above everything the
 * game already submitted (call after game layers, before
 * Aeron_Present). Draws directly into the swapchain pass at native
 * resolution when possible, else through a content-rect-sized render
 * target. Returns nonzero when a layer was submitted. */
int AeronUi_Submit(AeronUiContext* ctx);

/* ============================= containers ============================= */

enum {
	AERON_UI_WINDOW_NO_TITLE = 1 << 0,
};

typedef struct AeronUiWindowDesc {
	float width_ref;    /* reference px @1080p; 0 = theme default */
	float height_ref;   /* 0 = auto-size to content (one-frame settle) */
	float x_ref, y_ref; /* top-left when `centered` is zero */
	int   centered;
	int   flags;
} AeronUiWindowDesc;

/* Nonzero when the window is visible and its body should be declared.
 * Every open window captures input for the frame. */
int  AeronUi_BeginWindow(AeronUiContext* ctx, const char* title, const AeronUiWindowDesc* desc);
void AeronUi_EndWindow(AeronUiContext* ctx);

/* Splits the content width. weights NULL = equal columns. A NEGATIVE
 * weight is a fixed width in reference px (e.g. -220 = 220 ref px);
 * positive weights share the remainder proportionally. */
void AeronUi_BeginColumns(AeronUiContext* ctx, int count, const float* weights);
void AeronUi_NextColumn(AeronUiContext* ctx);
void AeronUi_EndColumns(AeronUiContext* ctx);

/* Tab strip + page: `*active` is the game-owned page index. Body
 * declared between Begin/End belongs to the active page. Q/E and
 * shoulder buttons cycle. Returns nonzero (always draws). */
int  AeronUi_BeginTabBar(AeronUiContext* ctx, const char* id, const char* const* titles, int count,
						 int* active);
void AeronUi_EndTabBar(AeronUiContext* ctx);

/* Fixed-height scrollable region. Focus navigation auto-scrolls the
 * focused widget into view; the wheel and scrollbar also scroll. */
int  AeronUi_BeginScroll(AeronUiContext* ctx, const char* id, float height_ref);
void AeronUi_EndScroll(AeronUiContext* ctx);

/* Modal dialog over the current screen: dims the backdrop, confines
 * focus, swallows outside clicks. Returns nonzero while *open. Cancel
 * closes it (sets *open = 0). */
int  AeronUi_BeginModal(AeronUiContext* ctx, const char* title, int* open, const AeronUiWindowDesc* desc);
void AeronUi_EndModal(AeronUiContext* ctx);

/* Disambiguates identical labels in loops (per-row controls lists). */
void AeronUi_PushId(AeronUiContext* ctx, int id);
void AeronUi_PopId(AeronUiContext* ctx);

/* ============================== widgets ============================== */
/* Labels use "Text##suffix" to separate display text from identity.
 * Value widgets return nonzero the frame the value changed. */

void AeronUi_Label(AeronUiContext* ctx, const char* text);
void AeronUi_Header(AeronUiContext* ctx, const char* text);
void AeronUi_Help(AeronUiContext* ctx, const char* text); /* dim, word-wrapped */
void AeronUi_Separator(AeronUiContext* ctx);
void AeronUi_Spacer(AeronUiContext* ctx, float height_ref);

int AeronUi_Button(AeronUiContext* ctx, const char* label);
int AeronUi_Toggle(AeronUiContext* ctx, const char* label, int* value);
int AeronUi_SliderInt(AeronUiContext* ctx, const char* label, int* value, int min, int max, int step,
					  const char* fmt /* e.g. "%d%%"; NULL = "%d" */);
int AeronUi_SliderFloat(AeronUiContext* ctx, const char* label, float* value, float min, float max,
						float step, const char* fmt /* NULL = "%.2f" */);
int AeronUi_Selector(AeronUiContext* ctx, const char* label, int* index, const char* const* options,
					 int count);

/* Rebind capture widget. Shows `display` (the game-formatted current
 * binding); activation enters capture mode — the toolkit then reports
 * capture_all so the game suppresses everything — and the next key,
 * mouse button, gamepad button, or gamepad axis push past 50% is
 * returned in `out`. Esc or a 5 s timeout cancels. The toolkit only
 * reports the raw input; mapping it to a binding model and persisting
 * stay game-side. */
typedef enum AeronUiRebindResult {
	AERON_UI_REBIND_NONE      = 0,
	AERON_UI_REBIND_STARTED   = 1,
	AERON_UI_REBIND_CAPTURED  = 2,
	AERON_UI_REBIND_CANCELLED = 3,
} AeronUiRebindResult;

typedef struct AeronUiCapturedInput {
	enum {
		AERON_UI_CAPTURE_KEY          = 0,
		AERON_UI_CAPTURE_MOUSE_BUTTON = 1,
		AERON_UI_CAPTURE_PAD_BUTTON   = 2,
		AERON_UI_CAPTURE_PAD_AXIS     = 3,
	} kind;
	int code;           /* AeronKey / mouse-button bit index /
						   AeronGamepadButton / AeronGamepadAxis */
	int controller;     /* pad captures: controller slot */
	int axis_direction; /* -1 / +1 for axis captures */
} AeronUiCapturedInput;

int AeronUi_Rebind(AeronUiContext* ctx, const char* label, const char* display, AeronUiCapturedInput* out);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_UI_H */

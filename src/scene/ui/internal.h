#ifndef AERON_SCENE_UI_INTERNAL_H
#define AERON_SCENE_UI_INTERNAL_H

/*
 * AeronUi internals shared by the UI translation units.
 *
 * Immediate-mode core: widgets record themselves into a per-frame index
 * (double-buffered so navigation and auto-sizing can consult the
 * previous frame), transient interaction state lives in a fixed
 * open-addressed hash keyed by widget id, and all drawing goes through
 * one AeronDrawList2D in content-rect pixel space.
 */

#include "aeron/scene/ui.h"

#include "aeron/aeron.h" /* Aeron_GetLogicalSize */
#include "aeron/log.h"
#include "aeron/scene/text2d.h"

#include <math.h>
#include <string.h>

#define AERON_UI_DEFAULT_WIDGETS 256
#define AERON_UI_DEFAULT_RECORDS 8192
#define AERON_UI_ID_STACK_CAP 8
#define AERON_UI_LAYOUT_CAP 8
#define AERON_UI_MAX_COLUMNS 8
#define AERON_UI_MAX_SCOPES 4
#define AERON_UI_MAX_SCROLLS 4
#define AERON_UI_STATE_SLOTS 256
#define AERON_UI_REFERENCE_H 1080.0f

/* Widget identity: FNV-1a of the label seeded by the id stack. */
typedef uint32_t AeronUiId;

typedef struct UiRect {
	float x, y, w, h;
} UiRect;

/* One declared widget in the frame index. */
typedef struct UiWidgetRec {
	AeronUiId id;
	UiRect    rect; /* content px, unclipped */
	uint8_t   focusable;
	uint8_t   scope;     /* modal scope depth at declaration */
	int8_t    column;    /* ordinal within its columns run; -1 outside */
	int16_t   col_run;   /* which BeginColumns run; -1 outside */
	AeronUiId scroll_id; /* owning scroll region; 0 outside */
} UiWidgetRec;

/* Transient per-widget state (scroll offsets, window auto-height, drag
 * anchors, capture phases). Slots recycle by least-recent frame. */
typedef struct UiStateSlot {
	AeronUiId id; /* 0 = empty */
	float     v0;
	float     v1;
	uint32_t  frame;
} UiStateSlot;

typedef enum UiLayoutKind {
	UI_LAYOUT_WINDOW = 0,
	UI_LAYOUT_SCROLL = 1,
} UiLayoutKind;

/* One entry of the container layout stack (window, modal, scroll).
 * Columns modify the top entry in place — they do not nest. */
typedef struct UiLayout {
	UiLayoutKind kind;
	float        x, w;     /* content region, px */
	float        cursor_y; /* next row top, px */
	AeronRectI   clip;     /* zero w/h = none */
	/* Columns state (columns == 0 when inactive). */
	int     columns;
	int     col_index;
	int16_t col_run;
	float   col_x[AERON_UI_MAX_COLUMNS];
	float   col_w[AERON_UI_MAX_COLUMNS];
	float   col_start_y;
	float   col_max_y;
	/* Region ownership: window auto-size measurement or scroll state. */
	AeronUiId owner_id;
	float     view_y; /* content start (window) / view top (scroll) */
	float     view_h; /* scroll view height */
	float     offset; /* scroll offset applied to the cursor */
} UiLayout;

/* Per-frame scroll-region info for wheel + focus-into-view resolution. */
typedef struct UiScrollRec {
	AeronUiId id;
	float     view_y, view_h;
	float     content_h;
} UiScrollRec;

/* Navigation directions (repeat accumulator index). */
enum { UI_DIR_UP = 0, UI_DIR_DOWN = 1, UI_DIR_LEFT = 2, UI_DIR_RIGHT = 3, UI_DIR_COUNT = 4 };

struct AeronUiContext {
	/* Configuration. */
	int            max_widgets;
	AeronUiSoundFn sound_fn;
	void*          sound_user;
	AeronUiTheme   theme;
	AeronUiFontSet fonts;

	AeronDrawList2D* list;

	/* Frame state. */
	uint32_t                  frame_id;
	int                       frame_active;
	float                     dt;
	float                     scale; /* output_h / 1080 */
	int                       out_w, out_h;
	const AeronInputSnapshot* input;
	float                     fade; /* open-fade multiplier 0..1 */

	/* Mouse in content px; valid when mouse_present. */
	float mouse_x, mouse_y;
	int   mouse_present;
	int   mouse_moved;
	float last_mouse_x, last_mouse_y;

	/* Widget index, double-buffered. */
	UiWidgetRec* widgets;
	int          widget_count;
	int          widget_dropped;
	UiWidgetRec* prev_widgets;
	int          prev_widget_count;

	/* Id stack. */
	uint32_t id_stack[AERON_UI_ID_STACK_CAP];
	int      id_stack_depth;

	/* Layout stack. */
	UiLayout layouts[AERON_UI_LAYOUT_CAP];
	int      layout_depth;
	int16_t  next_col_run;
	int      window_depth; /* Begin/EndWindow nesting guard */

	/* Focus + interaction. */
	AeronUiId focus_id;
	AeronUiId focus_at_frame_start;
	AeronUiId active_id;      /* mouse-captured widget (press/drag) */
	AeronUiId hot_id;         /* hovered this frame */
	int       scope_depth;    /* current modal scope while declaring */
	int       top_scope_prev; /* interactive scope (modal lag, 1 frame) */
	AeronUiId scope_saved_focus[AERON_UI_MAX_SCOPES];
	AeronUiId modal_open_id; /* modal declared this frame, 0 = none */

	/* Navigation intents for the current frame (consumed by widgets,
	 * leftovers resolved at EndFrame). */
	int nav[UI_DIR_COUNT];
	int nav_accept; /* activation edges */
	int nav_accept_down;
	int nav_cancel;   /* cancel edges */
	int nav_tab_next; /* tab-page cycling */
	int nav_tab_prev;

	/* Held-direction repeat + stick hysteresis. */
	float repeat_t[UI_DIR_COUNT];
	int   held_prev[UI_DIR_COUNT];
	int   stick_latch_x, stick_latch_y; /* -1 / 0 / +1 */

	/* Per-frame results. */
	int any_window; /* a window/modal was declared this frame */
	int any_window_prev;
	int cancel_consumed;
	int value_changed; /* an ADJUST sound was already played */

	/* Rebind capture (rebind.c). */
	AeronUiId rebind_id;        /* widget in capture mode, 0 = none */
	int       rebind_capturing; /* latched for EndFrame's capture_all */

	/* Scroll regions declared this frame. */
	UiScrollRec scrolls[AERON_UI_MAX_SCROLLS];
	int         scroll_count;

	/* Transient state hash. */
	UiStateSlot state[AERON_UI_STATE_SLOTS];

	/* Presentation path (chosen at BeginFrame). */
	int                direct_path;
	int                offline; /* explicit output dims, no engine */
	AeronRectI         content_rect;
	AeronRenderTarget* fallback_rt;
	int                fallback_w, fallback_h;
};

/* ---- context.c ---- */
AeronUiId    ui_make_id(AeronUiContext* ctx, const char* label);
const char*  ui_label_text(const char* label); /* text before "##" (label itself) */
int          ui_label_text_len(const char* label);
UiStateSlot* ui_state(AeronUiContext* ctx, AeronUiId id); /* NULL when table is full */
void         ui_play_sound(AeronUiContext* ctx, AeronUiSound sound);

/* ---- focus.c ---- */
void ui_collect_input(AeronUiContext* ctx);
void ui_resolve_navigation(AeronUiContext* ctx); /* EndFrame focus resolution */
/* Standard row behavior: hover tracking, hover-focus, click activation.
 * Returns nonzero when the widget was activated (click or accept). */
int ui_widget_behavior(AeronUiContext* ctx, AeronUiId id, const UiRect* rect, int focusable);
/* Nonzero while `id` is focused. */
int ui_is_focused(const AeronUiContext* ctx, AeronUiId id);
/* Consumes pending left/right intents for the focused value widget;
 * returns the net step delta (negative = left). */
int ui_consume_adjust(AeronUiContext* ctx, AeronUiId id);
int ui_mouse_in(const AeronUiContext* ctx, const UiRect* rect, const AeronRectI* clip);

/* ---- layout.c ---- */
UiLayout* ui_layout_top(AeronUiContext* ctx);
void      ui_layout_push(AeronUiContext* ctx, const UiLayout* layout);
void      ui_layout_pop(AeronUiContext* ctx);
/* Allocates the next row of `height` px; returns 0 when no container is
 * open. Advances the cursor by height + item_spacing. */
int ui_layout_row(AeronUiContext* ctx, float height, UiRect* out);
/* Records a widget into the frame index (drop + warn over capacity). */
void ui_record_widget(AeronUiContext* ctx, AeronUiId id, const UiRect* rect, int focusable);

/* ---- widgets.c ---- */
/* Splits a row into label/control regions per theme.label_fraction and
 * draws the label; returns the control region. */
UiRect ui_form_row_split(AeronUiContext* ctx, const char* label, const UiRect* row, const AeronRectI* clip);

/* ---- submit.c ---- */
AeronRenderTarget* ui_submit_ensure_fallback_rt(AeronUiContext* ctx);
void               ui_submit_release_fallback_rt(AeronUiContext* ctx);

/* ---- window.c ---- */
/* Shared body used by windows and modals. */
int  ui_begin_window_common(AeronUiContext* ctx, const char* title, const AeronUiWindowDesc* desc,
							AeronUiId id);
void ui_end_window_common(AeronUiContext* ctx);

/* ---- drawing helpers (draw.c) ---- */
/* Theme color -> PMA output color, applying the open-fade multiplier. */
void ui_pma(const AeronUiContext* ctx, const AeronUiColor color, float out[4]);
/* Themed rounded-rect surface. `bg`/`bg_low` straight-alpha theme
 * colors (bg_low ignored unless gradient). border/bevel optional. */
void ui_draw_surface(AeronUiContext* ctx, const UiRect* rect, const AeronUiColor bg,
					 const AeronUiColor bg_low, int gradient, const AeronUiColor border_color,
					 float border_px, int bevel, const AeronRectI* clip);
void ui_draw_fill(AeronUiContext* ctx, const UiRect* rect, const AeronUiColor color, const AeronRectI* clip);
void ui_draw_focus_outline(AeronUiContext* ctx, const UiRect* rect, const AeronRectI* clip);
void ui_draw_row_focus_bg(AeronUiContext* ctx, const UiRect* row, const AeronRectI* clip);
void ui_draw_row_focus_ring(AeronUiContext* ctx, const UiRect* row, const AeronRectI* clip);
void ui_draw_shadow(AeronUiContext* ctx, const UiRect* rect, const AeronRectI* clip);
/* Single line of themed text. size_px is already scaled (target px). */
void ui_draw_text(AeronUiContext* ctx, const AeronFontAtlas* font, float x, float y, AeronTextAlign align,
				  float size_px, const AeronUiColor color, const char* text, int len, const AeronRectI* clip);
float ui_text_width(const AeronUiContext* ctx, const AeronFontAtlas* font, float size_px, const char* text,
					int len);
/* Body/title font accessors (title falls back to regular). */
const AeronFontAtlas* ui_font_regular(const AeronUiContext* ctx);
const AeronFontAtlas* ui_font_title(const AeronUiContext* ctx);
/* Inter-glyph tracking for `font`, in atlas pixels. */
float ui_font_tracking_atlas(const AeronUiContext* ctx, const AeronFontAtlas* font);

/* Scaled metric: reference px @1080p -> target px. */
static inline float ui_ref(const AeronUiContext* ctx, float ref) { return ref * ctx->scale; }

/* Whole-pixel snap for crisp 1 px borders. */
static inline float ui_snap(float v) { return floorf(v + 0.5f); }

#endif /* AERON_SCENE_UI_INTERNAL_H */

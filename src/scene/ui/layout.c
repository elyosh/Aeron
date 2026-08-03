/*
 * AeronUi layout — container stack, row cursor, and columns.
 *
 * Deterministic single-pass layout: containers push a region, rows
 * advance a cursor, BeginColumns splits the remaining width in place
 * (columns do not nest). Auto-sized dimensions read the previous
 * frame's measurements from the transient state table.
 */

#include "internal.h"

UiLayout* ui_layout_top(AeronUiContext* ctx) {
	return ctx->layout_depth > 0 ? &ctx->layouts[ctx->layout_depth - 1] : NULL;
}

void ui_layout_push(AeronUiContext* ctx, const UiLayout* layout) {
	if (ctx->layout_depth >= AERON_UI_LAYOUT_CAP) {
		Aeron_LogWarn("aeron.scene", "ui layout stack cap (%d) hit", AERON_UI_LAYOUT_CAP);
		return;
	}
	ctx->layouts[ctx->layout_depth++] = *layout;
}

void ui_layout_pop(AeronUiContext* ctx) {
	if (ctx->layout_depth > 0) {
		ctx->layout_depth--;
	}
}

int ui_layout_row(AeronUiContext* ctx, float height, UiRect* out) {
	UiLayout* top = ui_layout_top(ctx);
	if (!top) {
		Aeron_LogWarn("aeron.scene", "ui widget declared outside a window");
		return 0;
	}
	if (top->columns > 0) {
		out->x = top->col_x[top->col_index];
		out->w = top->col_w[top->col_index];
	} else {
		out->x = top->x;
		out->w = top->w;
	}
	out->y = top->cursor_y;
	out->h = height;
	top->cursor_y += height + ui_ref(ctx, ctx->theme.item_spacing);
	if (top->columns > 0 && top->cursor_y > top->col_max_y) {
		top->col_max_y = top->cursor_y;
	}
	return 1;
}

void ui_record_widget(AeronUiContext* ctx, AeronUiId id, const UiRect* rect, int focusable) {
	if (ctx->widget_count >= ctx->max_widgets) {
		if (!ctx->widget_dropped) {
			Aeron_LogWarn("aeron.scene", "ui widget cap (%d) hit; dropping", ctx->max_widgets);
		}
		ctx->widget_dropped++;
		return;
	}
	UiWidgetRec* w = &ctx->widgets[ctx->widget_count++];
	w->id          = id;
	w->rect        = *rect;
	w->focusable   = (uint8_t)(focusable != 0);
	w->scope       = (uint8_t)ctx->scope_depth;
	w->column      = -1;
	w->col_run     = -1;
	w->scroll_id   = 0;

	const UiLayout* top = ui_layout_top(ctx);
	if (top && top->columns > 0) {
		w->column  = (int8_t)top->col_index;
		w->col_run = top->col_run;
	}
	/* Nearest enclosing scroll region owns the widget for
	 * focus-into-view resolution. */
	for (int i = ctx->layout_depth - 1; i >= 0; i--) {
		if (ctx->layouts[i].kind == UI_LAYOUT_SCROLL) {
			w->scroll_id = ctx->layouts[i].owner_id;
			break;
		}
	}
}

void AeronUi_BeginColumns(AeronUiContext* ctx, int count, const float* weights) {
	UiLayout* top = ctx ? ui_layout_top(ctx) : NULL;
	if (!top || count <= 0) {
		return;
	}
	if (top->columns > 0) {
		Aeron_LogWarn("aeron.scene", "ui columns do not nest");
		return;
	}
	if (count > AERON_UI_MAX_COLUMNS) {
		Aeron_LogWarn("aeron.scene", "ui column cap (%d) hit; clamping", AERON_UI_MAX_COLUMNS);
		count = AERON_UI_MAX_COLUMNS;
	}

	/* Fixed widths (negative weights, in reference px) come off the top;
	 * positive weights share the remainder. */
	const float spacing     = ui_ref(ctx, ctx->theme.column_spacing);
	float       flex_total  = 0.0f;
	float       fixed_total = 0.0f;
	for (int i = 0; i < count; i++) {
		const float weight = weights ? weights[i] : 1.0f;
		if (weight < 0.0f) {
			fixed_total += ui_ref(ctx, -weight);
		} else {
			flex_total += weight > 0.0f ? weight : 1.0f;
		}
	}
	const float flex_space = fmaxf(0.0f, top->w - fixed_total - spacing * (float)(count - 1));

	float x = top->x;
	for (int i = 0; i < count; i++) {
		const float weight = weights ? weights[i] : 1.0f;
		float       width;
		if (weight < 0.0f) {
			width = ui_ref(ctx, -weight);
		} else {
			width = flex_total > 0.0f ? flex_space * ((weight > 0.0f ? weight : 1.0f) / flex_total) : 0.0f;
		}
		top->col_x[i] = ui_snap(x);
		top->col_w[i] = ui_snap(width);
		x += width + spacing;
	}
	top->columns     = count;
	top->col_index   = 0;
	top->col_run     = ctx->next_col_run++;
	top->col_start_y = top->cursor_y;
	top->col_max_y   = top->cursor_y;
}

void AeronUi_NextColumn(AeronUiContext* ctx) {
	UiLayout* top = ctx ? ui_layout_top(ctx) : NULL;
	if (!top || top->columns <= 0) {
		return;
	}
	if (top->col_index + 1 >= top->columns) {
		Aeron_LogWarn("aeron.scene", "ui NextColumn past the last column");
		return;
	}
	top->col_index++;
	top->cursor_y = top->col_start_y; /* each column runs its own stack */
}

void AeronUi_EndColumns(AeronUiContext* ctx) {
	UiLayout* top = ctx ? ui_layout_top(ctx) : NULL;
	if (!top || top->columns <= 0) {
		return;
	}
	top->columns  = 0;
	top->cursor_y = top->col_max_y; /* advance past the tallest column */
}

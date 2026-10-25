#include <obs-module.h>
#include <math.h>
#include <string.h>
#include "plugin-macros.generated.h"
#include "markers.h"

void cm_markers_init(struct cm_markers *m)
{
	pthread_mutex_init(&m->mutex, NULL);
	m->n = 0;
}

void cm_markers_free(struct cm_markers *m)
{
	pthread_mutex_destroy(&m->mutex);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void cm_markers_load(struct cm_markers *m, obs_data_t *settings)
{
	struct cm_marker items[CM_MARKERS_MAX];
	int n = 0;

	obs_data_array_t *array = obs_data_get_array(settings, "markers");
	const size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < count && n < CM_MARKERS_MAX; i++) {
		obs_data_t *d = obs_data_array_item(array, i);
		struct cm_marker *k = items + n++;
		memset(k, 0, sizeof(*k));
		snprintf(k->name, sizeof(k->name), "%s", obs_data_get_string(d, "name"));
		k->kind = (int)obs_data_get_int(d, "kind");
		k->level = clampf((float)obs_data_get_double(d, "level") / 100.0f, -0.1f, 1.1f);
		k->hue = (float)obs_data_get_double(d, "hue");
		k->sat = clampf((float)obs_data_get_double(d, "sat") / 100.0f, 0.0f, 1.2f);
		k->color = obs_data_has_user_value(d, "color") ? (uint32_t)obs_data_get_int(d, "color") : 0xFFFFD040;
		k->style = (int)obs_data_get_int(d, "style");
		k->shape = (int)obs_data_get_int(d, "shape");
		k->width = obs_data_has_user_value(d, "width") ? (float)obs_data_get_double(d, "width") : 1.5f;
		k->width = clampf(k->width, 0.5f, 8.0f);
		k->show_label = obs_data_has_user_value(d, "show_label") ? obs_data_get_bool(d, "show_label") : true;
		obs_data_release(d);
	}
	obs_data_array_release(array);

	pthread_mutex_lock(&m->mutex);
	memcpy(m->items, items, sizeof(items[0]) * n);
	m->n = n;
	pthread_mutex_unlock(&m->mutex);
}

int cm_markers_copy(struct cm_markers *m, struct cm_marker *out)
{
	pthread_mutex_lock(&m->mutex);
	const int n = m->n;
	memcpy(out, m->items, sizeof(m->items[0]) * n);
	pthread_mutex_unlock(&m->mutex);
	return n;
}

/* Quads are drawn in immediate mode; libobs holds at most 512 vertices per batch. */
#define QUADS_PER_BATCH 80

struct quad_batch
{
	int n;
	bool open;
};

static void batch_quad(struct quad_batch *b, float ax, float ay, float bx, float by, float cx, float cy, float dx,
		       float dy)
{
	if (!b->open) {
		gs_render_start(false);
		b->open = true;
		b->n = 0;
	}
	gs_vertex2f(ax, ay);
	gs_vertex2f(bx, by);
	gs_vertex2f(cx, cy);
	gs_vertex2f(cx, cy);
	gs_vertex2f(bx, by);
	gs_vertex2f(dx, dy);
	if (++b->n >= QUADS_PER_BATCH) {
		gs_render_stop(GS_TRIS);
		b->open = false;
	}
}

static void batch_end(struct quad_batch *b)
{
	if (b->open)
		gs_render_stop(GS_TRIS);
	b->open = false;
}

/* A thick segment as a quad; the thickness is in screen pixels, converted per axis. */
static void segment_quad(struct quad_batch *b, float x0, float y0, float x1, float y1, float half_px, float ux,
			 float uy)
{
	// direction in screen pixels
	const float sx = (x1 - x0) / ux, sy = (y1 - y0) / uy;
	const float len = sqrtf(sx * sx + sy * sy);
	if (len < 1e-4f)
		return;
	const float nx = -sy / len * half_px * ux, ny = sx / len * half_px * uy;
	batch_quad(b, x0 + nx, y0 + ny, x1 + nx, y1 + ny, x0 - nx, y0 - ny, x1 - nx, y1 - ny);
}

static void begin_solid(uint32_t color, gs_effect_t **solid)
{
	*solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(*solid, "color"), color);
}

void cm_marker_draw_line(float x0, float y0, float x1, float y1, uint32_t color, int style, float width_px, float ux,
			 float uy)
{
	gs_effect_t *solid;
	begin_solid(color, &solid);

	const float sx = (x1 - x0) / ux, sy = (y1 - y0) / uy;
	const float len_px = sqrtf(sx * sx + sy * sy);
	if (len_px < 1e-3f)
		return;

	float on_px = len_px, period_px = len_px;
	if (style == CM_MARKER_DASHED) {
		on_px = 8.0f + width_px * 2.0f;
		period_px = on_px + 5.0f + width_px;
	} else if (style == CM_MARKER_DOTTED) {
		on_px = fmaxf(width_px, 1.5f);
		period_px = on_px + 3.0f + width_px;
	}
	// keep the batch count sensible on very long lines
	if (len_px / period_px > 400.0f) {
		const float k = len_px / period_px / 400.0f;
		on_px *= k;
		period_px *= k;
	}

	while (gs_effect_loop(solid, "Solid")) {
		struct quad_batch b = {0};
		for (float t = 0.0f; t < len_px; t += period_px) {
			const float t1 = fminf(t + on_px, len_px);
			const float a = t / len_px, c = t1 / len_px;
			segment_quad(&b, x0 + (x1 - x0) * a, y0 + (y1 - y0) * a, x0 + (x1 - x0) * c, y0 + (y1 - y0) * c,
				     width_px * 0.5f, ux, uy);
		}
		batch_end(&b);
	}
}

void cm_marker_draw_point(float x, float y, uint32_t color, int shape, float size_px, float ux, float uy)
{
	gs_effect_t *solid;
	begin_solid(color, &solid);

	const float r = 3.0f + size_px * 2.0f; // radius in screen pixels
	const float t = fmaxf(size_px * 0.6f, 1.0f);

	while (gs_effect_loop(solid, "Solid")) {
		struct quad_batch b = {0};
		switch (shape) {
		case CM_MARKER_SHAPE_CROSS:
			segment_quad(&b, x - r * ux, y, x + r * ux, y, t * 0.5f, ux, uy);
			segment_quad(&b, x, y - r * uy, x, y + r * uy, t * 0.5f, ux, uy);
			break;
		case CM_MARKER_SHAPE_SQUARE:
			segment_quad(&b, x - r * ux, y - r * uy, x + r * ux, y - r * uy, t * 0.5f, ux, uy);
			segment_quad(&b, x + r * ux, y - r * uy, x + r * ux, y + r * uy, t * 0.5f, ux, uy);
			segment_quad(&b, x + r * ux, y + r * uy, x - r * ux, y + r * uy, t * 0.5f, ux, uy);
			segment_quad(&b, x - r * ux, y + r * uy, x - r * ux, y - r * uy, t * 0.5f, ux, uy);
			break;
		case CM_MARKER_SHAPE_DOT: {
			// filled disc as a fan of quads
			const int n = 16;
			for (int i = 0; i < n; i++) {
				const float a0 = (float)i / n * 2.0f * (float)M_PI;
				const float a1 = (float)(i + 1) / n * 2.0f * (float)M_PI;
				const float rr = r * 0.7f;
				batch_quad(&b, x, y, x, y, x + rr * cosf(a0) * ux, y - rr * sinf(a0) * uy,
					   x + rr * cosf(a1) * ux, y - rr * sinf(a1) * uy);
			}
			break;
		}
		case CM_MARKER_SHAPE_CIRCLE:
		default: {
			const int n = 24;
			for (int i = 0; i < n; i++) {
				const float a0 = (float)i / n * 2.0f * (float)M_PI;
				const float a1 = (float)(i + 1) / n * 2.0f * (float)M_PI;
				segment_quad(&b, x + r * cosf(a0) * ux, y - r * sinf(a0) * uy, x + r * cosf(a1) * ux,
					     y - r * sinf(a1) * uy, t * 0.5f, ux, uy);
			}
			// center dot
			segment_quad(&b, x - 1.0f * ux, y, x + 1.0f * ux, y, 1.0f, ux, uy);
			break;
		}
		}
		batch_end(&b);
	}
}

/*
 * Quad balance: the picture in a grid (2x2 up to 4x4). Every cell shows its average color, its luma and how far
 * it is from the whole picture, and an arrow for its color cast. Uneven light on a set or a pitch, vignetting and a
 * lens that tints the corners stand out.
 */
#include <obs-module.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"
#include "draw-util.h"

#define QB_W 384
#define QB_H 236
#define GRID_H 216
#define MAX_GRID 4

struct cell
{
	float luma, cb, cr; // %, the cast in % (0 is neutral)
	float r, g, b;
	bool valid;
};

struct qb_source
{
	struct cm_source cm;
	pthread_mutex_t mutex;
	struct cell cells[MAX_GRID][MAX_GRID];
	struct cell all;
	int grid;
	float luma_tol, color_tol;
};

static const char *qb_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("QuadBalance");
}

static void qb_surface_cb(void *data, struct cm_surface_data *sd);

static void qb_update(void *data, obs_data_t *settings)
{
	struct qb_source *src = data;
	cm_update(&src->cm, settings);
	src->grid = (int)obs_data_get_int(settings, "qb_grid");
	if (src->grid < 2 || src->grid > MAX_GRID)
		src->grid = 2;
	src->luma_tol = (float)obs_data_get_double(settings, "qb_luma_tol");
	src->color_tol = (float)obs_data_get_double(settings, "qb_color_tol");
}

static void *qb_create(obs_data_t *settings, obs_source_t *source)
{
	struct qb_source *src = bzalloc(sizeof(struct qb_source));
	pthread_mutex_init(&src->mutex, NULL);
	src->cm.flags = CM_FLAG_CONVERT_YUV;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, qb_surface_cb, src);
	qb_update(src, settings);
	return src;
}

static void qb_destroy(void *data)
{
	struct qb_source *src = data;
	cm_destroy(&src->cm);
	pthread_mutex_destroy(&src->mutex);
	bfree(src);
}

static void qb_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "qb_grid", 2);
	obs_data_set_default_double(settings, "qb_luma_tol", 5.0);
	obs_data_set_default_double(settings, "qb_color_tol", 2.0);
}

static obs_properties_t *qb_get_properties(void *data)
{
	struct qb_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_property_t *p = obs_properties_add_list(props, "qb_grid", obs_module_text("QuadBalance.Grid"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "2 x 2", 2);
	obs_property_list_add_int(p, "3 x 3", 3);
	obs_property_list_add_int(p, "4 x 4", 4);
	p = obs_properties_add_float_slider(props, "qb_luma_tol", obs_module_text("QuadBalance.LumaTol"), 0.5, 20.0,
					    0.5);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, "qb_color_tol", obs_module_text("QuadBalance.ColorTol"), 0.2, 10.0,
					    0.1);
	obs_property_float_set_suffix(p, " %");
	return props;
}

static uint32_t qb_get_width(void *data)
{
	struct qb_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : QB_W;
}

static uint32_t qb_get_height(void *data)
{
	struct qb_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : QB_H;
}

static void finish(struct cell *c, double su, double sv, double sy, uint64_t n, int cs)
{
	c->valid = n > 0;
	if (!n)
		return;
	const float kr = cs == CM_CS_601 ? 0.299f : cm_cs_is_hdr(cs) ? 0.2627f : 0.2126f;
	const float kb = cs == CM_CS_601 ? 0.114f : cm_cs_is_hdr(cs) ? 0.0593f : 0.0722f;
	const float u = (float)(su / n) - 128.0f, v = (float)(sv / n) - 128.0f;
	c->luma = ((float)(sy / n) - 16.0f) / 219.0f * 100.0f;
	c->cb = u / 224.0f * 100.0f;
	c->cr = v / 224.0f * 100.0f;
	const float y = c->luma / 100.0f;
	c->r = y + 2.0f * (1.0f - kr) * (v / 224.0f);
	c->b = y + 2.0f * (1.0f - kb) * (u / 224.0f);
	c->g = (y - kr * c->r - kb * c->b) / (1.0f - kr - kb);
}

static void qb_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct qb_source *src = data;
	if (!sd->yuv_data || !sd->width || !sd->height)
		return;
	const int g = src->grid;
	double su[MAX_GRID][MAX_GRID] = {{0}}, sv[MAX_GRID][MAX_GRID] = {{0}}, sy[MAX_GRID][MAX_GRID] = {{0}};
	uint64_t n[MAX_GRID][MAX_GRID] = {{0}};
	double tu = 0, tv = 0, ty = 0;
	uint64_t tn = 0;
	for (uint32_t y = 0; y < sd->height; y++) {
		const int gy = (int)((uint64_t)y * g / sd->height);
		const uint8_t *p = sd->yuv_data + (size_t)sd->linesize * y;
		for (uint32_t x = 0; x < sd->width; x++, p += 4) {
			if (!p[3])
				continue;
			const int gx = (int)((uint64_t)x * g / sd->width);
			su[gy][gx] += p[0], sv[gy][gx] += p[2], sy[gy][gx] += p[1];
			n[gy][gx]++;
			tu += p[0], tv += p[2], ty += p[1];
			tn++;
		}
	}
	struct cell cells[MAX_GRID][MAX_GRID] = {{{0}}}, all = {0};
	for (int j = 0; j < g; j++)
		for (int i = 0; i < g; i++)
			finish(&cells[j][i], su[j][i], sv[j][i], sy[j][i], n[j][i], sd->colorspace);
	finish(&all, tu, tv, ty, tn, sd->colorspace);
	pthread_mutex_lock(&src->mutex);
	memcpy(src->cells, cells, sizeof(cells));
	src->all = all;
	pthread_mutex_unlock(&src->mutex);
}

static void qb_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct qb_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	struct cell cells[MAX_GRID][MAX_GRID], all;
	pthread_mutex_lock(&src->mutex);
	memcpy(cells, src->cells, sizeof(cells));
	all = src->all;
	pthread_mutex_unlock(&src->mutex);
	const int g = src->grid;

	const struct text_scale ts = text_scale_from_viewport((float)QB_W, (float)QB_H);
	du_fill_rect(0, 0, (float)QB_W, (float)QB_H, 0xFF0C0C0E);
	const float cw = (float)QB_W / g, ch = (float)GRID_H / g;
	float lmin = 1e9f, lmax = -1e9f, cmax = 0.0f;
	char buf[64];
	for (int j = 0; j < g; j++)
		for (int i = 0; i < g; i++) {
			const struct cell *c = &cells[j][i];
			const float x0 = i * cw, y0 = j * ch, x1 = x0 + cw, y1 = y0 + ch;
			if (!c->valid) {
				du_rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 0x40FFFFFF);
				continue;
			}
			lmin = fminf(lmin, c->luma), lmax = fmaxf(lmax, c->luma);
			// the cell's average color, a little darker so the text stays readable
			du_fill_rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1,
				     du_argb(c->r * 0.6f, c->g * 0.6f, c->b * 0.6f, 1.0f));
			const float dl = c->luma - all.luma;
			const float dc = hypotf(c->cb - all.cb, c->cr - all.cr);
			cmax = fmaxf(cmax, dc);
			const bool l_ok = fabsf(dl) <= src->luma_tol, c_ok = dc <= src->color_tol;
			du_rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, l_ok && c_ok ? 0x5060E060 : 0xC0FFA030);
			// the cast against the whole picture: an arrow from the middle, full cell for 4 x the tolerance
			const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
			const float k = fminf(cw, ch) * 0.45f / (src->color_tol * 4.0f);
			const float ax = mx + (c->cb - all.cb) * k, ay = my - (c->cr - all.cr) * k;
			du_line(mx, my, ax, ay, c_ok ? 0xC0FFFFFF : 0xFFFFC040);
			du_circle(ax, ay, 1.6f, c_ok ? 0xC0FFFFFF : 0xFFFFC040, true);
			snprintf(buf, sizeof(buf), "Y %.0f%%", c->luma);
			text_draw(&ts, buf, x0 + 4.0f * ts.ux, y0 + 3.0f * ts.uy, 10.0f, 0xFFFFFFFF, TEXT_ALIGN_LEFT,
				  TEXT_VALIGN_TOP);
			snprintf(buf, sizeof(buf), "%+.1f", dl);
			text_draw(&ts, buf, x1 - 4.0f * ts.ux, y0 + 3.0f * ts.uy, 10.0f, l_ok ? 0xFF80F080 : 0xFFFFB040,
				  TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
			snprintf(buf, sizeof(buf), "%.1f%%", dc);
			text_draw(&ts, buf, x1 - 4.0f * ts.ux, y1 - 3.0f * ts.uy, 9.0f, c_ok ? 0xC0FFFFFF : 0xFFFFC040,
				  TEXT_ALIGN_RIGHT, TEXT_VALIGN_BOTTOM);
		}
	if (all.valid) {
		snprintf(buf, sizeof(buf), "%s %.1f%%   %s %.1f%%   Y %.0f%%",
			 obs_module_text("QuadBalance.LumaSpread"), lmax - lmin,
			 obs_module_text("QuadBalance.ColorSpread"), cmax, all.luma);
		const bool ok = lmax - lmin <= src->luma_tol * 2.0f && cmax <= src->color_tol;
		text_draw(&ts, buf, 4.0f * ts.ux, QB_H - 3.0f * ts.uy, 10.0f, ok ? 0xFF80F080 : 0xFFFFB040,
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_BOTTOM);
	}
}

const struct obs_source_info colormonitor_quadbalance = {
	.id = ID_PREFIX "quadbalance_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = qb_get_name,
	.create = qb_create,
	.destroy = qb_destroy,
	.update = qb_update,
	.get_defaults = qb_get_defaults,
	.get_properties = qb_get_properties,
	.get_width = qb_get_width,
	.get_height = qb_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = qb_render,
	.video_tick = cm_tick,
};

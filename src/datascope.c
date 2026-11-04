/*
 * Data analyser: the code values of a block of pixels around the first color picker pin (or the middle of the
 * picture), as numbers in a grid. On a video source with the raw input filter the values are the exact codes of
 * the signal, one per pixel of the frame (8- or 10-bit, including superwhite and sub-black); otherwise they come
 * from the scope's capture, at its scale.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "roi.h"
#include "raw-input.h"
#include "text-render.h"

#define DS_W 960
#define DS_H 600
#define DS_COLS_MAX 15
#define DS_ROWS_MAX 11

enum ds_show_e {
	DS_SHOW_Y = 0,
	DS_SHOW_YCC = 1, // Y, Cb, Cr
	DS_SHOW_RGB = 2,
};

enum ds_unit_e {
	DS_UNIT_10 = 0, // 10-bit codes (64 to 940 is 0 % to 100 %)
	DS_UNIT_8 = 1,  // 8-bit codes (16 to 235)
	DS_UNIT_PCT = 2,
};

struct ds_cell
{
	float ycc[3]; // Y, Cb, Cr as 10-bit codes
	float rgb[3]; // R', G', B', 0..1 of the signal
	bool have_ycc;
};

struct ds_source
{
	struct cm_source cm;
	pthread_mutex_t mutex;
	struct ds_cell cells[DS_ROWS_MAX * DS_COLS_MAX];
	struct ds_cell work[DS_ROWS_MAX * DS_COLS_MAX]; // the pipeline thread's measurement
	int n_cols, n_rows;                             // of the last measurement
	bool valid, raw, rgb_source;
	float u, v; // where
	bool at_pin;
	char status[64];

	int cols, rows, show, unit;
};

static void ds_update(void *, obs_data_t *);
static void ds_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *ds_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DataAnalyser");
}

static void *ds_create(obs_data_t *settings, obs_source_t *source)
{
	struct ds_source *src = bzalloc(sizeof(struct ds_source));
	pthread_mutex_init(&src->mutex, NULL);
	src->cm.flags = CM_FLAG_CONVERT_RGB | CM_FLAG_CONVERT_YUV | CM_FLAG_EXT;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, ds_surface_cb, src);
	ds_update(src, settings);
	return src;
}

static void ds_destroy(void *data)
{
	struct ds_source *src = data;
	cm_destroy(&src->cm);
	pthread_mutex_destroy(&src->mutex);
	bfree(src);
}

static void ds_update(void *data, obs_data_t *settings)
{
	struct ds_source *src = data;
	cm_update(&src->cm, settings);
	src->cols = (int)obs_data_get_int(settings, "ds_cols") | 1; // odd, so there is a middle pixel
	src->rows = (int)obs_data_get_int(settings, "ds_rows") | 1;
	src->cols = src->cols > DS_COLS_MAX ? DS_COLS_MAX : src->cols < 1 ? 1 : src->cols;
	src->rows = src->rows > DS_ROWS_MAX ? DS_ROWS_MAX : src->rows < 1 ? 1 : src->rows;
	src->show = (int)obs_data_get_int(settings, "ds_show");
	src->unit = (int)obs_data_get_int(settings, "ds_unit");
}

static void ds_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 1);
	obs_data_set_default_int(settings, "ds_cols", 9);
	obs_data_set_default_int(settings, "ds_rows", 7);
	obs_data_set_default_int(settings, "ds_show", DS_SHOW_YCC);
	obs_data_set_default_int(settings, "ds_unit", DS_UNIT_10);
}

static obs_properties_t *ds_get_properties(void *data)
{
	struct ds_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_property_t *p = obs_properties_add_list(props, "ds_show", obs_module_text("DataAnalyser.Show"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Y", DS_SHOW_Y);
	obs_property_list_add_int(p, "Y Cb Cr", DS_SHOW_YCC);
	obs_property_list_add_int(p, "R G B", DS_SHOW_RGB);
	p = obs_properties_add_list(props, "ds_unit", obs_module_text("DataAnalyser.Unit"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("DataAnalyser.Unit.10"), DS_UNIT_10);
	obs_property_list_add_int(p, obs_module_text("DataAnalyser.Unit.8"), DS_UNIT_8);
	obs_property_list_add_int(p, obs_module_text("DataAnalyser.Unit.Pct"), DS_UNIT_PCT);
	obs_properties_add_int_slider(props, "ds_cols", obs_module_text("DataAnalyser.Cols"), 1, DS_COLS_MAX, 2);
	obs_properties_add_int_slider(props, "ds_rows", obs_module_text("DataAnalyser.Rows"), 1, DS_ROWS_MAX, 2);
	return props;
}

static uint32_t ds_get_width(void *data)
{
	struct ds_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : DS_W;
}

static uint32_t ds_get_height(void *data)
{
	struct ds_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : DS_H;
}

/* Pipeline thread: the block of pixels around the pin. */
static void ds_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct ds_source *src = data;
	if (!sd->rgb_data || !sd->yuv_data || !sd->width || !sd->height)
		return;
	const int cols = src->cols, rows = src->rows;
	struct cm_source *cap = cm_is_roi(&src->cm) ? &src->cm.roi->cm : &src->cm;

	// where: the first pin of the dock, or the middle
	float u = 0.5f, v = 0.5f;
	const bool at_pin = cm_is_roi(&src->cm) && roi_first_pin(src->cm.roi, &u, &v);

	struct ds_cell *cells = src->work;
	bool raw = false, rgb_source = false, full = false;
	char status[64] = "";

	// the exact codes from the raw input filter of the source, when it has one
	obs_source_t *filter = cap->raw_filter ? obs_weak_source_get_source(cap->raw_filter) : NULL;
	if (filter && sd->ext_data) {
		struct raw_sample samples[DS_ROWS_MAX * DS_COLS_MAX];
		raw = raw_input_sample(filter, u, v, cols, rows, samples, &rgb_source, &full, status, sizeof(status));
		for (int i = 0; raw && i < cols * rows; i++) {
			for (int k = 0; k < 3; k++) {
				cells[i].ycc[k] = samples[i].code[k];
				cells[i].rgb[k] = samples[i].rgb[k];
			}
			cells[i].have_ycc = !rgb_source;
		}
	}
	obs_source_release(filter);

	if (!raw) {
		// the capture: full-range 8-bit R'G'B' and Y'CbCr, maybe scaled and cropped to the selection
		const float tw = (float)(cap->texrender_width ? cap->texrender_width : sd->width);
		const float th = (float)(cap->texrender_height ? cap->texrender_height : sd->height);
		const bool cropped = (cap->flags & CM_FLAG_ROI) && cap->x0 >= 0 && cap->x0 < cap->x1;
		const int ox = cropped ? cap->x0 : 0, oy = cropped ? cap->y0 : 0;
		const int cx = (int)(u * tw) - ox, cy = (int)(v * th) - oy;
		for (int j = 0; j < rows; j++) {
			for (int i = 0; i < cols; i++) {
				int x = cx + i - cols / 2, y = cy + j - rows / 2;
				x = x < 0 ? 0 : x >= (int)sd->width ? (int)sd->width - 1 : x;
				y = y < 0 ? 0 : y >= (int)sd->height ? (int)sd->height - 1 : y;
				const uint8_t *p = sd->rgb_data + (size_t)sd->linesize * y + (size_t)x * 4;
				const uint8_t *q = sd->yuv_data + (size_t)sd->linesize * y + (size_t)x * 4;
				struct ds_cell *c = cells + j * cols + i;
				c->rgb[0] = p[2] / 255.0f, c->rgb[1] = p[1] / 255.0f, c->rgb[2] = p[0] / 255.0f;
				// as 10-bit video range codes
				c->ycc[0] = 64.0f + 876.0f * q[1] / 255.0f;
				c->ycc[1] = 512.0f + 896.0f * (q[0] - 128.0f) / 255.0f;
				c->ycc[2] = 512.0f + 896.0f * (q[2] - 128.0f) / 255.0f;
				c->have_ycc = true;
			}
		}
		const int scale = cap->target_scale > 0 ? cap->target_scale : 1;
		if (scale > 1)
			snprintf(status, sizeof(status), "8-bit capture, every %d pixels (Scale 1 for each one)",
				 scale);
		else
			snprintf(status, sizeof(status), "8-bit capture");
	}

	pthread_mutex_lock(&src->mutex);
	memcpy(src->cells, cells, sizeof(cells[0]) * cols * rows);
	src->n_cols = cols;
	src->n_rows = rows;
	src->valid = true;
	src->raw = raw;
	src->rgb_source = rgb_source;
	src->u = u;
	src->v = v;
	src->at_pin = at_pin;
	snprintf(src->status, sizeof(src->status), "%s", status);
	pthread_mutex_unlock(&src->mutex);
}

static void fill(float x0, float y0, float x1, float y1, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(x0, y0);
		gs_vertex2f(x1, y0);
		gs_vertex2f(x0, y1);
		gs_vertex2f(x1, y1);
		gs_render_stop(GS_TRISTRIP);
	}
}

/* A value in the chosen unit: v is a 10-bit code (luma or R, G, B) or, for chroma, a 10-bit code around 512. */
static void format_value(char *buf, size_t size, float code10, bool chroma, int unit)
{
	if (unit == DS_UNIT_8)
		snprintf(buf, size, "%d", (int)lroundf(code10 / 4.0f));
	else if (unit == DS_UNIT_PCT)
		snprintf(buf, size, "%.1f", chroma ? (code10 - 512.0f) / 896.0f * 100.0f : (code10 - 64.0f) / 8.76f);
	else
		snprintf(buf, size, "%d", (int)lroundf(code10));
}

static void ds_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct ds_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	const float W = (float)DS_W, H = (float)DS_H;
	const struct text_scale ts = text_scale_from_viewport(W, H);
	fill(0.0f, 0.0f, W, H, 0xFF101013);

	static struct ds_cell cells[DS_ROWS_MAX * DS_COLS_MAX];
	pthread_mutex_lock(&src->mutex);
	const bool valid = src->valid;
	const int cols = src->n_cols, rows = src->n_rows;
	const bool raw = src->raw, rgb_source = src->rgb_source, at_pin = src->at_pin;
	const float u = src->u, v = src->v;
	char status[64];
	snprintf(status, sizeof(status), "%s", src->status);
	memcpy(cells, src->cells, sizeof(cells[0]) * (valid ? cols * rows : 0));
	pthread_mutex_unlock(&src->mutex);

	const float m = 8.0f * ts.ux, head = 26.0f * ts.uy;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s   %s %.1f%%, %.1f%%", status,
		 obs_module_text(at_pin ? "DataAnalyser.AtPin" : "DataAnalyser.AtMiddle"), u * 100.0f, v * 100.0f);
	text_draw(&ts, buf, m, 6.0f * ts.uy, 12.0f, raw ? 0xFFF0D060 : 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	if (!valid || cols <= 0 || rows <= 0) {
		text_draw(&ts, obs_module_text("DataAnalyser.Hint"), m, head + 6.0f * ts.uy, 12.0f, 0xFFD8D8DC,
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		return;
	}

	const int show = src->show == DS_SHOW_RGB || (rgb_source && src->show == DS_SHOW_YCC) ? DS_SHOW_RGB : src->show;
	const int lines = show == DS_SHOW_Y ? 1 : 3;
	const float gw = W - 2.0f * m, gh = H - head - m;
	const float cw = gw / cols, ch = gh / rows;
	const float px = fminf(14.0f, fminf(ch / lines * 0.7f / ts.uy, cw * 0.22f / ts.ux));
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < cols; i++) {
			const struct ds_cell *c = cells + j * cols + i;
			const float x0 = m + i * cw, y0 = head + j * ch;
			fill(x0 + 1.0f, y0 + 1.0f, x0 + cw - 1.0f, y0 + ch - 1.0f, 0xFF1B1B20);
			// the pixel's color as a strip on the left
			uint32_t argb = 0xFF000000;
			for (int k = 0; k < 3; k++) {
				const float t = fminf(fmaxf(c->rgb[k], 0.0f), 1.0f);
				argb |= (uint32_t)(t * 255.0f) << (16 - 8 * k);
			}
			fill(x0 + 1.0f, y0 + 1.0f, x0 + cw * 0.12f, y0 + ch - 1.0f, argb);
			if (i == cols / 2 && j == rows / 2) {
				// the pixel at the pin
				const float t = 1.5f * ts.ux;
				fill(x0, y0, x0 + cw, y0 + t, 0xFFFFD040);
				fill(x0, y0 + ch - t, x0 + cw, y0 + ch, 0xFFFFD040);
				fill(x0, y0, x0 + t, y0 + ch, 0xFFFFD040);
				fill(x0 + cw - t, y0, x0 + cw, y0 + ch, 0xFFFFD040);
			}
			static const char *names_ycc[3] = {"Y", "Cb", "Cr"}, *names_rgb[3] = {"R", "G", "B"};
			static const uint32_t colors_rgb[3] = {0xFFFF8080, 0xFF80E080, 0xFF80A0FF};
			for (int k = 0; k < lines; k++) {
				float val;
				bool chroma = false;
				if (show == DS_SHOW_RGB) {
					// R, G, B as 10-bit video range codes of the signal
					val = rgb_source && raw ? c->ycc[k] : 64.0f + 876.0f * c->rgb[k];
				} else {
					val = c->ycc[k];
					chroma = k > 0;
				}
				char num[24];
				format_value(num, sizeof(num), val, chroma, src->unit);
				// beyond the legal range in orange
				const bool out = !chroma && (val < 64.0f || val > 940.0f);
				const float yl = y0 + ch * (k + 0.5f) / lines;
				text_draw(&ts, show == DS_SHOW_RGB ? names_rgb[k] : names_ycc[k], x0 + cw * 0.16f, yl,
					  px * 0.8f, show == DS_SHOW_RGB ? colors_rgb[k] : 0xFF8D8D96, TEXT_ALIGN_LEFT,
					  TEXT_VALIGN_MIDDLE);
				text_draw(&ts, num, x0 + cw - 6.0f * ts.ux, yl, px, out ? 0xFFFFA040 : 0xFFE6E6EA,
					  TEXT_ALIGN_RIGHT, TEXT_VALIGN_MIDDLE);
			}
		}
	}
}

const struct obs_source_info colormonitor_datascope = {
	.id = ID_PREFIX "data_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = ds_get_name,
	.create = ds_create,
	.destroy = ds_destroy,
	.update = ds_update,
	.get_defaults = ds_get_defaults,
	.get_properties = ds_get_properties,
	.get_width = ds_get_width,
	.get_height = ds_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = ds_render,
	.video_tick = cm_tick,
};

/*
 * Line plot: the values of one picture line as graphs, R, G, B and luma across the width.
 * Useful to check a test pattern or one exact line of the picture.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define LP_W 512
#define LP_H 256
#define LP_MAX_SAMPLES 4096

enum lp_components_e {
	LP_RGBY = 0,
	LP_RGB = 1,
	LP_Y = 2,
};

struct lp_source
{
	struct cm_source cm;

	pthread_mutex_t mutex;
	uint8_t values[4][LP_MAX_SAMPLES]; // R, G, B, Y
	uint8_t valid[LP_MAX_SAMPLES];
	uint32_t n;

	float line; // 0..1 from the top
	int components;
	bool graticule;
};

static void lp_update(void *, obs_data_t *);
static void lp_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *lp_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LinePlot");
}

static void *lp_create(obs_data_t *settings, obs_source_t *source)
{
	struct lp_source *src = bzalloc(sizeof(struct lp_source));
	pthread_mutex_init(&src->mutex, NULL);
	src->cm.flags = CM_FLAG_CONVERT_RGB | CM_FLAG_CONVERT_YUV;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, lp_surface_cb, src);
	lp_update(src, settings);
	return src;
}

static void lp_destroy(void *data)
{
	struct lp_source *src = data;
	cm_destroy(&src->cm);
	pthread_mutex_destroy(&src->mutex);
	bfree(src);
}

static void lp_update(void *data, obs_data_t *settings)
{
	struct lp_source *src = data;
	cm_update(&src->cm, settings);
	src->line = (float)obs_data_get_double(settings, "line") / 100.0f;
	src->components = (int)obs_data_get_int(settings, "lp_components");
	src->graticule = obs_data_get_bool(settings, "show_graticule");
}

static void lp_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 1);
	obs_data_set_default_double(settings, "line", 50.0);
	obs_data_set_default_int(settings, "lp_components", LP_RGBY);
	obs_data_set_default_bool(settings, "show_graticule", true);
}

static obs_properties_t *lp_get_properties(void *data)
{
	struct lp_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_properties_add_float_slider(props, "line", obs_module_text("LinePlot.Prop.Line"), 0.0, 100.0, 0.1);
	obs_property_t *prop = obs_properties_add_list(props, "lp_components", obs_module_text("Components"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "RGB + Y", LP_RGBY);
	obs_property_list_add_int(prop, "RGB", LP_RGB);
	obs_property_list_add_int(prop, obs_module_text("Luminance"), LP_Y);
	obs_properties_add_bool(props, "show_graticule", obs_module_text("Graticule"));
	return props;
}

static uint32_t lp_get_width(void *data)
{
	struct lp_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : LP_W;
}

static uint32_t lp_get_height(void *data)
{
	struct lp_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : LP_H;
}

static void lp_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct lp_source *src = data;
	if (!sd->rgb_data || !sd->yuv_data || !sd->width || !sd->height)
		return;
	uint32_t row = (uint32_t)(src->line * (float)(sd->height - 1) + 0.5f);
	if (row >= sd->height)
		row = sd->height - 1;
	const uint8_t *rgb = sd->rgb_data + (size_t)sd->linesize * row;
	const uint8_t *yuv = sd->yuv_data + (size_t)sd->linesize * row;
	const uint32_t n = sd->width < LP_MAX_SAMPLES ? sd->width : LP_MAX_SAMPLES;

	pthread_mutex_lock(&src->mutex);
	for (uint32_t x = 0; x < n; x++) {
		src->values[0][x] = rgb[x * 4 + 2];
		src->values[1][x] = rgb[x * 4 + 1];
		src->values[2][x] = rgb[x * 4 + 0];
		src->values[3][x] = yuv[x * 4 + 1];
		src->valid[x] = rgb[x * 4 + 3] ? 1 : 0;
	}
	src->n = n;
	pthread_mutex_unlock(&src->mutex);
}

/* A polyline in batches; immediate mode holds at most 512 vertices. */
static void draw_trace(const uint8_t *v, const uint8_t *valid, uint32_t n, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	// at most LP_W points: average the samples that fall on the same column
	const uint32_t cols = n < LP_W ? n : LP_W;
	while (gs_effect_loop(solid, "Solid")) {
		uint32_t in_batch = 0;
		bool open = false;
		float px = 0.0f, py = 0.0f;
		bool have_prev = false;
		for (uint32_t c = 0; c < cols; c++) {
			const uint32_t a = c * n / cols, b = (c + 1) * n / cols;
			uint32_t sum = 0, cnt = 0;
			for (uint32_t i = a; i < b; i++) {
				if (valid[i]) {
					sum += v[i];
					cnt++;
				}
			}
			if (!cnt) {
				have_prev = false;
				continue;
			}
			const float x = (c + 0.5f) * (float)LP_W / cols;
			const float y = (float)LP_H - (float)sum / cnt * (float)LP_H / 255.0f;
			if (have_prev) {
				if (!open) {
					gs_render_start(false);
					open = true;
					in_batch = 0;
				}
				gs_vertex2f(px, py);
				gs_vertex2f(x, y);
				in_batch += 2;
				if (in_batch >= 500) {
					gs_render_stop(GS_LINES);
					open = false;
				}
			}
			px = x;
			py = y;
			have_prev = true;
		}
		if (open)
			gs_render_stop(GS_LINES);
	}
}

static void lp_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct lp_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	if (src->graticule) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x60FFBF00);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			for (int i = 0; i <= 4; i++) {
				const float y = LP_H * i / 4.0f;
				gs_vertex2f(0.0f, y);
				gs_vertex2f((float)LP_W, y);
			}
			gs_render_stop(GS_LINES);
		}
		const struct text_scale ts = text_scale_from_viewport((float)LP_W, (float)LP_H);
		const float m = 3.0f * ts.ux;
		text_draw(&ts, "100%", m, 0.0f, 10.0f, 0xC0FFD060, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		text_draw(&ts, "50%", m, LP_H * 0.5f, 10.0f, 0xC0FFD060, TEXT_ALIGN_LEFT, TEXT_VALIGN_BOTTOM);
		char buf[32];
		snprintf(buf, sizeof(buf), "line %.1f%%", src->line * 100.0f);
		text_draw(&ts, buf, LP_W - m, 0.0f, 10.0f, 0xC0FFFFFF, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
	}

	static uint8_t values[4][LP_MAX_SAMPLES];
	static uint8_t valid[LP_MAX_SAMPLES];
	uint32_t n;
	pthread_mutex_lock(&src->mutex);
	n = src->n;
	memcpy(values, src->values, sizeof(values));
	memcpy(valid, src->valid, sizeof(valid));
	pthread_mutex_unlock(&src->mutex);
	if (!n)
		return;

	if (src->components != LP_Y) {
		draw_trace(values[0], valid, n, 0xE0FF5050);
		draw_trace(values[1], valid, n, 0xE050E050);
		draw_trace(values[2], valid, n, 0xE05090FF);
	}
	if (src->components != LP_RGB)
		draw_trace(values[3], valid, n, 0xF0F0F0F0);
}

const struct obs_source_info colormonitor_lineplot = {
	.id = ID_PREFIX "lineplot_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = lp_get_name,
	.create = lp_create,
	.destroy = lp_destroy,
	.update = lp_update,
	.get_defaults = lp_get_defaults,
	.get_properties = lp_get_properties,
	.get_width = lp_get_width,
	.get_height = lp_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = lp_render,
	.video_tick = cm_tick,
};

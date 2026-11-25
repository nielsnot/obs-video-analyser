#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <graphics/matrix4.h>
#include <graphics/image-file.h>
#include "plugin-macros.generated.h"
#include "obs-convenience.h"
#include "common.h"
#include "util.h"
#include "text-render.h"
#include "draw-util.h"
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ENABLE_PROFILE
#define PROFILE_START(x) profile_start(x)
#define PROFILE_END(x) profile_end(x)
static const char *prof_render_name = "focuspeaking_render";
#else // ENABLE_PROFILE
#define PROFILE_START(x)
#define PROFILE_END(x)
#endif // ! ENABLE_PROFILE

#define DEFAULT_PEAKING_COLOR 0xFFFF5400 // ABGR
#define DEFAULT_PEAKING_THRESHOLD 0.05

/* common structure for source and filter */
struct fp_source
{
	gs_effect_t *effect;

	/* properties */
	uint32_t peaking_color;
	float peaking_threshold;
	bool actual_size;
};

struct fps_source
{
	struct cm_source cm;
	struct fp_source fp;

	// focus meter: the strength of the sharpest edges, and the best seen while turning the focus
	bool meter;
	float hold_s; // how long the best value stays
	pthread_mutex_t mutex;
	float focus;      // smoothed, 0..100
	float best;       // held best
	uint64_t best_ns; // when the best was seen
	bool has_focus;
};

struct fpf_source
{
	struct fp_source fp;
	obs_source_t *context;
};

static const char *fp_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FocusPeaking.Name");
}

static void fps_update(void *, obs_data_t *);
static void fpf_update(void *, obs_data_t *);

static void fp_init(struct fp_source *src)
{
	obs_enter_graphics();
	src->effect = create_effect_from_module_file("focuspeaking.effect");
	obs_leave_graphics();
}

/* The focus value of a picture: the mean of the strongest 1 % of the luma gradients, scaled so a crisp picture
 * reads about 60..100. It depends on the subject; what counts is the peak while turning the focus. */
static void fps_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct fps_source *src = data;
	if (!src->meter || !sd->yuv_data || sd->width < 3 || sd->height < 3)
		return;
	uint32_t hist[512] = {0};
	uint64_t n = 0;
	for (uint32_t y = 1; y + 1 < sd->height; y++) {
		const uint8_t *a = sd->yuv_data + (size_t)sd->linesize * (y - 1), *m = a + sd->linesize,
			      *c = m + sd->linesize;
		for (uint32_t x = 1; x + 1 < sd->width; x++) {
			const uint32_t o = x * 4 + 1;
			if (!m[o + 2])
				continue;
			const int g = abs((int)m[o + 4] - (int)m[o - 4]) + abs((int)c[o] - (int)a[o]);
			hist[g > 511 ? 511 : g]++;
			n++;
		}
	}
	if (!n)
		return;
	// mean of the top 1 %
	uint64_t want = n / 100 + 1, got = 0;
	double sum = 0.0;
	for (int i = 511; i >= 0 && got < want; i--) {
		const uint64_t take = hist[i] < want - got ? hist[i] : want - got;
		sum += (double)i * take;
		got += take;
	}
	const float v = fminf(100.0f, (float)(sum / got) / 1.6f);
	const uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&src->mutex);
	src->focus = src->has_focus ? src->focus * 0.6f + v * 0.4f : v;
	src->has_focus = true;
	if (src->focus >= src->best || now - src->best_ns > (uint64_t)(src->hold_s * 1e9f)) {
		src->best = src->focus;
		src->best_ns = now;
	}
	pthread_mutex_unlock(&src->mutex);
}

static void *fps_create(obs_data_t *settings, obs_source_t *source)
{
	struct fps_source *src = bzalloc(sizeof(struct fps_source));

	pthread_mutex_init(&src->mutex, NULL);
	src->cm.flags = CM_FLAG_RAW_TEXTURE;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, fps_surface_cb, src);
	fp_init(&src->fp);

	fps_update(src, settings);

	return src;
}

static void *fpf_create(obs_data_t *settings, obs_source_t *source)
{
	struct fpf_source *src = bzalloc(sizeof(struct fpf_source));

	fp_init(&src->fp);
	src->context = source;

	fpf_update(src, settings);

	return src;
}

static void fp_destroy(struct fp_source *src)
{
	UNUSED_PARAMETER(src);
}

static void fps_destroy(void *data)
{
	struct fps_source *src = data;

	cm_destroy(&src->cm);
	fp_destroy(&src->fp);
	pthread_mutex_destroy(&src->mutex);
	bfree(src);
}

static void fpf_destroy(void *data)
{
	struct fpf_source *src = data;
	fp_destroy(&src->fp);
	bfree(src);
}

static void fp_update(struct fp_source *src, obs_data_t *settings)
{
	src->peaking_color = (uint32_t)obs_data_get_int(settings, "peaking_color");
	src->peaking_threshold = (float)obs_data_get_double(settings, "peaking_threshold");
	src->actual_size = obs_data_get_bool(settings, "actual_size");
}

static void fps_update(void *data, obs_data_t *settings)
{
	struct fps_source *src = data;
	cm_update(&src->cm, settings);
	fp_update(&src->fp, settings);
	src->meter = obs_data_get_bool(settings, "fp_meter");
	src->hold_s = (float)obs_data_get_int(settings, "fp_hold");
	// the meter needs the luma on the CPU; without it only the picture is captured
	src->cm.flags = CM_FLAG_RAW_TEXTURE | (src->meter ? CM_FLAG_CONVERT_YUV : 0);
}

static void fpf_update(void *data, obs_data_t *settings)
{
	struct fpf_source *src = data;
	fp_update(&src->fp, settings);
}

static void fp_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "peaking_color", DEFAULT_PEAKING_COLOR);
	obs_data_set_default_double(settings, "peaking_threshold", DEFAULT_PEAKING_THRESHOLD);
	obs_data_set_default_bool(settings, "fp_meter", true);
	obs_data_set_default_int(settings, "fp_hold", 5);
}

static void fp_get_properties(obs_properties_t *props)
{
	obs_properties_add_color(props, "peaking_color", obs_module_text("FocusPeaking.Prop.PeakingColor"));
	obs_properties_add_float(props, "peaking_threshold", obs_module_text("FocusPeaking.Prop.PeakingThreshold"),
				 0.001, 0.1, 0.001);
	obs_properties_add_bool(props, "actual_size", obs_module_text("FocusPeaking.Prop.ActualSize"));
}

static obs_properties_t *fps_get_properties(void *data)
{
	struct fps_source *src = data;
	obs_properties_t *props;
	props = obs_properties_create();

	cm_get_properties(&src->cm, props);
	fp_get_properties(props);
	obs_properties_add_bool(props, "fp_meter", obs_module_text("FocusPeaking.Prop.Meter"));
	obs_property_t *p =
		obs_properties_add_int_slider(props, "fp_hold", obs_module_text("FocusPeaking.Prop.Hold"), 1, 30, 1);
	obs_property_int_set_suffix(p, " s");

	return props;
}

static obs_properties_t *fpf_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props;
	props = obs_properties_create();

	fp_get_properties(props);

	return props;
}

static uint32_t fps_get_width(void *data)
{
	struct fps_source *src = data;
	return cm_bypass_get_width(&src->cm);
}

static uint32_t fps_get_height(void *data)
{
	struct fps_source *src = data;
	return cm_bypass_get_height(&src->cm);
}

static uint32_t fpf_get_width(void *data)
{
	struct fpf_source *src = data;
	obs_source_t *target = obs_filter_get_target(src->context);
	return obs_source_get_base_width(target);
}

static uint32_t fpf_get_height(void *data)
{
	struct fpf_source *src = data;
	obs_source_t *target = obs_filter_get_target(src->context);
	return obs_source_get_base_height(target);
}

static const char *draw_name()
{
	return "DrawFocusPeaking";
}

static inline uint32_t swap_rb(uint32_t c)
{
	uint32_t r = c & 0xFF;
	uint32_t b = (c >> 16) & 0xFF;
	return (c & 0xFF00FF00) | (r << 16) | b;
}

static void set_actual_size_matrix(uint32_t cx, uint32_t cy)
{
	struct gs_rect rect;
	gs_get_viewport(&rect);

	float xcoe = (float)cx / (float)rect.cx;
	float ycoe = (float)cy / (float)rect.cy;
	float xoff = (float)(rect.cx - (int)cx) * 0.5f * xcoe;
	float yoff = (float)(rect.cy - (int)cy) * 0.5f * ycoe;

	struct matrix4 tr = {
		{.ptr = {xcoe, 0.0f, 0.0f, 0.0f}},
		{.ptr = {0.0f, ycoe, 0.0f, 0.0f}},
		{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
		{.ptr = {xoff, yoff, 0.0f, 1.0f}},
	};
	gs_matrix_mul(&tr);
}

static void set_effect_params(struct fp_source *src, uint32_t cx, uint32_t cy, bool mask_outside)
{
	gs_effect_t *e = src->effect;
	gs_effect_set_float(gs_effect_get_param_by_name(e, "mask_outside"), mask_outside ? 1.0f : 0.0f);

	const struct vec2 dxy = {
		.x = 1.0f / cx,
		.y = 1.0f / cy,
	};

	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "dxy"), &dxy);
	gs_effect_set_color(gs_effect_get_param_by_name(e, "peaking_color"), swap_rb(src->peaking_color));
	gs_effect_set_float(gs_effect_get_param_by_name(e, "peaking_threshold"), src->peaking_threshold);
}

static void fps_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct fps_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}

	cm_render_target(&src->cm);

	PROFILE_START(prof_render_name);

	gs_texture_t *tex = cm_bypass_get_texture(&src->cm);
	gs_effect_t *e = src->fp.effect;
	if (e && tex) {
		uint32_t cx = cm_bypass_get_width(&src->cm);
		uint32_t cy = cm_bypass_get_height(&src->cm);

		if (src->fp.actual_size) {
			gs_matrix_push();
			set_actual_size_matrix(cx, cy);
		}

		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
		// one texel of the texture sampled, which can be taller than the picture shown
		set_effect_params(&src->fp, gs_texture_get_width(tex), gs_texture_get_height(tex), true);
		const char *draw = draw_name();
		while (gs_effect_loop(e, draw))
			gs_draw_sprite_subregion(tex, 0, 0, 0, cx, cy);

		if (src->fp.actual_size)
			gs_matrix_pop();
	}

	if (src->meter && src->has_focus) {
		// a bar along the bottom: the focus now, the best of the last seconds as a tick, green when at it
		const float cx = (float)cm_bypass_get_width(&src->cm), cy = (float)cm_bypass_get_height(&src->cm);
		if (cx > 0.0f && cy > 0.0f) {
			float focus, best;
			pthread_mutex_lock(&src->mutex);
			focus = src->focus;
			best = src->best;
			pthread_mutex_unlock(&src->mutex);
			const struct text_scale ts = text_scale_from_viewport(cx, cy);
			const float h = 18.0f * ts.uy, y0 = cy - h - 6.0f * ts.uy, x0 = 8.0f * ts.ux;
			const float x1 = cx * 0.5f, w = x1 - x0;
			const bool at_best = best > 1.0f && focus >= best * 0.95f;
			du_fill_rect(x0 - 4.0f * ts.ux, y0 - 4.0f * ts.uy, cx - 8.0f * ts.ux, y0 + h + 4.0f * ts.uy,
				     0xA0000000);
			du_fill_rect(x0, y0, x0 + w * focus / 100.0f, y0 + h, at_best ? 0xFF40D060 : 0xFFE0E0E0);
			du_fill_rect(x0 + w * best / 100.0f - 1.5f * ts.ux, y0 - 2.0f * ts.uy,
				     x0 + w * best / 100.0f + 1.5f * ts.ux, y0 + h + 2.0f * ts.uy, 0xFFFFC040);
			char buf[96];
			snprintf(buf, sizeof(buf), "%s %.0f  \xe2\x96\xb2 %.0f%s",
				 obs_module_text("FocusPeaking.Meter"), focus, best,
				 at_best ? obs_module_text("FocusPeaking.Meter.Best") : "");
			text_draw(&ts, buf, x1 + 8.0f * ts.ux, y0 + h * 0.5f, 12.0f, 0xFFFFFFFF, TEXT_ALIGN_LEFT,
				  TEXT_VALIGN_MIDDLE);
		}
	}

	PROFILE_END(prof_render_name);
}

static void fpf_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct fpf_source *src = data;
	gs_effect_t *e = src->fp.effect;

	if (!e)
		return;

	if (!obs_source_process_filter_begin(src->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;

	obs_source_t *target = obs_filter_get_target(src->context);
	if (!target)
		return;

	uint32_t cx = obs_source_get_base_width(target);
	uint32_t cy = obs_source_get_base_height(target);

	set_effect_params(&src->fp, cx, cy, false);

	gs_blend_state_push();
	gs_reset_blend_state();

	if (src->fp.actual_size) {
		gs_matrix_push();
		set_actual_size_matrix(cx, cy);
	}

	const char *draw = draw_name();
	obs_source_process_filter_tech_end(src->context, e, 0, 0, draw);

	if (src->fp.actual_size)
		gs_matrix_pop();

	gs_blend_state_pop();
}

const struct obs_source_info colormonitor_focuspeaking = {
	.id = ID_PREFIX "focuspeaking_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = fp_get_name,
	.create = fps_create,
	.destroy = fps_destroy,
	.update = fps_update,
	.get_defaults = fp_get_defaults,
	.get_properties = fps_get_properties,
	.get_width = fps_get_width,
	.get_height = fps_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = fps_render,
	.video_tick = cm_tick,
};

const struct obs_source_info colormonitor_focuspeaking_filter = {
	.id = ID_PREFIX "focuspeaking_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = fp_get_name,
	.create = fpf_create,
	.destroy = fpf_destroy,
	.update = fpf_update,
	.get_defaults = fp_get_defaults,
	.get_properties = fpf_get_properties,
	.get_width = fpf_get_width,
	.get_height = fpf_get_height,
	.video_render = fpf_render,
};

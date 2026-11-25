/*
 * Color inspector: a vectorscope magnified on the quadrant of the skin tones (or a whole circle), beside a table of
 * targets: skin and the six primary and secondary colors. For every target it shows how much of the picture lies
 * near that hue, how far its hue is from the target and how saturated it is.
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
#include "vs-trace.h"

#define CI_W 448
#define CI_H 256
#define N_TARGETS 7
#define WINDOW_DEG 15.0f

struct target_stat
{
	float share; // % of the selection
	float dhue;  // average hue difference to the target, degrees
	float sat;   // average saturation, % of the outer ring
	bool valid;
};

struct ci_source
{
	struct cm_source cm;
	pthread_mutex_t mutex;
	struct vst_state trace;
	uint32_t *tex_buf;
	bool tex_new;
	gs_texture_t *tex;
	struct target_stat stats[N_TARGETS];
	int colorspace;

	int layout;
	int intensity;
	int curve, smooth;
	float persistence;
	uint32_t skin_color; // 0xBBGGRR
};

static const char *target_names[N_TARGETS] = {"Skin", "R", "Y", "G", "C", "B", "M"};
static const float target_rgb[N_TARGETS - 1][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};

static const char *ci_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ColorInspector");
}

static void ci_surface_cb(void *data, struct cm_surface_data *sd);

static void ci_update(void *data, obs_data_t *settings)
{
	struct ci_source *src = data;
	cm_update(&src->cm, settings);
	src->layout = (int)obs_data_get_int(settings, "vs_layout");
	src->intensity = (int)obs_data_get_int(settings, "intensity");
	if (src->intensity < 1)
		src->intensity = 1;
	src->curve = (int)obs_data_get_int(settings, "trace_curve");
	src->smooth = (int)obs_data_get_int(settings, "trace_smooth");
	src->persistence = (float)obs_data_get_int(settings, "persistence") / 100.0f;
	src->skin_color = (uint32_t)obs_data_get_int(settings, "graticule_skintone_color") & 0xFFFFFF;
}

static void *ci_create(obs_data_t *settings, obs_source_t *source)
{
	struct ci_source *src = bzalloc(sizeof(struct ci_source));
	pthread_mutex_init(&src->mutex, NULL);
	vst_init(&src->trace);
	src->cm.flags = CM_FLAG_CONVERT_YUV;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, ci_surface_cb, src);
	ci_update(src, settings);
	return src;
}

static void ci_destroy(void *data)
{
	struct ci_source *src = data;
	obs_enter_graphics();
	gs_texture_destroy(src->tex);
	obs_leave_graphics();
	cm_destroy(&src->cm);
	vst_free(&src->trace);
	bfree(src->tex_buf);
	pthread_mutex_destroy(&src->mutex);
	bfree(src);
}

static void ci_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "vs_layout", VST_QUADRANT);
	obs_data_set_default_int(settings, "intensity", 40);
	obs_data_set_default_int(settings, "trace_curve", VST_SOFT);
	obs_data_set_default_int(settings, "trace_smooth", 1);
	obs_data_set_default_int(settings, "graticule_skintone_color", 0x80A0D8); // BGR of a light skin tone
}

static obs_properties_t *ci_get_properties(void *data)
{
	struct ci_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_property_t *p = obs_properties_add_list(props, "vs_layout", obs_module_text("VS.Prop.Layout"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("VS.Prop.Layout.Quadrant"), VST_QUADRANT);
	obs_property_list_add_int(p, obs_module_text("VS.Prop.Layout.Semicircle"), VST_SEMICIRCLE);
	obs_property_list_add_int(p, obs_module_text("VS.Prop.Layout.Circle"), VST_CIRCLE);
	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);
	properties_add_trace(props);
	obs_properties_add_color(props, "graticule_skintone_color", obs_module_text("Skin tone color"));
	return props;
}

static uint32_t ci_get_width(void *data)
{
	struct ci_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : CI_W;
}

static uint32_t ci_get_height(void *data)
{
	struct ci_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : CI_H;
}

static void params(const struct ci_source *src, int cs, struct vst_params *p)
{
	memset(p, 0, sizeof(*p));
	p->projection = VST_CBCR;
	p->layout = src->layout;
	p->colorspace = cs;
}

/* The Cb/Cr direction (radians) of target i: the skin color or a primary or secondary. */
static float target_angle(const struct ci_source *src, int cs, int i)
{
	struct vst_params p;
	params(src, cs, &p);
	float a, r;
	if (i == 0) {
		const uint32_t c = src->skin_color;
		vst_polar_rgb(&p, (c & 0xFF) / 255.0f, (c >> 8 & 0xFF) / 255.0f, (c >> 16 & 0xFF) / 255.0f, &a, &r);
	} else {
		vst_polar_rgb(&p, target_rgb[i - 1][0], target_rgb[i - 1][1], target_rgb[i - 1][2], &a, &r);
	}
	return a;
}

static void ci_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct ci_source *src = data;
	if (!sd->yuv_data || !sd->width)
		return;
	struct vst_params p;
	params(src, sd->colorspace, &p);
	vst_accumulate(&src->trace, &p, sd->yuv_data, sd->width, sd->height, sd->linesize, src->persistence);
	vst_build(&src->trace, src->curve, (float)src->intensity, src->smooth, true);

	// the targets
	float ta[N_TARGETS];
	for (int i = 0; i < N_TARGETS; i++)
		ta[i] = target_angle(src, sd->colorspace, i);
	double dh[N_TARGETS] = {0}, sat[N_TARGETS] = {0};
	uint64_t n[N_TARGETS] = {0}, total = 0;
	const float window = WINDOW_DEG * 3.14159265f / 180.0f;
	for (uint32_t y = 0; y < sd->height; y++) {
		const uint8_t *px = sd->yuv_data + (size_t)sd->linesize * y;
		for (uint32_t x = 0; x < sd->width; x++, px += 4) {
			if (!px[3])
				continue;
			total++;
			const int du = px[0] - 128, dv = px[2] - 128;
			const float rad = sqrtf((float)(du * du + dv * dv)) / 127.5f;
			if (rad < 0.03f)
				continue; // nearly neutral: no hue
			const float a = atan2f((float)dv, (float)du);
			for (int i = 0; i < N_TARGETS; i++) {
				float d = a - ta[i];
				while (d > 3.14159265f)
					d -= 2.0f * 3.14159265f;
				while (d < -3.14159265f)
					d += 2.0f * 3.14159265f;
				if (fabsf(d) <= window) {
					dh[i] += d;
					sat[i] += rad;
					n[i]++;
				}
			}
		}
	}
	struct target_stat st[N_TARGETS];
	for (int i = 0; i < N_TARGETS; i++) {
		st[i].valid = n[i] > 0;
		st[i].share = total ? 100.0f * n[i] / total : 0.0f;
		st[i].dhue = n[i] ? (float)(dh[i] / n[i]) * 180.0f / 3.14159265f : 0.0f;
		st[i].sat = n[i] ? (float)(sat[i] / n[i]) * 100.0f : 0.0f;
	}
	pthread_mutex_lock(&src->mutex);
	if (!src->tex_buf)
		src->tex_buf = bzalloc(VST_SIZE * VST_SIZE * 4);
	memcpy(src->tex_buf, src->trace.rgba, VST_SIZE * VST_SIZE * 4);
	src->tex_new = true;
	memcpy(src->stats, st, sizeof(st));
	src->colorspace = sd->colorspace;
	pthread_mutex_unlock(&src->mutex);
}

static void ring(const struct vst_params *p, float radius, uint32_t color)
{
	const int n = 144;
	for (int i = 0; i < n; i++) {
		const float a0 = 2.0f * 3.14159265f * i / n, a1 = 2.0f * 3.14159265f * (i + 1) / n;
		float x0, y0, x1, y1;
		if (vst_map(p, a0, radius, &x0, &y0) && vst_map(p, a1, radius, &x1, &y1) && fabsf(x1 - x0) < 40.0f)
			du_line(x0, y0, x1, y1, color);
	}
}

static void ci_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct ci_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	struct target_stat st[N_TARGETS];
	int cs;
	pthread_mutex_lock(&src->mutex);
	if (src->tex_buf && src->tex_new) {
		const uint8_t *buf = (const uint8_t *)src->tex_buf;
		if (!src->tex)
			src->tex = gs_texture_create(VST_SIZE, VST_SIZE, GS_RGBA, 1, &buf, GS_DYNAMIC);
		else
			gs_texture_set_image(src->tex, buf, VST_SIZE * 4, false);
		src->tex_new = false;
	}
	memcpy(st, src->stats, sizeof(st));
	cs = src->colorspace ? src->colorspace : CM_CS_709;
	pthread_mutex_unlock(&src->mutex);

	const struct text_scale ts = text_scale_from_viewport((float)CI_W, (float)CI_H);
	du_fill_rect(0, 0, (float)CI_W, (float)CI_H, 0xFF0A0A0C);
	if (src->tex) {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), src->tex);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(src->tex, 0, VST_SIZE, VST_SIZE);
		gs_blend_state_pop();
	}

	// rings, the target directions and their windows
	struct vst_params p;
	params(src, cs, &p);
	for (int r = 1; r <= 4; r++)
		ring(&p, r / 4.0f - (r == 4 ? 0.001f : 0.0f), r == 4 ? 0x70FFFFFF : 0x30FFFFFF);
	for (int i = 0; i < N_TARGETS; i++) {
		const float a = target_angle(src, cs, i);
		float x0, y0, x1, y1;
		vst_map(&p, a, 0.0f, &x0, &y0);
		if (!vst_map(&p, a, 0.999f, &x1, &y1))
			continue;
		const uint32_t col = i == 0 ? 0xA0FFC8A0
					    : du_argb(0.3f + 0.7f * target_rgb[i - 1][0],
						      0.3f + 0.7f * target_rgb[i - 1][1],
						      0.3f + 0.7f * target_rgb[i - 1][2], 0.7f);
		du_line(x0, y0, x1, y1, col);
		text_draw(&ts, target_names[i], x1, y1, 9.0f, col | 0xFF000000, TEXT_ALIGN_CENTER, TEXT_VALIGN_BOTTOM);
	}

	// the table
	const float tx = VST_SIZE + 10.0f;
	float ty = 6.0f;
	text_draw(&ts, obs_module_text("ColorInspector.Header"), tx, ty, 10.0f, 0xC0FFFFFF, TEXT_ALIGN_LEFT,
		  TEXT_VALIGN_TOP);
	ty += 18.0f * ts.uy;
	char buf[64];
	for (int i = 0; i < N_TARGETS; i++) {
		const uint32_t chip =
			i == 0 ? du_argb((src->skin_color & 0xFF) / 255.0f, (src->skin_color >> 8 & 0xFF) / 255.0f,
					 (src->skin_color >> 16 & 0xFF) / 255.0f, 1.0f)
			       : du_argb(target_rgb[i - 1][0], target_rgb[i - 1][1], target_rgb[i - 1][2], 1.0f);
		du_fill_rect(tx, ty + 1.0f, tx + 10.0f, ty + 11.0f, chip);
		text_draw(&ts, i == 0 ? obs_module_text("ColorInspector.Skin") : target_names[i], tx + 14.0f, ty, 10.0f,
			  0xFFFFFFFF, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		// the share as a bar
		const float bx = tx + 60.0f, bw = 60.0f;
		du_fill_rect(bx, ty + 3.0f, bx + bw, ty + 9.0f, 0x30FFFFFF);
		du_fill_rect(bx, ty + 3.0f, bx + bw * fminf(st[i].share / 50.0f, 1.0f), ty + 9.0f, chip);
		if (st[i].valid && st[i].share >= 0.05f) {
			snprintf(buf, sizeof(buf), "%.1f%%", st[i].share);
			text_draw(&ts, buf, bx + bw + 4.0f, ty, 9.0f, 0xC0FFFFFF, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
			ty += 13.0f * ts.uy;
			const bool ok = fabsf(st[i].dhue) <= 5.0f;
			snprintf(buf, sizeof(buf), "%s %+.1f\xc2\xb0   sat %.0f%%",
				 obs_module_text("ColorInspector.Hue"), st[i].dhue, st[i].sat);
			text_draw(&ts, buf, tx + 14.0f, ty, 9.0f, ok ? 0xFF80F080 : 0xFFFFC040, TEXT_ALIGN_LEFT,
				  TEXT_VALIGN_TOP);
			ty += 15.0f * ts.uy;
		} else {
			text_draw(&ts, "-", bx + bw + 4.0f, ty, 9.0f, 0x80FFFFFF, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
			ty += 18.0f * ts.uy;
		}
	}
}

const struct obs_source_info colormonitor_colorinspector = {
	.id = ID_PREFIX "colorinspector_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = ci_get_name,
	.create = ci_create,
	.destroy = ci_destroy,
	.update = ci_update,
	.get_defaults = ci_get_defaults,
	.get_properties = ci_get_properties,
	.get_width = ci_get_width,
	.get_height = ci_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = ci_render,
	.video_tick = cm_tick,
};

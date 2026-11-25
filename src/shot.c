/*
 * Shot evaluator: a row of traffic lights that judge the picture by plain measurements, without AI: exposure,
 * clipped highlights, crushed blacks, contrast, focus, noise, white balance and legal colors. Every check can be
 * switched off, and every limit is in the options.
 */
#include <obs-module.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"
#include "draw-util.h"

#define SHOT_W 440
#define SHOT_H 280

enum check {
	CK_EXPOSURE,
	CK_CLIP,
	CK_CRUSH,
	CK_CONTRAST,
	CK_FOCUS,
	CK_NOISE,
	CK_WB,
	CK_GAMUT,
	N_CHECKS,
};

enum light { L_OFF, L_GREEN, L_AMBER, L_RED };

static const char *check_key[N_CHECKS] = {"shot_exposure", "shot_clip",  "shot_crush", "shot_contrast",
					  "shot_focus",    "shot_noise", "shot_wb",    "shot_gamut"};
static const char *check_name[N_CHECKS] = {"Shot.Exposure", "Shot.Clip",  "Shot.Crush", "Shot.Contrast",
					   "Shot.Focus",    "Shot.Noise", "Shot.WB",    "Shot.Gamut"};

struct result
{
	bool valid;
	float mean_luma;    // %
	float clip;         // % of the pixels at or above 100 % in luma or any channel
	float crush;        // % of the pixels at or below 0 %
	float p2, p98;      // luma percentiles, %
	float focus;        // edge strength, 0..100
	float noise;        // estimated noise sigma, % of the range
	float wb_cb, wb_cr; // cast of the near-neutral pixels, %
	float wb_share;     // share of near-neutral pixels, %
	float gamut;        // % of the pixels with a channel out of 0..100 %
};

struct shot_source
{
	struct cm_source cm;
	pthread_mutex_t mutex;
	struct result res;
	float focus_avg; // smoothed
	bool enabled[N_CHECKS];
	int expo_low, expo_high;
	float clip_max, crush_max, focus_min, noise_max, wb_max, gamut_max;
};

static const char *shot_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Shot");
}

static void shot_surface_cb(void *data, struct cm_surface_data *sd);

static void shot_update(void *data, obs_data_t *settings)
{
	struct shot_source *s = data;
	cm_update(&s->cm, settings);
	for (int i = 0; i < N_CHECKS; i++)
		s->enabled[i] = obs_data_get_bool(settings, check_key[i]);
	s->expo_low = (int)obs_data_get_int(settings, "shot_expo_low");
	s->expo_high = (int)obs_data_get_int(settings, "shot_expo_high");
	if (s->expo_high <= s->expo_low)
		s->expo_high = s->expo_low + 1;
	s->clip_max = (float)obs_data_get_double(settings, "shot_clip_max");
	s->crush_max = (float)obs_data_get_double(settings, "shot_crush_max");
	s->focus_min = (float)obs_data_get_int(settings, "shot_focus_min");
	s->noise_max = (float)obs_data_get_double(settings, "shot_noise_max");
	s->wb_max = (float)obs_data_get_double(settings, "shot_wb_max");
	s->gamut_max = (float)obs_data_get_double(settings, "shot_gamut_max");
}

static void *shot_create(obs_data_t *settings, obs_source_t *source)
{
	struct shot_source *s = bzalloc(sizeof(struct shot_source));
	pthread_mutex_init(&s->mutex, NULL);
	s->cm.flags = CM_FLAG_CONVERT_YUV;
	cm_create(&s->cm, settings, source);
	cm_request(&s->cm, shot_surface_cb, s);
	shot_update(s, settings);
	return s;
}

static void shot_destroy(void *data)
{
	struct shot_source *s = data;
	cm_destroy(&s->cm);
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static void shot_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	for (int i = 0; i < N_CHECKS; i++)
		obs_data_set_default_bool(settings, check_key[i], true);
	obs_data_set_default_int(settings, "shot_expo_low", 25);
	obs_data_set_default_int(settings, "shot_expo_high", 65);
	obs_data_set_default_double(settings, "shot_clip_max", 1.0);
	obs_data_set_default_double(settings, "shot_crush_max", 2.0);
	obs_data_set_default_int(settings, "shot_focus_min", 35);
	obs_data_set_default_double(settings, "shot_noise_max", 1.0);
	obs_data_set_default_double(settings, "shot_wb_max", 2.0);
	obs_data_set_default_double(settings, "shot_gamut_max", 1.0);
}

static obs_properties_t *shot_get_properties(void *data)
{
	struct shot_source *s = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&s->cm, props);
	for (int i = 0; i < N_CHECKS; i++)
		obs_properties_add_bool(props, check_key[i], obs_module_text(check_name[i]));
	obs_property_t *p;
	p = obs_properties_add_int_slider(props, "shot_expo_low", obs_module_text("Shot.ExpoLow"), 5, 50, 1);
	obs_property_int_set_suffix(p, " %");
	p = obs_properties_add_int_slider(props, "shot_expo_high", obs_module_text("Shot.ExpoHigh"), 40, 90, 1);
	obs_property_int_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, "shot_clip_max", obs_module_text("Shot.ClipMax"), 0.1, 10.0, 0.1);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, "shot_crush_max", obs_module_text("Shot.CrushMax"), 0.1, 20.0, 0.1);
	obs_property_float_set_suffix(p, " %");
	obs_properties_add_int_slider(props, "shot_focus_min", obs_module_text("Shot.FocusMin"), 5, 90, 1);
	p = obs_properties_add_float_slider(props, "shot_noise_max", obs_module_text("Shot.NoiseMax"), 0.2, 5.0, 0.1);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, "shot_wb_max", obs_module_text("Shot.WBMax"), 0.5, 10.0, 0.1);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, "shot_gamut_max", obs_module_text("Shot.GamutMax"), 0.1, 10.0, 0.1);
	obs_property_float_set_suffix(p, " %");
	return props;
}

static uint32_t shot_get_width(void *data)
{
	struct shot_source *s = data;
	return s->cm.bypass ? cm_bypass_get_width(&s->cm) : SHOT_W;
}

static uint32_t shot_get_height(void *data)
{
	struct shot_source *s = data;
	return s->cm.bypass ? cm_bypass_get_height(&s->cm) : SHOT_H;
}

static void shot_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct shot_source *s = data;
	if (!sd->yuv_data || sd->width < 8 || sd->height < 8)
		return;
	const int cs = sd->colorspace ? sd->colorspace : CM_CS_709;
	const float kr = cs == CM_CS_601 ? 0.299f : cm_cs_is_hdr(cs) ? 0.2627f : 0.2126f;
	const float kb = cs == CM_CS_601 ? 0.114f : cm_cs_is_hdr(cs) ? 0.0593f : 0.0722f;
	const uint32_t w = sd->width, h = sd->height, ls = sd->linesize;
	uint64_t hist[256] = {0}, n = 0, n_clip = 0, n_crush = 0, n_gamut = 0, n_neutral = 0;
	double sum_y = 0, sum_nu = 0, sum_nv = 0;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *p = sd->yuv_data + (size_t)ls * y;
		for (uint32_t x = 0; x < w; x++, p += 4) {
			if (!p[3])
				continue;
			const int Y = p[1];
			hist[Y]++;
			n++;
			sum_y += Y;
			const float yy = (Y - 16) / 219.0f, cb = (p[0] - 128) / 224.0f, cr = (p[2] - 128) / 224.0f;
			const float r = yy + 2.0f * (1.0f - kr) * cr, b = yy + 2.0f * (1.0f - kb) * cb;
			const float g = (yy - kr * r - kb * b) / (1.0f - kr - kb);
			const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
			if (Y >= 235 || mx >= 0.995f)
				n_clip++;
			if (Y <= 16)
				n_crush++;
			if (mx > 1.02f || mn < -0.02f)
				n_gamut++;
			// near neutral, not too dark or too bright: the white balance reference
			if (yy > 0.15f && yy < 0.9f && hypotf(cb, cr) < 0.06f) {
				sum_nu += cb;
				sum_nv += cr;
				n_neutral++;
			}
		}
	}
	if (!n)
		return;
	struct result r = {0};
	r.valid = true;
	r.mean_luma = ((float)(sum_y / n) - 16.0f) / 2.19f;
	r.clip = 100.0f * n_clip / n;
	r.crush = 100.0f * n_crush / n;
	r.gamut = 100.0f * n_gamut / n;
	uint64_t acc = 0;
	int p2 = -1, p98 = -1;
	for (int i = 0; i < 256; i++) {
		acc += hist[i];
		if (p2 < 0 && acc >= n * 2 / 100)
			p2 = i;
		if (p98 < 0 && acc >= n * 98 / 100)
			p98 = i;
	}
	r.p2 = (p2 - 16) / 2.19f;
	r.p98 = (p98 - 16) / 2.19f;
	r.wb_share = 100.0f * n_neutral / n;
	if (n_neutral) {
		r.wb_cb = (float)(sum_nu / n_neutral) * 100.0f;
		r.wb_cr = (float)(sum_nv / n_neutral) * 100.0f;
	}

	// focus: the strength of the strongest edges (top 1 % of the luma gradients), noise: Immerkaer's estimate from
	// the mean absolute Laplacian, both on the luma of the selection
	uint32_t ghist[256] = {0};
	uint64_t gn = 0;
	double lap_sum = 0;
	uint64_t lap_n = 0;
	for (uint32_t y = 1; y + 1 < h; y++) {
		const uint8_t *a = sd->yuv_data + (size_t)ls * (y - 1), *m = a + ls, *c = m + ls;
		for (uint32_t x = 1; x + 1 < w; x++) {
			const uint32_t o = x * 4 + 1;
			if (!m[o + 2])
				continue;
			const int gx = (int)m[o + 4] - (int)m[o - 4], gy = (int)c[o] - (int)a[o];
			int g = abs(gx) + abs(gy);
			ghist[g > 255 ? 255 : g]++;
			gn++;
			const int lap = (int)a[o - 4] - 2 * a[o] + a[o + 4] - 2 * m[o - 4] + 4 * m[o] - 2 * m[o + 4] +
					c[o - 4] - 2 * c[o] + c[o + 4];
			lap_sum += abs(lap);
			lap_n++;
		}
	}
	if (gn) {
		uint64_t top = 0;
		int g99 = 255;
		for (int i = 255; i >= 0; i--) {
			top += ghist[i];
			if (top >= gn / 100) {
				g99 = i;
				break;
			}
		}
		// a hard edge across the full range gives about 219 per two pixels; scale so that a crisp picture at
		// half resolution reads around 60..100
		r.focus = fminf(100.0f, g99 / 1.2f);
	}
	if (lap_n)
		r.noise = (float)(sqrt(3.14159265 / 2.0) / 6.0 * lap_sum / lap_n) / 2.19f;

	pthread_mutex_lock(&s->mutex);
	s->focus_avg = s->res.valid ? s->focus_avg * 0.7f + r.focus * 0.3f : r.focus;
	s->res = r;
	pthread_mutex_unlock(&s->mutex);
}

static enum light grade_max(float v, float max)
{
	return v <= max ? L_GREEN : v <= 3.0f * max ? L_AMBER : L_RED;
}

static void shot_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct shot_source *s = data;
	if (s->cm.bypass) {
		cm_bypass_render(&s->cm);
		return;
	}
	cm_render_target(&s->cm);
	struct result r;
	float focus;
	pthread_mutex_lock(&s->mutex);
	r = s->res;
	focus = s->focus_avg;
	pthread_mutex_unlock(&s->mutex);

	const struct text_scale ts = text_scale_from_viewport((float)SHOT_W, (float)SHOT_H);
	du_fill_rect(0, 0, SHOT_W, SHOT_H, 0xFF0C0C0E);
	if (!r.valid) {
		text_draw(&ts, obs_module_text("Shot.NoPicture"), SHOT_W / 2.0f, SHOT_H / 2.0f, 12.0f, 0x90FFFFFF,
			  TEXT_ALIGN_CENTER, TEXT_VALIGN_MIDDLE);
		return;
	}

	enum light lights[N_CHECKS];
	char values[N_CHECKS][64];
	const char *hints[N_CHECKS];
	// exposure
	lights[CK_EXPOSURE] = r.mean_luma >= s->expo_low && r.mean_luma <= s->expo_high             ? L_GREEN
			      : r.mean_luma >= s->expo_low - 10 && r.mean_luma <= s->expo_high + 10 ? L_AMBER
												    : L_RED;
	snprintf(values[CK_EXPOSURE], 64, "%.0f%%", r.mean_luma);
	hints[CK_EXPOSURE] = r.mean_luma < s->expo_low    ? obs_module_text("Shot.Hint.Dark")
			     : r.mean_luma > s->expo_high ? obs_module_text("Shot.Hint.Bright")
							  : obs_module_text("Shot.Hint.Ok");
	lights[CK_CLIP] = grade_max(r.clip, s->clip_max);
	snprintf(values[CK_CLIP], 64, "%.1f%%", r.clip);
	hints[CK_CLIP] = lights[CK_CLIP] == L_GREEN ? obs_module_text("Shot.Hint.Ok")
						    : obs_module_text("Shot.Hint.Clip");
	lights[CK_CRUSH] = grade_max(r.crush, s->crush_max);
	snprintf(values[CK_CRUSH], 64, "%.1f%%", r.crush);
	hints[CK_CRUSH] = lights[CK_CRUSH] == L_GREEN ? obs_module_text("Shot.Hint.Ok")
						      : obs_module_text("Shot.Hint.Crush");
	const float span = r.p98 - r.p2;
	lights[CK_CONTRAST] = span >= 50.0f ? L_GREEN : span >= 30.0f ? L_AMBER : L_RED;
	snprintf(values[CK_CONTRAST], 64, "%.0f..%.0f%%", r.p2, r.p98);
	hints[CK_CONTRAST] = lights[CK_CONTRAST] == L_GREEN ? obs_module_text("Shot.Hint.Ok")
							    : obs_module_text("Shot.Hint.Flat");
	lights[CK_FOCUS] = focus >= s->focus_min ? L_GREEN : focus >= s->focus_min * 0.6f ? L_AMBER : L_RED;
	snprintf(values[CK_FOCUS], 64, "%.0f", focus);
	hints[CK_FOCUS] = lights[CK_FOCUS] == L_GREEN ? obs_module_text("Shot.Hint.Ok")
						      : obs_module_text("Shot.Hint.Soft");
	lights[CK_NOISE] = r.noise <= s->noise_max ? L_GREEN : r.noise <= 2.0f * s->noise_max ? L_AMBER : L_RED;
	snprintf(values[CK_NOISE], 64, "%.2f%%", r.noise);
	hints[CK_NOISE] = lights[CK_NOISE] == L_GREEN ? obs_module_text("Shot.Hint.Ok")
						      : obs_module_text("Shot.Hint.Noise");
	const float cast = hypotf(r.wb_cb, r.wb_cr);
	if (r.wb_share < 2.0f) {
		lights[CK_WB] = L_OFF; // too little neutral to judge
		snprintf(values[CK_WB], 64, "-");
		hints[CK_WB] = obs_module_text("Shot.Hint.NoNeutral");
	} else {
		lights[CK_WB] = cast <= s->wb_max ? L_GREEN : cast <= 2.0f * s->wb_max ? L_AMBER : L_RED;
		snprintf(values[CK_WB], 64, "%.1f%%", cast);
		hints[CK_WB] = lights[CK_WB] == L_GREEN     ? obs_module_text("Shot.Hint.Ok")
			       : r.wb_cb > 0 && r.wb_cr < 0 ? obs_module_text("Shot.Hint.Blue")
			       : r.wb_cr > 0 && r.wb_cb < 0 ? obs_module_text("Shot.Hint.Warm")
			       : r.wb_cr > 0                ? obs_module_text("Shot.Hint.Magenta")
							    : obs_module_text("Shot.Hint.Green");
	}
	lights[CK_GAMUT] = grade_max(r.gamut, s->gamut_max);
	snprintf(values[CK_GAMUT], 64, "%.1f%%", r.gamut);
	hints[CK_GAMUT] = lights[CK_GAMUT] == L_GREEN ? obs_module_text("Shot.Hint.Ok")
						      : obs_module_text("Shot.Hint.Gamut");

	const uint32_t light_col[4] = {0xFF505050, cm_ok_color(), cm_warn_color(), cm_bad_color()};
	enum light worst = L_GREEN;
	int shown = 0;
	for (int i = 0; i < N_CHECKS; i++)
		if (s->enabled[i]) {
			shown++;
			if (lights[i] > worst)
				worst = lights[i];
		}

	// the verdict on top
	static const char *verdict_key[4] = {"Shot.Verdict.Ok", "Shot.Verdict.Ok", "Shot.Verdict.Check",
					     "Shot.Verdict.Fix"};
	du_circle(22.0f, 20.0f, 9.0f, light_col[worst], true);
	text_draw(&ts, obs_module_text(verdict_key[worst]), 40.0f, 20.0f, 14.0f, 0xFFFFFFFF, TEXT_ALIGN_LEFT,
		  TEXT_VALIGN_MIDDLE);
	du_line(8.0f, 38.0f, SHOT_W - 8.0f, 38.0f, 0x40FFFFFF);

	if (!shown)
		return;
	const float row_h = fminf(28.0f, (SHOT_H - 46.0f) / shown);
	float y = 46.0f + row_h / 2.0f;
	for (int i = 0; i < N_CHECKS; i++) {
		if (!s->enabled[i])
			continue;
		du_circle(22.0f, y, 7.0f, light_col[lights[i]], true);
		text_draw(&ts, obs_module_text(check_name[i]), 38.0f, y, 11.0f, 0xFFFFFFFF, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_MIDDLE);
		text_draw(&ts, values[i], 210.0f, y, 11.0f, 0xFFE0E0E0, TEXT_ALIGN_RIGHT, TEXT_VALIGN_MIDDLE);
		text_draw(&ts, hints[i], 222.0f, y, 9.0f, lights[i] <= L_GREEN ? 0x90FFFFFF : 0xE0FFFFFF,
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
		y += row_h;
	}
}

const struct obs_source_info colormonitor_shot = {
	.id = ID_PREFIX "shot_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = shot_get_name,
	.create = shot_create,
	.destroy = shot_destroy,
	.update = shot_update,
	.get_defaults = shot_get_defaults,
	.get_properties = shot_get_properties,
	.get_width = shot_get_width,
	.get_height = shot_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = shot_render,
	.video_tick = cm_tick,
};

/*
 * Skin tone: finds the skin in the picture with a hue / saturation / luma window (HSL, the same as the qualifier of
 * the dock selection) and shows it over the picture: yellow within the tolerance of the target hue, magenta when
 * redder, green when more yellow; everything else is dimmed grey. Beside it the figures: how much of the picture is
 * skin, its average hue against the target, saturation and luma.
 *
 * "Set from selection" centers the window on the skin in the dock selection (draw a rectangle on a face, or follow
 * the faces); "Use as selection" puts the window into the dock's qualifier, so every scope measures only the skin.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define ST_W 1200
#define ST_H 520

struct st_stats
{
	float share;          // of the selected pixels that are skin
	float hue, sat, luma; // averages of the skin (degrees, 0..1, 0..1)
	float on, low, high;  // shares of the skin within, below and above the tolerance
	bool valid;
};

struct st_source
{
	struct cm_source cm;
	pthread_mutex_t mutex;

	// the overlay picture (pipeline thread writes, render uploads)
	uint8_t *img;
	uint32_t img_w, img_h;
	bool img_new;
	gs_texture_t *tex;
	uint32_t tex_w, tex_h;
	struct st_stats stats;

	// settings
	float hue, width, tol;      // degrees: target hue, width of the window, tolerance for "on target"
	float sat_min, sat_max;     // 0..1
	float luma_min, luma_max;   // 0..1
	volatile bool want_find;    // set the window from the selection (pipeline thread)
	volatile bool want_apply;   // put the window into the dock's qualifier (tick)
	volatile bool store_window; // write the window found into the settings (tick)
	float found[6];
};

static void st_update(void *, obs_data_t *);
static void st_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *st_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SkinTone");
}

static void st_cmd(void *data, calldata_t *cd)
{
	struct st_source *src = data;
	const char *cmd = calldata_string(cd, "cmd");
	if (cmd && !strcmp(cmd, "find"))
		src->want_find = true;
	else if (cmd && !strcmp(cmd, "apply"))
		src->want_apply = true;
}

static void *st_create(obs_data_t *settings, obs_source_t *source)
{
	struct st_source *src = bzalloc(sizeof(struct st_source));
	pthread_mutex_init(&src->mutex, NULL);
	src->cm.flags = CM_FLAG_CONVERT_RGB | CM_FLAG_CONVERT_YUV;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, st_surface_cb, src);
	proc_handler_add(obs_source_get_proc_handler(source), "void va_skin(in string cmd)", st_cmd, src);
	st_update(src, settings);
	return src;
}

static void st_destroy(void *data)
{
	struct st_source *src = data;
	obs_enter_graphics();
	gs_texture_destroy(src->tex);
	obs_leave_graphics();
	cm_destroy(&src->cm);
	pthread_mutex_destroy(&src->mutex);
	bfree(src->img);
	bfree(src);
}

static void st_update(void *data, obs_data_t *settings)
{
	struct st_source *src = data;
	cm_update(&src->cm, settings);
	src->hue = (float)obs_data_get_double(settings, "st_hue");
	src->width = (float)obs_data_get_double(settings, "st_width");
	src->tol = (float)obs_data_get_double(settings, "st_tol");
	src->sat_min = (float)obs_data_get_double(settings, "st_sat_min") / 100.0f;
	src->sat_max = (float)obs_data_get_double(settings, "st_sat_max") / 100.0f;
	src->luma_min = (float)obs_data_get_double(settings, "st_luma_min") / 100.0f;
	src->luma_max = (float)obs_data_get_double(settings, "st_luma_max") / 100.0f;
}

static void st_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_double(settings, "st_hue", 22.0);
	obs_data_set_default_double(settings, "st_width", 30.0);
	obs_data_set_default_double(settings, "st_tol", 5.0);
	obs_data_set_default_double(settings, "st_sat_min", 15.0);
	obs_data_set_default_double(settings, "st_sat_max", 75.0);
	obs_data_set_default_double(settings, "st_luma_min", 10.0);
	obs_data_set_default_double(settings, "st_luma_max", 90.0);
}

static bool find_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct st_source *)data)->want_find = true;
	return false;
}

static bool apply_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct st_source *)data)->want_apply = true;
	return false;
}

static obs_properties_t *st_get_properties(void *data)
{
	struct st_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_properties_add_float_slider(props, "st_hue", obs_module_text("SkinTone.Hue"), 0.0, 60.0, 0.5);
	obs_properties_add_float_slider(props, "st_width", obs_module_text("SkinTone.Width"), 4.0, 90.0, 1.0);
	obs_properties_add_float_slider(props, "st_tol", obs_module_text("SkinTone.Tol"), 1.0, 20.0, 0.5);
	obs_properties_add_float_slider(props, "st_sat_min", obs_module_text("SkinTone.SatMin"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "st_sat_max", obs_module_text("SkinTone.SatMax"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "st_luma_min", obs_module_text("SkinTone.LumaMin"), 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, "st_luma_max", obs_module_text("SkinTone.LumaMax"), 0.0, 100.0, 1.0);
	obs_properties_add_button2(props, "st_find", obs_module_text("SkinTone.Find"), find_clicked, src);
	obs_properties_add_button2(props, "st_apply", obs_module_text("SkinTone.Apply"), apply_clicked, src);
	return props;
}

static uint32_t st_get_width(void *data)
{
	struct st_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : ST_W;
}

static uint32_t st_get_height(void *data)
{
	struct st_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : ST_H;
}

/* HSL hue (degrees, red 0, yellow 60), saturation and lightness of 8-bit R'G'B'; luma with Rec. 709 weights. */
static void hsl(int r, int g, int b, float *h, float *s, float *luma)
{
	const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	const float c = (mx - mn) / 255.0f, l = (mx + mn) / 510.0f;
	const float denom = 1.0f - fabsf(2.0f * l - 1.0f);
	*s = denom > 1e-5f ? c / denom : 0.0f;
	*luma = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
	if (mx == mn) {
		*h = -1.0f;
		return;
	}
	float hh;
	if (mx == r)
		hh = (float)(g - b) / (mx - mn);
	else if (mx == g)
		hh = (float)(b - r) / (mx - mn) + 2.0f;
	else
		hh = (float)(r - g) / (mx - mn) + 4.0f;
	hh *= 60.0f;
	*h = hh < 0.0f ? hh + 360.0f : hh;
}

/* The signed difference a - b in degrees, -180..180. */
static float hue_diff(float a, float b)
{
	float d = a - b;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return d;
}

static int cmp_float(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

static void st_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct st_source *src = data;
	if (!sd->rgb_data || !sd->width || !sd->height)
		return;
	const uint32_t w = sd->width, h = sd->height;

	// set from the selection: the median hue of the colored pixels, and the spread of saturation and luma
	if (src->want_find) {
		src->want_find = false;
		const size_t max_n = (size_t)w * h / 4 + 1;
		float *hues = bmalloc(sizeof(float) * max_n), *sats = bmalloc(sizeof(float) * max_n);
		float *lumas = bmalloc(sizeof(float) * max_n);
		size_t n = 0;
		for (uint32_t y = 0; y < h; y += 2) {
			const uint8_t *p = sd->rgb_data + (size_t)sd->linesize * y;
			for (uint32_t x = 0; x < w && n < max_n; x += 2) {
				const uint8_t *q = p + x * 4;
				if (!q[3])
					continue;
				float hh, ss, ll;
				hsl(q[2], q[1], q[0], &hh, &ss, &ll);
				// skin is warm, not grey, not black or white
				if (hh < 0.0f || ss < 0.08f || ll < 0.05f || ll > 0.95f)
					continue;
				if (hh > 90.0f && hh < 300.0f)
					continue;
				hues[n] = hh > 180.0f ? hh - 360.0f : hh;
				sats[n] = ss;
				lumas[n] = ll;
				n++;
			}
		}
		if (n > 20) {
			qsort(hues, n, sizeof(float), cmp_float);
			qsort(sats, n, sizeof(float), cmp_float);
			qsort(lumas, n, sizeof(float), cmp_float);
			const float hc = hues[n / 2];
			const float hw = fmaxf(10.0f, 2.0f * fmaxf(hues[n * 9 / 10] - hc, hc - hues[n / 10]));
			src->found[0] = hc < 0.0f ? hc + 360.0f : hc;
			src->found[1] = fminf(hw, 90.0f);
			src->found[2] = fmaxf(0.0f, sats[n / 20] - 0.05f) * 100.0f;
			src->found[3] = fminf(1.0f, sats[n * 19 / 20] + 0.05f) * 100.0f;
			src->found[4] = fmaxf(0.0f, lumas[n / 20] - 0.05f) * 100.0f;
			src->found[5] = fminf(1.0f, lumas[n * 19 / 20] + 0.05f) * 100.0f;
			src->store_window = true;
		}
		bfree(hues);
		bfree(sats);
		bfree(lumas);
	}

	pthread_mutex_lock(&src->mutex);
	if (src->img_w != w || src->img_h != h || !src->img) {
		bfree(src->img);
		src->img = bmalloc((size_t)w * h * 4);
		src->img_w = w;
		src->img_h = h;
	}
	const float half = src->width * 0.5f;
	uint64_t n_sel = 0, n_skin = 0, n_on = 0, n_low = 0, n_high = 0;
	double sum_dh = 0.0, sum_s = 0.0, sum_l = 0.0;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *p = sd->rgb_data + (size_t)sd->linesize * y;
		uint8_t *o = src->img + (size_t)w * 4 * y;
		for (uint32_t x = 0; x < w; x++, p += 4, o += 4) {
			const int r = p[2], g = p[1], b = p[0];
			float hh, ss, ll;
			hsl(r, g, b, &hh, &ss, &ll);
			// dimmed grey for everything that is not skin (and darker still outside the selection)
			const uint8_t grey = (uint8_t)(ll * (p[3] ? 110.0f : 50.0f));
			o[0] = o[1] = o[2] = grey;
			o[3] = 255;
			if (!p[3])
				continue;
			n_sel++;
			const float dh = hh < 0.0f ? 999.0f : hue_diff(hh, src->hue);
			if (fabsf(dh) > half || ss < src->sat_min || ss > src->sat_max || ll < src->luma_min ||
			    ll > src->luma_max)
				continue;
			n_skin++;
			sum_dh += dh;
			sum_s += ss;
			sum_l += ll;
			// yellow on target, magenta redder, green more yellow; brightness follows the pixel
			const float k = 0.45f + 0.55f * ll;
			if (fabsf(dh) <= src->tol) {
				n_on++;
				o[2] = (uint8_t)(255 * k), o[1] = (uint8_t)(230 * k), o[0] = (uint8_t)(40 * k);
			} else if (dh < 0.0f) {
				n_low++;
				o[2] = (uint8_t)(230 * k), o[1] = (uint8_t)(60 * k), o[0] = (uint8_t)(220 * k);
			} else {
				n_high++;
				o[2] = (uint8_t)(70 * k), o[1] = (uint8_t)(220 * k), o[0] = (uint8_t)(80 * k);
			}
		}
	}
	struct st_stats *st = &src->stats;
	st->valid = n_sel > 0;
	st->share = n_sel ? (float)n_skin / n_sel : 0.0f;
	st->hue = n_skin ? src->hue + (float)(sum_dh / n_skin) : 0.0f;
	st->sat = n_skin ? (float)(sum_s / n_skin) : 0.0f;
	st->luma = n_skin ? (float)(sum_l / n_skin) : 0.0f;
	st->on = n_skin ? (float)n_on / n_skin : 0.0f;
	st->low = n_skin ? (float)n_low / n_skin : 0.0f;
	st->high = n_skin ? (float)n_high / n_skin : 0.0f;
	src->img_new = true;
	pthread_mutex_unlock(&src->mutex);
}

static void st_tick(void *data, float seconds)
{
	struct st_source *src = data;
	cm_tick(&src->cm, seconds);
	if (src->store_window) {
		src->store_window = false;
		obs_data_t *d = obs_data_create();
		obs_data_set_double(d, "st_hue", src->found[0]);
		obs_data_set_double(d, "st_width", src->found[1]);
		obs_data_set_double(d, "st_sat_min", src->found[2]);
		obs_data_set_double(d, "st_sat_max", src->found[3]);
		obs_data_set_double(d, "st_luma_min", src->found[4]);
		obs_data_set_double(d, "st_luma_max", src->found[5]);
		obs_source_update(src->cm.self, d);
		obs_data_release(d);
	}
	if (src->want_apply) {
		src->want_apply = false;
		// the dock's qualifier uses the same HSL window
		if (src->cm.roi_src) {
			obs_data_t *d = obs_data_create();
			obs_data_set_bool(d, "qual_enable", true);
			obs_data_set_int(d, "qual_hue_center", (int)lroundf(src->hue));
			obs_data_set_int(d, "qual_hue_width", (int)lroundf(src->width));
			obs_data_set_int(d, "qual_sat_min", (int)lroundf(src->sat_min * 100.0f));
			obs_data_set_int(d, "qual_sat_max", (int)lroundf(src->sat_max * 100.0f));
			obs_data_set_int(d, "qual_luma_min", (int)lroundf(src->luma_min * 100.0f));
			obs_data_set_int(d, "qual_luma_max", (int)lroundf(src->luma_max * 100.0f));
			obs_source_update(src->cm.roi_src, d);
			obs_data_release(d);
		}
	}
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

static void st_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct st_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	const float W = (float)ST_W, H = (float)ST_H;
	const struct text_scale ts = text_scale_from_viewport(W, H);
	fill(0.0f, 0.0f, W, H, 0xFF101013);

	struct st_stats st;
	uint32_t iw = 0, ih = 0;
	pthread_mutex_lock(&src->mutex);
	st = src->stats;
	if (src->img && (src->img_new || !src->tex)) {
		if (src->tex && (src->tex_w != src->img_w || src->tex_h != src->img_h)) {
			gs_texture_destroy(src->tex);
			src->tex = NULL;
		}
		if (!src->tex) {
			const uint8_t *d = src->img;
			src->tex = gs_texture_create(src->img_w, src->img_h, GS_BGRA, 1, &d, GS_DYNAMIC);
			src->tex_w = src->img_w;
			src->tex_h = src->img_h;
		} else {
			gs_texture_set_image(src->tex, src->img, src->img_w * 4, false);
		}
		src->img_new = false;
	}
	iw = src->tex_w;
	ih = src->tex_h;
	pthread_mutex_unlock(&src->mutex);

	// the picture on the left, with its aspect
	const float m = 8.0f, pw = 760.0f, ph = H - 2.0f * m;
	float dw = pw, dh = ph;
	if (iw && ih) {
		if (pw / ph > (float)iw / ih)
			dw = ph * iw / ih;
		else
			dh = pw * ih / iw;
	}
	const float px0 = m + (pw - dw) * 0.5f, py0 = m + (ph - dh) * 0.5f;
	if (src->tex) {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), src->tex);
		gs_matrix_push();
		gs_matrix_translate3f(px0, py0, 0.0f);
		gs_matrix_scale3f(dw / iw, dh / ih, 1.0f);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(src->tex, 0, iw, ih);
		gs_matrix_pop();
	}

	// the figures on the right
	const float x = m + pw + 16.0f, tw = W - x - m;
	float y = m;
	const float px = 13.0f, lh = (px + 7.0f) * ts.uy;
	char buf[160];
	text_draw(&ts, obs_module_text("SkinTone"), x, y, px + 2.0f, 0xFFE6E6EA, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	y += lh * 1.3f;
	// the key
	static const struct
	{
		uint32_t color;
		const char *key;
	} key[3] = {{0xFFFFE628, "SkinTone.On"}, {0xFFE63CDC, "SkinTone.Low"}, {0xFF46DC50, "SkinTone.High"}};
	for (int k = 0; k < 3; k++) {
		fill(x, y + 3.0f * ts.uy, x + 12.0f * ts.ux, y + 15.0f * ts.uy, key[k].color);
		text_draw(&ts, obs_module_text(key[k].key), x + 18.0f * ts.ux, y, px - 1.0f, 0xFFB8B8C0,
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		y += lh;
	}
	y += lh * 0.4f;
	if (!st.valid) {
		text_draw(&ts, obs_module_text("SkinTone.NoSignal"), x, y, px, 0xFF8D8D96, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_TOP);
		return;
	}
	struct
	{
		const char *label;
		char value[48];
	} rows[7];
	int n = 0;
	rows[n].label = obs_module_text("SkinTone.Share");
	snprintf(rows[n++].value, sizeof(rows[0].value), "%.1f%%", st.share * 100.0f);
	rows[n].label = obs_module_text("SkinTone.HueRow");
	snprintf(rows[n++].value, sizeof(rows[0].value), "%.1f\xc2\xb0 (%+.1f\xc2\xb0)", st.hue,
		 hue_diff(st.hue, src->hue));
	rows[n].label = obs_module_text("SkinTone.SatRow");
	snprintf(rows[n++].value, sizeof(rows[0].value), "%.0f%%", st.sat * 100.0f);
	rows[n].label = obs_module_text("SkinTone.LumaRow");
	snprintf(rows[n++].value, sizeof(rows[0].value), "%.0f%%", st.luma * 100.0f);
	rows[n].label = obs_module_text("SkinTone.On");
	snprintf(rows[n++].value, sizeof(rows[0].value), "%.0f%%", st.on * 100.0f);
	rows[n].label = obs_module_text("SkinTone.Low");
	snprintf(rows[n++].value, sizeof(rows[0].value), "%.0f%%", st.low * 100.0f);
	rows[n].label = obs_module_text("SkinTone.High");
	snprintf(rows[n++].value, sizeof(rows[0].value), "%.0f%%", st.high * 100.0f);
	for (int k = 0; k < n; k++) {
		if (k % 2 == 0)
			fill(x, y - 2.0f * ts.uy, x + tw, y + lh - 3.0f * ts.uy, 0xFF17171B);
		text_draw(&ts, rows[k].label, x + 4.0f * ts.ux, y, px, 0xFFB8B8C0, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		text_draw(&ts, rows[k].value, x + tw - 4.0f * ts.ux, y, px, 0xFFE6E6EA, TEXT_ALIGN_RIGHT,
			  TEXT_VALIGN_TOP);
		y += lh;
	}
	y += lh * 0.4f;
	snprintf(buf, sizeof(buf), "%s %.0f\xc2\xb0 \xc2\xb1%.0f\xc2\xb0, sat %.0f-%.0f%%, luma %.0f-%.0f%%",
		 obs_module_text("SkinTone.Window"), src->hue, src->width * 0.5f, src->sat_min * 100.0f,
		 src->sat_max * 100.0f, src->luma_min * 100.0f, src->luma_max * 100.0f);
	text_draw(&ts, buf, x, y, px - 2.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
}

const struct obs_source_info colormonitor_skintone = {
	.id = ID_PREFIX "skintone_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = st_get_name,
	.create = st_create,
	.destroy = st_destroy,
	.update = st_update,
	.get_defaults = st_get_defaults,
	.get_properties = st_get_properties,
	.get_width = st_get_width,
	.get_height = st_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = st_render,
	.video_tick = st_tick,
};

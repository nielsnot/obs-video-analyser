/*
 * Color balance plot: the average color cast of the shadows, the mid tones and the highlights as three points on a
 * magnified Cb/Cr plane. A neutral camera has all three in the middle; a camera that goes blue in the shadows and
 * yellow in the highlights shows a line through the middle. A short trail shows how the points move.
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

#define BAL_W 400
#define BAL_H 256
#define PLOT 256
#define TRAIL 40

struct band
{
	float cb, cr;  // average cast in % (0 is neutral)
	float luma;    // average luma in %
	float share;   // share of the selection in %
	float r, g, b; // average color, 0..1
	bool valid;
};

struct bal_source
{
	struct cm_source cm;
	pthread_mutex_t mutex;
	struct band bands[3];
	float trail[3][TRAIL][2];
	int trail_n, trail_pos;
	int colorspace;

	int range; // % at the outer ring
	int split_low, split_high;
	bool show_trail;
	float tolerance; // % within which a band counts as neutral
};

static const char *bal_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Balance");
}

static void bal_surface_cb(void *data, struct cm_surface_data *sd);

static void bal_update(void *data, obs_data_t *settings)
{
	struct bal_source *src = data;
	cm_update(&src->cm, settings);
	src->range = (int)obs_data_get_int(settings, "bal_range");
	if (src->range < 1)
		src->range = 10;
	src->split_low = (int)obs_data_get_int(settings, "bal_low");
	src->split_high = (int)obs_data_get_int(settings, "bal_high");
	if (src->split_high <= src->split_low)
		src->split_high = src->split_low + 1;
	src->show_trail = obs_data_get_bool(settings, "bal_trail");
	src->tolerance = (float)obs_data_get_double(settings, "bal_tol");
}

static void *bal_create(obs_data_t *settings, obs_source_t *source)
{
	struct bal_source *src = bzalloc(sizeof(struct bal_source));
	pthread_mutex_init(&src->mutex, NULL);
	src->cm.flags = CM_FLAG_CONVERT_YUV;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, bal_surface_cb, src);
	bal_update(src, settings);
	return src;
}

static void bal_destroy(void *data)
{
	struct bal_source *src = data;
	cm_destroy(&src->cm);
	pthread_mutex_destroy(&src->mutex);
	bfree(src);
}

static void bal_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "bal_range", 10);
	obs_data_set_default_int(settings, "bal_low", 33);
	obs_data_set_default_int(settings, "bal_high", 66);
	obs_data_set_default_bool(settings, "bal_trail", true);
	obs_data_set_default_double(settings, "bal_tol", 1.0);
}

static obs_properties_t *bal_get_properties(void *data)
{
	struct bal_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_property_t *p = obs_properties_add_list(props, "bal_range", obs_module_text("Balance.Range"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "\xc2\xb1 5 %", 5);
	obs_property_list_add_int(p, "\xc2\xb1 10 %", 10);
	obs_property_list_add_int(p, "\xc2\xb1 20 %", 20);
	obs_property_list_add_int(p, "\xc2\xb1 40 %", 40);
	p = obs_properties_add_int_slider(props, "bal_low", obs_module_text("Balance.Low"), 5, 60, 1);
	obs_property_int_set_suffix(p, " %");
	p = obs_properties_add_int_slider(props, "bal_high", obs_module_text("Balance.High"), 40, 95, 1);
	obs_property_int_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, "bal_tol", obs_module_text("Balance.Tolerance"), 0.2, 5.0, 0.1);
	obs_property_float_set_suffix(p, " %");
	obs_properties_add_bool(props, "bal_trail", obs_module_text("Balance.Trail"));
	return props;
}

static uint32_t bal_get_width(void *data)
{
	struct bal_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : BAL_W;
}

static uint32_t bal_get_height(void *data)
{
	struct bal_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : BAL_H;
}

static void matrix(int cs, float *kr, float *kb)
{
	*kr = cs == CM_CS_601 ? 0.299f : cm_cs_is_hdr(cs) ? 0.2627f : 0.2126f;
	*kb = cs == CM_CS_601 ? 0.114f : cm_cs_is_hdr(cs) ? 0.0593f : 0.0722f;
}

static void bal_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct bal_source *src = data;
	if (!sd->yuv_data || !sd->width)
		return;
	double su[3] = {0}, sv[3] = {0}, sy[3] = {0};
	uint64_t n[3] = {0}, total = 0;
	const int lo = 16 + 219 * src->split_low / 100, hi = 16 + 219 * src->split_high / 100;
	for (uint32_t y = 0; y < sd->height; y++) {
		const uint8_t *p = sd->yuv_data + (size_t)sd->linesize * y;
		for (uint32_t x = 0; x < sd->width; x++, p += 4) {
			if (!p[3])
				continue;
			const int luma = p[1];
			const int k = luma < lo ? 0 : luma < hi ? 1 : 2;
			su[k] += p[0];
			sv[k] += p[2];
			sy[k] += luma;
			n[k]++;
			total++;
		}
	}
	float kr, kb;
	matrix(sd->colorspace, &kr, &kb);
	struct band b[3];
	for (int k = 0; k < 3; k++) {
		b[k].valid = n[k] > 0;
		if (!b[k].valid)
			continue;
		const float u = (float)(su[k] / n[k]) - 128.0f, v = (float)(sv[k] / n[k]) - 128.0f;
		b[k].cb = u / 224.0f * 100.0f;
		b[k].cr = v / 224.0f * 100.0f;
		b[k].luma = ((float)(sy[k] / n[k]) - 16.0f) / 219.0f * 100.0f;
		b[k].share = total ? 100.0f * n[k] / total : 0.0f;
		const float yy = b[k].luma / 100.0f, cb = u / 224.0f, cr = v / 224.0f;
		b[k].r = yy + 2.0f * (1.0f - kr) * cr;
		b[k].b = yy + 2.0f * (1.0f - kb) * cb;
		b[k].g = (yy - kr * b[k].r - kb * b[k].b) / (1.0f - kr - kb);
	}
	pthread_mutex_lock(&src->mutex);
	memcpy(src->bands, b, sizeof(b));
	for (int k = 0; k < 3; k++) {
		src->trail[k][src->trail_pos][0] = b[k].valid ? b[k].cb : NAN;
		src->trail[k][src->trail_pos][1] = b[k].valid ? b[k].cr : NAN;
	}
	src->trail_pos = (src->trail_pos + 1) % TRAIL;
	if (src->trail_n < TRAIL)
		src->trail_n++;
	src->colorspace = sd->colorspace;
	pthread_mutex_unlock(&src->mutex);
}

/* Where a cast (Cb %, Cr %) lies in the plot. */
static void to_plot(const struct bal_source *src, float cb, float cr, float *x, float *y)
{
	const float k = (PLOT / 2.0f - 8.0f) / (float)src->range;
	*x = PLOT / 2.0f + cb * k;
	*y = PLOT / 2.0f - cr * k;
}

/* The name of the hue a cast points to, for the text. */
static const char *cast_name(int cs, float cb, float cr)
{
	static const char *names[6] = {"red", "yellow", "green", "cyan", "blue", "magenta"};
	static const float bars[6][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};
	float kr, kb;
	matrix(cs, &kr, &kb);
	const float a = atan2f(cr, cb);
	int best = 0;
	float best_d = 10.0f;
	for (int i = 0; i < 6; i++) {
		const float y = kr * bars[i][0] + (1 - kr - kb) * bars[i][1] + kb * bars[i][2];
		const float ai = atan2f((bars[i][0] - y) / (1 - kr), (bars[i][2] - y) / (1 - kb));
		float d = fabsf(a - ai);
		if (d > 3.14159265f)
			d = 2.0f * 3.14159265f - d;
		if (d < best_d)
			best_d = d, best = i;
	}
	return names[best];
}

static void bal_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct bal_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	struct band b[3];
	float trail[3][TRAIL][2];
	int trail_n, trail_pos, cs;
	pthread_mutex_lock(&src->mutex);
	memcpy(b, src->bands, sizeof(b));
	memcpy(trail, src->trail, sizeof(trail));
	trail_n = src->trail_n;
	trail_pos = src->trail_pos;
	cs = src->colorspace ? src->colorspace : CM_CS_709;
	pthread_mutex_unlock(&src->mutex);

	const struct text_scale ts = text_scale_from_viewport((float)BAL_W, (float)BAL_H);
	const float c = PLOT / 2.0f, r_out = PLOT / 2.0f - 8.0f;
	du_fill_rect(0, 0, (float)BAL_W, (float)BAL_H, 0xFF0C0C0E);

	// rings at a quarter, half and the whole range, the axes and the tolerance circle
	for (int i = 1; i <= 4; i++)
		du_circle(c, c, r_out * i / 4.0f, i == 4 ? 0x70FFFFFF : 0x30FFFFFF, false);
	du_line(c - r_out, c, c + r_out, c, 0x30FFFFFF);
	du_line(c, c - r_out, c, c + r_out, 0x30FFFFFF);
	du_circle(c, c, r_out * src->tolerance / src->range, 0x6040E060, false);
	char buf[96];
	snprintf(buf, sizeof(buf), "%d%%", src->range);
	text_draw(&ts, buf, c + 3.0f * ts.ux, c - r_out + 1.0f * ts.uy, 9.0f, 0x90FFFFFF, TEXT_ALIGN_LEFT,
		  TEXT_VALIGN_TOP);

	// the directions of the six hues at the outer ring
	{
		static const char *names[6] = {"R", "Y", "G", "C", "B", "M"};
		static const float bars[6][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};
		float kr, kb;
		matrix(cs, &kr, &kb);
		for (int i = 0; i < 6; i++) {
			const float y = kr * bars[i][0] + (1 - kr - kb) * bars[i][1] + kb * bars[i][2];
			const float a = atan2f((bars[i][0] - y) / (1 - kr), (bars[i][2] - y) / (1 - kb));
			const uint32_t col = du_argb(0.3f + 0.7f * bars[i][0], 0.3f + 0.7f * bars[i][1],
						     0.3f + 0.7f * bars[i][2], 0.9f);
			text_draw(&ts, names[i], c + (r_out + 1.0f) * cosf(a), c - (r_out + 1.0f) * sinf(a), 9.0f, col,
				  TEXT_ALIGN_CENTER, TEXT_VALIGN_MIDDLE);
		}
	}

	static const char *band_letter[3] = {"S", "M", "H"};
	static const char *band_key[3] = {"Balance.Shadows", "Balance.Mids", "Balance.Highlights"};
	static const uint32_t band_color[3] = {0xFF6A8CFF, 0xFFC8C8C8, 0xFFFFFFFF};

	// trails, oldest faintest
	if (src->show_trail)
		for (int k = 0; k < 3; k++)
			for (int i = 0; i < trail_n; i++) {
				const int idx = (trail_pos - 1 - i + 2 * TRAIL) % TRAIL;
				const float cb = trail[k][idx][0], cr = trail[k][idx][1];
				if (isnan(cb))
					continue;
				float x, y;
				to_plot(src, cb, cr, &x, &y);
				const uint32_t a = (uint32_t)(0x60 * (1.0f - (float)i / TRAIL));
				du_fill_rect(x - 0.8f, y - 0.8f, x + 0.8f, y + 0.8f,
					     (a << 24) | (band_color[k] & 0xFFFFFF));
			}

	// the three points with lines between them
	float px[3], py[3];
	for (int k = 0; k < 3; k++) {
		to_plot(src, b[k].valid ? b[k].cb : 0.0f, b[k].valid ? b[k].cr : 0.0f, &px[k], &py[k]);
		px[k] = fminf(fmaxf(px[k], 2.0f), PLOT - 2.0f);
		py[k] = fminf(fmaxf(py[k], 2.0f), PLOT - 2.0f);
	}
	if (b[0].valid && b[1].valid)
		du_line(px[0], py[0], px[1], py[1], 0x80FFFFFF);
	if (b[1].valid && b[2].valid)
		du_line(px[1], py[1], px[2], py[2], 0x80FFFFFF);
	for (int k = 0; k < 3; k++) {
		if (!b[k].valid)
			continue;
		const float rad = 5.0f;
		du_circle(px[k], py[k], rad, du_argb(b[k].r, b[k].g, b[k].b, 1.0f), true);
		du_circle(px[k], py[k], rad, band_color[k], false);
		text_draw(&ts, band_letter[k], px[k] + 7.0f * ts.ux, py[k] - 6.0f * ts.uy, 10.0f, band_color[k],
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_BOTTOM);
	}

	// the numbers
	const float tx = PLOT + 8.0f;
	float ty = 8.0f;
	text_draw(&ts, obs_module_text("Balance"), tx, ty, 12.0f, 0xFFFFFFFF, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	ty += 22.0f * ts.uy;
	for (int k = 0; k < 3; k++) {
		text_draw(&ts, obs_module_text(band_key[k]), tx, ty, 11.0f, band_color[k], TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_TOP);
		ty += 15.0f * ts.uy;
		if (!b[k].valid) {
			text_draw(&ts, "-", tx, ty, 10.0f, 0x90FFFFFF, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
			ty += 26.0f * ts.uy;
			continue;
		}
		const float mag = hypotf(b[k].cb, b[k].cr);
		const bool ok = mag <= src->tolerance;
		if (ok)
			snprintf(buf, sizeof(buf), "%s  %.1f%%", obs_module_text("Balance.Neutral"), mag);
		else
			snprintf(buf, sizeof(buf), "%.1f%% %s", mag, cast_name(cs, b[k].cb, b[k].cr));
		text_draw(&ts, buf, tx, ty, 10.0f, ok ? 0xFF60E060 : 0xFFFFC040, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		ty += 13.0f * ts.uy;
		snprintf(buf, sizeof(buf), "Cb %+.1f  Cr %+.1f  Y %.0f%%  %.0f%% px", b[k].cb, b[k].cr, b[k].luma,
			 b[k].share);
		text_draw(&ts, buf, tx, ty, 9.0f, 0xB0FFFFFF, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		ty += 18.0f * ts.uy;
	}
}

const struct obs_source_info colormonitor_balance = {
	.id = ID_PREFIX "balance_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = bal_get_name,
	.create = bal_create,
	.destroy = bal_destroy,
	.update = bal_update,
	.get_defaults = bal_get_defaults,
	.get_properties = bal_get_properties,
	.get_width = bal_get_width,
	.get_height = bal_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = bal_render,
	.video_tick = cm_tick,
};

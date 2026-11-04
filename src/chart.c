/*
 * Color chart: measures the patches of a color chart in the dock selection: the 24 of a ColorChecker Classic
 * (6 x 4, with published values), or any grid of columns x rows for other charts,
 * and compares them with a reference: the same chart as one camera saw it ("Set reference"), or the
 * published values of the chart. Draw a rectangle around the patches in the ROI view; the scope samples
 * the middle of every cell, so the gaps between the patches do not count.
 *
 * Shown per patch: the reference and the live color side by side with the difference (Delta E).
 * Summary: exposure and white balance from the grey row, saturation over the colored patches, the hue
 * of the six primaries, and the largest differences. Facts first; a remark only when far off.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define CH_W 1200
#define CH_H 520
#define N_PATCH 24 // ColorChecker Classic
#define PATCH_MAX (14 * 12)

enum chart_type_e {
	CHART_CLASSIC = 0, // ColorChecker Classic 24, 6 x 4: published values and named primaries
	CHART_14X10 = 1,   // 140 patches, 14 x 10 (for example a ColorChecker SG)
	CHART_CUSTOM = 2,  // columns x rows of your own
};

enum chart_layout_e {
	LAYOUT_6X4 = 0,     // as printed, white patch bottom left
	LAYOUT_6X4_180 = 1, // upside down
	LAYOUT_4X6_CW = 2,  // turned a quarter clockwise
	LAYOUT_4X6_CCW = 3, // turned a quarter counter-clockwise
};

enum chart_ref_e {
	REF_CAPTURED = 0,  // the chart as one camera saw it
	REF_PUBLISHED = 1, // the chart's published values
};

/* Published sRGB / Rec. 709 values of the 24 patches (approximate, D65, 8 bit). */
static const uint8_t published[N_PATCH][3] = {
	{115, 82, 68},   {194, 150, 130}, {98, 122, 157},  {87, 108, 67},   {133, 128, 177}, {103, 189, 170},
	{214, 126, 44},  {80, 91, 166},   {193, 90, 99},   {94, 60, 108},   {157, 188, 64},  {224, 163, 46},
	{56, 61, 150},   {70, 148, 73},   {175, 54, 60},   {231, 199, 31},  {187, 86, 149},  {8, 133, 161},
	{243, 243, 242}, {200, 200, 200}, {160, 160, 160}, {122, 122, 121}, {85, 85, 85},    {52, 52, 52},
};

static const char *patch_names[N_PATCH] = {
	"Dark skin", "Light skin",  "Blue sky",    "Foliage",   "Blue flower",  "Bluish green",
	"Orange",    "Purple blue", "Mod. red",    "Purple",    "Yellow green", "Orange yellow",
	"Blue",      "Green",       "Red",         "Yellow",    "Magenta",      "Cyan",
	"White",     "Neutral 8",   "Neutral 6.5", "Neutral 5", "Neutral 3.5",  "Black",
};

struct patch
{
	float rgb[3]; // 0..255 signal
	float spread; // luma standard deviation inside the sample, to tell a patch from something else
};

struct chart_source
{
	struct cm_source cm;

	pthread_mutex_t mutex;
	struct patch live[PATCH_MAX];
	struct patch work[PATCH_MAX]; // the pipeline thread's measurement
	bool live_valid;
	uint32_t live_w, live_h;
	int live_n; // patches in live

	struct patch ref[PATCH_MAX];
	bool ref_valid;
	volatile bool want_ref, want_clear;
	volatile bool store_ref;

	int layout;
	int ref_kind;
	int type;       // chart_type_e
	int cols, rows; // the grid as printed
	int n_patch;    // cols x rows
	bool advice;
	unsigned out_flags; // rows outside their tolerance, with a little hysteresis (render thread)
};

static void chart_update(void *, obs_data_t *);
static void chart_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *chart_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Chart");
}

static void chart_cmd_cb(void *data, calldata_t *cd)
{
	struct chart_source *src = data;
	const char *cmd = calldata_string(cd, "cmd");
	if (cmd && !strcmp(cmd, "set_base"))
		src->want_ref = true;
	else if (cmd && !strcmp(cmd, "clear_base"))
		src->want_clear = true;
}

static void *chart_create(obs_data_t *settings, obs_source_t *source)
{
	struct chart_source *src = bzalloc(sizeof(struct chart_source));
	pthread_mutex_init(&src->mutex, NULL);
	src->cm.flags = CM_FLAG_CONVERT_RGB;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, chart_surface_cb, src);
	proc_handler_add(obs_source_get_proc_handler(source), "void va_faces(in string cmd)", chart_cmd_cb, src);
	chart_update(src, settings);
	return src;
}

static void chart_destroy(void *data)
{
	struct chart_source *src = data;
	cm_destroy(&src->cm);
	pthread_mutex_destroy(&src->mutex);
	bfree(src);
}

/* The reference is kept in the settings as 72 numbers: "r g b r g b ...". */
static void chart_update(void *data, obs_data_t *settings)
{
	struct chart_source *src = data;
	cm_update(&src->cm, settings);
	src->layout = (int)obs_data_get_int(settings, "chart_layout");
	src->ref_kind = (int)obs_data_get_int(settings, "chart_ref_kind");
	src->advice = obs_data_get_bool(settings, "chart_advice");
	src->type = (int)obs_data_get_int(settings, "chart_type");
	int cols = 6, rows = 4;
	if (src->type == CHART_14X10) {
		cols = 14, rows = 10;
	} else if (src->type == CHART_CUSTOM) {
		cols = (int)obs_data_get_int(settings, "chart_cols");
		rows = (int)obs_data_get_int(settings, "chart_rows");
		cols = cols < 1 ? 1 : cols > 14 ? 14 : cols;
		rows = rows < 1 ? 1 : rows > 12 ? 12 : rows;
	}
	// published values only exist for the ColorChecker Classic
	if (src->type != CHART_CLASSIC)
		src->ref_kind = REF_CAPTURED;

	const char *s = obs_data_get_string(settings, "chart_ref");
	struct patch *ref = bzalloc(sizeof(struct patch) * PATCH_MAX);
	int n = 0;
	while (s && *s && n < PATCH_MAX * 3) {
		char *end = NULL;
		const double v = strtod(s, &end);
		if (end == s)
			break;
		ref[n / 3].rgb[n % 3] = (float)v;
		n++;
		s = end;
	}
	pthread_mutex_lock(&src->mutex);
	src->cols = cols;
	src->rows = rows;
	src->n_patch = cols * rows;
	// a reference of another chart does not count
	src->ref_valid = n == src->n_patch * 3;
	if (src->ref_valid)
		memcpy(src->ref, ref, sizeof(ref[0]) * src->n_patch);
	pthread_mutex_unlock(&src->mutex);
	bfree(ref);
}

static bool set_ref_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct chart_source *)data)->want_ref = true;
	return false;
}

static bool clear_ref_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct chart_source *)data)->want_clear = true;
	return false;
}

static void chart_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 1);
	obs_data_set_default_int(settings, "chart_layout", LAYOUT_6X4);
	obs_data_set_default_int(settings, "chart_ref_kind", REF_CAPTURED);
	obs_data_set_default_bool(settings, "chart_advice", true);
	obs_data_set_default_int(settings, "chart_type", CHART_CLASSIC);
	obs_data_set_default_int(settings, "chart_cols", 6);
	obs_data_set_default_int(settings, "chart_rows", 4);
}

static bool chart_type_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int type = (int)obs_data_get_int(settings, "chart_type");
	obs_property_set_visible(obs_properties_get(props, "chart_cols"), type == CHART_CUSTOM);
	obs_property_set_visible(obs_properties_get(props, "chart_rows"), type == CHART_CUSTOM);
	obs_property_set_visible(obs_properties_get(props, "chart_ref_kind"), type == CHART_CLASSIC);
	return true;
}

static obs_properties_t *chart_get_properties(void *data)
{
	struct chart_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_property_t *prop = obs_properties_add_list(props, "chart_type", obs_module_text("Chart.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Chart.Type.Classic"), CHART_CLASSIC);
	obs_property_list_add_int(prop, obs_module_text("Chart.Type.14x10"), CHART_14X10);
	obs_property_list_add_int(prop, obs_module_text("Chart.Type.Custom"), CHART_CUSTOM);
	obs_property_set_modified_callback(prop, chart_type_changed);
	obs_properties_add_int_slider(props, "chart_cols", obs_module_text("Chart.Cols"), 1, 14, 1);
	obs_properties_add_int_slider(props, "chart_rows", obs_module_text("Chart.Rows"), 1, 12, 1);
	prop = obs_properties_add_list(props, "chart_layout", obs_module_text("Chart.Layout"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Chart.Layout.6x4"), LAYOUT_6X4);
	obs_property_list_add_int(prop, obs_module_text("Chart.Layout.6x4.180"), LAYOUT_6X4_180);
	obs_property_list_add_int(prop, obs_module_text("Chart.Layout.4x6.CW"), LAYOUT_4X6_CW);
	obs_property_list_add_int(prop, obs_module_text("Chart.Layout.4x6.CCW"), LAYOUT_4X6_CCW);
	prop = obs_properties_add_list(props, "chart_ref_kind", obs_module_text("Chart.CompareWith"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Chart.CompareWith.Captured"), REF_CAPTURED);
	obs_property_list_add_int(prop, obs_module_text("Chart.CompareWith.Published"), REF_PUBLISHED);
	obs_properties_add_bool(props, "chart_advice", obs_module_text("Faces.Advice"));
	obs_properties_add_button2(props, "chart_set", obs_module_text("Chart.SetRef"), set_ref_clicked, src);
	obs_properties_add_button2(props, "chart_clear", obs_module_text("Chart.ClearRef"), clear_ref_clicked, src);
	return props;
}

static uint32_t chart_get_width(void *data)
{
	struct chart_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : CH_W;
}

static uint32_t chart_get_height(void *data)
{
	struct chart_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : CH_H;
}

/* Where patch i (as printed: pc across, pr down, first patch top left) is in the picture, in cells. */
static void patch_cell(int layout, int pc, int pr, int i, int *col, int *row, int *cols, int *rows)
{
	const int c = i % pc, r = i / pc;
	switch (layout) {
	case LAYOUT_6X4_180:
		*cols = pc, *rows = pr, *col = pc - 1 - c, *row = pr - 1 - r;
		break;
	case LAYOUT_4X6_CW:
		*cols = pr, *rows = pc, *col = pr - 1 - r, *row = c;
		break;
	case LAYOUT_4X6_CCW:
		*cols = pr, *rows = pc, *col = r, *row = pc - 1 - c;
		break;
	default:
		*cols = pc, *rows = pr, *col = c, *row = r;
		break;
	}
}

/* Pipeline thread: the average of the middle of every cell. */
static void chart_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct chart_source *src = data;
	{
		static uint64_t last_log;
		const uint64_t now = os_gettime_ns();
		if (now - last_log > 3000000000ULL) {
			last_log = now;
			blog(LOG_INFO, "[" PLUGIN_NAME "] chart input %ux%u rgb %s", sd->width, sd->height,
			     sd->rgb_data ? "yes" : "no");
		}
	}
	if (!sd->rgb_data || sd->width < 12 || sd->height < 12)
		return;

	struct patch *live = src->work; // this source's pipeline thread only
	const int np = src->n_patch, pc = src->cols, pr = src->rows;
	for (int i = 0; i < np; i++) {
		int col, row, cols, rows;
		patch_cell(src->layout, pc, pr, i, &col, &row, &cols, &rows);
		const uint32_t x0 = (uint32_t)((col + 0.3f) / cols * sd->width);
		const uint32_t x1 = (uint32_t)((col + 0.7f) / cols * sd->width);
		const uint32_t y0 = (uint32_t)((row + 0.3f) / rows * sd->height);
		const uint32_t y1 = (uint32_t)((row + 0.7f) / rows * sd->height);
		double sum[3] = {0, 0, 0}, sl = 0, sl2 = 0;
		uint32_t n = 0;
		for (uint32_t y = y0; y < y1; y++) {
			const uint8_t *p = sd->rgb_data + (size_t)sd->linesize * y + (size_t)x0 * 4;
			for (uint32_t x = x0; x < x1; x++, p += 4) {
				if (!p[3])
					continue;
				sum[0] += p[2];
				sum[1] += p[1];
				sum[2] += p[0];
				const double l = 0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0];
				sl += l;
				sl2 += l * l;
				n++;
			}
		}
		if (!n)
			return;
		for (int k = 0; k < 3; k++)
			live[i].rgb[k] = (float)(sum[k] / n);
		const double m = sl / n;
		live[i].spread = (float)sqrt(fmax(0.0, sl2 / n - m * m));
	}

	pthread_mutex_lock(&src->mutex);
	// steady numbers: each patch follows the picture over a few frames instead of jumping
	if (src->live_valid && sd->width == src->live_w && sd->height == src->live_h && src->live_n == np) {
		for (int i = 0; i < np; i++) {
			for (int k = 0; k < 3; k++)
				src->live[i].rgb[k] += (live[i].rgb[k] - src->live[i].rgb[k]) * 0.25f;
			src->live[i].spread = live[i].spread;
		}
	} else {
		memcpy(src->live, live, sizeof(live[0]) * np);
	}
	src->live_n = np;
	src->live_valid = true;
	src->live_w = sd->width;
	src->live_h = sd->height;
	pthread_mutex_unlock(&src->mutex);
}

static void chart_tick(void *data, float seconds)
{
	struct chart_source *src = data;
	cm_tick(&src->cm, seconds);

	if (src->want_ref || src->want_clear) {
		static char buf[PATCH_MAX * 3 * 8 + 1];
		buf[0] = 0;
		pthread_mutex_lock(&src->mutex);
		const bool set = src->want_ref && src->live_valid && src->live_n == src->n_patch;
		if (set) {
			size_t n = 0;
			for (int i = 0; i < src->n_patch; i++)
				for (int k = 0; k < 3; k++)
					n += snprintf(buf + n, sizeof(buf) - n, "%.1f ", src->live[i].rgb[k]);
		}
		src->want_ref = false;
		const bool clear = src->want_clear;
		src->want_clear = false;
		pthread_mutex_unlock(&src->mutex);
		if (set || clear) {
			obs_data_t *d = obs_data_create();
			obs_data_set_string(d, "chart_ref", set ? buf : "");
			if (set)
				obs_data_set_int(d, "chart_ref_kind", REF_CAPTURED);
			obs_source_update(src->cm.self, d);
			obs_data_release(d);
		}
	}
}

/* ---------- color math ---------- */

static float srgb_to_linear(float v)
{
	v /= 255.0f;
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static float lab_f(float t)
{
	return t > 0.008856f ? cbrtf(t) : 7.787f * t + 16.0f / 116.0f;
}

/* R'G'B' (Rec. 709 / sRGB primaries, D65) to CIE L*a*b*. */
static void to_lab(const float rgb[3], float lab[3])
{
	const float r = srgb_to_linear(rgb[0]), g = srgb_to_linear(rgb[1]), b = srgb_to_linear(rgb[2]);
	const float x = (0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.95047f;
	const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	const float z = (0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.08883f;
	const float fx = lab_f(x), fy = lab_f(y), fz = lab_f(z);
	lab[0] = 116.0f * fy - 16.0f;
	lab[1] = 500.0f * (fx - fy);
	lab[2] = 200.0f * (fy - fz);
}

static float delta_e(const float a[3], const float b[3])
{
	float la[3], lb[3];
	to_lab(a, la);
	to_lab(b, lb);
	return sqrtf((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) +
		     (la[2] - lb[2]) * (la[2] - lb[2]));
}

static float chroma(const float rgb[3])
{
	float lab[3];
	to_lab(rgb, lab);
	return hypotf(lab[1], lab[2]);
}

static float hue_deg(const float rgb[3])
{
	float lab[3];
	to_lab(rgb, lab);
	float h = atan2f(lab[2], lab[1]) * 180.0f / (float)M_PI;
	return h < 0.0f ? h + 360.0f : h;
}

static float wrap180(float d)
{
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return d;
}

/* ---------- drawing ---------- */

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

static uint32_t argb(const float rgb[3])
{
	const uint32_t r = (uint32_t)fminf(fmaxf(rgb[0], 0.0f), 255.0f);
	const uint32_t g = (uint32_t)fminf(fmaxf(rgb[1], 0.0f), 255.0f);
	const uint32_t b = (uint32_t)fminf(fmaxf(rgb[2], 0.0f), 255.0f);
	return 0xFF000000 | r << 16 | g << 8 | b; // colors here are 0xAARRGGBB
}

static float wrapped(const struct text_scale *ts, const char *text, float x, float y, float width, float px,
		     uint32_t color, float y_max)
{
	char line[512];
	const char *p = text;
	const float lh = (px + 4.0f) * ts->uy;
	while (*p && y + lh <= y_max) {
		size_t fit = 0, len = 0;
		while (p[len]) {
			size_t next = len;
			while (p[next] == ' ')
				next++;
			while (p[next] && p[next] != ' ')
				next++;
			if (next >= sizeof(line))
				break;
			memcpy(line, p, next);
			line[next] = 0;
			if (text_width(ts, line, px) > width && fit)
				break;
			fit = next;
			len = next;
		}
		if (!fit)
			fit = strlen(p) < sizeof(line) - 1 ? strlen(p) : sizeof(line) - 1;
		memcpy(line, p, fit);
		line[fit] = 0;
		text_draw(ts, line, x, y, px, color, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		y += lh;
		p += fit;
		while (*p == ' ')
			p++;
	}
	return y;
}

/* A check mark in a box of size sz at (x, y). */
static void draw_check(const struct text_scale *ts, float x, float y, float sz, uint32_t color)
{
	const float t = 2.0f * ts->uy;
	const float ax = x + sz * 0.12f, ay = y + sz * 0.55f, bx = x + sz * 0.4f, by = y + sz * 0.82f;
	const float cx = x + sz * 0.9f, cy = y + sz * 0.18f;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(ax, ay - t);
		gs_vertex2f(ax, ay + t);
		gs_vertex2f(bx, by - t);
		gs_vertex2f(bx, by + t);
		gs_vertex2f(cx, cy - t);
		gs_vertex2f(cx, cy + t);
		gs_render_stop(GS_TRISTRIP);
	}
}

/* In or out of the tolerance, with a little hysteresis so a mark does not flicker. */
static bool outside(unsigned *flags, int bit, float value, float tol)
{
	const bool was = (*flags >> bit) & 1;
	const bool now = was ? fabsf(value) > tol * 0.8f : fabsf(value) > tol * 1.2f;
	*flags = now ? *flags | 1u << bit : *flags & ~(1u << bit);
	return now;
}

static const char *stops_words(float stops)
{
	const float a = fabsf(stops);
	return a < 0.375f ? "a quarter stop" : a < 0.75f ? "half a stop" : a < 1.25f ? "a stop" : "more than a stop";
}

static void chart_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct chart_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	const float W = (float)CH_W, H = (float)CH_H;
	struct text_scale ts = text_scale_from_viewport(W, H);
	// never taller than the tile: with a large text size the text shrinks until the table fits
	{
		const float need = 19.0f * (12.5f + 7.0f) * ts.uy;
		if (need > H) {
			const float k = H / need;
			ts.ux *= k;
			ts.uy *= k;
		}
	}
	fill(0.0f, 0.0f, W, H, 0xFF101013);

	static struct patch live[PATCH_MAX];
	static struct patch ref_captured[PATCH_MAX];
	pthread_mutex_lock(&src->mutex);
	const int np = src->n_patch, pc = src->cols, pr = src->rows;
	const bool classic = src->type == CHART_CLASSIC;
	const bool have_live = src->live_valid && src->live_n == np;
	const bool have_captured = src->ref_valid;
	memcpy(live, src->live, sizeof(live[0]) * np);
	memcpy(ref_captured, src->ref, sizeof(ref_captured[0]) * np);
	pthread_mutex_unlock(&src->mutex);

	static float ref[PATCH_MAX][3];
	bool have_ref = false;
	const char *ref_name = "reference";
	if (src->ref_kind == REF_PUBLISHED && classic) {
		for (int i = 0; i < N_PATCH; i++)
			for (int k = 0; k < 3; k++)
				ref[i][k] = published[i][k];
		have_ref = true;
		ref_name = "published values";
	} else if (have_captured) {
		for (int i = 0; i < np; i++)
			for (int k = 0; k < 3; k++)
				ref[i][k] = ref_captured[i].rgb[k];
		have_ref = true;
	}

	// the patches as printed: the reference on top, live below, the difference in the middle
	const float m = 8.0f;
	const float gw = 600.0f, gh = 400.0f;
	const float cw = gw / pc, chh = gh / pr;
	const float gap = fminf(4.0f, fminf(cw, chh) * 0.08f);
	// the Delta E number fits small cells too, or is left out for a colored dot
	const float de_px = fminf(12.0f, fminf(chh * 0.3f / ts.uy, cw * 0.28f / ts.ux));
	for (int i = 0; i < np; i++) {
		const float x0 = m + (i % pc) * cw, y0 = m + (i / pc) * chh;
		const float x1 = x0 + cw - gap, y1 = y0 + chh - gap;
		const float ym = (y0 + y1) * 0.5f;
		if (have_ref)
			fill(x0, y0, x1, ym, argb(ref[i]));
		else
			fill(x0, y0, x1, ym, 0xFF26262B);
		if (have_live)
			fill(x0, have_ref ? ym : y0, x1, y1, argb(live[i].rgb));
		if (have_live && have_ref) {
			const float d = delta_e(live[i].rgb, ref[i]);
			char buf[16];
			snprintf(buf, sizeof(buf), "%.1f", d);
			const uint32_t c = d < 3.0f ? 0xFF6FCF86 : d < 6.0f ? 0xFFFFA040 : 0xFFFF5A5A;
			if (de_px >= 7.0f) {
				const float tw = text_width(&ts, buf, de_px);
				const float hh = (de_px * 0.5f + 2.0f) * ts.uy;
				fill((x0 + x1) * 0.5f - tw * 0.5f - 3.0f * ts.ux, ym - hh,
				     (x0 + x1) * 0.5f + tw * 0.5f + 3.0f * ts.ux, ym + hh, 0xC0000000);
				text_draw(&ts, buf, (x0 + x1) * 0.5f, ym, de_px, c, TEXT_ALIGN_CENTER,
					  TEXT_VALIGN_MIDDLE);
			} else {
				const float r = fminf(cw, chh) * 0.12f;
				fill((x0 + x1) * 0.5f - r, ym - r, (x0 + x1) * 0.5f + r, ym + r, c);
			}
		}
	}
	text_draw(&ts, have_ref ? obs_module_text("Chart.Legend") : obs_module_text("Chart.LegendLive"), m,
		  m + gh + 6.0f * ts.uy, 11.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);

	// the text on the right
	const float x = m + gw + 16.0f, text_w = W - x - m;
	float y = m;
	char buf[1600];
	if (!have_live) {
		wrapped(&ts, obs_module_text("Chart.NoSignal"), x, y, text_w, 13.0f, 0xFF8D8D96, H - m);
		return;
	}
	// a patch with a lot of detail is probably not a patch: the rectangle is off
	int busy = 0;
	for (int i = 0; i < np; i++)
		busy += live[i].spread > 12.0f;
	if (busy > np / 6) {
		y = wrapped(&ts, obs_module_text("Chart.Misaligned"), x, y, text_w, 12.0f, 0xFFFFC040, H - m);
		y += 6.0f * ts.uy;
	}
	if (!have_ref) {
		wrapped(&ts, obs_module_text("Chart.Hint"), x, y, text_w, 13.0f, 0xFFD8D8DC, H - m);
		return;
	}
	// the comparison as a table in fixed places: Reference | Difference | Live, a check per row
	const float px = 12.5f, lh = (px + 7.0f) * ts.uy;
	const uint32_t c_head = 0xFF8D8D96, c_val = 0xFFE6E6EA, c_dim = 0xFFB8B8C0;
	const float lw = text_w * 0.26f, bar_w = text_w * 0.2f;
	const float fcw = (text_w - lw - bar_w - 34.0f * ts.ux) / 3.0f;
	const float col_ref = x + lw + fcw - 4.0f * ts.ux, col_diff = col_ref + fcw;
	const float col_live = col_diff + 34.0f * ts.ux + bar_w + fcw;
	text_draw(&ts, src->ref_kind == REF_PUBLISHED ? "Published" : "Reference", col_ref, y, px * 0.9f, c_head,
		  TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
	text_draw(&ts, "Difference", col_diff, y, px * 0.9f, c_head, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
	text_draw(&ts, "Live", col_live, y, px * 0.9f, c_head, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
	y += lh;

	// the numbers
	float de_sum = 0.0f, de_max = 0.0f;
	for (int i = 0; i < np; i++) {
		const float d = delta_e(live[i].rgb, ref[i]);
		de_sum += d;
		de_max = fmaxf(de_max, d);
	}
	// The greys and the colored patches: on the Classic the middle greys (which do not clip) and the first
	// 18; on any other chart the patches that are grey or colored in the reference.
	static bool is_grey[PATCH_MAX], is_color[PATCH_MAX];
	int n_grey = 0, n_color = 0;
	for (int i = 0; i < np; i++) {
		if (classic) {
			is_grey[i] = i >= 19 && i <= 22;
			is_color[i] = i < 18;
		} else {
			const float *b = ref[i];
			const float l = (0.2126f * b[0] + 0.7152f * b[1] + 0.0722f * b[2]) / 2.55f;
			is_grey[i] = chroma(b) < 5.0f && l > 8.0f && l < 92.0f;
			is_color[i] = chroma(b) > 15.0f;
		}
		n_grey += is_grey[i];
		n_color += is_color[i];
	}
	float gl = 0, gr = 0, rl = 0, rr = 0, bl = 0, br = 0;
	for (int i = 0; i < np; i++) {
		if (!is_grey[i])
			continue;
		const float *a = live[i].rgb, *b = ref[i];
		gl += (0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2]) / 2.55f / n_grey;
		gr += (0.2126f * b[0] + 0.7152f * b[1] + 0.0722f * b[2]) / 2.55f / n_grey;
		rl += a[0] / fmaxf(a[1], 1.0f) * 100.0f / n_grey;
		rr += b[0] / fmaxf(b[1], 1.0f) * 100.0f / n_grey;
		bl += a[2] / fmaxf(a[1], 1.0f) * 100.0f / n_grey;
		br += b[2] / fmaxf(b[1], 1.0f) * 100.0f / n_grey;
	}
	float cl = 0, cr = 0, hue_sum = 0, hue_max = 0;
	int n_hue = 0;
	for (int i = 0; i < np; i++) {
		if (!is_color[i])
			continue;
		cl += chroma(live[i].rgb) / n_color;
		cr += chroma(ref[i]) / n_color;
		const float dh = wrap180(hue_deg(live[i].rgb) - hue_deg(ref[i]));
		hue_sum += dh;
		if (fabsf(dh) > fabsf(hue_max))
			hue_max = dh;
		n_hue++;
	}
	const float stops = gl > 0.5f && gr > 0.5f ? 2.4f * log2f(gl / gr) : 0.0f;

	struct row
	{
		const char *label;
		bool has_values;
		float ref, live, diff, tol;
		const char *fmt_val, *fmt_diff;
	} rows[13] = {
		{"Delta E average", false, 0, 0, de_sum / np, 3.0f, "", "%.1f"},
		{"Delta E largest", false, 0, 0, de_max, 6.0f, "", "%.1f"},
		{"Greys luma", true, gr, gl, gl - gr, 3.0f, "%.1f%%", "%+.1f%%"},
		{"Red / green", true, rr, rl, rl - rr, 2.0f, "%.1f%%", "%+.1f%%"},
		{"Blue / green", true, br, bl, bl - br, 2.0f, "%.1f%%", "%+.1f%%"},
		{"Saturation", true, cr, cl, (cl / fmaxf(cr, 0.01f) - 1.0f) * 100.0f, 5.0f, "%.1f", "%+.0f%%"},
	};
	// rows 2..4 need greys, row 5 colored patches; without them they show a dash
	const bool has_grey = n_grey > 0, has_color = n_color > 0;
	int nrows = 6;
	if (classic) {
		for (int i = 12; i < 18; i++) {
			const float hr = hue_deg(ref[i]), hl = hue_deg(live[i].rgb);
			rows[nrows++] = (struct row){patch_names[i], true,           hr, hl, wrap180(hl - hr), 4.0f,
						     "%.0f\xc2\xb0", "%+.1f\xc2\xb0"};
		}
	} else if (n_hue) {
		rows[nrows++] = (struct row){"Hue, average", false, 0, 0, hue_sum / n_hue, 3.0f, "", "%+.1f\xc2\xb0"};
		rows[nrows++] = (struct row){"Hue, largest", false, 0, 0, hue_max, 8.0f, "", "%+.1f\xc2\xb0"};
	}
	bool any_out = false;
	bool out[13];
	for (int k = 0; k < nrows; k++) {
		const struct row *w = rows + k;
		const bool missing = (k >= 2 && k <= 4 && !has_grey) || (k == 5 && !has_color);
		// Delta E has no sign: it is out above its limit
		out[k] = !missing && outside(&src->out_flags, k, k < 2 ? fmaxf(w->diff, 0.0f) : w->diff, w->tol);
		any_out |= out[k];
		if (k % 2 == 0)
			fill(x, y - 2.0f * ts.uy, x + text_w, y + lh - 3.0f * ts.uy, 0xFF17171B);
		text_draw(&ts, w->label, x + 4.0f * ts.ux, y, px, c_dim, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		if (missing) {
			// this chart has no grey (or colored) patches to measure it with
			text_draw(&ts, "\xe2\x80\x94", col_diff, y, px, c_head, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
			y += lh;
			continue;
		}
		if (w->has_values) {
			snprintf(buf, sizeof(buf), w->fmt_val, w->ref);
			text_draw(&ts, buf, col_ref, y, px, c_val, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
			snprintf(buf, sizeof(buf), w->fmt_val, w->live);
			text_draw(&ts, buf, col_live, y, px, c_val, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
		}
		snprintf(buf, sizeof(buf), w->fmt_diff, w->diff);
		text_draw(&ts, buf, col_diff, y, px, out[k] ? 0xFFFFA040 : 0xFF6FCF86, TEXT_ALIGN_RIGHT,
			  TEXT_VALIGN_TOP);
		const float box = px * ts.uy;
		if (!out[k])
			draw_check(&ts, col_diff + 10.0f * ts.ux, y + 1.0f * ts.uy, box, 0xFF6FCF86);
		else
			fill(col_diff + 14.0f * ts.ux, y + box * 0.35f, col_diff + 14.0f * ts.ux + box * 0.6f,
			     y + box * 0.65f, 0xFFFFA040);
		// the difference on a scale of three tolerances each side (Delta E from zero up)
		const float bx0 = col_diff + 30.0f * ts.ux, bx1 = bx0 + bar_w, yc = y + box * 0.5f;
		const bool one_sided = k < 2;
		const float zero = one_sided ? bx0 : (bx0 + bx1) * 0.5f;
		const float unit = one_sided ? (bx1 - bx0) / 3.0f : (bx1 - bx0) / 6.0f;
		fill(bx0, yc - 1.0f * ts.uy, bx1, yc + 1.0f * ts.uy, 0xFF3A3A40);
		fill(one_sided ? zero : zero - unit, yc - box * 0.3f, zero + unit, yc + box * 0.3f, 0xFF2F5A3A);
		fill(zero - 0.75f * ts.ux, yc - box * 0.45f, zero + 0.75f * ts.ux, yc + box * 0.45f, 0xFF8D8D96);
		const float v = fmaxf(one_sided ? 0.0f : -3.0f, fminf(3.0f, w->diff / w->tol));
		const float mx = zero + v * unit;
		fill(mx - 3.0f * ts.ux, yc - box * 0.45f, mx + 3.0f * ts.ux, yc + box * 0.45f,
		     out[k] ? 0xFFFFA040 : 0xFF6FCF86);
		y += lh;
	}
	y += 6.0f * ts.uy;

	// the explanation: only for what is off, in words that do not change with every frame
	size_t n = 0;
	buf[0] = 0;
#define ADD(...)                                                              \
	do {                                                                  \
		if (n < sizeof(buf))                                          \
			n += snprintf(buf + n, sizeof(buf) - n, __VA_ARGS__); \
	} while (0)
	if (!any_out) {
		draw_check(&ts, x, y, px * ts.uy, 0xFF6FCF86);
		snprintf(buf, sizeof(buf), "Within tolerance of the %s", ref_name);
		text_draw(&ts, buf, x + (px + 6.0f) * ts.ux, y, px, 0xFF6FCF86, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		y += lh;
		buf[0] = 0;
	} else {
		if (out[2]) {
			ADD("Exposure: the greys are %s than the %s", gl < gr ? "darker" : "brighter", ref_name);
			if (!cm_cs_is_hdr(cm_get_colorspace(&src->cm)) && fabsf(stops) >= 0.1f)
				ADD(", about %s", stops_words(stops));
			ADD("; %s exposure (iris, gain or ND). ", gl < gr ? "raise" : "lower");
		}
		if (out[3] || out[4]) {
			ADD("White balance: on the greys");
			if (out[3])
				ADD(" red is %s", rl > rr ? "higher" : "lower");
			if (out[3] && out[4])
				ADD(" and");
			if (out[4])
				ADD(" blue is %s", bl > br ? "higher" : "lower");
			ADD("; %s%s%s. ", out[3] ? (rl > rr ? "lower the R gain" : "raise the R gain") : "",
			    out[3] && out[4] ? ", " : "",
			    out[4] ? (bl > br ? "lower the B gain" : "raise the B gain") : "");
		}
		if (out[5])
			ADD("Saturation: the colors are %s saturated than the %s. ", cl < cr ? "less" : "more",
			    ref_name);
		bool hue_head = false;
		for (int k = 6; k < nrows; k++) {
			if (!out[k])
				continue;
			if (classic)
				ADD("%s%s toward %s", hue_head ? ", " : "Hue: ", rows[k].label,
				    rows[k].diff > 0.0f ? "the next color counter-clockwise"
							: "the next color clockwise");
			else
				ADD("%s%s the colors turn %s",
				    hue_head ? ", " : "Hue: ", k == 6 ? "on average" : "at most",
				    rows[k].diff > 0.0f ? "counter-clockwise" : "clockwise");
			hue_head = true;
		}
		if (hue_head)
			ADD(" on the vectorscope. ");
		if (src->advice && (de_sum / np > 6.0f || fabsf(stops) > 0.7f))
			ADD("Clearly different from the %s. ", ref_name);
	}
#undef ADD
	if (buf[0])
		y = wrapped(&ts, buf, x, y, text_w, 12.0f, 0xFFFFC890, H - m);
	if (cm_cs_is_hdr(cm_get_colorspace(&src->cm)))
		wrapped(&ts, obs_module_text("Chart.HdrNote"), x, y + 6.0f * ts.uy, text_w, 11.0f, 0xFF8D8D96, H - m);
}

const struct obs_source_info colormonitor_chart = {
	.id = ID_PREFIX "chart_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = chart_get_name,
	.create = chart_create,
	.destroy = chart_destroy,
	.update = chart_update,
	.get_defaults = chart_get_defaults,
	.get_properties = chart_get_properties,
	.get_width = chart_get_width,
	.get_height = chart_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = chart_render,
	.video_tick = chart_tick,
};

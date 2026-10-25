/*
 * QC: signal statistics and checks with an error log.
 *
 * Every frame the picture (or the dock selection) is measured:
 * - minimum and maximum of R, G, B and luma, average picture level
 * - share of clipped whites and crushed blacks
 * - for HLG: MaxCLL / MaxFALL in nits and the share of colors outside Rec. 709
 * - black borders (letterbox / pillarbox), black picture and frozen picture
 * Checks that stay in violation for longer than a minimum time are written to a log,
 * shown in the tile and optionally appended to a CSV file.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define QC_W 480
#define QC_H 400
#define QC_LOG_MAX 64

enum qc_check_e {
	QC_CLIP = 0,
	QC_CRUSH,
	QC_RGB,
	QC_GAMUT,
	QC_BORDER,
	QC_BLACK,
	QC_FREEZE,
	QC_N_CHECKS,
};

static const char *check_keys[QC_N_CHECKS] = {"qc_check_clip",   "qc_check_crush", "qc_check_rgb",   "qc_check_gamut",
					      "qc_check_border", "qc_check_black", "qc_check_freeze"};
static const char *check_names[QC_N_CHECKS] = {"QC.Check.Clip",   "QC.Check.Crush", "QC.Check.RGB",   "QC.Check.Gamut",
					       "QC.Check.Border", "QC.Check.Black", "QC.Check.Freeze"};

struct qc_frame
{
	bool valid;
	int colorspace;
	uint32_t width, height;
	float min[4], max[4]; // R, G, B, Y; 0..1 of the signal
	float apl;            // average luma
	float clip_share, crush_share, rgb_share, gamut_share;
	float cll, fall; // HLG: brightest pixel and frame average, in nits (maxRGB)
	int border[4];   // black rows/columns: top, bottom, left, right (captured pixels)
	uint64_t hash;
};

struct qc_event
{
	int check;
	char detail[48];
	time_t start;
	double duration; // seconds; < 0 while still going on
};

struct qc_source
{
	struct cm_source cm;

	pthread_mutex_t mutex;
	struct qc_frame frame;
	float max_cll, max_fall;
	struct qc_event log[QC_LOG_MAX];
	int n_log, i_log; // ring buffer, i_log is the next slot

	// check state (pipeline thread)
	bool active[QC_N_CHECKS];    // currently in violation
	uint64_t since[QC_N_CHECKS]; // violation start, ns
	bool logged[QC_N_CHECKS];    // already in the log
	int log_slot[QC_N_CHECKS];
	uint64_t last_hash;
	uint64_t hash_since;

	float lin_hlg[256];

	// settings
	bool enabled[QC_N_CHECKS];
	float luma_hi, luma_lo, tolerance, min_duration, freeze_seconds;
	int border_px;
	char *log_file;
	volatile bool want_reset;
};

static void qc_update(void *, obs_data_t *);
static void qc_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *qc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("QC");
}

static void *qc_create(obs_data_t *settings, obs_source_t *source)
{
	struct qc_source *src = bzalloc(sizeof(struct qc_source));
	pthread_mutex_init(&src->mutex, NULL);
	for (int i = 0; i < 256; i++) {
		const float e = i / 255.0f;
		src->lin_hlg[i] = e <= 0.5f ? e * e / 3.0f
					    : (expf((e - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
	}

	src->cm.flags = CM_FLAG_CONVERT_RGB | CM_FLAG_CONVERT_YUV;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, qc_surface_cb, src);

	qc_update(src, settings);
	return src;
}

static void qc_destroy(void *data)
{
	struct qc_source *src = data;
	cm_destroy(&src->cm);
	pthread_mutex_destroy(&src->mutex);
	bfree(src->log_file);
	bfree(src);
}

static void qc_update(void *data, obs_data_t *settings)
{
	struct qc_source *src = data;
	cm_update(&src->cm, settings);

	for (int i = 0; i < QC_N_CHECKS; i++)
		src->enabled[i] = obs_data_get_bool(settings, check_keys[i]);
	src->luma_hi = (float)obs_data_get_double(settings, "qc_luma_hi") / 100.0f;
	src->luma_lo = (float)obs_data_get_double(settings, "qc_luma_lo") / 100.0f;
	src->tolerance = (float)obs_data_get_double(settings, "qc_tolerance") / 100.0f;
	src->min_duration = (float)obs_data_get_double(settings, "qc_min_duration");
	src->freeze_seconds = (float)obs_data_get_double(settings, "qc_freeze_seconds");
	src->border_px = (int)obs_data_get_int(settings, "qc_border_px");

	pthread_mutex_lock(&src->mutex);
	bfree(src->log_file);
	const char *f = obs_data_get_string(settings, "qc_log_file");
	src->log_file = f && *f ? bstrdup(f) : NULL;
	pthread_mutex_unlock(&src->mutex);
}

static void qc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_bool(settings, "qc_check_clip", true);
	obs_data_set_default_bool(settings, "qc_check_crush", true);
	obs_data_set_default_bool(settings, "qc_check_rgb", false);
	obs_data_set_default_bool(settings, "qc_check_gamut", true);
	obs_data_set_default_bool(settings, "qc_check_border", true);
	obs_data_set_default_bool(settings, "qc_check_black", true);
	obs_data_set_default_bool(settings, "qc_check_freeze", false);
	obs_data_set_default_double(settings, "qc_luma_hi", 99.5);
	obs_data_set_default_double(settings, "qc_luma_lo", 0.5);
	obs_data_set_default_double(settings, "qc_tolerance", 1.0);
	obs_data_set_default_double(settings, "qc_min_duration", 0.5);
	obs_data_set_default_double(settings, "qc_freeze_seconds", 2.0);
	obs_data_set_default_int(settings, "qc_border_px", 4);
}

static bool qc_reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct qc_source *src = data;
	src->want_reset = true;
	return false;
}

static obs_properties_t *qc_get_properties(void *data)
{
	struct qc_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);

	obs_properties_t *checks = obs_properties_create();
	for (int i = 0; i < QC_N_CHECKS; i++)
		obs_properties_add_bool(checks, check_keys[i], obs_module_text(check_names[i]));
	obs_properties_add_group(props, "qc_checks", obs_module_text("QC.Checks"), OBS_GROUP_NORMAL, checks);

	obs_properties_t *limits = obs_properties_create();
	obs_properties_add_float_slider(limits, "qc_luma_hi", obs_module_text("QC.LumaHi"), 50.0, 100.0, 0.5);
	obs_properties_add_float_slider(limits, "qc_luma_lo", obs_module_text("QC.LumaLo"), 0.0, 20.0, 0.5);
	obs_properties_add_float_slider(limits, "qc_tolerance", obs_module_text("QC.Tolerance"), 0.0, 20.0, 0.1);
	obs_properties_add_float_slider(limits, "qc_min_duration", obs_module_text("QC.MinDuration"), 0.0, 10.0, 0.1);
	obs_properties_add_int_slider(limits, "qc_border_px", obs_module_text("QC.BorderPx"), 1, 100, 1);
	obs_properties_add_float_slider(limits, "qc_freeze_seconds", obs_module_text("QC.FreezeSeconds"), 0.5, 30.0,
					0.5);
	obs_properties_add_group(props, "qc_limits", obs_module_text("QC.Limits"), OBS_GROUP_NORMAL, limits);

	obs_properties_add_path(props, "qc_log_file", obs_module_text("QC.LogFile"), OBS_PATH_FILE_SAVE, "CSV (*.csv)",
				NULL);
	obs_properties_add_button2(props, "qc_reset", obs_module_text("QC.Reset"), qc_reset_clicked, src);
	return props;
}

static uint32_t qc_get_width(void *data)
{
	struct qc_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : QC_W;
}

static uint32_t qc_get_height(void *data)
{
	struct qc_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : QC_H;
}

/* ---------- measuring (pipeline thread) ---------- */

static inline bool is_black(const uint8_t *yuv_px, float lo)
{
	return yuv_px[1] <= (uint8_t)(lo * 255.0f + 4.0f);
}

static void measure_borders(const struct cm_surface_data *sd, int border[4])
{
	// a row or column counts as black when every sampled pixel of it is near black
	const uint32_t w = sd->width, h = sd->height;
	const uint8_t *yuv = sd->yuv_data;
	const float lo = 0.02f;
	int top = 0, bottom = 0, left = 0, right = 0;
	for (uint32_t y = 0; y < h / 2; y++, top++) {
		const uint8_t *row = yuv + (size_t)sd->linesize * y;
		bool black = true;
		for (uint32_t x = 0; x < w && black; x += 2)
			black = is_black(row + x * 4, lo);
		if (!black)
			break;
	}
	for (uint32_t y = h; y-- > h / 2; bottom++) {
		const uint8_t *row = yuv + (size_t)sd->linesize * y;
		bool black = true;
		for (uint32_t x = 0; x < w && black; x += 2)
			black = is_black(row + x * 4, lo);
		if (!black)
			break;
	}
	for (uint32_t x = 0; x < w / 2; x++, left++) {
		bool black = true;
		for (uint32_t y = 0; y < h && black; y += 2)
			black = is_black(yuv + (size_t)sd->linesize * y + x * 4, lo);
		if (!black)
			break;
	}
	for (uint32_t x = w; x-- > w / 2; right++) {
		bool black = true;
		for (uint32_t y = 0; y < h && black; y += 2)
			black = is_black(yuv + (size_t)sd->linesize * y + x * 4, lo);
		if (!black)
			break;
	}
	// a fully black picture is not a border
	if (top >= (int)h / 2) {
		top = bottom = left = right = 0;
	}
	border[0] = top;
	border[1] = bottom;
	border[2] = left;
	border[3] = right;
}

static void measure(struct qc_source *src, const struct cm_surface_data *sd, struct qc_frame *f)
{
	memset(f, 0, sizeof(*f));
	f->colorspace = sd->colorspace;
	f->width = sd->width;
	f->height = sd->height;
	for (int c = 0; c < 4; c++) {
		f->min[c] = 1.0f;
		f->max[c] = 0.0f;
	}

	const bool hlg = sd->colorspace == CM_CS_2100_HLG;
	const uint8_t hi = (uint8_t)fminf(src->luma_hi * 255.0f, 255.0f);
	const uint8_t lo = (uint8_t)fmaxf(src->luma_lo * 255.0f, 0.0f);
	uint64_t n = 0, n_clip = 0, n_crush = 0, n_rgb = 0, n_gamut = 0;
	double sum_y = 0.0, sum_nits = 0.0;
	uint8_t mn[4] = {255, 255, 255, 255}, mx[4] = {0, 0, 0, 0};
	float cll = 0.0f;
	uint64_t hash = 1469598103934665603ull;

	for (uint32_t y = 0; y < sd->height; y++) {
		const uint8_t *rgb = sd->rgb_data + (size_t)sd->linesize * y;
		const uint8_t *yuv = sd->yuv_data + (size_t)sd->linesize * y;
		for (uint32_t x = 0; x < sd->width; x++, rgb += 4, yuv += 4) {
			if ((x & 7) == 0 && (y & 3) == 0)
				hash = (hash ^ (uint64_t)(rgb[0] | rgb[1] << 8 | rgb[2] << 16)) * 1099511628211ull;
			if (!rgb[3])
				continue; // outside the selection
			const uint8_t b = rgb[0], g = rgb[1], r = rgb[2], l = yuv[1];
			n++;
			sum_y += l;
			const uint8_t v[4] = {r, g, b, l};
			for (int c = 0; c < 4; c++) {
				if (v[c] < mn[c])
					mn[c] = v[c];
				if (v[c] > mx[c])
					mx[c] = v[c];
			}
			if (l >= hi)
				n_clip++;
			if (l <= lo)
				n_crush++;
			if (r >= hi || g >= hi || b >= hi || r <= lo || g <= lo || b <= lo)
				n_rgb++;
			if (hlg) {
				const float er = src->lin_hlg[r], eg = src->lin_hlg[g], eb = src->lin_hlg[b];
				const float ys = 0.2627f * er + 0.6780f * eg + 0.0593f * eb;
				const float m = fmaxf(er, fmaxf(eg, eb));
				// HLG OOTF for a 1000 nits display, system gamma 1.2
				const float nits = ys > 0.0f ? 1000.0f * powf(ys, 0.2f) * m : 0.0f;
				sum_nits += nits;
				if (nits > cll)
					cll = nits;
				// Rec. 2020 to Rec. 709; a negative component is outside Rec. 709
				const float r7 = 1.6605f * er - 0.5876f * eg - 0.0728f * eb;
				const float g7 = -0.1246f * er + 1.1329f * eg - 0.0083f * eb;
				const float b7 = -0.0182f * er - 0.1006f * eg + 1.1187f * eb;
				const float tol = -0.002f * fmaxf(m, 0.01f);
				if (r7 < tol || g7 < tol || b7 < tol)
					n_gamut++;
			}
		}
	}

	f->hash = hash;
	if (!n)
		return;
	f->valid = true;
	for (int c = 0; c < 4; c++) {
		f->min[c] = mn[c] / 255.0f;
		f->max[c] = mx[c] / 255.0f;
	}
	f->apl = (float)(sum_y / n / 255.0);
	f->clip_share = (float)n_clip / n;
	f->crush_share = (float)n_crush / n;
	f->rgb_share = (float)n_rgb / n;
	f->gamut_share = (float)n_gamut / n;
	f->cll = cll;
	f->fall = (float)(sum_nits / n);
	measure_borders(sd, f->border);
}

static void append_csv(const char *path, const struct qc_event *e)
{
	if (!path)
		return;
	const bool exists = os_file_exists(path);
	FILE *fp = os_fopen(path, "a");
	if (!fp)
		return;
	if (!exists)
		fprintf(fp, "date,time,check,detail,duration_s\n");
	struct tm tm;
#ifdef _WIN32
	localtime_s(&tm, &e->start);
#else
	localtime_r(&e->start, &tm);
#endif
	fprintf(fp, "%04d-%02d-%02d,%02d:%02d:%02d,\"%s\",\"%s\",%.2f\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec, obs_module_text(check_names[e->check]), e->detail, e->duration);
	fclose(fp);
}

/* Updates one check; called with the mutex held. */
static void update_check(struct qc_source *src, int check, bool violation, const char *detail, uint64_t now)
{
	if (!src->enabled[check])
		violation = false;

	if (violation && !src->active[check]) {
		src->active[check] = true;
		src->since[check] = now;
		src->logged[check] = false;
	}

	if (violation && !src->logged[check] && (now - src->since[check]) >= (uint64_t)(src->min_duration * 1e9)) {
		struct qc_event *e = src->log + src->i_log;
		memset(e, 0, sizeof(*e));
		e->check = check;
		snprintf(e->detail, sizeof(e->detail), "%s", detail);
		e->start = time(NULL) - (time_t)((now - src->since[check]) / 1000000000ull);
		e->duration = -1.0;
		src->log_slot[check] = src->i_log;
		src->i_log = (src->i_log + 1) % QC_LOG_MAX;
		if (src->n_log < QC_LOG_MAX)
			src->n_log++;
		src->logged[check] = true;
	}

	if (src->logged[check]) {
		struct qc_event *e = src->log + src->log_slot[check];
		if (e->check == check && e->duration < 0.0) {
			if (!violation) {
				e->duration = (now - src->since[check]) / 1e9;
				append_csv(src->log_file, e);
			} else {
				snprintf(e->detail, sizeof(e->detail), "%s", detail);
			}
		}
	}

	if (!violation) {
		src->active[check] = false;
		src->logged[check] = false;
	}
}

static void qc_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct qc_source *src = data;
	if (!sd->rgb_data || !sd->yuv_data || !sd->width || !sd->height)
		return;

	struct qc_frame f;
	measure(src, sd, &f);
	const uint64_t now = os_gettime_ns();

	pthread_mutex_lock(&src->mutex);
	if (src->want_reset) {
		src->want_reset = false;
		src->max_cll = src->max_fall = 0.0f;
		src->n_log = src->i_log = 0;
		memset(src->active, 0, sizeof(src->active));
		memset(src->logged, 0, sizeof(src->logged));
	}
	src->frame = f;
	if (f.valid && f.colorspace == CM_CS_2100_HLG) {
		if (f.cll > src->max_cll)
			src->max_cll = f.cll;
		if (f.fall > src->max_fall)
			src->max_fall = f.fall;
	}

	char d[48];
	snprintf(d, sizeof(d), "%.1f%% of pixels", f.clip_share * 100.0f);
	update_check(src, QC_CLIP, f.valid && f.clip_share > src->tolerance, d, now);
	snprintf(d, sizeof(d), "%.1f%% of pixels", f.crush_share * 100.0f);
	update_check(src, QC_CRUSH, f.valid && f.crush_share > src->tolerance, d, now);
	snprintf(d, sizeof(d), "%.1f%% of pixels", f.rgb_share * 100.0f);
	update_check(src, QC_RGB, f.valid && f.rgb_share > src->tolerance, d, now);
	snprintf(d, sizeof(d), "%.1f%% of pixels", f.gamut_share * 100.0f);
	update_check(src, QC_GAMUT, f.valid && f.colorspace == CM_CS_2100_HLG && f.gamut_share > src->tolerance, d,
		     now);

	// borders in pixels of the source picture
	const int scale = src->cm.target_scale > 0 ? src->cm.target_scale : 1;
	int bmax = 0;
	for (int i = 0; i < 4; i++)
		if (f.border[i] > bmax)
			bmax = f.border[i];
	snprintf(d, sizeof(d), "T%d B%d L%d R%d px", f.border[0] * scale, f.border[1] * scale, f.border[2] * scale,
		 f.border[3] * scale);
	update_check(src, QC_BORDER, f.valid && bmax * scale >= src->border_px, d, now);

	snprintf(d, sizeof(d), "APL %.1f%%", f.apl * 100.0f);
	update_check(src, QC_BLACK, f.valid && f.apl < 0.02f, d, now);

	// frozen: the same picture for longer than the set time
	if (f.hash != src->last_hash) {
		src->last_hash = f.hash;
		src->hash_since = now;
	}
	const double frozen = (now - src->hash_since) / 1e9;
	snprintf(d, sizeof(d), "%.0f s unchanged", frozen);
	update_check(src, QC_FREEZE, f.valid && frozen >= src->freeze_seconds, d, now);
	pthread_mutex_unlock(&src->mutex);
}

/* ---------- drawing ---------- */

static void fill_rect(float x0, float y0, float x1, float y1, uint32_t color)
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

#define COLOR_TEXT 0xFFE6E6EA
#define COLOR_DIM 0xFF8D8D96
#define COLOR_OK 0xFF6FCF86
#define COLOR_BAD 0xFFFF5A50
#define COLOR_WARN 0xFFFFC040

static void qc_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct qc_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	struct qc_frame f;
	float max_cll, max_fall;
	bool active[QC_N_CHECKS];
	struct qc_event log[QC_LOG_MAX];
	int n_log, i_log;
	pthread_mutex_lock(&src->mutex);
	f = src->frame;
	max_cll = src->max_cll;
	max_fall = src->max_fall;
	memcpy(active, src->active, sizeof(active));
	memcpy(log, src->log, sizeof(log));
	n_log = src->n_log;
	i_log = src->i_log;
	pthread_mutex_unlock(&src->mutex);

	const float W = (float)QC_W, H = (float)QC_H;
	const struct text_scale ts = text_scale_from_viewport(W, H);
	const float px = 12.0f;
	const float lh = (px + 5.0f) * ts.uy; // line height
	const float pad = 8.0f * ts.ux;
	float y = 6.0f * ts.uy;

	fill_rect(0.0f, 0.0f, W, H, 0xFF101013);

	// header: overall status
	int n_bad = 0;
	for (int i = 0; i < QC_N_CHECKS; i++)
		n_bad += active[i] && src->enabled[i];
	const bool hlg = f.colorspace == CM_CS_2100_HLG;
	text_draw(&ts, hlg ? "QC  HLG" : "QC", pad, y, px + 2.0f, COLOR_TEXT, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	{
		char buf[48];
		if (!f.valid)
			snprintf(buf, sizeof(buf), "%s", obs_module_text("QC.NoSignal"));
		else if (n_bad)
			snprintf(buf, sizeof(buf), "%d %s", n_bad, obs_module_text("QC.Problems"));
		else
			snprintf(buf, sizeof(buf), "%s", obs_module_text("QC.AllOK"));
		text_draw(&ts, buf, W - pad, y, px + 2.0f,
			  !f.valid ? COLOR_DIM
			  : n_bad  ? COLOR_BAD
				   : COLOR_OK,
			  TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
	}
	y += lh * 1.4f;

	// min / max bars: R, G, B, Y
	static const char *names[4] = {"R", "G", "B", "Y"};
	static const uint32_t colors[4] = {0xFFFF6060, 0xFF60E060, 0xFF6090FF, 0xFFE0E0E0};
	const float bar_x0 = pad + 18.0f * ts.ux;
	const float bar_x1 = W - pad - 110.0f * ts.ux;
	for (int c = 0; c < 4; c++) {
		text_draw(&ts, names[c], pad, y, px, colors[c], TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		const float by0 = y + 3.0f * ts.uy, by1 = y + lh - 4.0f * ts.uy;
		fill_rect(bar_x0, by0, bar_x1, by1, 0xFF26262B);
		if (f.valid) {
			const float a = bar_x0 + (bar_x1 - bar_x0) * f.min[c];
			const float b = bar_x0 + (bar_x1 - bar_x0) * f.max[c];
			fill_rect(a, by0, fmaxf(b, a + 2.0f * ts.ux), by1, (colors[c] & 0x00FFFFFF) | 0xC0000000);
			char buf[32];
			snprintf(buf, sizeof(buf), "%.1f - %.1f%%", f.min[c] * 100.0f, f.max[c] * 100.0f);
			text_draw(&ts, buf, W - pad, y, px, COLOR_TEXT, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
		}
		y += lh;
	}

	// figures
	if (f.valid) {
		char buf[96];
		snprintf(buf, sizeof(buf), "APL %.1f%%   Clip %.1f%%   Crush %.1f%%", f.apl * 100.0f,
			 f.clip_share * 100.0f, f.crush_share * 100.0f);
		text_draw(&ts, buf, pad, y, px, COLOR_TEXT, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		y += lh;
		if (hlg) {
			snprintf(buf, sizeof(buf), "CLL %.0f  FALL %.0f nits   MaxCLL %.0f  MaxFALL %.0f", f.cll,
				 f.fall, max_cll, max_fall);
			text_draw(&ts, buf, pad, y, px, COLOR_TEXT, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
			y += lh;
			snprintf(buf, sizeof(buf), "Outside Rec. 709: %.2f%% of pixels", f.gamut_share * 100.0f);
			text_draw(&ts, buf, pad, y, px, f.gamut_share > src->tolerance ? COLOR_WARN : COLOR_TEXT,
				  TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
			y += lh;
		}
		const int s = src->cm.target_scale > 0 ? src->cm.target_scale : 1;
		snprintf(buf, sizeof(buf), "Borders  top %d  bottom %d  left %d  right %d px", f.border[0] * s,
			 f.border[1] * s, f.border[2] * s, f.border[3] * s);
		text_draw(&ts, buf, pad, y, px, COLOR_TEXT, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		y += lh;
	}

	// checks: a dot per enabled check
	y += lh * 0.3f;
	float x = pad;
	for (int i = 0; i < QC_N_CHECKS; i++) {
		if (!src->enabled[i])
			continue;
		const char *name = obs_module_text(check_names[i]);
		const float tw = text_width(&ts, name, px - 1.0f);
		if (x + tw + 16.0f * ts.ux > W - pad) {
			x = pad;
			y += lh;
		}
		const uint32_t c = active[i] ? COLOR_BAD : COLOR_OK;
		fill_rect(x, y + 4.0f * ts.uy, x + 7.0f * ts.ux, y + 11.0f * ts.uy, c);
		text_draw(&ts, name, x + 11.0f * ts.ux, y, px - 1.0f, active[i] ? COLOR_BAD : COLOR_DIM,
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		x += tw + 24.0f * ts.ux;
	}
	y += lh * 1.3f;

	// log, newest first
	fill_rect(pad, y - 3.0f * ts.uy, W - pad, y - 2.0f * ts.uy, 0xFF303036);
	text_draw(&ts, obs_module_text("QC.Log"), pad, y, px - 1.0f, COLOR_DIM, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	y += lh;
	if (!n_log) {
		text_draw(&ts, obs_module_text("QC.LogEmpty"), pad, y, px - 1.0f, COLOR_DIM, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_TOP);
	}
	for (int k = 0; k < n_log && y + lh <= H; k++) {
		const struct qc_event *e = log + ((i_log - 1 - k + QC_LOG_MAX * 2) % QC_LOG_MAX);
		struct tm tm;
#ifdef _WIN32
		localtime_s(&tm, &e->start);
#else
		localtime_r(&e->start, &tm);
#endif
		char buf[128];
		if (e->duration < 0.0)
			snprintf(buf, sizeof(buf), "%02d:%02d:%02d  %s  %s  (ongoing)", tm.tm_hour, tm.tm_min,
				 tm.tm_sec, obs_module_text(check_names[e->check]), e->detail);
		else
			snprintf(buf, sizeof(buf), "%02d:%02d:%02d  %s  %s  %.1f s", tm.tm_hour, tm.tm_min, tm.tm_sec,
				 obs_module_text(check_names[e->check]), e->detail, e->duration);
		text_draw(&ts, buf, pad, y, px - 1.0f, e->duration < 0.0 ? COLOR_BAD : COLOR_TEXT, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_TOP);
		y += lh;
	}
}

const struct obs_source_info colormonitor_qc = {
	.id = ID_PREFIX "qc_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = qc_get_name,
	.create = qc_create,
	.destroy = qc_destroy,
	.update = qc_update,
	.get_defaults = qc_get_defaults,
	.get_properties = qc_get_properties,
	.get_width = qc_get_width,
	.get_height = qc_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = qc_render,
	.video_tick = cm_tick,
};

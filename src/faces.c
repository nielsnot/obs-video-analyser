/*
 * Faces: finds and follows faces in the picture and measures them.
 *
 * A hidden probe source captures the scope's source, runs face detection and measures the
 * scene (luma median and percentiles, near black, near clipping) and the skin of the largest
 * face (luma, hue against the skin tone line, saturation). The scope shows the picture with the
 * faces marked, the face up close and a short written assessment.
 *
 * ◉ Snapshot keeps the face and its measurements; while comparing, the kept face is shown next to
 * the live one with the differences, so after switching to another camera you see at once
 * whether it matches.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"
#include "face.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FC_W 1200
#define FC_H 260
#define SKIN_LINE_DEG 123.0f // the skin tone (I) line on the vectorscope

/* ---------- probe: one camera ---------- */

struct face_stats
{
	bool valid;
	int colorspace;
	// scene, in % of the signal
	float median, p10, p90, p99, near_black, near_clip;
	// faces
	int n_faces;
	struct face_box faces[FACE_MAX];
	bool skin_valid;
	float skin_luma; // median, %
	float skin_hue;  // degrees on the vectorscope, counter-clockwise from +U
	float skin_sat;  // % of the outer ring
	uint8_t skin_rgb[3];
};

struct probe_source
{
	struct cm_source cm;
	struct face_detector *fd;
	pthread_mutex_t mutex;
	struct face_stats stats;
};

static const char *probe_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Video Analyser face probe";
}

static void probe_surface_cb(void *data, struct cm_surface_data *sd);

static void *probe_create(obs_data_t *settings, obs_source_t *source)
{
	struct probe_source *p = bzalloc(sizeof(struct probe_source));
	pthread_mutex_init(&p->mutex, NULL);
	p->fd = face_detector_create();
	p->cm.flags = CM_FLAG_CONVERT_RGB | CM_FLAG_CONVERT_YUV;
	cm_create(&p->cm, settings, source);
	cm_request(&p->cm, probe_surface_cb, p);
	cm_update(&p->cm, settings);
	return p;
}

static void probe_destroy(void *data)
{
	struct probe_source *p = data;
	cm_destroy(&p->cm);
	face_detector_destroy(p->fd);
	pthread_mutex_destroy(&p->mutex);
	bfree(p);
}

static void probe_update(void *data, obs_data_t *settings)
{
	struct probe_source *p = data;
	cm_update(&p->cm, settings);
}

static uint32_t probe_width(void *data)
{
	UNUSED_PARAMETER(data);
	return 16;
}

static void probe_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct probe_source *p = data;
	cm_render_target(&p->cm);
}

static float percentile(const uint32_t *hist, uint64_t total, float q)
{
	const uint64_t want = (uint64_t)(total * q);
	uint64_t acc = 0;
	for (int i = 0; i < 256; i++) {
		acc += hist[i];
		if (acc > want)
			return i / 2.55f;
	}
	return 100.0f;
}

static void probe_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct probe_source *p = data;
	if (!sd->rgb_data || !sd->yuv_data || !sd->width || !sd->height)
		return;

	face_detector_submit(p->fd, sd->rgb_data, sd->width, sd->height, sd->linesize);

	struct face_stats st = {0};
	st.colorspace = sd->colorspace;
	uint32_t hist[256] = {0};
	uint64_t n = 0;
	for (uint32_t y = 0; y < sd->height; y += 2) {
		const uint8_t *yuv = sd->yuv_data + (size_t)sd->linesize * y;
		for (uint32_t x = 0; x < sd->width; x += 2) {
			hist[yuv[x * 4 + 1]]++;
			n++;
		}
	}
	if (!n)
		return;
	st.valid = true;
	st.median = percentile(hist, n, 0.5f);
	st.p10 = percentile(hist, n, 0.1f);
	st.p90 = percentile(hist, n, 0.9f);
	st.p99 = percentile(hist, n, 0.99f);
	uint64_t nb = 0, nc = 0;
	for (int i = 0; i <= 5; i++)
		nb += hist[i];
	for (int i = 250; i < 256; i++)
		nc += hist[i];
	st.near_black = 100.0f * nb / n;
	st.near_clip = 100.0f * nc / n;

	st.n_faces = face_detector_get(p->fd, st.faces, FACE_MAX);
	if (st.n_faces > 0) {
		// skin of the largest face: cheeks and nose, between the eyes and the mouth
		const struct face_box *f = st.faces;
		const int x0 = (int)((f->x + f->w * 0.25f) * sd->width), x1 = (int)((f->x + f->w * 0.75f) * sd->width);
		const int y0 = (int)((f->y + f->h * 0.40f) * sd->height),
			  y1 = (int)((f->y + f->h * 0.75f) * sd->height);
		uint32_t sh[256] = {0};
		uint64_t sn = 0;
		double su = 0.0, sv = 0.0, sr = 0.0, sg = 0.0, sb = 0.0;
		for (int y = y0 < 0 ? 0 : y0; y < y1 && y < (int)sd->height; y++) {
			const uint8_t *yuv = sd->yuv_data + (size_t)sd->linesize * y;
			const uint8_t *rgb = sd->rgb_data + (size_t)sd->linesize * y;
			for (int x = x0 < 0 ? 0 : x0; x < x1 && x < (int)sd->width; x++) {
				const uint8_t l = yuv[x * 4 + 1];
				if (l < 13 || l > 242)
					continue; // shadows and specular highlights are not skin color
				sh[l]++;
				su += yuv[x * 4 + 0] - 128.0;
				sv += yuv[x * 4 + 2] - 128.0;
				sb += rgb[x * 4 + 0];
				sg += rgb[x * 4 + 1];
				sr += rgb[x * 4 + 2];
				sn++;
			}
		}
		if (sn > 20) {
			st.skin_valid = true;
			st.skin_luma = percentile(sh, sn, 0.5f);
			const double u = su / sn, v = sv / sn;
			double a = atan2(v, u) * 180.0 / M_PI;
			if (a < 0.0)
				a += 360.0;
			st.skin_hue = (float)a;
			st.skin_sat = (float)(hypot(u, v) / 127.0 * 100.0);
			st.skin_rgb[0] = (uint8_t)(sr / sn);
			st.skin_rgb[1] = (uint8_t)(sg / sn);
			st.skin_rgb[2] = (uint8_t)(sb / sn);
		}
	}

	pthread_mutex_lock(&p->mutex);
	p->stats = st;
	pthread_mutex_unlock(&p->mutex);
}

static void probe_tick(void *data, float seconds)
{
	cm_tick(data, seconds);
}

const struct obs_source_info colormonitor_face_probe = {
	.id = ID_PREFIX "face_probe",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED,
	.get_name = probe_get_name,
	.create = probe_create,
	.destroy = probe_destroy,
	.update = probe_update,
	.get_width = probe_width,
	.get_height = probe_width,
	.enum_active_sources = cm_enum_sources,
	.video_render = probe_render,
	.video_tick = probe_tick,
};

/* ---------- the Faces scope ---------- */

struct faces_source
{
	obs_source_t *self;
	obs_source_t *probe;
	char *name;
	int scale;
	bool advice;

	// snapshot: the face and its measurements at the moment of the snapshot
	volatile bool want_snapshot;
	volatile bool compare;
	struct face_stats ref;
	bool have_ref;
	gs_texture_t *ref_tex;
};

static const char *faces_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Faces");
}

static void faces_snapshot_cb(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	((struct faces_source *)data)->want_snapshot = true;
}

static void faces_compare_cb(void *data, calldata_t *cd)
{
	((struct faces_source *)data)->compare = calldata_bool(cd, "on");
}

static void faces_update(void *data, obs_data_t *settings)
{
	struct faces_source *src = data;
	src->scale = (int)obs_data_get_int(settings, "target_scale");
	src->advice = obs_data_get_bool(settings, "face_advice");
	const char *name = obs_data_get_string(settings, "target_name");
	if (!name)
		name = "";
	if (src->name && !strcmp(src->name, name) && src->probe) {
		obs_data_t *ps = obs_data_create();
		obs_data_set_int(ps, "target_scale", src->scale > 0 ? src->scale : 2);
		obs_source_update(src->probe, ps);
		obs_data_release(ps);
		return;
	}
	bfree(src->name);
	src->name = bstrdup(name);
	obs_source_release(src->probe);
	obs_data_t *ps = obs_data_create();
	obs_data_set_string(ps, "target_name", src->name);
	obs_data_set_int(ps, "target_scale", src->scale > 0 ? src->scale : 2);
	src->probe = obs_source_create_private(ID_PREFIX "face_probe", "face probe", ps);
	obs_data_release(ps);
}

static void *faces_create(obs_data_t *settings, obs_source_t *source)
{
	struct faces_source *src = bzalloc(sizeof(struct faces_source));
	src->self = source;
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void va_snapshot()", faces_snapshot_cb, src);
	proc_handler_add(ph, "void va_set_compare(in bool on)", faces_compare_cb, src);
	faces_update(src, settings);
	return src;
}

static void faces_destroy(void *data)
{
	struct faces_source *src = data;
	obs_source_release(src->probe);
	obs_enter_graphics();
	gs_texture_destroy(src->ref_tex);
	obs_leave_graphics();
	bfree(src->name);
	bfree(src);
}

static void faces_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_bool(settings, "face_advice", true);
}

static obs_properties_t *faces_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_list(props, "target_name", obs_module_text("Source"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	property_list_add_sources(prop, NULL);
	obs_properties_add_int(props, "target_scale", obs_module_text("Scale"), 1, 16, 1);
	obs_properties_add_bool(props, "face_advice", obs_module_text("Faces.Advice"));
	return props;
}

static uint32_t faces_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	return FC_W;
}

static uint32_t faces_get_height(void *data)
{
	UNUSED_PARAMETER(data);
	return FC_H;
}

static void faces_tick(void *data, float seconds)
{
	struct faces_source *src = data;
	if (src->probe)
		probe_tick(obs_obj_get_data(src->probe), seconds);
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

static void outline(float x0, float y0, float x1, float y1, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(x0, y0);
		gs_vertex2f(x1, y0);
		gs_vertex2f(x1, y0);
		gs_vertex2f(x1, y1);
		gs_vertex2f(x1, y1);
		gs_vertex2f(x0, y1);
		gs_vertex2f(x0, y1);
		gs_vertex2f(x0, y0);
		gs_render_stop(GS_LINES);
	}
}

/* Draws text wrapped to a width; returns the y below it. */
static float wrapped(const struct text_scale *ts, const char *text, float x, float y, float width, float px,
		     uint32_t color, float y_max)
{
	char line[512];
	const char *p = text;
	const float lh = (px + 4.0f) * ts->uy;
	while (*p && y + lh <= y_max) {
		size_t len = 0, fit = 0;
		while (p[len]) {
			// try the next word
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

/* Typical face levels for the signal: SDR R'G'B', HLG or PQ (in % of the signal). */
static void face_range(int colorspace, float *lo, float *hi)
{
	if (colorspace == CM_CS_2100_HLG) {
		*lo = 45.0f;
		*hi = 70.0f;
	} else if (colorspace == CM_CS_2100_PQ) {
		*lo = 40.0f;
		*hi = 60.0f;
	} else {
		*lo = 40.0f;
		*hi = 75.0f;
	}
}

/* The region around a face, a little larger than the box and square on the picture, in pixels. */
static bool face_crop(const struct face_box *f, uint32_t cw, uint32_t ch, uint32_t *x, uint32_t *y, uint32_t *w,
		      uint32_t *h)
{
	const float side = fmaxf(f->w * cw, f->h * ch) * 1.5f;
	const float cx = (f->x + f->w * 0.5f) * cw, cy = (f->y + f->h * 0.5f) * ch;
	float x0 = cx - side * 0.5f, y0 = cy - side * 0.5f;
	x0 = fmaxf(0.0f, fminf(x0, cw - side));
	y0 = fmaxf(0.0f, fminf(y0, ch - side));
	*x = (uint32_t)fmaxf(x0, 0.0f);
	*y = (uint32_t)fmaxf(y0, 0.0f);
	*w = (uint32_t)fminf(side, (float)cw - *x);
	*h = (uint32_t)fminf(side, (float)ch - *y);
	return *w >= 4 && *h >= 4;
}

static void draw_tex_region(gs_texture_t *tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, float dx, float dy,
			    float dw, float dh)
{
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
	gs_matrix_push();
	gs_matrix_translate3f(dx, dy, 0.0f);
	gs_matrix_scale3f(dw / w, dh / h, 1.0f);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite_subregion(tex, 0, x, y, w, h);
	gs_matrix_pop();
}

static void assessment(const struct face_stats *s, const struct face_stats *ref, bool advice, char *out, size_t size)
{
	size_t n = 0;
#define ADD(...)                                                       \
	do {                                                           \
		if (n < size)                                          \
			n += snprintf(out + n, size - n, __VA_ARGS__); \
	} while (0)

	if (!s->skin_valid) {
		ADD("%s", obs_module_text("Faces.NoFace"));
		return;
	}
	ADD("Face: luma median %.1f%%, hue %.0f\xc2\xb0, saturation %.0f%%. ", s->skin_luma, s->skin_hue, s->skin_sat);
	const float dev = s->skin_hue - SKIN_LINE_DEG;
	if (fabsf(dev) <= 4.0f)
		ADD("Hue is on the skin tone line. ");
	else
		ADD("Hue is %.0f\xc2\xb0 toward %s relative to the skin tone line. ", fabsf(dev),
		    dev < 0.0f ? "red/magenta" : "yellow/green");

	if (ref && ref->skin_valid) {
		const float dl = s->skin_luma - ref->skin_luma;
		const float dh = s->skin_hue - ref->skin_hue;
		const float ds = s->skin_sat - ref->skin_sat;
		// about 2.4 stops per doubling of the signal for SDR video
		const float stops = s->skin_luma > 0.5f && ref->skin_luma > 0.5f
					    ? 2.4f * log2f(ref->skin_luma / s->skin_luma)
					    : 0.0f;
		ADD("Against the snapshot: luma %+.1f%%", dl);
		if (fabsf(stops) >= 0.1f && s->colorspace < CM_CS_2100_HLG)
			ADD(" (about %+.1f stop to match)", stops);
		ADD(", hue %+.0f\xc2\xb0, saturation %+.0f%%. ", dh, ds);
		if (fabsf(dl) <= 3.0f && fabsf(dh) <= 4.0f && fabsf(ds) <= 5.0f)
			ADD("It matches the snapshot. ");
		else if (advice)
			ADD("Match %s%s%s to the snapshot. ", fabsf(dl) > 3.0f ? "exposure" : "",
			    fabsf(dl) > 3.0f && (fabsf(dh) > 4.0f || fabsf(ds) > 5.0f) ? " and " : "",
			    fabsf(dh) > 4.0f || fabsf(ds) > 5.0f ? "color" : "");
	}

	if (advice) {
		float lo, hi;
		face_range(s->colorspace, &lo, &hi);
		if (s->skin_luma < lo)
			ADD("The face is darker than usual (%.0f-%.0f%%); open the iris or add light if the shot allows. ",
			    lo, hi);
		else if (s->skin_luma > hi)
			ADD("The face is brighter than usual (%.0f-%.0f%%); watch the highlights. ", lo, hi);
		if (fabsf(dev) > 6.0f)
			ADD("This may reflect lighting, makeup or natural undertone. Compare against a trusted reference "
			    "and verify neutral lighting and white balance first. If a cast is confirmed, broadcast: "
			    "consider R/B paint or skin-detail hue; prosumer: consider WB temperature/tint or the "
			    "picture-profile hue. Preserve natural complexion differences. ");
	}
#undef ADD
}

static void face_panel(const struct text_scale *ts, gs_texture_t *tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		       float px, float py, float size, const char *label, uint32_t color)
{
	fill(px, py, px + size, py + size, 0xFF000000);
	if (tex && w && h)
		draw_tex_region(tex, x, y, w, h, px, py, size, size);
	outline(px, py, px + size, py + size, color);
	fill(px, py + size - 16.0f * ts->uy, px + size, py + size, 0xA0000000);
	text_draw(ts, label, px + size * 0.5f, py + size - 2.0f * ts->uy, 10.0f, color, TEXT_ALIGN_CENTER,
		  TEXT_VALIGN_BOTTOM);
}

static void faces_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct faces_source *src = data;
	const float W = (float)FC_W, H = (float)FC_H;
	const struct text_scale ts = text_scale_from_viewport(W, H);
	fill(0.0f, 0.0f, W, H, 0xFF101013);

	struct probe_source *p = src->probe ? obs_obj_get_data(src->probe) : NULL;
	struct face_stats st = {0};
	if (p) {
		cm_render_target(&p->cm); // keeps the capture running
		pthread_mutex_lock(&p->mutex);
		st = p->stats;
		pthread_mutex_unlock(&p->mutex);
	}
	gs_texture_t *tex = p ? cm_bypass_get_texture(&p->cm) : NULL;
	const uint32_t cw = p ? cm_bypass_get_width(&p->cm) : 0, ch = p ? cm_bypass_get_height(&p->cm) : 0;
	uint32_t fx = 0, fy = 0, fw = 0, fh = 0;
	const bool face = tex && cw && ch && st.n_faces > 0 && face_crop(st.faces, cw, ch, &fx, &fy, &fw, &fh);

	// snapshot: keep the face crop and the measurements
	if (src->want_snapshot) {
		src->want_snapshot = false;
		if (face && st.skin_valid) {
			gs_texture_destroy(src->ref_tex);
			src->ref_tex = gs_texture_create(fw, fh, gs_texture_get_color_format(tex), 1, NULL, 0);
			if (src->ref_tex)
				gs_copy_texture_region(src->ref_tex, 0, 0, tex, fx, fy, fw, fh);
			src->ref = st;
			src->have_ref = src->ref_tex != NULL;
		}
	}
	const bool show_ref = src->have_ref && src->compare;

	// the picture with the faces marked
	const float y0 = 8.0f * ts.uy, y1 = H - 8.0f * ts.uy;
	const float th = y1 - y0, tw = th * 16.0f / 9.0f, tx = 8.0f * ts.ux;
	fill(tx, y0, tx + tw, y1, 0xFF000000);
	if (tex && cw && ch)
		draw_tex_region(tex, 0, 0, cw, ch, tx, y0, tw, th);
	for (int k = 0; k < st.n_faces; k++) {
		const struct face_box *f = st.faces + k;
		outline(tx + f->x * tw, y0 + f->y * th, tx + (f->x + f->w) * tw, y0 + (f->y + f->h) * th,
			k == 0 ? 0xFF6FCF86 : 0xFFB8B8C0);
	}

	// the face up close, and the kept one next to it
	const float size = th;
	float x = tx + tw + 10.0f * ts.ux;
	face_panel(&ts, face ? tex : NULL, fx, fy, fw, fh, x, y0, size, obs_module_text("Faces.Live"), 0xFF6FCF86);
	x += size + 6.0f * ts.ux;
	if (show_ref) {
		face_panel(&ts, src->ref_tex, 0, 0, gs_texture_get_width(src->ref_tex),
			   gs_texture_get_height(src->ref_tex), x, y0, size, obs_module_text("Faces.Snapshot"),
			   0xFFFF59D9);
		x += size + 6.0f * ts.ux;
	}

	// text
	x += 6.0f * ts.ux;
	const float text_w = W - x - 10.0f * ts.ux;
	char buf[1200];
	const char *name = obs_module_text("Faces.DockSelection");
	if (src->name && is_program_name(src->name))
		name = "Program";
	else if (src->name && *src->name && !is_roi_source_name(src->name))
		name = src->name;
	if (!p || !st.valid) {
		snprintf(buf, sizeof(buf), "%s  -  %s", name, obs_module_text("QC.NoSignal"));
		text_draw(&ts, buf, x, y0, 13.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		return;
	}
	snprintf(buf, sizeof(buf), "%s  \xe2\x80\xa2  %d %s", name, st.n_faces,
		 st.n_faces == 1 ? obs_module_text("Faces.FaceFound") : obs_module_text("Faces.FacesFound"));
	float y = y0;
	text_draw(&ts, buf, x, y, 13.0f, 0xFFF0F0F2, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	y += 18.0f * ts.uy;
	snprintf(buf, sizeof(buf),
		 "Scene luma: median %.1f%%, P10-P90 %.1f-%.1f, P99 %.0f; near black %.0f%%, near clipping %.0f%%.",
		 st.median, st.p10, st.p90, st.p99, st.near_black, st.near_clip);
	y = wrapped(&ts, buf, x, y, text_w, 11.5f, 0xFFD8D8DC, y1);
	assessment(&st, show_ref ? &src->ref : NULL, src->advice, buf, sizeof(buf));
	y = wrapped(&ts, buf, x, y + 4.0f * ts.uy, text_w, 11.5f, 0xFFD8D8DC, y1);
	if (!src->have_ref)
		wrapped(&ts, obs_module_text("Faces.SnapshotHint"), x, y + 4.0f * ts.uy, text_w, 10.5f, 0xFF8D8D96, y1);
}

const struct obs_source_info colormonitor_faces = {
	.id = ID_PREFIX "faces_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = faces_get_name,
	.create = faces_create,
	.destroy = faces_destroy,
	.update = faces_update,
	.get_defaults = faces_get_defaults,
	.get_properties = faces_get_properties,
	.get_width = faces_get_width,
	.get_height = faces_get_height,
	.video_render = faces_render,
	.video_tick = faces_tick,
};

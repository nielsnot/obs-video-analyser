/*
 * Faces: finds and follows faces in up to four cameras and compares them.
 *
 * Each camera gets a hidden probe source that captures it, runs face detection and measures
 * the scene (luma median and percentiles, near black, near clipping) and the skin of the
 * largest face (luma, hue against the skin tone line, saturation). The scope shows a thumbnail
 * with the faces marked and a short written assessment per camera, and compares every camera
 * with the first one.
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

#define FC_CAMS 4
#define FC_W 960
#define FC_ROW 150
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
	obs_source_t *probes[FC_CAMS];
	char *names[FC_CAMS];
	int scale;
	bool advice;
};

static const char *faces_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Faces");
}

static const char *cam_key[FC_CAMS] = {"face_cam1", "face_cam2", "face_cam3", "face_cam4"};

static void faces_update(void *data, obs_data_t *settings)
{
	struct faces_source *src = data;
	src->scale = (int)obs_data_get_int(settings, "target_scale");
	src->advice = obs_data_get_bool(settings, "face_advice");
	for (int i = 0; i < FC_CAMS; i++) {
		// the first camera follows the scope's own source setting (the dock selection by default)
		const char *name = i == 0 ? obs_data_get_string(settings, "target_name")
					  : obs_data_get_string(settings, cam_key[i]);
		const bool none = i > 0 && (!name || !*name);
		if (src->names[i] && name && !strcmp(src->names[i], name) && (src->probes[i] || none))
			goto keep;
		bfree(src->names[i]);
		src->names[i] = bstrdup(name ? name : "");
		obs_source_release(src->probes[i]);
		src->probes[i] = NULL;
		if (!none) {
			obs_data_t *ps = obs_data_create();
			obs_data_set_string(ps, "target_name", src->names[i]);
			obs_data_set_int(ps, "target_scale", src->scale > 0 ? src->scale : 2);
			src->probes[i] = obs_source_create_private(ID_PREFIX "face_probe", "face probe", ps);
			obs_data_release(ps);
		}
		continue;
	keep:
		if (src->probes[i]) {
			obs_data_t *ps = obs_data_create();
			obs_data_set_int(ps, "target_scale", src->scale > 0 ? src->scale : 2);
			obs_source_update(src->probes[i], ps);
			obs_data_release(ps);
		}
	}
}

static void *faces_create(obs_data_t *settings, obs_source_t *source)
{
	struct faces_source *src = bzalloc(sizeof(struct faces_source));
	src->self = source;
	faces_update(src, settings);
	return src;
}

static void faces_destroy(void *data)
{
	struct faces_source *src = data;
	for (int i = 0; i < FC_CAMS; i++) {
		obs_source_release(src->probes[i]);
		bfree(src->names[i]);
	}
	bfree(src);
}

static void faces_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_bool(settings, "face_advice", true);
}

static bool add_video_source(void *param, obs_source_t *s)
{
	obs_property_t *prop = param;
	const uint32_t flags = obs_source_get_output_flags(s);
	if ((flags & OBS_SOURCE_VIDEO) && !(flags & OBS_SOURCE_CAP_DISABLED)) {
		const char *id = obs_source_get_id(s);
		if (id && !strncmp(id, ID_PREFIX, strlen(ID_PREFIX)))
			return true; // not our own scopes
		const char *name = obs_source_get_name(s);
		obs_property_list_add_string(prop, name, name);
	}
	return true;
}

static obs_properties_t *faces_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_list(props, "target_name", obs_module_text("Faces.Cam1"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	property_list_add_sources(prop, NULL);
	obs_properties_add_int(props, "target_scale", obs_module_text("Scale"), 1, 16, 1);
	for (int i = 1; i < FC_CAMS; i++) {
		char key[32];
		snprintf(key, sizeof(key), "Faces.Cam%d", i + 1);
		prop = obs_properties_add_list(props, cam_key[i], obs_module_text(key), OBS_COMBO_TYPE_LIST,
					       OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(prop, obs_module_text("Faces.None"), "");
		obs_enum_sources(add_video_source, prop);
		obs_enum_scenes(add_video_source, prop);
	}
	obs_properties_add_bool(props, "face_advice", obs_module_text("Faces.Advice"));
	return props;
}

static uint32_t faces_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	return FC_W;
}

static int n_rows(const struct faces_source *src)
{
	int n = 0;
	for (int i = 0; i < FC_CAMS; i++)
		n += src->probes[i] != NULL;
	return n ? n : 1;
}

static uint32_t faces_get_height(void *data)
{
	return (uint32_t)(FC_ROW * n_rows(data));
}

static void faces_tick(void *data, float seconds)
{
	struct faces_source *src = data;
	for (int i = 0; i < FC_CAMS; i++)
		if (src->probes[i])
			probe_tick(obs_obj_get_data(src->probes[i]), seconds);
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

static void assessment(const struct face_stats *s, const struct face_stats *ref, int cam, bool advice, char *out,
		       size_t size)
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

	if (ref && ref != s && ref->skin_valid) {
		const float dl = s->skin_luma - ref->skin_luma;
		const float dh = s->skin_hue - ref->skin_hue;
		const float ds = s->skin_sat - ref->skin_sat;
		// about 2.4 stops per doubling of the signal for SDR video
		const float stops = s->skin_luma > 0.5f && ref->skin_luma > 0.5f
					    ? 2.4f * log2f(ref->skin_luma / s->skin_luma)
					    : 0.0f;
		ADD("Against camera 1: luma %+.1f%%", dl);
		if (fabsf(stops) >= 0.1f && s->colorspace < CM_CS_2100_HLG)
			ADD(" (about %+.1f stop to match)", stops);
		ADD(", hue %+.0f\xc2\xb0, saturation %+.0f%%.", dh, ds);
		if (advice && (fabsf(dl) > 3.0f || fabsf(dh) > 4.0f || fabsf(ds) > 5.0f))
			ADD(" Match %s%s%s to camera 1.", fabsf(dl) > 3.0f ? "exposure" : "",
			    fabsf(dl) > 3.0f && (fabsf(dh) > 4.0f || fabsf(ds) > 5.0f) ? " and " : "",
			    fabsf(dh) > 4.0f || fabsf(ds) > 5.0f ? "color" : "");
	}
	UNUSED_PARAMETER(cam);
#undef ADD
}

static void faces_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct faces_source *src = data;
	const float W = (float)FC_W, H = (float)faces_get_height(src);
	const struct text_scale ts = text_scale_from_viewport(W, H);
	fill(0.0f, 0.0f, W, H, 0xFF101013);

	struct face_stats stats[FC_CAMS];
	bool have[FC_CAMS] = {false};
	for (int i = 0; i < FC_CAMS; i++) {
		if (!src->probes[i])
			continue;
		struct probe_source *p = obs_obj_get_data(src->probes[i]);
		if (!p)
			continue;
		cm_render_target(&p->cm); // keeps the camera's capture running
		pthread_mutex_lock(&p->mutex);
		stats[i] = p->stats;
		pthread_mutex_unlock(&p->mutex);
		have[i] = true;
	}

	const struct face_stats *ref = have[0] ? &stats[0] : NULL;
	float row_y = 0.0f;
	for (int i = 0; i < FC_CAMS; i++) {
		if (!src->probes[i])
			continue;
		struct probe_source *p = obs_obj_get_data(src->probes[i]);
		const float y0 = row_y + 6.0f * ts.uy, y1 = row_y + FC_ROW - 6.0f * ts.uy;
		row_y += FC_ROW;
		if (i > 0)
			fill(8.0f * ts.ux, y0 - 6.0f * ts.uy, W - 8.0f * ts.ux, y0 - 5.0f * ts.uy, 0xFF303036);

		// thumbnail with the faces marked
		const float th = y1 - y0, tw = th * 16.0f / 9.0f;
		const float tx = 8.0f * ts.ux;
		fill(tx, y0, tx + tw, y1, 0xFF000000);
		gs_texture_t *tex = p ? cm_bypass_get_texture(&p->cm) : NULL;
		const uint32_t cw = p ? cm_bypass_get_width(&p->cm) : 0, ch = p ? cm_bypass_get_height(&p->cm) : 0;
		if (tex && cw && ch) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
			gs_matrix_push();
			gs_matrix_translate3f(tx, y0, 0.0f);
			gs_matrix_scale3f(tw / cw, th / ch, 1.0f);
			while (gs_effect_loop(def, "Draw"))
				gs_draw_sprite_subregion(tex, 0, 0, 0, cw, ch);
			gs_matrix_pop();
		}
		if (have[i]) {
			for (int k = 0; k < stats[i].n_faces; k++) {
				const struct face_box *f = stats[i].faces + k;
				outline(tx + f->x * tw, y0 + f->y * th, tx + (f->x + f->w) * tw,
					y0 + (f->y + f->h) * th, k == 0 ? 0xFF6FCF86 : 0xFFB8B8C0);
			}
		}

		// text
		const float x = tx + tw + 12.0f * ts.ux;
		const float swatch = 40.0f * ts.ux;
		const float text_w = W - x - swatch - 24.0f * ts.ux;
		char buf[1024];
		const char *name = src->names[i] && *src->names[i] ? src->names[i]
								   : obs_module_text("Faces.DockSelection");
		if (is_program_name(src->names[i]))
			name = "Program";
		else if (is_roi_source_name(src->names[i]))
			name = obs_module_text("Faces.DockSelection");
		if (!have[i] || !stats[i].valid) {
			snprintf(buf, sizeof(buf), "Camera %d  %s  -  %s", i + 1, name, obs_module_text("QC.NoSignal"));
			text_draw(&ts, buf, x, y0, 13.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
			continue;
		}
		const struct face_stats *s = &stats[i];
		snprintf(buf, sizeof(buf), "Camera %d  %s  \xe2\x80\xa2  %d %s", i + 1, name, s->n_faces,
			 s->n_faces == 1 ? obs_module_text("Faces.FaceFound") : obs_module_text("Faces.FacesFound"));
		float y = y0;
		text_draw(&ts, buf, x, y, 13.0f, 0xFFF0F0F2, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		y += 18.0f * ts.uy;
		snprintf(buf, sizeof(buf),
			 "Scene luma: median %.1f%%, P10-P90 %.1f-%.1f, P99 %.0f; near black %.0f%%, "
			 "near clipping %.0f%%.",
			 s->median, s->p10, s->p90, s->p99, s->near_black, s->near_clip);
		y = wrapped(&ts, buf, x, y, text_w, 11.5f, 0xFFD8D8DC, y1);
		assessment(s, i > 0 ? ref : NULL, i + 1, src->advice, buf, sizeof(buf));
		wrapped(&ts, buf, x, y, text_w, 11.5f, 0xFFD8D8DC, y1);

		// average skin color
		if (s->skin_valid) {
			const uint32_t c = 0xFF000000 | ((uint32_t)s->skin_rgb[0] << 16) |
					   ((uint32_t)s->skin_rgb[1] << 8) | s->skin_rgb[2];
			fill(W - swatch - 12.0f * ts.ux, y0 + 20.0f * ts.uy, W - 12.0f * ts.ux,
			     y0 + 20.0f * ts.uy + swatch * ts.uy / ts.ux, c);
		}
	}
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

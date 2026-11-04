/*
 * 3D color cube: the pixels as points in a 3D color space, each in its own color, with the black-white axis
 * upright: the RGB cube, Y'CbCr (a HectorScope: luma up, the chroma plane across), HSV or CIE L*a*b*.
 * It turns slowly; drag to turn it yourself, Shift + drag to move it, wheel to zoom.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define CB_SIZE 360
#define CB_MAX_POINTS 16384

struct cb_source
{
	struct cm_source cm;

	pthread_mutex_t mutex;
	uint8_t points[CB_MAX_POINTS][3]; // R, G, B
	uint32_t n_points;

	gs_vertbuffer_t *vb;

	float yaw, pitch, zoom; // degrees, degrees, factor
	float spin;             // degrees per second
	bool dragging;
	int drag_x, drag_y;
	float drag_yaw, drag_pitch;
	float point_size;
	bool edges;
	int model;          // cb_model_e
	float pan_x, pan_y; // in tile units
	bool panning;
	int pan_mx, pan_my;
	float pan_x0, pan_y0;
};

enum cb_model_e {
	CB_RGB = 0,
	CB_YCBCR = 1, // HectorScope
	CB_HSV = 2,
	CB_LAB = 3,
};

static void cb_update(void *, obs_data_t *);
static void cb_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *cb_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Cube");
}

static void *cb_create(obs_data_t *settings, obs_source_t *source)
{
	struct cb_source *src = bzalloc(sizeof(struct cb_source));
	pthread_mutex_init(&src->mutex, NULL);
	src->cm.flags = CM_FLAG_CONVERT_RGB;
	src->yaw = 30.0f;
	src->pitch = 20.0f;
	src->zoom = 1.0f;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, cb_surface_cb, src);
	cb_update(src, settings);
	return src;
}

static void cb_destroy(void *data)
{
	struct cb_source *src = data;
	obs_enter_graphics();
	vertexbuffer_destroy_safe(src->vb);
	obs_leave_graphics();
	cm_destroy(&src->cm);
	pthread_mutex_destroy(&src->mutex);
	bfree(src);
}

static void cb_update(void *data, obs_data_t *settings)
{
	struct cb_source *src = data;
	cm_update(&src->cm, settings);
	src->spin = (float)obs_data_get_double(settings, "spin");
	src->point_size = (float)obs_data_get_double(settings, "point_size");
	src->edges = obs_data_get_bool(settings, "edges");
	src->model = (int)obs_data_get_int(settings, "cube_model");
}

static bool reset_view_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct cb_source *src = data;
	src->yaw = 30.0f;
	src->pitch = 20.0f;
	src->zoom = 1.0f;
	src->pan_x = src->pan_y = 0.0f;
	return false;
}

static void cb_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 4);
	obs_data_set_default_double(settings, "spin", 15.0);
	obs_data_set_default_double(settings, "point_size", 1.5);
	obs_data_set_default_bool(settings, "edges", true);
}

static obs_properties_t *cb_get_properties(void *data)
{
	struct cb_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_properties_add_float_slider(props, "spin", obs_module_text("Cube.Prop.Spin"), 0.0, 90.0, 1.0);
	obs_properties_add_float_slider(props, "point_size", obs_module_text("Cube.Prop.PointSize"), 1.0, 4.0, 0.5);
	obs_properties_add_bool(props, "edges", obs_module_text("Cube.Prop.Edges"));
	obs_property_t *p = obs_properties_add_list(props, "cube_model", obs_module_text("Cube.Prop.Model"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Cube.Model.RGB"), CB_RGB);
	obs_property_list_add_int(p, obs_module_text("Cube.Model.YCbCr"), CB_YCBCR);
	obs_property_list_add_int(p, obs_module_text("Cube.Model.HSV"), CB_HSV);
	obs_property_list_add_int(p, obs_module_text("Cube.Model.Lab"), CB_LAB);
	obs_properties_add_button2(props, "cube_reset", obs_module_text("Cube.Prop.Reset"), reset_view_clicked, src);
	return props;
}

static uint32_t cb_get_width(void *data)
{
	struct cb_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : CB_SIZE;
}

static uint32_t cb_get_height(void *data)
{
	struct cb_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : CB_SIZE;
}

static void cb_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct cb_source *src = data;
	if (!sd->rgb_data || !sd->width || !sd->height)
		return;
	// an even spread of at most CB_MAX_POINTS pixels
	const uint64_t total = (uint64_t)sd->width * sd->height;
	uint32_t step = (uint32_t)((total + CB_MAX_POINTS - 1) / CB_MAX_POINTS);
	if (step < 1)
		step = 1;
	pthread_mutex_lock(&src->mutex);
	uint32_t n = 0;
	for (uint64_t i = 0; i < total && n < CB_MAX_POINTS; i += step) {
		const uint32_t x = (uint32_t)(i % sd->width), y = (uint32_t)(i / sd->width);
		const uint8_t *p = sd->rgb_data + (size_t)sd->linesize * y + x * 4;
		if (!p[3])
			continue;
		src->points[n][0] = p[2];
		src->points[n][1] = p[1];
		src->points[n][2] = p[0];
		n++;
	}
	src->n_points = n;
	pthread_mutex_unlock(&src->mutex);
}

/* Rotation that stands the cube on its black corner (grey axis up), then yaw and pitch. */
static void make_rotation(float yaw_deg, float pitch_deg, float m[9])
{
	// align (1,1,1)/sqrt(3) with +y: rotate about axis (-1,0,1)/sqrt(2) by acos(1/sqrt(3))
	const float ang = acosf(1.0f / sqrtf(3.0f));
	const float ax = -1.0f / sqrtf(2.0f), ay = 0.0f, az = 1.0f / sqrtf(2.0f);
	const float c = cosf(ang), s = sinf(ang), t = 1.0f - c;
	const float r0[9] = {
		t * ax * ax + c,      t * ax * ay - s * az, t * ax * az + s * ay, t * ax * ay + s * az, t * ay * ay + c,
		t * ay * az - s * ax, t * ax * az - s * ay, t * ay * az + s * ax, t * az * az + c,
	};
	const float cy = cosf(yaw_deg * (float)M_PI / 180.0f), sy = sinf(yaw_deg * (float)M_PI / 180.0f);
	const float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
	const float cp = cosf(pitch_deg * (float)M_PI / 180.0f), sp = sinf(pitch_deg * (float)M_PI / 180.0f);
	const float rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
	float a[9];
	// m = rx * ry * r0
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			a[i * 3 + j] =
				ry[i * 3 + 0] * r0[0 + j] + ry[i * 3 + 1] * r0[3 + j] + ry[i * 3 + 2] * r0[6 + j];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			m[i * 3 + j] = rx[i * 3 + 0] * a[0 + j] + rx[i * 3 + 1] * a[3 + j] + rx[i * 3 + 2] * a[6 + j];
}

static float lab_f(float t)
{
	return t > 0.008856f ? cbrtf(t) : 7.787f * t + 16.0f / 116.0f;
}

static float srgb_lin(float v)
{
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

/* A color in the chosen model, centered on the middle of the black-white axis (0, 0, 0), with that axis along
 * y: x and z across, about -0.5..0.5. */
static void model_coords(int model, float r, float g, float b, float *x, float *y, float *z)
{
	switch (model) {
	case CB_YCBCR: {
		const float yy = 0.2126f * r + 0.7152f * g + 0.0722f * b;
		*x = (b - yy) / 1.8556f;
		*z = (r - yy) / 1.5748f;
		*y = yy - 0.5f;
		break;
	}
	case CB_HSV: {
		const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), c = mx - mn;
		float h = 0.0f;
		if (c > 1e-5f) {
			if (mx == r)
				h = fmodf((g - b) / c + 6.0f, 6.0f);
			else if (mx == g)
				h = (b - r) / c + 2.0f;
			else
				h = (r - g) / c + 4.0f;
		}
		const float sat = mx > 1e-5f ? c / mx : 0.0f, a = h * (float)M_PI / 3.0f;
		*x = 0.5f * sat * cosf(a);
		*z = -0.5f * sat * sinf(a);
		*y = mx - 0.5f;
		break;
	}
	case CB_LAB: {
		const float lr = srgb_lin(r), lg = srgb_lin(g), lb = srgb_lin(b);
		const float X = (0.4124f * lr + 0.3576f * lg + 0.1805f * lb) / 0.95047f;
		const float Y = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
		const float Z = (0.0193f * lr + 0.1192f * lg + 0.9505f * lb) / 1.08883f;
		const float fx = lab_f(X), fy = lab_f(Y), fz = lab_f(Z);
		*y = (116.0f * fy - 16.0f) / 100.0f - 0.5f;
		*x = 500.0f * (fx - fy) / 200.0f;
		*z = -200.0f * (fy - fz) / 200.0f;
		break;
	}
	default: // RGB: make_rotation stands the cube on its black corner
		*x = r - 0.5f;
		*y = g - 0.5f;
		*z = b - 0.5f;
		break;
	}
}

/* Rotation for the cylinder models: only yaw and pitch, the black-white axis is y already. */
static void make_rotation_upright(float yaw_deg, float pitch_deg, float m[9])
{
	const float cy = cosf(yaw_deg * (float)M_PI / 180.0f), sy = sinf(yaw_deg * (float)M_PI / 180.0f);
	const float cp = cosf(pitch_deg * (float)M_PI / 180.0f), sp = sinf(pitch_deg * (float)M_PI / 180.0f);
	// m = rx * ry
	const float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
	const float rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			m[i * 3 + j] =
				rx[i * 3 + 0] * ry[0 + j] + rx[i * 3 + 1] * ry[3 + j] + rx[i * 3 + 2] * ry[6 + j];
}

static inline void project_model(const struct cb_source *src, const float m[9], float r, float g, float b, float scale,
				 float *x, float *y)
{
	float a, c, d;
	model_coords(src->model, r, g, b, &a, &c, &d);
	const float px = m[0] * a + m[1] * c + m[2] * d;
	const float py = m[3] * a + m[4] * c + m[5] * d;
	*x = CB_SIZE * 0.5f + px * scale + src->pan_x * CB_SIZE;
	*y = CB_SIZE * 0.5f - py * scale + src->pan_y * CB_SIZE;
}

static void cb_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct cb_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	float m[9];
	const bool rgb = src->model == CB_RGB;
	if (rgb)
		make_rotation(src->yaw, src->pitch, m);
	else
		make_rotation_upright(src->yaw, src->pitch, m);
	// the grey axis of the cube is sqrt(3) long; the cylinders are 1 high and 1 across
	const float scale = CB_SIZE * (rgb ? 0.5f / 0.95f : 0.62f) * src->zoom;
	const struct text_scale ts = text_scale_from_viewport((float)CB_SIZE, (float)CB_SIZE);

	// points, each a small cross so the size shows at any tile size
	pthread_mutex_lock(&src->mutex);
	const uint32_t n = src->n_points;
	const int per = src->point_size > 1.25f ? 4 : 1; // vertices per point
	if (!src->vb) {
		struct gs_vb_data *vbd = gs_vbdata_create();
		vbd->num = CB_MAX_POINTS * 4;
		vbd->points = bzalloc(sizeof(struct vec3) * vbd->num);
		vbd->colors = bzalloc(sizeof(uint32_t) * vbd->num);
		src->vb = gs_vertexbuffer_create(vbd, GS_DYNAMIC);
	}
	struct gs_vb_data *vbd = src->vb ? gs_vertexbuffer_get_data(src->vb) : NULL;
	uint32_t nv = 0;
	if (vbd) {
		const float d = src->point_size * 0.5f;
		for (uint32_t i = 0; i < n; i++) {
			const uint8_t *p = src->points[i];
			float x, y;
			project_model(src, m, p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, scale, &x, &y);
			const uint32_t color = 0xFF000000 | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
			if (per == 1) {
				vec3_set(vbd->points + nv, x, y, 0.0f);
				vbd->colors[nv++] = color;
			} else {
				vec3_set(vbd->points + nv, x - d * ts.ux, y, 0.0f);
				vbd->colors[nv++] = color;
				vec3_set(vbd->points + nv, x + d * ts.ux, y, 0.0f);
				vbd->colors[nv++] = color;
				vec3_set(vbd->points + nv, x, y - d * ts.uy, 0.0f);
				vbd->colors[nv++] = color;
				vec3_set(vbd->points + nv, x, y + d * ts.uy, 0.0f);
				vbd->colors[nv++] = color;
			}
		}
	}
	pthread_mutex_unlock(&src->mutex);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (nv) {
		gs_vertexbuffer_flush(src->vb);
		gs_load_vertexbuffer(src->vb);
		gs_load_indexbuffer(NULL);
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFFFFFFFF);
		while (gs_effect_loop(solid, "SolidColored"))
			gs_draw(per == 1 ? GS_POINTS : GS_LINES, 0, nv);
		gs_load_vertexbuffer(NULL);
	}

	if (src->edges && !rgb) {
		// the cylinder: rings at the bottom, middle and top, the axis, and the primaries on the outside
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x60FFFFFF);
		const float rr = src->model == CB_YCBCR ? 0.35f : 0.5f;
		while (gs_effect_loop(solid, "Solid")) {
			for (int ring = 0; ring < 3; ring++) {
				const float yy = -0.5f + 0.5f * ring;
				gs_render_start(false);
				for (int i = 0; i < 72; i++) {
					for (int k = 0; k < 2; k++) {
						const float a = (i + k) * 2.0f * (float)M_PI / 72.0f;
						const float px = m[0] * rr * cosf(a) + m[1] * yy + m[2] * rr * sinf(a);
						const float py = m[3] * rr * cosf(a) + m[4] * yy + m[5] * rr * sinf(a);
						gs_vertex2f(CB_SIZE * 0.5f + px * scale + src->pan_x * CB_SIZE,
							    CB_SIZE * 0.5f - py * scale + src->pan_y * CB_SIZE);
					}
				}
				gs_render_stop(GS_LINES);
			}
			gs_render_start(false);
			float x0, y0, x1, y1;
			project_model(src, m, 0, 0, 0, scale, &x0, &y0);
			project_model(src, m, 1, 1, 1, scale, &x1, &y1);
			gs_vertex2f(x0, y0);
			gs_vertex2f(x1, y1);
			gs_render_stop(GS_LINES);
		}
		static const float prim[6][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};
		static const char *pn[6] = {"R", "Y", "G", "C", "B", "M"};
		static const uint32_t pc[6] = {0xFFFF6060, 0xFFFFFF60, 0xFF60E060, 0xFF60FFFF, 0xFF6090FF, 0xFFFF60FF};
		for (int k = 0; k < 6; k++) {
			float x, y;
			project_model(src, m, prim[k][0], prim[k][1], prim[k][2], scale, &x, &y);
			text_draw(&ts, pn[k], x + 4.0f * ts.ux, y, 11.0f, pc[k], TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
		}
		static const char *names[4] = {"R G B", "Y' Cb Cr", "H S V", "L* a* b*"};
		text_draw(&ts, names[src->model], 4.0f * ts.ux, 4.0f * ts.uy, 11.0f, 0xC0FFFFFF, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_TOP);
	}
	if (src->edges && rgb) {
		static const float corners[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
						    {1, 1, 0}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
		static const int edges[12][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 4}, {1, 5}, {2, 4},
						 {2, 6}, {3, 5}, {3, 6}, {4, 7}, {5, 7}, {6, 7}};
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x60FFFFFF);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			for (int e = 0; e < 12; e++) {
				for (int k = 0; k < 2; k++) {
					const float *c = corners[edges[e][k]];
					float x, y;
					project_model(src, m, c[0], c[1], c[2], scale, &x, &y);
					gs_vertex2f(x, y);
				}
			}
			// grey axis
			float x0, y0, x1, y1;
			project_model(src, m, 0, 0, 0, scale, &x0, &y0);
			project_model(src, m, 1, 1, 1, scale, &x1, &y1);
			gs_vertex2f(x0, y0);
			gs_vertex2f(x1, y1);
			gs_render_stop(GS_LINES);
		}
		static const char *names[8] = {"K", "R", "G", "B", "Y", "M", "C", "W"};
		static const uint32_t colors[8] = {0xFF909090, 0xFFFF6060, 0xFF60E060, 0xFF6090FF,
						   0xFFFFFF60, 0xFFFF60FF, 0xFF60FFFF, 0xFFFFFFFF};
		for (int k = 0; k < 8; k++) {
			float x, y;
			project_model(src, m, corners[k][0], corners[k][1], corners[k][2], scale, &x, &y);
			text_draw(&ts, names[k], x + 4.0f * ts.ux, y, 11.0f, colors[k], TEXT_ALIGN_LEFT,
				  TEXT_VALIGN_MIDDLE);
		}
	}
}

static void cb_tick(void *data, float seconds)
{
	struct cb_source *src = data;
	cm_tick(&src->cm, seconds);
	if (!src->dragging && src->spin > 0.0f)
		src->yaw = fmodf(src->yaw + src->spin * seconds, 360.0f);
}

static void cb_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			   uint32_t click_count)
{
	UNUSED_PARAMETER(click_count);
	struct cb_source *src = data;
	if (type != MOUSE_LEFT)
		return;
	if (!mouse_up && (event->modifiers & INTERACT_SHIFT_KEY)) {
		// Shift + drag moves the view
		src->panning = true;
		src->pan_mx = event->x;
		src->pan_my = event->y;
		src->pan_x0 = src->pan_x;
		src->pan_y0 = src->pan_y;
		return;
	}
	if (mouse_up)
		src->panning = false;
	if (!mouse_up) {
		src->dragging = true;
		src->drag_x = event->x;
		src->drag_y = event->y;
		src->drag_yaw = src->yaw;
		src->drag_pitch = src->pitch;
	} else {
		src->dragging = false;
	}
}

static void cb_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct cb_source *src = data;
	// The dock's display window can report a short "leave" in the middle of a drag; the drag only ends
	// when the button is let go, so the cube does not jump.
	const bool held = (event->modifiers & INTERACT_MOUSE_LEFT) != 0;
	if (src->panning) {
		if (!held) {
			src->panning = false;
			return;
		}
		src->pan_x = src->pan_x0 + (event->x - src->pan_mx) / (float)CB_SIZE;
		src->pan_y = src->pan_y0 + (event->y - src->pan_my) / (float)CB_SIZE;
		return;
	}
	if (!held) {
		if (mouse_leave)
			src->dragging = false;
		return;
	}
	if (!src->dragging) {
		// the button came back into the tile still held: carry on from here
		src->dragging = true;
		src->drag_x = event->x;
		src->drag_y = event->y;
		src->drag_yaw = src->yaw;
		src->drag_pitch = src->pitch;
		return;
	}
	src->yaw = src->drag_yaw + (event->x - src->drag_x) * 0.5f;
	float pitch = src->drag_pitch + (event->y - src->drag_y) * 0.5f;
	src->pitch = pitch < -89.0f ? -89.0f : pitch > 89.0f ? 89.0f : pitch;
}

static void cb_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	UNUSED_PARAMETER(event);
	UNUSED_PARAMETER(x_delta);
	struct cb_source *src = data;
	float z = src->zoom * (y_delta > 0 ? 1.15f : y_delta < 0 ? 1.0f / 1.15f : 1.0f);
	src->zoom = z < 0.5f ? 0.5f : z > 4.0f ? 4.0f : z;
}

const struct obs_source_info colormonitor_cube = {
	.id = ID_PREFIX "cube_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION,
	.get_name = cb_get_name,
	.create = cb_create,
	.destroy = cb_destroy,
	.update = cb_update,
	.get_defaults = cb_get_defaults,
	.get_properties = cb_get_properties,
	.get_width = cb_get_width,
	.get_height = cb_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = cb_render,
	.video_tick = cb_tick,
	.mouse_click = cb_mouse_click,
	.mouse_move = cb_mouse_move,
	.mouse_wheel = cb_mouse_wheel,
};

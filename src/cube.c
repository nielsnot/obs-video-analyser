/*
 * 3D color cube: the pixels as points in the RGB cube, each in its own color, with the
 * black-white axis upright. It turns slowly; drag to turn it yourself, wheel to zoom.
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

static inline void project(const float m[9], float r, float g, float b, float scale, float *x, float *y, float *z)
{
	r -= 0.5f;
	g -= 0.5f;
	b -= 0.5f;
	const float px = m[0] * r + m[1] * g + m[2] * b;
	const float py = m[3] * r + m[4] * g + m[5] * b;
	const float pz = m[6] * r + m[7] * g + m[8] * b;
	*x = CB_SIZE * 0.5f + px * scale;
	*y = CB_SIZE * 0.5f - py * scale;
	if (z)
		*z = pz;
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
	make_rotation(src->yaw, src->pitch, m);
	const float scale = CB_SIZE * 0.5f / 0.95f * src->zoom; // the grey axis is sqrt(3) long
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
			project(m, p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, scale, &x, &y, NULL);
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

	if (src->edges) {
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
					project(m, c[0], c[1], c[2], scale, &x, &y, NULL);
					gs_vertex2f(x, y);
				}
			}
			// grey axis
			float x0, y0, x1, y1;
			project(m, 0, 0, 0, scale, &x0, &y0, NULL);
			project(m, 1, 1, 1, scale, &x1, &y1, NULL);
			gs_vertex2f(x0, y0);
			gs_vertex2f(x1, y1);
			gs_render_stop(GS_LINES);
		}
		static const char *names[8] = {"K", "R", "G", "B", "Y", "M", "C", "W"};
		static const uint32_t colors[8] = {0xFF909090, 0xFFFF6060, 0xFF60E060, 0xFF6090FF,
						   0xFFFFFF60, 0xFFFF60FF, 0xFF60FFFF, 0xFFFFFFFF};
		for (int k = 0; k < 8; k++) {
			float x, y;
			project(m, corners[k][0], corners[k][1], corners[k][2], scale, &x, &y, NULL);
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
	if (mouse_leave) {
		src->dragging = false;
		return;
	}
	if (!src->dragging)
		return;
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

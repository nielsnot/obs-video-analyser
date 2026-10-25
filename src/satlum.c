/*
 * Sat / Lum plot: every pixel plotted by its saturation (across) and luma (up).
 * Shows at a glance whether highlights or shadows carry too much color.
 * Saturation is the chroma distance from grey in the analyzed signal, 100 % at the
 * edge of the vectorscope's outer ring.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <math.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define SL_SIZE 256

struct sl_source
{
	struct cm_source cm;

	gs_texture_t *tex;
	uint8_t *tex_buf[2];
	volatile int w_tex_buf;
	gs_effect_t *effect;

	int intensity;
	bool colored;
	bool graticule;
};

static void sl_update(void *, obs_data_t *);
static void sl_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *sl_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SatLum");
}

static void *sl_create(obs_data_t *settings, obs_source_t *source)
{
	struct sl_source *src = bzalloc(sizeof(struct sl_source));
	src->cm.flags = CM_FLAG_CONVERT_YUV;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, sl_surface_cb, src);

	obs_enter_graphics();
	src->effect = create_effect_from_module_file("vectorscope.effect");
	obs_leave_graphics();

	sl_update(src, settings);
	return src;
}

static void sl_destroy(void *data)
{
	struct sl_source *src = data;
	obs_enter_graphics();
	gs_texture_destroy(src->tex);
	obs_leave_graphics();
	cm_destroy(&src->cm);
	bfree(src->tex_buf[0]);
	bfree(src->tex_buf[1]);
	bfree(src);
}

static void sl_update(void *data, obs_data_t *settings)
{
	struct sl_source *src = data;
	cm_update(&src->cm, settings);
	src->intensity = (int)obs_data_get_int(settings, "intensity");
	if (src->intensity < 1)
		src->intensity = 1;
	src->colored = obs_data_get_bool(settings, "colored");
	src->graticule = obs_data_get_bool(settings, "show_graticule");
}

static void sl_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "intensity", 25);
	obs_data_set_default_bool(settings, "colored", true);
	obs_data_set_default_bool(settings, "show_graticule", true);
}

static obs_properties_t *sl_get_properties(void *data)
{
	struct sl_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);
	obs_properties_add_bool(props, "colored", obs_module_text("SatLum.Prop.Colored"));
	obs_properties_add_bool(props, "show_graticule", obs_module_text("Graticule"));
	return props;
}

static uint32_t sl_get_width(void *data)
{
	struct sl_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : SL_SIZE;
}

static uint32_t sl_get_height(void *data)
{
	struct sl_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : SL_SIZE;
}

static void sl_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct sl_source *src = data;
	if (!sd->yuv_data || !sd->width)
		return;
	if (!src->tex_buf[src->w_tex_buf])
		src->tex_buf[src->w_tex_buf] = bzalloc(SL_SIZE * SL_SIZE);
	uint8_t *dbuf = src->tex_buf[src->w_tex_buf];
	memset(dbuf, 0, SL_SIZE * SL_SIZE);

	for (uint32_t y = 0; y < sd->height; y++) {
		const uint8_t *p = sd->yuv_data + (size_t)sd->linesize * y;
		for (uint32_t x = 0; x < sd->width; x++, p += 4) {
			if (!p[3])
				continue;
			// p: V, Y, U (BGRA order of the conversion: z=U in B, y=Y in G, x=V in R)
			const int u = p[0] - 128, v = p[2] - 128, l = p[1];
			int s = (int)(sqrtf((float)(u * u + v * v)) * 2.0f); // 127 -> 254
			if (s > 255)
				s = 255;
			uint8_t *c = dbuf + (SL_SIZE - 1 - l) * SL_SIZE + s;
			if (*c < 255)
				++*c;
		}
	}
	src->w_tex_buf ^= 1;
}

static void sl_render_graticule(void)
{
	const uint32_t color = 0x80FFBF00;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		for (int i = 1; i < 4; i++) {
			const float t = SL_SIZE * i / 4.0f;
			gs_vertex2f(0.0f, t);
			gs_vertex2f((float)SL_SIZE, t);
			gs_vertex2f(t, 0.0f);
			gs_vertex2f(t, (float)SL_SIZE);
		}
		gs_render_stop(GS_LINES);
	}
	const struct text_scale ts = text_scale_from_viewport((float)SL_SIZE, (float)SL_SIZE);
	const float px = 10.0f, m = 3.0f * ts.ux;
	text_draw(&ts, "Luma", m, m, px, 0xE0FFD060, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	text_draw(&ts, "Saturation", SL_SIZE - m, SL_SIZE - m, px, 0xE0FFD060, TEXT_ALIGN_RIGHT, TEXT_VALIGN_BOTTOM);
	text_draw(&ts, "50%", m, SL_SIZE * 0.5f, px, 0xA0FFD060, TEXT_ALIGN_LEFT, TEXT_VALIGN_BOTTOM);
	text_draw(&ts, "50%", SL_SIZE * 0.5f + m, SL_SIZE - m, px, 0xA0FFD060, TEXT_ALIGN_LEFT, TEXT_VALIGN_BOTTOM);
}

static void sl_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct sl_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	const int r = src->w_tex_buf ^ 1;
	if (src->tex_buf[r] && src->effect) {
		const uint8_t *buf = src->tex_buf[r];
		if (!src->tex)
			src->tex = gs_texture_create(SL_SIZE, SL_SIZE, GS_R8, 1, &buf, GS_DYNAMIC);
		else
			gs_texture_set_image(src->tex, buf, SL_SIZE, false);

		gs_effect_t *e = src->effect;
		struct vec4 color;
		struct vec3 cu, cv;
		vec3_zero(&cv);
		if (src->colored) {
			// from white at no saturation towards orange-red at full saturation
			vec4_set(&color, 0.75f, 0.85f, 0.95f, 1.0f);
			vec3_set(&cu, 0.25f, -0.25f, -0.45f);
		} else {
			vec4_set(&color, 1.0f, 1.0f, 1.0f, 1.0f);
			vec3_zero(&cu);
		}
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), src->tex);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "intensity"), (float)src->intensity);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, "color"), &color);
		gs_effect_set_vec3(gs_effect_get_param_by_name(e, "color_u"), &cu);
		gs_effect_set_vec3(gs_effect_get_param_by_name(e, "color_v"), &cv);
		while (gs_effect_loop(e, "Draw"))
			gs_draw_sprite(src->tex, 0, SL_SIZE, SL_SIZE);
	}
	if (src->graticule)
		sl_render_graticule();
}

const struct obs_source_info colormonitor_satlum = {
	.id = ID_PREFIX "satlum_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = sl_get_name,
	.create = sl_create,
	.destroy = sl_destroy,
	.update = sl_update,
	.get_defaults = sl_get_defaults,
	.get_properties = sl_get_properties,
	.get_width = sl_get_width,
	.get_height = sl_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = sl_render,
	.video_tick = cm_tick,
};

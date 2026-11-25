#include <obs-module.h>
#include <math.h>
#include "draw-util.h"

static gs_effect_t *solid_with(uint32_t argb)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), argb);
	return solid;
}

void du_fill_rect(float x0, float y0, float x1, float y1, uint32_t argb)
{
	gs_effect_t *solid = solid_with(argb);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(x0, y0), gs_vertex2f(x1, y0), gs_vertex2f(x0, y1);
		gs_vertex2f(x1, y0), gs_vertex2f(x1, y1), gs_vertex2f(x0, y1);
		gs_render_stop(GS_TRIS);
	}
}

void du_line(float x0, float y0, float x1, float y1, uint32_t argb)
{
	gs_effect_t *solid = solid_with(argb);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(x0, y0), gs_vertex2f(x1, y1);
		gs_render_stop(GS_LINES);
	}
}

void du_rect(float x0, float y0, float x1, float y1, uint32_t argb)
{
	gs_effect_t *solid = solid_with(argb);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(x0, y0), gs_vertex2f(x1, y0);
		gs_vertex2f(x1, y0), gs_vertex2f(x1, y1);
		gs_vertex2f(x1, y1), gs_vertex2f(x0, y1);
		gs_vertex2f(x0, y1), gs_vertex2f(x0, y0);
		gs_render_stop(GS_LINES);
	}
}

void du_circle(float cx, float cy, float r, uint32_t argb, bool filled)
{
	const int n = 48;
	gs_effect_t *solid = solid_with(argb);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		for (int i = 0; i < n; i++) {
			const float a0 = 2.0f * 3.14159265f * i / n, a1 = 2.0f * 3.14159265f * (i + 1) / n;
			if (filled)
				gs_vertex2f(cx, cy);
			gs_vertex2f(cx + r * cosf(a0), cy + r * sinf(a0));
			gs_vertex2f(cx + r * cosf(a1), cy + r * sinf(a1));
		}
		gs_render_stop(filled ? GS_TRIS : GS_LINES);
	}
}

uint32_t du_argb(float r, float g, float b, float a)
{
	const float c[4] = {a, r, g, b};
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		float x = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];
		v = v << 8 | (uint32_t)(x * 255.0f + 0.5f);
	}
	return v;
}

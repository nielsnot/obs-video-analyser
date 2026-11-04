/*
 * CIE 1931 chromaticity diagram.
 *
 * Each pixel is converted from Rec. 709 R'G'B' (or the Rec. 2100 HLG signal with Rec. 2020
 * primaries) to linear light, then to CIE XYZ and plotted at
 * its xy chromaticity. The spectral locus, the Rec. 709, DCI-P3 and Rec. 2020 gamut triangles
 * and the D65 white point are drawn as reference.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <math.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define CIE_W 256
#define CIE_H 288 // same scale on both axes: 0.8 across, 0.9 up
#define CIE_X_MAX 0.8f
#define CIE_Y_MAX 0.9f

struct cie_source
{
	struct cm_source cm;

	gs_texture_t *tex;
	uint8_t *tex_buf[2];
	volatile int w_tex_buf;
	gs_effect_t *effect;
	float lin[256];     // R'G'B' code value to linear light
	float lin_hlg[256]; // HLG signal code value to scene light
	float lin_pq[256];  // PQ signal code value to display light

	int intensity;
	bool show_709, show_p3, show_2020, show_grid;
	bool show_ap0, show_ap1, show_prophoto, show_adobe, show_planck;
	bool uv; // CIE 1976 u'v' instead of 1931 xy
};

/* The scale of the u'v' diagram: the same on both axes */
#define UV_MAX 0.65f

/* xy to a position in the diagram (pixels), in xy or in u'v'. */
static inline void to_screen(bool uv, float x, float y, float *px, float *py)
{
	if (uv) {
		const float d = -2.0f * x + 12.0f * y + 3.0f;
		const float u = d != 0.0f ? 4.0f * x / d : 0.0f, v = d != 0.0f ? 9.0f * y / d : 0.0f;
		*px = u / UV_MAX * CIE_W;
		*py = (1.0f - v / (UV_MAX * CIE_H / CIE_W)) * CIE_H;
	} else {
		*px = x / CIE_X_MAX * CIE_W;
		*py = (1.0f - y / CIE_Y_MAX) * CIE_H;
	}
}

static void cie_update(void *, obs_data_t *);
static void cie_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *cie_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("CIE");
}

static void *cie_create(obs_data_t *settings, obs_source_t *source)
{
	struct cie_source *src = bzalloc(sizeof(struct cie_source));

	// BT.1886-like display gamma
	for (int i = 0; i < 256; i++)
		src->lin[i] = powf(i / 255.0f, 2.4f);
	// inverse HLG OETF; the OOTF scales R, G and B alike and leaves the chromaticity as it is
	for (int i = 0; i < 256; i++) {
		src->lin_hlg[i] = hlg_to_scene(i / 255.0f);
		src->lin_pq[i] = pq_to_linear(i / 255.0f);
	}

	src->cm.flags = CM_FLAG_CONVERT_RGB;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, cie_surface_cb, src);

	obs_enter_graphics();
	src->effect = create_effect_from_module_file("cie.effect");
	obs_leave_graphics();

	cie_update(src, settings);
	return src;
}

static void cie_destroy(void *data)
{
	struct cie_source *src = data;

	obs_enter_graphics();
	gs_texture_destroy(src->tex);
	obs_leave_graphics();

	cm_destroy(&src->cm);
	bfree(src->tex_buf[0]);
	bfree(src->tex_buf[1]);
	bfree(src);
}

static void cie_update(void *data, obs_data_t *settings)
{
	struct cie_source *src = data;
	cm_update(&src->cm, settings);

	src->intensity = (int)obs_data_get_int(settings, "intensity");
	if (src->intensity < 1)
		src->intensity = 1;
	src->show_709 = obs_data_get_bool(settings, "show_709");
	src->show_p3 = obs_data_get_bool(settings, "show_p3");
	src->show_2020 = obs_data_get_bool(settings, "show_2020");
	src->show_grid = obs_data_get_bool(settings, "show_graticule");
	src->show_ap0 = obs_data_get_bool(settings, "show_ap0");
	src->show_ap1 = obs_data_get_bool(settings, "show_ap1");
	src->show_prophoto = obs_data_get_bool(settings, "show_prophoto");
	src->show_adobe = obs_data_get_bool(settings, "show_adobe");
	src->show_planck = obs_data_get_bool(settings, "show_planck");
	src->uv = obs_data_get_bool(settings, "cie_uv");
}

static void cie_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "intensity", 40);
	obs_data_set_default_bool(settings, "show_709", true);
	obs_data_set_default_bool(settings, "show_p3", false);
	obs_data_set_default_bool(settings, "show_2020", true);
	obs_data_set_default_bool(settings, "show_graticule", true);
}

static obs_properties_t *cie_get_properties(void *data)
{
	struct cie_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);

	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);
	obs_properties_add_bool(props, "show_709", obs_module_text("CIE.Prop.709"));
	obs_properties_add_bool(props, "show_p3", obs_module_text("CIE.Prop.P3"));
	obs_properties_add_bool(props, "show_2020", obs_module_text("CIE.Prop.2020"));
	obs_properties_add_bool(props, "show_adobe", obs_module_text("CIE.Prop.Adobe"));
	obs_properties_add_bool(props, "show_prophoto", obs_module_text("CIE.Prop.ProPhoto"));
	obs_properties_add_bool(props, "show_ap1", obs_module_text("CIE.Prop.AP1"));
	obs_properties_add_bool(props, "show_ap0", obs_module_text("CIE.Prop.AP0"));
	obs_properties_add_bool(props, "show_planck", obs_module_text("CIE.Prop.Planck"));
	obs_properties_add_bool(props, "cie_uv", obs_module_text("CIE.Prop.UV"));
	obs_properties_add_bool(props, "show_graticule", obs_module_text("Graticule"));
	return props;
}

static uint32_t cie_get_width(void *data)
{
	struct cie_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : CIE_W;
}

static uint32_t cie_get_height(void *data)
{
	struct cie_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : CIE_H;
}

static void cie_draw(const struct cie_source *src, uint8_t *dbuf, const struct cm_surface_data *surface_data)
{
	memset(dbuf, 0, CIE_W * CIE_H);

	const bool hlg = cm_cs_is_hdr(surface_data->colorspace);
	const float *lin = surface_data->colorspace == CM_CS_2100_PQ    ? src->lin_pq
			   : surface_data->colorspace == CM_CS_2100_HLG ? src->lin_hlg
									: src->lin;
	// RGB to XYZ, D65: Rec. 2020 primaries for HLG, Rec. 709 / sRGB otherwise
	static const float m709[9] = {0.4124f, 0.3576f, 0.1805f, 0.2126f, 0.7152f, 0.0722f, 0.0193f, 0.1192f, 0.9505f};
	static const float m2020[9] = {0.6370f, 0.1446f, 0.1689f, 0.2627f, 0.6780f, 0.0593f, 0.0000f, 0.0281f, 1.0610f};
	const float *m = hlg ? m2020 : m709;

	for (uint32_t y = 0; y < surface_data->height; y++) {
		const uint8_t *p = surface_data->rgb_data + (size_t)surface_data->linesize * y;
		for (uint32_t x = 0; x < surface_data->width; x++, p += 4) {
			if (!p[3])
				continue; // outside the selection
			const float b = lin[p[0]], g = lin[p[1]], r = lin[p[2]];
			const float X = m[0] * r + m[1] * g + m[2] * b;
			const float Y = m[3] * r + m[4] * g + m[5] * b;
			const float Z = m[6] * r + m[7] * g + m[8] * b;
			const float sum = X + Y + Z;
			if (sum < 0.0005f)
				continue; // black has no chromaticity
			float fx, fy;
			to_screen(src->uv, X / sum, Y / sum, &fx, &fy);
			const int px = (int)fx, py = (int)fy;
			if (px < 0 || px >= CIE_W || py < 0 || py >= CIE_H)
				continue;
			uint8_t *c = dbuf + py * CIE_W + px;
			if (*c < 255)
				++*c;
		}
	}
}

static void cie_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct cie_source *src = data;
	if (!surface_data->rgb_data || !surface_data->width)
		return;
	if (!src->tex_buf[src->w_tex_buf])
		src->tex_buf[src->w_tex_buf] = bzalloc(CIE_W * CIE_H);
	cie_draw(src, src->tex_buf[src->w_tex_buf], surface_data);
	src->w_tex_buf ^= 1;
}

/* CIE 1931 2-degree spectral locus, 380 to 700 nm in 5 nm steps. */
static const float spectral_locus[][2] = {
	{0.1741f, 0.0050f}, {0.1740f, 0.0050f}, {0.1738f, 0.0049f}, {0.1736f, 0.0049f}, {0.1733f, 0.0048f},
	{0.1730f, 0.0048f}, {0.1726f, 0.0048f}, {0.1721f, 0.0048f}, {0.1714f, 0.0051f}, {0.1703f, 0.0058f},
	{0.1689f, 0.0069f}, {0.1669f, 0.0086f}, {0.1644f, 0.0109f}, {0.1611f, 0.0138f}, {0.1566f, 0.0177f},
	{0.1510f, 0.0227f}, {0.1440f, 0.0297f}, {0.1355f, 0.0399f}, {0.1241f, 0.0578f}, {0.1096f, 0.0868f},
	{0.0913f, 0.1327f}, {0.0687f, 0.2007f}, {0.0454f, 0.2950f}, {0.0235f, 0.4127f}, {0.0082f, 0.5384f},
	{0.0039f, 0.6548f}, {0.0139f, 0.7502f}, {0.0389f, 0.8120f}, {0.0743f, 0.8338f}, {0.1142f, 0.8262f},
	{0.1547f, 0.8059f}, {0.1929f, 0.7816f}, {0.2296f, 0.7543f}, {0.2658f, 0.7243f}, {0.3016f, 0.6923f},
	{0.3373f, 0.6589f}, {0.3731f, 0.6245f}, {0.4087f, 0.5896f}, {0.4441f, 0.5547f}, {0.4788f, 0.5202f},
	{0.5125f, 0.4866f}, {0.5448f, 0.4544f}, {0.5752f, 0.4242f}, {0.6029f, 0.3965f}, {0.6270f, 0.3725f},
	{0.6482f, 0.3514f}, {0.6658f, 0.3340f}, {0.6801f, 0.3197f}, {0.6915f, 0.3083f}, {0.7006f, 0.2993f},
	{0.7079f, 0.2920f}, {0.7140f, 0.2859f}, {0.7190f, 0.2809f}, {0.7230f, 0.2770f}, {0.7260f, 0.2740f},
	{0.7283f, 0.2717f}, {0.7300f, 0.2700f}, {0.7311f, 0.2689f}, {0.7320f, 0.2680f}, {0.7327f, 0.2673f},
	{0.7334f, 0.2666f}, {0.7340f, 0.2660f}, {0.7344f, 0.2656f}, {0.7346f, 0.2654f}, {0.7347f, 0.2653f},
};

/* The diagram being drawn is in u'v' (graphics thread only). */
static bool g_uv;

static inline float sx_y(float x, float y)
{
	float px, py;
	to_screen(g_uv, x, y, &px, &py);
	return px;
}
static inline float sy_x(float x, float y)
{
	float px, py;
	to_screen(g_uv, x, y, &px, &py);
	return py;
}

static void draw_lines(const float (*pts)[2], int n, bool closed, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		for (int i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
			const int j = (i + 1) % n;
			gs_vertex2f(sx_y(pts[i][0], pts[i][1]), sy_x(pts[i][0], pts[i][1]));
			gs_vertex2f(sx_y(pts[j][0], pts[j][1]), sy_x(pts[j][0], pts[j][1]));
		}
		gs_render_stop(GS_LINES);
	}
}

/* The Planckian locus (Kim et al. approximation), xy at a color temperature from 1667 to 25000 K. */
static void planck_xy(float t, float *x, float *y)
{
	const double T = t;
	double xc;
	if (T < 4000.0)
		xc = -0.2661239e9 / (T * T * T) - 0.2343589e6 / (T * T) + 0.8776956e3 / T + 0.179910;
	else
		xc = -3.0258469e9 / (T * T * T) + 2.1070379e6 / (T * T) + 0.2226347e3 / T + 0.240390;
	double yc;
	if (T < 2222.0)
		yc = -1.1063814 * xc * xc * xc - 1.34811020 * xc * xc + 2.18555832 * xc - 0.20219683;
	else if (T < 4000.0)
		yc = -0.9549476 * xc * xc * xc - 1.37418593 * xc * xc + 2.09137015 * xc - 0.16748867;
	else
		yc = 3.0817580 * xc * xc * xc - 5.87338670 * xc * xc + 3.75112997 * xc - 0.37001483;
	*x = (float)xc;
	*y = (float)yc;
}

static void cie_render_graticule(const struct cie_source *src)
{
	const struct text_scale ts = text_scale_from_viewport((float)CIE_W, (float)CIE_H);
	const float px = 11.0f;
	g_uv = src->uv;

	if (src->show_grid && src->uv) {
		// u'v' axes, every 0.1
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x40A0A0A0);
		const float k = CIE_W / UV_MAX;
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			for (int i = 1; i <= 6; i++) {
				gs_vertex2f(i * 0.1f * k, 0.0f);
				gs_vertex2f(i * 0.1f * k, (float)CIE_H);
				gs_vertex2f(0.0f, CIE_H - i * 0.1f * k);
				gs_vertex2f((float)CIE_W, CIE_H - i * 0.1f * k);
			}
			gs_render_stop(GS_LINES);
		}
		for (int i = 1; i <= 6; i++) {
			char buf[8];
			snprintf(buf, sizeof(buf), "0.%d", i);
			text_draw(&ts, buf, i * 0.1f * k, (float)CIE_H, px, 0x90C8C8C8, TEXT_ALIGN_CENTER,
				  TEXT_VALIGN_BOTTOM);
			text_draw(&ts, buf, 2.0f * ts.ux, CIE_H - i * 0.1f * k, px, 0x90C8C8C8, TEXT_ALIGN_LEFT,
				  TEXT_VALIGN_MIDDLE);
		}
		text_draw(&ts, "u'v'", (float)CIE_W - 3.0f * ts.ux, 3.0f * ts.uy, px, 0xC0FFFFFF, TEXT_ALIGN_RIGHT,
			  TEXT_VALIGN_TOP);
	} else if (src->show_grid) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x40A0A0A0);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			for (int i = 1; i <= 8; i++) {
				gs_vertex2f(i * 0.1f / CIE_X_MAX * CIE_W, 0.0f);
				gs_vertex2f(i * 0.1f / CIE_X_MAX * CIE_W, (float)CIE_H);
			}
			for (int i = 1; i <= 9; i++) {
				gs_vertex2f(0.0f, (1.0f - i * 0.1f / CIE_Y_MAX) * CIE_H);
				gs_vertex2f((float)CIE_W, (1.0f - i * 0.1f / CIE_Y_MAX) * CIE_H);
			}
			gs_render_stop(GS_LINES);
		}
		for (int i = 1; i <= 7; i++) {
			char buf[8];
			snprintf(buf, sizeof(buf), "0.%d", i);
			text_draw(&ts, buf, i * 0.1f / CIE_X_MAX * CIE_W, (float)CIE_H, px, 0x90C8C8C8,
				  TEXT_ALIGN_CENTER, TEXT_VALIGN_BOTTOM);
			text_draw(&ts, buf, 2.0f * ts.ux, (1.0f - i * 0.1f / CIE_Y_MAX) * CIE_H, px, 0x90C8C8C8,
				  TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
		}
	}

	// spectral locus closed by the line of purples
	draw_lines(spectral_locus, (int)(sizeof(spectral_locus) / sizeof(spectral_locus[0])), true, 0xC0E0E0E0);

	// more gamuts
	static const struct
	{
		float p[3][2];
		const char *name;
		uint32_t color;
	} more[4] = {
		{{{0.7347f, 0.2653f}, {0.0000f, 1.0000f}, {0.0001f, -0.0770f}}, "ACES AP0", 0x90FF80FF},
		{{{0.7130f, 0.2930f}, {0.1650f, 0.8300f}, {0.1280f, 0.0440f}}, "ACES AP1", 0x90C0FF80},
		{{{0.7347f, 0.2653f}, {0.1596f, 0.8404f}, {0.0366f, 0.0001f}}, "ProPhoto", 0x9080C0FF},
		{{{0.6400f, 0.3300f}, {0.2100f, 0.7100f}, {0.1500f, 0.0600f}}, "Adobe RGB", 0x90FFA0A0},
	};
	const bool show_more[4] = {src->show_ap0, src->show_ap1, src->show_prophoto, src->show_adobe};
	for (int k = 0; k < 4; k++) {
		if (!show_more[k])
			continue;
		draw_lines(more[k].p, 3, true, more[k].color);
		const float gx = more[k].p[1][0], gy = fminf(more[k].p[1][1], 0.88f);
		text_draw(&ts, more[k].name, sx_y(gx, gy) + 4.0f * ts.ux, sy_x(gx, gy), px, more[k].color | 0xFF000000,
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
	}
	// the Planckian locus with a few color temperatures
	if (src->show_planck) {
		float pl[60][2];
		int n = 0;
		for (float t = 1667.0f; t <= 25000.0f && n < 60; t *= 1.05f, n++)
			planck_xy(t, &pl[n][0], &pl[n][1]);
		draw_lines((const float(*)[2])pl, n, false, 0xC0FFC080);
		static const int temps[5] = {2000, 3200, 4300, 5600, 10000};
		for (int k = 0; k < 5; k++) {
			float x, y;
			planck_xy((float)temps[k], &x, &y);
			char buf[16];
			snprintf(buf, sizeof(buf), "%dK", temps[k]);
			const float qx = sx_y(x, y), qy = sy_x(x, y);
			gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
			gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xE0FFC080);
			while (gs_effect_loop(solid, "Solid")) {
				gs_render_start(false);
				gs_vertex2f(qx, qy - 2.5f);
				gs_vertex2f(qx, qy + 2.5f);
				gs_render_stop(GS_LINES);
			}
			text_draw(&ts, buf, qx, qy + 3.0f * ts.uy, px - 2.0f, 0xD0FFC080, TEXT_ALIGN_CENTER,
				  TEXT_VALIGN_TOP);
		}
	}

	static const float rec709[3][2] = {{0.640f, 0.330f}, {0.300f, 0.600f}, {0.150f, 0.060f}};
	static const float p3[3][2] = {{0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}};
	static const float rec2020[3][2] = {{0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}};
	if (src->show_2020) {
		draw_lines(rec2020, 3, true, 0xB0FFFFFF);
		text_draw(&ts, "Rec.2020", sx_y(rec2020[1][0], rec2020[1][1]) + 4.0f * ts.ux,
			  sy_x(rec2020[1][0], rec2020[1][1]), px, 0xB0FFFFFF, TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
	}
	if (src->show_p3) {
		draw_lines(p3, 3, true, 0xB0FFC040);
		text_draw(&ts, "P3", sx_y(p3[1][0], p3[1][1]) + 4.0f * ts.ux, sy_x(p3[1][0], p3[1][1]), px, 0xB0FFC040,
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
	}
	if (src->show_709) {
		draw_lines(rec709, 3, true, 0xC060E0FF);
		text_draw(&ts, "Rec.709", sx_y(rec709[1][0], rec709[1][1]) + 4.0f * ts.ux,
			  sy_x(rec709[1][0], rec709[1][1]), px, 0xC060E0FF, TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
	}

	// D65 white point
	const float wx = sx_y(0.3127f, 0.3290f), wy = sy_x(0.3127f, 0.3290f);
	const float d = 3.0f;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xE0FFFFFF);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(wx - d, wy);
		gs_vertex2f(wx + d, wy);
		gs_vertex2f(wx, wy - d);
		gs_vertex2f(wx, wy + d);
		gs_render_stop(GS_LINES);
	}
}

static void cie_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct cie_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}

	cm_render_target(&src->cm);

	const int r_tex_buf = src->w_tex_buf ^ 1;
	if (src->tex_buf[r_tex_buf] && src->effect) {
		const uint8_t *buf = src->tex_buf[r_tex_buf];
		if (!src->tex)
			src->tex = gs_texture_create(CIE_W, CIE_H, GS_R8, 1, &buf, GS_DYNAMIC);
		else
			gs_texture_set_image(src->tex, buf, CIE_W, false);

		gs_effect_set_texture(gs_effect_get_param_by_name(src->effect, "image"), src->tex);
		gs_effect_set_float(gs_effect_get_param_by_name(src->effect, "intensity"), (float)src->intensity);
		while (gs_effect_loop(src->effect, "Draw"))
			gs_draw_sprite(src->tex, 0, CIE_W, CIE_H);
	}

	cie_render_graticule(src);
}

const struct obs_source_info colormonitor_cie = {
	.id = ID_PREFIX "cie_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = cie_get_name,
	.create = cie_create,
	.destroy = cie_destroy,
	.update = cie_update,
	.get_defaults = cie_get_defaults,
	.get_properties = cie_get_properties,
	.get_width = cie_get_width,
	.get_height = cie_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = cie_render,
	.video_tick = cm_tick,
};

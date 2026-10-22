#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <graphics/matrix4.h>
#include <graphics/image-file.h>
#include "plugin-macros.generated.h"
#include "obs-convenience.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#ifdef ENABLE_PROFILE
#define PROFILE_START(x) profile_start(x)
#define PROFILE_END(x) profile_end(x)
static const char *prof_render_name = "zebra_render";
#else // ENABLE_PROFILE
#define PROFILE_START(x)
#define PROFILE_END(x)
#endif // ! ENABLE_PROFILE

enum show_key_e {
	show_key_none = 0,
	show_key_left = 1,
	show_key_right = 2,
	show_key_outside = 3,
	show_key_top = 4,
	show_key_bottom = 5,
	show_key_below = 6,
	show_key_table = 7, // false color only: table beside the picture
};

/* Built-in false color bands, bottom to top; must match falsecolor.effect. */
#define FC_N_BANDS 12
static const struct
{
	float y_max;    // band holds luma below this (the last band is 100 % and above)
	uint32_t color; // 0xAARRGGBB
	const char *range;
	const char *label_key;
} fc_bands[FC_N_BANDS] = {
	{0.02f, 0xFFD938FF, "0-2%", "FC.Band.BlackClip"},   {0.10f, 0xFF0000FF, "2-10%", "FC.Band.DeepShadow"},
	{0.20f, 0xFF548CFF, "10-20%", "FC.Band.Shadow"},    {0.42f, 0xFF4D4D4D, "20-42%", "FC.Band.Dark"},
	{0.48f, 0xFF99FF00, "42-48%", "FC.Band.MidGrey"},   {0.52f, 0xFF808080, "48-52%", "FC.Band.Mid"},
	{0.58f, 0xFFF29E9E, "52-58%", "FC.Band.Skin"},      {0.78f, 0xFFB3B3B3, "58-78%", "FC.Band.Bright"},
	{0.84f, 0xFFB3B300, "78-84%", "FC.Band.Highlight"}, {0.94f, 0xFFFFFF00, "84-94%", "FC.Band.BrightHighlight"},
	{1.00f, 0xFFE68000, "94-100%", "FC.Band.NearClip"}, {2.00f, 0xFFE63300, ">100%", "FC.Band.Clip"},
};

// common structure for source and filter
struct zb_source
{
	gs_effect_t *effect;

	/* properties */
	float zebra_th_low, zebra_th_high;
	float zebra_tm;
	enum show_key_e show_key;
	char *falsecolor_lut_filename;

	/* internal data */
	gs_texture_t *key_tex;
	gs_image_file_t key_label_img;
	gs_vertbuffer_t *key_label_vbuf;
	gs_image_file_t falsecolor_lut;
	bool is_falsecolor;
};

struct zbs_source
{
	struct cm_source cm;
	struct zb_source zb;

	// share of the analyzed pixels in each false color band, for the table
	float coverage[2][FC_N_BANDS];
	volatile int w_coverage;
	bool has_coverage;
};

static inline bool zb_has_table(const struct zb_source *zb)
{
	return zb->is_falsecolor && zb->show_key == show_key_table && !zb->falsecolor_lut.texture;
}

static void fcs_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct zbs_source *src = data;
	const uint8_t *yuv = surface_data->yuv_data;
	if (!yuv || !surface_data->width)
		return;

	// Luma code values 0..255 to band index, built once per call.
	uint8_t band_of[256];
	for (int v = 0, b = 0; v < 256; v++) {
		const float y = v / 255.0f;
		while (b < FC_N_BANDS - 1 && y >= fc_bands[b].y_max)
			b++;
		band_of[v] = (uint8_t)b;
	}

	uint32_t counts[FC_N_BANDS] = {0};
	uint32_t total = 0;
	for (uint32_t y = 0; y < surface_data->height; y++) {
		const uint8_t *p = yuv + (size_t)surface_data->linesize * y;
		for (uint32_t x = 0; x < surface_data->width; x++, p += 4) {
			if (!p[3])
				continue; // outside the selection
			counts[band_of[p[1]]]++;
			total++;
		}
	}

	float *out = src->coverage[src->w_coverage];
	for (int b = 0; b < FC_N_BANDS; b++)
		out[b] = total ? (float)counts[b] / (float)total : 0.0f;
	src->w_coverage ^= 1;
	src->has_coverage = true;
}

struct zbf_source
{
	struct zb_source zb;
	int colorspace;
	obs_source_t *context;
};

static const char *zb_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Zebra");
}

static const char *fc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("False Color");
}

static void zbs_update(void *, obs_data_t *);
static void zbf_update(void *, obs_data_t *);

static void zb_init(struct zb_source *src)
{
	obs_enter_graphics();
	src->effect = create_effect_from_module_file(src->is_falsecolor ? "falsecolor.effect" : "zebra.effect");
	obs_leave_graphics();
}

static void *zbs_create(obs_data_t *settings, obs_source_t *source)
{
	struct zbs_source *src = bzalloc(sizeof(struct zbs_source));

	src->cm.flags = CM_FLAG_RAW_TEXTURE;
	cm_create(&src->cm, settings, source);
	zb_init(&src->zb);

	zbs_update(src, settings);

	return src;
}

static void *zbf_create(obs_data_t *settings, obs_source_t *source)
{
	struct zbf_source *src = bzalloc(sizeof(struct zbf_source));

	zb_init(&src->zb);
	src->context = source;

	zbf_update(src, settings);

	return src;
}

static void *fcs_create(obs_data_t *settings, obs_source_t *source)
{
	struct zbs_source *src = bzalloc(sizeof(struct zbs_source));
	src->zb.is_falsecolor = true;

	src->cm.flags = CM_FLAG_RAW_TEXTURE;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, fcs_surface_cb, src);
	zb_init(&src->zb);

	zbs_update(src, settings);

	return src;
}

static void *fcf_create(obs_data_t *settings, obs_source_t *source)
{
	struct zbf_source *src = bzalloc(sizeof(struct zbf_source));
	src->zb.is_falsecolor = true;

	zb_init(&src->zb);
	src->context = source;

	zbf_update(src, settings);

	return src;
}

static void falsecolor_lut_unload(struct zb_source *src)
{
	if (!src->falsecolor_lut.loaded)
		return;
	obs_enter_graphics();
	gs_image_file_free(&src->falsecolor_lut);
	obs_leave_graphics();
}

static void zb_destroy(struct zb_source *src)
{
	if (src->key_tex) {
		obs_enter_graphics();
		gs_texture_destroy(src->key_tex);
		gs_image_file_free(&src->key_label_img);
		gs_vertexbuffer_destroy(src->key_label_vbuf);
		obs_leave_graphics();
	}

	if (src->is_falsecolor) {
		falsecolor_lut_unload(src);
		bfree(src->falsecolor_lut_filename);
	}
}

static void zbs_destroy(void *data)
{
	struct zbs_source *src = data;

	cm_destroy(&src->cm);
	zb_destroy(&src->zb);
	bfree(src);
}

static void zbf_destroy(void *data)
{
	struct zbf_source *src = data;
	zb_destroy(&src->zb);
	bfree(src);
}

static void falsecolor_lut_load(struct zb_source *src, const char *lut_filename)
{
	falsecolor_lut_unload(src);

	blog(LOG_INFO, "Loading LUT file '%s'...", lut_filename);
	gs_image_file_init(&src->falsecolor_lut, lut_filename);

	obs_enter_graphics();
	gs_image_file_init_texture(&src->falsecolor_lut);
	obs_leave_graphics();
}

static void zb_update(struct zb_source *src, obs_data_t *settings)
{
	if (!src->is_falsecolor) {
		/* zebra */
		src->zebra_th_low = obs_data_get_int(settings, "zebra_th_low") * 1e-2f;
		src->zebra_th_high = obs_data_get_int(settings, "zebra_th_high") * 1e-2f;
	} else {
		/* falsecolor */
		bool lut = obs_data_get_bool(settings, "falsecolor_lut");
		const char *lut_filename = lut ? obs_data_get_string(settings, "falsecolor_lut_filename") : NULL;
		if (!lut_filename) {
			falsecolor_lut_unload(src);
			bfree(src->falsecolor_lut_filename);
			src->falsecolor_lut_filename = NULL;
		} else if (!src->falsecolor_lut_filename || strcmp(lut_filename, src->falsecolor_lut_filename) != 0) {
			falsecolor_lut_load(src, lut_filename);
			bfree(src->falsecolor_lut_filename);
			src->falsecolor_lut_filename = bstrdup(lut_filename);
		}
	}

	src->show_key = obs_data_get_int(settings, "show_key");
}

static void zbs_update(void *data, obs_data_t *settings)
{
	struct zbs_source *src = data;
	cm_update(&src->cm, settings);
	zb_update(&src->zb, settings);

	// The table's percentages need luma on the CPU.
	src->cm.flags = CM_FLAG_RAW_TEXTURE | (zb_has_table(&src->zb) ? CM_FLAG_CONVERT_YUV : 0);
}

static void zbf_update(void *data, obs_data_t *settings)
{
	struct zbf_source *src = data;
	zb_update(&src->zb, settings);

	int colorspace = (int)obs_data_get_int(settings, "colorspace");
	src->colorspace = calc_colorspace(colorspace);
}

static void zb_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "zebra_th_low", 75);
	obs_data_set_default_int(settings, "zebra_th_high", 100);
}

static void zb_get_properties(obs_properties_t *props, bool is_falsecolor)
{
	obs_property_t *prop;

	if (!is_falsecolor) {
		/* zebra */
		prop = obs_properties_add_int(props, "zebra_th_low", obs_module_text("Threshold (lower)"), 50, 100, 1);
		obs_property_int_set_suffix(prop, "%");
		prop = obs_properties_add_int(props, "zebra_th_high", obs_module_text("Threshold (high)"), 50, 100, 1);
		obs_property_int_set_suffix(prop, "%");
	} else {
		/* falsecolor */
		struct dstr filename_filters = {0};
		dstr_copy(&filename_filters, obs_module_text("FalseColor.Prop.LUTFile.Filter.Image"));
		dstr_cat(&filename_filters, " (*.bmp *.jpg *.jpeg *.tga *.gif *.png);;");
		dstr_cat(&filename_filters, obs_module_text("FalseColor.Prop.LUTFile.Filter.All"));
		dstr_cat(&filename_filters, " (*.*)");
		obs_properties_add_bool(props, "falsecolor_lut", obs_module_text("FalseColor.Prop.LUT"));
		obs_properties_add_path(props, "falsecolor_lut_filename", obs_module_text("FalseColor.Prop.LUTFile"),
					OBS_PATH_FILE, filename_filters.array, NULL);
		dstr_free(&filename_filters);

		prop = obs_properties_add_list(props, "show_key", obs_module_text("Prop.ShowKey"), OBS_COMBO_TYPE_LIST,
					       OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(prop, obs_module_text("Prop.ShowKey.None"), show_key_none);
		obs_property_list_add_int(prop, obs_module_text("Prop.ShowKey.Left"), show_key_left);
		obs_property_list_add_int(prop, obs_module_text("Prop.ShowKey.Right"), show_key_right);
		obs_property_list_add_int(prop, obs_module_text("Prop.ShowKey.Outside"), show_key_outside);
		obs_property_list_add_int(prop, obs_module_text("Prop.ShowKey.Top"), show_key_top);
		obs_property_list_add_int(prop, obs_module_text("Prop.ShowKey.Bottom"), show_key_bottom);
		obs_property_list_add_int(prop, obs_module_text("Prop.ShowKey.Below"), show_key_below);
		if (is_falsecolor)
			obs_property_list_add_int(prop, obs_module_text("Prop.ShowKey.Table"), show_key_table);
	}

	properties_add_colorspace(props, "colorspace", obs_module_text("Color space"));
}

static obs_properties_t *zbs_get_properties(void *data)
{
	struct zbs_source *src = data;
	obs_properties_t *props;
	props = obs_properties_create();

	cm_get_properties(&src->cm, props);
	zb_get_properties(props, false);

	return props;
}

static obs_properties_t *zbf_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props;
	props = obs_properties_create();

	zb_get_properties(props, false);

	return props;
}

static obs_properties_t *fcs_get_properties(void *data)
{
	struct zbs_source *src = data;
	obs_properties_t *props;
	props = obs_properties_create();

	cm_get_properties(&src->cm, props);
	zb_get_properties(props, true);

	return props;
}

static obs_properties_t *fcf_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props;
	props = obs_properties_create();

	zb_get_properties(props, true);

	return props;
}

static uint32_t zbs_get_width(void *data)
{
	struct zbs_source *src = data;
	uint32_t w = cm_bypass_get_width(&src->cm);
	if (!src->cm.bypass && src->zb.show_key == show_key_outside)
		return w * 11 / 10;
	if (!src->cm.bypass && zb_has_table(&src->zb))
		return w + cm_bypass_get_height(&src->cm) * 3 / 4;
	return w;
}

static uint32_t zbs_get_height(void *data)
{
	struct zbs_source *src = data;
	uint32_t h = cm_bypass_get_height(&src->cm);
	if (!src->cm.bypass && src->zb.show_key == show_key_below)
		return h * 12 / 10;
	return h;
}

static uint32_t fcf_get_width(void *data)
{
	struct zbf_source *src = data;
	obs_source_t *target = obs_filter_get_target(src->context);
	uint32_t w = obs_source_get_base_width(target);
	if (src->zb.show_key == show_key_outside)
		return w * 11 / 10;
	if (zb_has_table(&src->zb))
		return w + obs_source_get_base_height(target) * 3 / 4;
	return w;
}

static uint32_t fcf_get_height(void *data)
{
	struct zbf_source *src = data;
	obs_source_t *target = obs_filter_get_target(src->context);
	uint32_t h = obs_source_get_base_height(target);
	if (src->zb.show_key == show_key_below)
		return h * 12 / 10;
	return h;
}

static const char *draw_name(int colorspace, bool is_falsecolor)
{
	if (colorspace == 1 && is_falsecolor)
		return "DrawFalseColor601";
	else if (colorspace == 1)
		return "DrawZebra601";
	else if (is_falsecolor)
		return "DrawFalseColor709";
	else
		return "DrawZebra709";
}

static void set_effect_params(struct zb_source *src)
{
	gs_effect_t *e = src->effect;

	if (!src->is_falsecolor) {
		/* zebra */
		gs_effect_set_float(gs_effect_get_param_by_name(e, "zebra_th_low"), src->zebra_th_low);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "zebra_th_high"), src->zebra_th_high);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "zebra_tm"), src->zebra_tm);
	} else {
		/* falsecolor */
		gs_texture_t *lut = src->falsecolor_lut.texture;
		gs_effect_set_bool(gs_effect_get_param_by_name(e, "use_lut"), !!lut);
		if (lut)
			gs_effect_set_texture(gs_effect_get_param_by_name(e, "lut"), lut);
	}
}

static void zb_create_key_tex(struct zb_source *src)
{
#define N 256
	uint8_t *buf = bmalloc(1 * N * 4);
	for (uint32_t i = 0; i < N; i++) {
		buf[i * 4 + 0] = i * 256 / N;
		buf[i * 4 + 1] = i * 256 / N;
		buf[i * 4 + 2] = i * 256 / N;
		buf[i * 4 + 3] = 0xFF;
	}
	const uint8_t *cbuf = buf;
	src->key_tex = gs_texture_create(N, 1, GS_BGRX, 1, &cbuf, 0);
	bfree(buf);
#undef N

	if (!src->key_label_img.loaded) {
		char *f = obs_module_file("falsecolor-key.png");
		gs_image_file_init(&src->key_label_img, f);
		if (!src->key_label_img.loaded)
			blog(LOG_ERROR, "Cannot load falsecolor-key.png (%s)", f);
		gs_image_file_init_texture(&src->key_label_img);
		bfree(f);
	}
}

static void zb_render_key(struct zb_source *src, const char *draw, uint32_t width, uint32_t height)
{
	struct key_def_s
	{
		/* outer box */
		float x0, y0, x1, y1;
		/* key */
		float xk, yk;
		float cxk, cyk;
		uint32_t bg_color;
		bool is_vertical;
	};
	const struct key_def_s points[] = {
		[show_key_left] =
			{
				.x0 = 0.01f,
				.y0 = 0.1f,
				.x1 = 0.09f,
				.y1 = 0.9f,
				.xk = 0.06f,
				.yk = 0.88f,
				.cxk = 0.025f,
				.cyk = -0.76f / 256,
				.bg_color = 0x80000000,
				.is_vertical = true,
			},
		[show_key_right] =
			{
				.x0 = 0.91f,
				.y0 = 0.1f,
				.x1 = 0.99f,
				.y1 = 0.9f,
				.xk = 0.96f,
				.yk = 0.88f,
				.cxk = 0.025f,
				.cyk = -0.76f / 256,
				.bg_color = 0x80000000,
				.is_vertical = true,
			},
		[show_key_outside] =
			{
				.x0 = 1.00f,
				.y0 = 0.0f,
				.x1 = 1.10f,
				.y1 = 1.0f,
				.xk = 1.06f,
				.yk = 0.95f,
				.cxk = 0.03f,
				.cyk = -0.90f / 256,
				.bg_color = 0xFF000000,
				.is_vertical = true,
			},
		[show_key_top] =
			{
				.x0 = 0.1f,
				.y0 = 0.01f,
				.x1 = 0.9f,
				.y1 = 0.09f,
				.xk = 0.12f,
				.yk = 0.05f,
				.cxk = 0.76f / 256,
				.cyk = -0.025f,
				.bg_color = 0x80000000,
				.is_vertical = false,
			},
		[show_key_bottom] =
			{
				.x0 = 0.1f,
				.y0 = 0.91f,
				.x1 = 0.9f,
				.y1 = 0.99f,
				.xk = 0.12f,
				.yk = 0.95f,
				.cxk = 0.76f / 256,
				.cyk = -0.025f,
				.bg_color = 0x80000000,
				.is_vertical = false,
			},
		[show_key_below] =
			{
				.x0 = 0.0f,
				.y0 = 1.00f,
				.x1 = 1.0f,
				.y1 = 1.20f,
				.xk = 0.05f,
				.yk = 1.08f,
				.cxk = 0.90f / 256,
				.cyk = -0.060f,
				.bg_color = 0xFF000000,
				.is_vertical = false,
			},
	};

	if (src->show_key <= show_key_none || src->show_key >= sizeof(points) / sizeof(*points))
		return;

	const struct key_def_s *def = points + src->show_key;

	if (!src->key_tex)
		zb_create_key_tex(src);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), def->bg_color);
	while (gs_effect_loop(effect, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(def->x0 * width, def->y0 * height);
		gs_vertex2f(def->x1 * width, def->y0 * height);
		gs_vertex2f(def->x1 * width, def->y1 * height);
		gs_vertex2f(def->x1 * width, def->y1 * height);
		gs_vertex2f(def->x0 * width, def->y1 * height);
		gs_vertex2f(def->x0 * width, def->y0 * height);
		gs_render_stop(GS_TRISTRIP);
	}

	effect = src->effect;
	if (!effect)
		return;
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src->key_tex);
	set_effect_params(src);

	gs_matrix_push();
	struct matrix4 tran;
	if (def->is_vertical) {
		vec4_set(&tran.x, 0.0f, def->cyk * height, 0.0f, 0.0f);
		vec4_set(&tran.y, def->cxk * width, 0.0f, 0.0f, 0.0f);
	} else {
		vec4_set(&tran.x, def->cxk * width, 0.0f, 0.0f, 0.0f);
		vec4_set(&tran.y, 0.0f, def->cyk * height, 0.0f, 0.0f);
	}
	vec4_set(&tran.z, 0.0f, 0.0f, 1.0f, 0.0f);
	vec4_set(&tran.t, def->xk * width, def->yk * height, 0.0f, 1.0f);
	gs_matrix_mul(&tran);

	while (gs_effect_loop(effect, draw)) {
		gs_draw_sprite(src->key_tex, 0, 0, 0);
	}

	gs_matrix_pop();

	if (!src->key_label_img.loaded)
		return;

	effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!src->key_label_vbuf)
		src->key_label_vbuf = create_uv_vbuffer(11 * 6, false);
	struct gs_vb_data *vdata = gs_vertexbuffer_get_data(src->key_label_vbuf);
	struct vec2 *tvarray = (struct vec2 *)vdata->tvarray[0].array;
	for (uint32_t i = 0; i < 11; i++) {
		float x, y, w, h;
		int img_cx, img_cy;
		if (def->is_vertical) {
			x = width * def->x0;
			y = height * (def->yk + def->cyk * 256 * i / 10);
			w = width * (def->xk - def->x0);
			h = height * fabsf(def->cyk * 256) / 10;
			img_cx = src->key_label_img.cx * 55; // cx
			img_cy = src->key_label_img.cy * 2;  // cy * 2 / 55
		} else {
			x = width * (def->xk + def->cxk * 256 * i / 10);
			y = height * def->yk;
			w = width * def->cxk * 256 / 11;
			h = height * (def->y1 - def->yk);
			img_cx = src->key_label_img.cx * 55; // cx
			img_cy = src->key_label_img.cy * 3;  // cy * 3 / 55
		}
		// if w / h > cx / cy
		if (w * img_cy > h * img_cx) {
			float nw = h * img_cx / img_cy;
			if (def->is_vertical)
				x += (w - nw) * 0.5f;
			w = nw;
		} else {
			h = w * img_cy / img_cx;
		}
		if (def->is_vertical)
			y -= h * 0.5f;
		else
			x -= w * 0.5f;
		set_v3_rect(vdata->points + i * 6, x, y, w, h);
		if (def->is_vertical) {
			set_v2_uv(tvarray + i * 6, 0.0f, i / 27.5f, 1.f, (i + 1) / 27.5f);
		} else {
			set_v2_uv(tvarray + i * 6, 0.0f, (22 + i * 3) / 55.f, 1.f, (25 + i * 3) / 55.f);
		}
	}

	draw_uv_vbuffer(src->key_label_vbuf, src->key_label_img.texture, effect, "Draw", 11 * 6);
}

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

/* Table beside the picture: one row per band with its color, luma range, meaning and,
 * when known, the share of the picture in that band. Drawn at x0..x0+tw over the height. */
static void zb_render_table(float x0, float tw, float height, float total_width, const float *coverage)
{
	const struct text_scale ts = text_scale_from_viewport(total_width, height);
	const float row_h = height / FC_N_BANDS;
	const float row_px = row_h / ts.uy;
	float font_px = row_px * 0.62f;
	if (font_px > 20.0f)
		font_px = 20.0f;

	fill_rect(x0, 0.0f, x0 + tw, height, 0xFF000000);
	if (font_px < 6.0f)
		return; // too small to read

	const float gap = 6.0f * ts.ux;
	const float sw = row_px * 0.7f * ts.ux; // swatch is square on screen
	const float x_sw = x0 + gap;
	const float x_range = x_sw + sw + gap;
	const float x_label = x_range + text_width(&ts, "94-100%", font_px) + gap;
	const float x_right = x0 + tw - gap;

	for (int b = 0; b < FC_N_BANDS; b++) {
		const float y_top = (FC_N_BANDS - 1 - b) * row_h; // brightest band on top
		const float y_mid = y_top + row_h * 0.5f;
		const float pad_y = row_h * 0.15f;
		const float share = coverage ? coverage[b] : 0.0f;
		const bool present = coverage && share >= 0.0005f;

		if (present)
			fill_rect(x0, y_top, x0 + tw, y_top + row_h, 0xFF1E1E1E);
		fill_rect(x_sw, y_top + pad_y, x_sw + sw, y_top + row_h - pad_y, fc_bands[b].color);

		const uint32_t text_color = !coverage || present ? 0xFFE6E6E6 : 0xFF707070;
		text_draw(&ts, fc_bands[b].range, x_range, y_mid, font_px, text_color, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_MIDDLE);
		text_draw(&ts, obs_module_text(fc_bands[b].label_key), x_label, y_mid, font_px, text_color,
			  TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
		if (coverage) {
			char pct[16];
			snprintf(pct, sizeof(pct), "%.1f%%", share * 100.0f);
			text_draw(&ts, pct, x_right, y_mid, font_px, text_color, TEXT_ALIGN_RIGHT, TEXT_VALIGN_MIDDLE);
		}
	}

	// thin separators between rows
	for (int b = 1; b < FC_N_BANDS; b++)
		fill_rect(x0, b * row_h - 0.5f * ts.uy, x0 + tw, b * row_h + 0.5f * ts.uy, 0xFF2A2A2A);
}

static void zbs_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct zbs_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}

	cm_render_target(&src->cm);

	PROFILE_START(prof_render_name);

	gs_texture_t *tex = cm_bypass_get_texture(&src->cm);
	gs_effect_t *e = src->zb.effect;
	if (e && tex) {
		uint32_t cx = cm_bypass_get_width(&src->cm);
		uint32_t cy = cm_bypass_get_height(&src->cm);

		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
		set_effect_params(&src->zb);
		const char *draw = draw_name(src->cm.colorspace, src->zb.is_falsecolor);
		while (gs_effect_loop(e, draw))
			gs_draw_sprite_subregion(tex, 0, 0, 0, cx, cy);

		zb_render_key(&src->zb, draw, cx, cy);

		if (zb_has_table(&src->zb)) {
			const float tw = cy * 3.0f / 4.0f;
			const float *cov = src->has_coverage ? src->coverage[src->w_coverage ^ 1] : NULL;
			zb_render_table((float)cx, tw, (float)cy, cx + tw, cov);
		}
	}

	PROFILE_END(prof_render_name);
}

static void zbf_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct zbf_source *src = data;
	gs_effect_t *e = src->zb.effect;

	if (!e)
		return;

	if (!obs_source_process_filter_begin(src->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;

	set_effect_params(&src->zb);

	gs_blend_state_push();
	gs_reset_blend_state();

	const char *draw = draw_name(src->colorspace, src->zb.is_falsecolor);
	obs_source_process_filter_tech_end(src->context, e, 0, 0, draw);

	obs_source_t *target = obs_filter_get_target(src->context);
	if (target) {
		uint32_t cx = obs_source_get_base_width(target);
		uint32_t cy = obs_source_get_base_height(target);
		zb_render_key(&src->zb, draw, cx, cy);
		if (zb_has_table(&src->zb)) {
			const float tw = cy * 3.0f / 4.0f;
			zb_render_table((float)cx, tw, (float)cy, cx + tw, NULL);
		}
	}

	gs_blend_state_pop();
}

static void zb_tick(void *data, float seconds)
{
	struct zb_source *src = data;
	src->zebra_tm += seconds * 4.0f;
	if (src->zebra_tm > 12.0f)
		src->zebra_tm -= 12.0f;
}

static void zbs_tick(void *data, float seconds)
{
	struct zbs_source *src = data;
	cm_tick(data, seconds);
	zb_tick(&src->zb, seconds);
}

const struct obs_source_info colormonitor_zebra = {
	.id = ID_PREFIX "zebra_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = zb_get_name,
	.create = zbs_create,
	.destroy = zbs_destroy,
	.update = zbs_update,
	.get_defaults = zb_get_defaults,
	.get_properties = zbs_get_properties,
	.get_width = zbs_get_width,
	.get_height = zbs_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = zbs_render,
	.video_tick = zbs_tick,
};

const struct obs_source_info colormonitor_zebra_filter = {
	.id = ID_PREFIX "zebra_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = zb_get_name,
	.create = zbf_create,
	.destroy = zbf_destroy,
	.update = zbf_update,
	.get_defaults = zb_get_defaults,
	.get_properties = zbf_get_properties,
	.video_render = zbf_render,
	.video_tick = zb_tick,
};

const struct obs_source_info colormonitor_falsecolor = {
	.id = ID_PREFIX "falsecolor_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = fc_get_name,
	.create = fcs_create,
	.destroy = zbs_destroy,
	.update = zbs_update,
	.get_defaults = zb_get_defaults,
	.get_properties = fcs_get_properties,
	.get_width = zbs_get_width,
	.get_height = zbs_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = zbs_render,
	.video_tick = zbs_tick,
};

const struct obs_source_info colormonitor_falsecolor_filter = {
	.id = ID_PREFIX "falsecolor_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = fc_get_name,
	.create = fcf_create,
	.destroy = zbf_destroy,
	.update = zbf_update,
	.get_defaults = zb_get_defaults,
	.get_properties = fcf_get_properties,
	.get_width = fcf_get_width,
	.get_height = fcf_get_height,
	.video_render = zbf_render,
	.video_tick = zb_tick,
};

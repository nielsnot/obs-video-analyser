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

/* False color bands, bottom to top. A band holds luma below y_max; the last band of a scheme
 * has y_max above 1 and holds 100 % and over. A color of 0 shows the luma as grey. */
#define FC_MAX_BANDS 12
#define FC_GREY 0x00000000
// raw signal of a video source: two more bands beyond the legal range
#define FC_SUBBLACK 0xFF00E5FF   // below 0 %: cyan
#define FC_SUPERWHITE 0xFFFFFFFF // above 100 %: white

struct fc_band
{
	float y_max;
	uint32_t color; // 0xAARRGGBB, or FC_GREY
	char label[48];
};

struct fc_preset_band
{
	float y_max;
	uint32_t color;
	const char *label_key;
};

enum fc_preset_e {
	FC_PRESET_EXPOSURE = 0,
	FC_PRESET_CLIPPING = 1,
	FC_PRESET_SKIN = 2,
	FC_PRESET_SPECTRUM = 3,
	FC_PRESET_CUSTOM = 4,
	FC_PRESET_HLG = 5,
	FC_PRESET_PQ = 6,
};

static const struct fc_preset_band fc_preset_exposure[] = {
	{0.02f, 0xFFD938FF, "FC.Band.BlackClip"}, {0.10f, 0xFF0000FF, "FC.Band.DeepShadow"},
	{0.20f, 0xFF548CFF, "FC.Band.Shadow"},    {0.42f, 0xFF4D4D4D, "FC.Band.Dark"},
	{0.48f, 0xFF99FF00, "FC.Band.MidGrey"},   {0.52f, 0xFF808080, "FC.Band.Mid"},
	{0.58f, 0xFFF29E9E, "FC.Band.Skin"},      {0.78f, 0xFFB3B3B3, "FC.Band.Bright"},
	{0.84f, 0xFFB3B300, "FC.Band.Highlight"}, {0.94f, 0xFFFFFF00, "FC.Band.BrightHighlight"},
	{1.00f, 0xFFE68000, "FC.Band.NearClip"},  {2.00f, 0xFFE63300, "FC.Band.Clip"},
};
static const struct fc_preset_band fc_preset_clipping[] = {
	{0.02f, 0xFF2060FF, "FC.Band.BlackClip"},
	{0.98f, FC_GREY, "FC.Band.InRange"},
	{1.00f, 0xFFFFD000, "FC.Band.NearClip"},
	{2.00f, 0xFFFF2020, "FC.Band.Clip"},
};
static const struct fc_preset_band fc_preset_skin[] = {
	{0.02f, 0xFF2060FF, "FC.Band.BlackClip"}, {0.42f, FC_GREY, "FC.Band.Dark"},
	{0.48f, 0xFF40E040, "FC.Band.MidGrey"},   {0.52f, FC_GREY, "FC.Band.Mid"},
	{0.62f, 0xFFF2A0A0, "FC.Band.Skin"},      {0.70f, 0xFFE07850, "FC.Band.SkinBright"},
	{0.98f, FC_GREY, "FC.Band.Bright"},       {2.00f, 0xFFFF2020, "FC.Band.Clip"},
};
static const struct fc_preset_band fc_preset_spectrum[] = {
	{0.10f, 0xFF5000A0, "FC.Band.Decile"}, {0.20f, 0xFF2020FF, "FC.Band.Decile"},
	{0.30f, 0xFF0090FF, "FC.Band.Decile"}, {0.40f, 0xFF00D0C0, "FC.Band.Decile"},
	{0.50f, 0xFF00C040, "FC.Band.Decile"}, {0.60f, 0xFF90E000, "FC.Band.Decile"},
	{0.70f, 0xFFF0E000, "FC.Band.Decile"}, {0.80f, 0xFFFFA000, "FC.Band.Decile"},
	{0.90f, 0xFFFF5000, "FC.Band.Decile"}, {2.00f, 0xFFFF0040, "FC.Band.Decile"},
};

/* HLG signal levels for a 1000 nits display (Rec. 2100 HLG, BT.2408), from the nits of each band:
 * below 0.5 nits, 10, 22, 30 (18 % grey near 26), 39, 85 (skin), 190, 215 (reference white 203 nits at 75 %),
 * 459, 900 and 1000 nits (100 %). */
static const struct fc_preset_band fc_preset_hlg[] = {
	{0.073f, 0xFFD938FF, "FC.Band.BlackClip"}, {0.254f, 0xFF548CFF, "FC.Band.Shadow"},
	{0.353f, 0xFF4D4D4D, "FC.Band.Dark"},      {0.402f, 0xFF00D8E8, "FC.Band.Grey26"},
	{0.448f, 0xFF808080, "FC.Band.Mid"},       {0.600f, 0xFFF29E9E, "FC.Band.SkinNits"},
	{0.739f, 0xFFB3B3B3, "FC.Band.Bright"},    {0.759f, 0xFF40E060, "FC.Band.RefWhite"},
	{0.880f, 0xFF9A9A60, "FC.Band.Highlight"}, {0.984f, 0xFFFFFF00, "FC.Band.Above459"},
	{1.000f, 0xFFE68000, "FC.Band.NearPeak"},  {2.000f, 0xFFE63300, "FC.Band.Peak1000"},
};

/* PQ signal levels (SMPTE ST 2084, absolute nits), the same bands: below 0.5 nits, 10, 22, 30 (18 % grey near
 * 26), 39, 85 (skin), 190, 215 (reference white 203 nits), 500 (half of 1000), 900 and 1000 nits. */
static const struct fc_preset_band fc_preset_pq[] = {
	{0.118f, 0xFFD938FF, "FC.Band.BlackClip"}, {0.300f, 0xFF548CFF, "FC.Band.Shadow"},
	{0.365f, 0xFF4D4D4D, "FC.Band.Dark"},      {0.393f, 0xFF00D8E8, "FC.Band.Grey26"},
	{0.417f, 0xFF808080, "FC.Band.Mid"},       {0.492f, 0xFFF29E9E, "FC.Band.SkinNits"},
	{0.574f, 0xFFB3B3B3, "FC.Band.Bright"},    {0.587f, 0xFF40E060, "FC.Band.RefWhitePQ"},
	{0.677f, 0xFF9A9A60, "FC.Band.Highlight"}, {0.740f, 0xFFFFFF00, "FC.Band.Above500"},
	{0.752f, 0xFFE68000, "FC.Band.NearPeak"},  {2.000f, 0xFFE63300, "FC.Band.Above1000"},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

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
	struct cm_image key_label_img;
	gs_vertbuffer_t *key_label_vbuf;
	struct cm_image falsecolor_lut;
	bool is_falsecolor;

	/* false color scheme */
	struct fc_band bands[FC_MAX_BANDS];
	int n_bands;
	gs_texture_t *scheme_lut; // 256x1 color per luma, built from the bands
	volatile bool scheme_dirty;
	gs_texture_t *scheme_lut_raw; // 256x1 color per legal code (16 is 0 %, 235 is 100 %)
	gs_texture_t *key_tex_raw;
	bool raw_dirty;
	int gamut_mode; // false color: 0 levels, 1 outside Rec. 709, 2 outside DCI-P3
	float opacity;  // false color over the picture: 1 is false color only
};

struct zbs_source
{
	struct cm_source cm;
	struct zb_source zb;

	// share of the analyzed pixels in each false color band, for the table
	float coverage[2][FC_MAX_BANDS + 2]; // with the raw signal: then sub-black and superwhite
	volatile int w_coverage;
	bool has_coverage;

	// the raw signal of a video source, as legal codes (B = G = R = code, A = selection)
	pthread_mutex_t ext_mutex;
	uint8_t *ext_img;
	uint32_t ext_w, ext_h;
	bool ext_new;
	volatile uint64_t ext_seen; // ns
	volatile bool cov_raw;      // the coverage was measured on the raw signal
	gs_texture_t *ext_tex;
	uint32_t ext_tex_w, ext_tex_h;
};

static inline bool fcs_raw_active(const struct zbs_source *src)
{
	return !src->zb.falsecolor_lut.texture && os_gettime_ns() - src->ext_seen < 500000000ULL;
}

static inline bool zb_has_table(const struct zb_source *zb)
{
	// a user LUT file has no band definitions to list
	return zb->is_falsecolor && zb->show_key == show_key_table && !zb->falsecolor_lut.texture && zb->n_bands > 0;
}

static void fcs_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct zbs_source *src = data;
	const uint8_t *yuv = surface_data->yuv_data;
	if (!yuv || !surface_data->width)
		return;

	// Luma code values 0..255 to band index, built once per call. Zebra has no bands: it only keeps the raw signal.
	const int n_bands = src->zb.is_falsecolor ? src->zb.n_bands : 0;
	if (n_bands <= 0 && !(surface_data->ext_data && !src->zb.is_falsecolor))
		return;
	uint8_t band_of[256];
	for (int v = 0, b = 0; v < 256; v++) {
		const float y = v / 255.0f;
		while (b < n_bands - 1 && y >= src->zb.bands[b].y_max)
			b++;
		band_of[v] = (uint8_t)b;
	}

	uint32_t counts[FC_MAX_BANDS + 2] = {0};
	uint32_t total = 0;
	const uint8_t *ext = surface_data->ext_data;
	if (ext) {
		// the raw signal: keep it for the picture, and count the two bands beyond the legal range
		const uint32_t w = surface_data->width, h = surface_data->height;
		pthread_mutex_lock(&src->ext_mutex);
		if (src->ext_w != w || src->ext_h != h || !src->ext_img) {
			bfree(src->ext_img);
			src->ext_img = bmalloc((size_t)w * h * 4);
			src->ext_w = w;
			src->ext_h = h;
		}
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *e = ext + (size_t)surface_data->ext_linesize * y;
			const uint8_t *p = yuv + (size_t)surface_data->linesize * y;
			uint8_t *o = src->ext_img + (size_t)w * 4 * y;
			for (uint32_t x = 0; x < w; x++, e += 4, p += 4, o += 4) {
				o[0] = o[1] = o[2] = e[3];
				o[3] = p[3] ? 255 : 0;
				if (!p[3] || n_bands <= 0)
					continue;
				const float level = (e[3] - 16) / 219.0f;
				if (level < 0.0f)
					counts[n_bands]++;
				else if (e[3] > 235)
					counts[n_bands + 1]++;
				else {
					int b = 0;
					while (b < n_bands - 1 && level >= src->zb.bands[b].y_max)
						b++;
					counts[b]++;
				}
				total++;
			}
		}
		src->ext_new = true;
		pthread_mutex_unlock(&src->ext_mutex);
		src->ext_seen = os_gettime_ns();
		float *out = src->coverage[src->w_coverage];
		for (int b = 0; b < n_bands + 2; b++)
			out[b] = total ? (float)counts[b] / (float)total : 0.0f;
		src->cov_raw = true;
		src->w_coverage ^= 1;
		src->has_coverage = true;
		return;
	}
	src->cov_raw = false;
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
	for (int b = 0; b < n_bands; b++)
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
	pthread_mutex_init(&src->ext_mutex, NULL);

	src->cm.flags = CM_FLAG_RAW_TEXTURE;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, fcs_surface_cb, src); // the raw signal of a video source
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
	pthread_mutex_init(&src->ext_mutex, NULL);
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
	cm_image_free(&src->falsecolor_lut);
	obs_leave_graphics();
}

static void zb_destroy(struct zb_source *src)
{
	if (src->key_tex) {
		obs_enter_graphics();
		gs_texture_destroy(src->key_tex);
		cm_image_free(&src->key_label_img);
		vertexbuffer_destroy_safe(src->key_label_vbuf);
		obs_leave_graphics();
	}

	obs_enter_graphics();
	gs_texture_destroy(src->scheme_lut);
	gs_texture_destroy(src->scheme_lut_raw);
	gs_texture_destroy(src->key_tex_raw);
	obs_leave_graphics();

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
	obs_enter_graphics();
	gs_texture_destroy(src->ext_tex);
	obs_leave_graphics();
	pthread_mutex_destroy(&src->ext_mutex);
	bfree(src->ext_img);
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
	obs_enter_graphics();
	cm_image_load(&src->falsecolor_lut, lut_filename);
	obs_leave_graphics();
}

static void fc_set_bands(struct zb_source *src, const struct fc_preset_band *preset, size_t n)
{
	struct fc_band bands[FC_MAX_BANDS];
	if (n > FC_MAX_BANDS)
		n = FC_MAX_BANDS;
	for (size_t i = 0; i < n; i++) {
		bands[i].y_max = preset[i].y_max;
		bands[i].color = preset[i].color;
		if (!strcmp(preset[i].label_key, "FC.Band.Decile"))
			snprintf(bands[i].label, sizeof(bands[i].label), "%s %d", obs_module_text("FC.Band.Decile"),
				 (int)i + 1);
		else
			snprintf(bands[i].label, sizeof(bands[i].label), "%s", obs_module_text(preset[i].label_key));
	}
	memcpy(src->bands, bands, sizeof(bands));
	src->n_bands = (int)n;
	src->scheme_dirty = true;
	src->raw_dirty = true;
}

/* OBS stores colors as 0xAABBGGRR; the bands use 0xAARRGGBB. */
static inline uint32_t abgr_to_argb(uint32_t c)
{
	return (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
}

static void fc_load_scheme(struct zb_source *src, obs_data_t *settings)
{
	switch ((int)obs_data_get_int(settings, "fc_preset")) {
	case FC_PRESET_CLIPPING:
		fc_set_bands(src, fc_preset_clipping, ARRAY_LEN(fc_preset_clipping));
		return;
	case FC_PRESET_SKIN:
		fc_set_bands(src, fc_preset_skin, ARRAY_LEN(fc_preset_skin));
		return;
	case FC_PRESET_SPECTRUM:
		fc_set_bands(src, fc_preset_spectrum, ARRAY_LEN(fc_preset_spectrum));
		return;
	case FC_PRESET_HLG:
		fc_set_bands(src, fc_preset_hlg, ARRAY_LEN(fc_preset_hlg));
		return;
	case FC_PRESET_PQ:
		fc_set_bands(src, fc_preset_pq, ARRAY_LEN(fc_preset_pq));
		return;
	case FC_PRESET_CUSTOM:
		break;
	default:
		fc_set_bands(src, fc_preset_exposure, ARRAY_LEN(fc_preset_exposure));
		return;
	}

	// Custom: bands sorted by their upper limit; 101 % stands for "100 % and over".
	struct fc_band bands[FC_MAX_BANDS];
	int n = (int)obs_data_get_int(settings, "fc_custom_count");
	if (n < 1)
		n = 1;
	if (n > FC_MAX_BANDS)
		n = FC_MAX_BANDS;
	for (int i = 0; i < n; i++) {
		char key[32];
		snprintf(key, sizeof(key), "fc_c%d_max", i + 1);
		const int max_pct = (int)obs_data_get_int(settings, key);
		bands[i].y_max = max_pct > 100 ? 2.0f : max_pct / 100.0f;
		snprintf(key, sizeof(key), "fc_c%d_grey", i + 1);
		const bool grey = obs_data_get_bool(settings, key);
		snprintf(key, sizeof(key), "fc_c%d_color", i + 1);
		bands[i].color = grey ? FC_GREY
				      : (abgr_to_argb((uint32_t)obs_data_get_int(settings, key)) | 0xFF000000);
		snprintf(key, sizeof(key), "fc_c%d_label", i + 1);
		const char *label = obs_data_get_string(settings, key);
		snprintf(bands[i].label, sizeof(bands[i].label), "%s", label && *label ? label : "");
	}
	for (int i = 1; i < n; i++) // insertion sort by upper limit
		for (int j = i; j > 0 && bands[j].y_max < bands[j - 1].y_max; j--) {
			struct fc_band t = bands[j];
			bands[j] = bands[j - 1];
			bands[j - 1] = t;
		}
	bands[n - 1].y_max = 2.0f; // the top band always reaches above white
	memcpy(src->bands, bands, sizeof(bands));
	src->n_bands = n;
	src->scheme_dirty = true;
	src->raw_dirty = true;
}

/* Builds the 256-entry color table the false color shader samples by luma. Graphics thread. */
static void fc_update_scheme_lut(struct zb_source *src)
{
	if (!src->scheme_dirty && src->scheme_lut)
		return;
	src->scheme_dirty = false;

	uint8_t buf[256 * 4];
	for (int i = 0, b = 0; i < 256; i++) {
		const float y = i / 255.0f;
		while (b < src->n_bands - 1 && y >= src->bands[b].y_max)
			b++;
		const uint32_t c = src->n_bands ? src->bands[b].color : FC_GREY;
		uint8_t *t = buf + i * 4;
		if (c == FC_GREY) {
			t[0] = t[1] = t[2] = (uint8_t)i;
		} else {
			t[0] = c & 0xFF;         // B
			t[1] = (c >> 8) & 0xFF;  // G
			t[2] = (c >> 16) & 0xFF; // R
		}
		t[3] = 0xFF;
	}
	const uint8_t *data = buf;
	if (!src->scheme_lut)
		src->scheme_lut = gs_texture_create(256, 1, GS_BGRA, 1, &data, GS_DYNAMIC);
	else
		gs_texture_set_image(src->scheme_lut, buf, 256 * 4, false);
}

/* The color table for the raw signal, by legal code: sub-black below 16, superwhite above 235. */
static void fc_update_scheme_lut_raw(struct zb_source *src)
{
	if (!src->raw_dirty && src->scheme_lut_raw)
		return;
	src->raw_dirty = false;
	uint8_t buf[256 * 4];
	for (int i = 0; i < 256; i++) {
		const float level = (i - 16) / 219.0f;
		uint32_t c;
		if (i < 16) {
			c = FC_SUBBLACK;
		} else if (i > 235) {
			c = FC_SUPERWHITE;
		} else {
			int b = 0;
			while (b < src->n_bands - 1 && level >= src->bands[b].y_max)
				b++;
			c = src->n_bands ? src->bands[b].color : FC_GREY;
		}
		uint8_t *t = buf + i * 4;
		if (c == FC_GREY) {
			const float g = fminf(fmaxf(level, 0.0f), 1.0f) * 255.0f;
			t[0] = t[1] = t[2] = (uint8_t)g;
		} else {
			t[0] = c & 0xFF;
			t[1] = (c >> 8) & 0xFF;
			t[2] = (c >> 16) & 0xFF;
		}
		t[3] = 0xFF;
	}
	const uint8_t *data = buf;
	if (!src->scheme_lut_raw)
		src->scheme_lut_raw = gs_texture_create(256, 1, GS_BGRA, 1, &data, GS_DYNAMIC);
	else
		gs_texture_set_image(src->scheme_lut_raw, buf, 256 * 4, false);
	if (!src->key_tex_raw) {
		uint8_t k[256 * 4];
		for (int i = 0; i < 256; i++)
			k[i * 4] = k[i * 4 + 1] = k[i * 4 + 2] = (uint8_t)i, k[i * 4 + 3] = 255;
		const uint8_t *kd = k;
		src->key_tex_raw = gs_texture_create(256, 1, GS_BGRA, 1, &kd, 0);
	}
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

	if (src->is_falsecolor) {
		fc_load_scheme(src, settings);
		src->gamut_mode = (int)obs_data_get_int(settings, "fc_gamut");
		src->opacity = (float)obs_data_get_double(settings, "fc_opacity") / 100.0f;
	}
}

static void zbs_update(void *data, obs_data_t *settings)
{
	struct zbs_source *src = data;
	cm_update(&src->cm, settings);
	zb_update(&src->zb, settings);

	// The table's percentages need luma on the CPU.
	// False color also takes the raw signal of a video source, which needs the captured luma on the CPU too.
	src->cm.flags = CM_FLAG_RAW_TEXTURE | CM_FLAG_CONVERT_YUV | CM_FLAG_EXT;
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

	// a starting point for the custom false color scheme (colors as 0xAABBGGRR)
	static const struct
	{
		int max;
		uint32_t color;
		bool grey;
		const char *label;
	} custom[] = {
		{2, 0xFFFF6020, false, "Crushed"},   {42, 0xFF808080, true, "Shadows"},
		{48, 0xFF40E040, false, "Mid grey"}, {95, 0xFF808080, true, "Bright"},
		{101, 0xFF2020FF, false, "Clipped"},
	};
	obs_data_set_default_int(settings, "fc_preset", FC_PRESET_EXPOSURE);
	obs_data_set_default_int(settings, "fc_custom_count", (int)ARRAY_LEN(custom));
	for (int i = 1; i <= FC_MAX_BANDS; i++) {
		char key[32];
		const bool known = i <= (int)ARRAY_LEN(custom);
		snprintf(key, sizeof(key), "fc_c%d_max", i);
		obs_data_set_default_int(settings, key, known ? custom[i - 1].max : 101);
		snprintf(key, sizeof(key), "fc_c%d_color", i);
		obs_data_set_default_int(settings, key, known ? custom[i - 1].color : 0xFFFFFFFF);
		snprintf(key, sizeof(key), "fc_c%d_grey", i);
		obs_data_set_default_bool(settings, key, known && custom[i - 1].grey);
		snprintf(key, sizeof(key), "fc_c%d_label", i);
		obs_data_set_default_string(settings, key, known ? custom[i - 1].label : "");
	}
}

/* The false color scope shows its key below the picture by default. */
static void fc_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "fc_opacity", 100.0);
	zb_get_defaults(settings);
	obs_data_set_default_int(settings, "show_key", show_key_below);
}

/* Shows the custom band settings only for the custom scheme, and only as many as are used. */
static bool fc_scheme_props_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const bool custom = obs_data_get_int(settings, "fc_preset") == FC_PRESET_CUSTOM;
	const int n = (int)obs_data_get_int(settings, "fc_custom_count");
	obs_property_set_visible(obs_properties_get(props, "fc_custom_count"), custom);
	for (int i = 1; i <= FC_MAX_BANDS; i++) {
		char key[32];
		snprintf(key, sizeof(key), "fc_c%d", i);
		obs_property_set_visible(obs_properties_get(props, key), custom && i <= n);
	}
	return true;
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
		prop = obs_properties_add_list(props, "fc_gamut", obs_module_text("FC.Prop.Gamut"), OBS_COMBO_TYPE_LIST,
					       OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(prop, obs_module_text("FC.Gamut.Off"), 0);
		obs_property_list_add_int(prop, obs_module_text("FC.Gamut.709"), 1);
		obs_property_list_add_int(prop, obs_module_text("FC.Gamut.P3"), 2);
		prop = obs_properties_add_list(props, "fc_preset", obs_module_text("FC.Prop.Preset"),
					       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(prop, obs_module_text("FC.Preset.Exposure"), FC_PRESET_EXPOSURE);
		obs_property_list_add_int(prop, obs_module_text("FC.Preset.Clipping"), FC_PRESET_CLIPPING);
		obs_property_list_add_int(prop, obs_module_text("FC.Preset.Skin"), FC_PRESET_SKIN);
		obs_property_list_add_int(prop, obs_module_text("FC.Preset.Spectrum"), FC_PRESET_SPECTRUM);
		obs_property_list_add_int(prop, obs_module_text("FC.Preset.HLG"), FC_PRESET_HLG);
		obs_property_list_add_int(prop, obs_module_text("FC.Preset.PQ"), FC_PRESET_PQ);
		obs_property_list_add_int(prop, obs_module_text("FC.Preset.Custom"), FC_PRESET_CUSTOM);
		obs_property_set_modified_callback(prop, fc_scheme_props_changed);

		prop = obs_properties_add_int_slider(props, "fc_custom_count", obs_module_text("FC.Prop.CustomCount"),
						     1, FC_MAX_BANDS, 1);
		obs_property_set_modified_callback(prop, fc_scheme_props_changed);
		for (int i = 1; i <= FC_MAX_BANDS; i++) {
			char key[32], desc[96];
			obs_properties_t *band = obs_properties_create();
			snprintf(key, sizeof(key), "fc_c%d_max", i);
			prop = obs_properties_add_int(band, key, obs_module_text("FC.Prop.Band.Max"), 1, 101, 1);
			obs_property_int_set_suffix(prop, "%");
			snprintf(key, sizeof(key), "fc_c%d_color", i);
			obs_properties_add_color(band, key, obs_module_text("FC.Prop.Band.Color"));
			snprintf(key, sizeof(key), "fc_c%d_grey", i);
			obs_properties_add_bool(band, key, obs_module_text("FC.Prop.Band.Grey"));
			snprintf(key, sizeof(key), "fc_c%d_label", i);
			obs_properties_add_text(band, key, obs_module_text("FC.Prop.Band.Label"), OBS_TEXT_DEFAULT);
			snprintf(key, sizeof(key), "fc_c%d", i);
			snprintf(desc, sizeof(desc), "%s %d", obs_module_text("FC.Prop.Band"), i);
			obs_properties_add_group(props, key, desc, OBS_GROUP_NORMAL, band);
		}

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

/* Writes the color scheme as a 33-point 3D LUT (.cube): every R'G'B' to the color of its luma band. */
static bool fc_export_cube(const struct zb_source *src, const char *path)
{
	FILE *fp = os_fopen(path, "w");
	if (!fp)
		return false;
	fprintf(fp, "TITLE \"Video Analyser false color\"\nLUT_3D_SIZE 33\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n");
	for (int b = 0; b < 33; b++) {
		for (int g = 0; g < 33; g++) {
			for (int r = 0; r < 33; r++) {
				const float y = 0.2126f * r / 32.0f + 0.7152f * g / 32.0f + 0.0722f * b / 32.0f;
				int k = 0;
				while (k < src->n_bands - 1 && y >= src->bands[k].y_max)
					k++;
				const uint32_t c = src->n_bands ? src->bands[k].color : FC_GREY;
				if (c == FC_GREY)
					fprintf(fp, "%.6f %.6f %.6f\n", y, y, y);
				else
					fprintf(fp, "%.6f %.6f %.6f\n", ((c >> 16) & 0xFF) / 255.0f,
						((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f);
			}
		}
	}
	fclose(fp);
	return true;
}

static bool fc_export_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct zbs_source *src = data;
	obs_data_t *s = obs_source_get_settings(src->cm.self);
	const char *path = obs_data_get_string(s, "fc_export_path");
	if (path && *path) {
		if (fc_export_cube(&src->zb, path))
			blog(LOG_INFO, "[" PLUGIN_NAME "] false color exported to '%s'", path);
		else
			blog(LOG_WARNING, "[" PLUGIN_NAME "] could not write '%s'", path);
	}
	obs_data_release(s);
	return false;
}

static obs_properties_t *fcs_get_properties(void *data)
{
	struct zbs_source *src = data;
	obs_properties_t *props;
	props = obs_properties_create();

	cm_get_properties(&src->cm, props);
	zb_get_properties(props, true);
	obs_properties_add_float_slider(props, "fc_opacity", obs_module_text("FC.Prop.Opacity"), 0.0, 100.0, 1.0);
	obs_properties_add_path(props, "fc_export_path", obs_module_text("FC.Prop.ExportPath"), OBS_PATH_FILE_SAVE,
				"Cube LUT (*.cube)", NULL);
	obs_properties_add_button2(props, "fc_export", obs_module_text("FC.Prop.Export"), fc_export_clicked, src);

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
	if (colorspace == CM_CS_601)
		return is_falsecolor ? "DrawFalseColor601" : "DrawZebra601";
	else if (cm_cs_is_hdr(colorspace))
		return is_falsecolor ? "DrawFalseColor2020" : "DrawZebra2020";
	else if (is_falsecolor)
		return "DrawFalseColor709";
	else
		return "DrawZebra709";
}

static void set_effect_params_raw(struct zb_source *src, bool mask_outside)
{
	gs_effect_t *e = src->effect;
	fc_update_scheme_lut_raw(src);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "mask_outside"), mask_outside ? 1.0f : 0.0f);
	gs_effect_set_bool(gs_effect_get_param_by_name(e, "use_lut"), true);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "lut"), src->scheme_lut_raw);
}

static void set_effect_params(struct zb_source *src, bool mask_outside)
{
	gs_effect_t *e = src->effect;
	gs_effect_set_float(gs_effect_get_param_by_name(e, "mask_outside"), mask_outside ? 1.0f : 0.0f);

	if (!src->is_falsecolor) {
		/* zebra */
		gs_effect_set_float(gs_effect_get_param_by_name(e, "zebra_th_low"), src->zebra_th_low);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "zebra_th_high"), src->zebra_th_high);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "zebra_tm"), src->zebra_tm);
	} else {
		/* falsecolor */
		gs_texture_t *lut = src->falsecolor_lut.texture;
		if (!lut) {
			fc_update_scheme_lut(src);
			lut = src->scheme_lut;
		}
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
		cm_image_load(&src->key_label_img, f);
		if (!src->key_label_img.loaded)
			blog(LOG_ERROR, "Cannot load falsecolor-key.png (%s)", f);
		bfree(f);
	}
}

static void zb_render_key(struct zb_source *src, const char *draw, uint32_t width, uint32_t height, bool raw)
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
	if (raw) {
		set_effect_params_raw(src, false);
		draw = "DrawFalseColorRaw";
	} else {
		set_effect_params(src, false);
	}
	gs_texture_t *key = raw ? src->key_tex_raw : src->key_tex;
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), key);

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
		gs_draw_sprite(key, 0, 0, 0);
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
			y = height * (def->yk + def->cyk * 256 * (raw ? (16.0f + 21.9f * i) / 255.0f : i / 10.0f));
			w = width * (def->xk - def->x0);
			h = height * fabsf(def->cyk * 256) / 10;
			img_cx = src->key_label_img.cx * 55; // cx
			img_cy = src->key_label_img.cy * 2;  // cy * 2 / 55
		} else {
			x = width * (def->xk + def->cxk * 256 * (raw ? (16.0f + 21.9f * i) / 255.0f : i / 10.0f));
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
static void fc_band_range(const struct fc_band *bands, int b, char *buf, size_t size)
{
	const int lo = b == 0 ? 0 : (int)lroundf(bands[b - 1].y_max * 100.0f);
	if (bands[b].y_max < 0.0f) {
		snprintf(buf, size, "<0%%");
		return;
	}
	if (bands[b].y_max > 1.5f && lo >= 100) {
		snprintf(buf, size, ">100%%");
		return;
	}
	if (bands[b].y_max > 1.0f)
		snprintf(buf, size, lo >= 100 ? ">100%%" : "%d%%+", lo);
	else
		snprintf(buf, size, "%d-%d%%", lo, (int)lroundf(bands[b].y_max * 100.0f));
}

static void zb_render_table(const struct zb_source *src, float x0, float tw, float height, float total_width,
			    const float *cov_in, bool raw)
{
	// with the raw signal: sub-black below the first band and superwhite above the last
	struct fc_band bands[FC_MAX_BANDS + 2];
	float cov[FC_MAX_BANDS + 2];
	const float *coverage = cov_in ? cov : NULL;
	int n = 0;
	if (src->n_bands <= 0)
		return;
	if (raw) {
		bands[n] = (struct fc_band){-1.0f, FC_SUBBLACK, ""};
		snprintf(bands[n].label, sizeof(bands[n].label), "%s", obs_module_text("FC.Band.SubBlack"));
		cov[n++] = cov_in ? cov_in[src->n_bands] : 0.0f;
	}
	for (int b = 0; b < src->n_bands; b++) {
		bands[n] = src->bands[b];
		if (raw && bands[n].y_max > 1.0f)
			bands[n].y_max = 1.0f;
		cov[n++] = cov_in ? cov_in[b] : 0.0f;
	}
	if (raw) {
		bands[n] = (struct fc_band){2.0f, FC_SUPERWHITE, ""};
		snprintf(bands[n].label, sizeof(bands[n].label), "%s", obs_module_text("FC.Band.SuperWhite"));
		cov[n++] = cov_in ? cov_in[src->n_bands + 1] : 0.0f;
	}
	const struct text_scale ts = text_scale_from_viewport(total_width, height);
	const float row_h = height / n;
	const float row_px = row_h / ts.uy;
	float font_px = row_px * 0.62f;
	if (font_px > 20.0f)
		font_px = 20.0f;

	fill_rect(x0, 0.0f, x0 + tw, height, 0xFF000000);
	if (font_px < 6.0f)
		return; // too small to read

	const float gap = 6.0f * ts.ux;
	const float sw = fminf(row_px * 0.7f, 22.0f) * ts.ux; // swatch is square on screen
	const float x_sw = x0 + gap;
	const float x_range = x_sw + sw + gap;
	const float x_label = x_range + text_width(&ts, "94-100%", font_px) + gap;
	const float x_right = x0 + tw - gap;

	for (int b = 0; b < n; b++) {
		const float y_top = (n - 1 - b) * row_h; // brightest band on top
		const float y_mid = y_top + row_h * 0.5f;
		const float pad_y = fmaxf(row_h * 0.15f, (row_h - sw / ts.ux * ts.uy) * 0.5f);
		const float share = coverage ? coverage[b] : 0.0f;
		const bool present = coverage && share >= 0.0005f;

		if (present)
			fill_rect(x0, y_top, x0 + tw, y_top + row_h, 0xFF1E1E1E);
		if (bands[b].color == FC_GREY) {
			// a band shown as grey: half dark, half light
			const float xm = x_sw + sw * 0.5f;
			fill_rect(x_sw, y_top + pad_y, xm, y_top + row_h - pad_y, 0xFF404040);
			fill_rect(xm, y_top + pad_y, x_sw + sw, y_top + row_h - pad_y, 0xFFB0B0B0);
		} else {
			fill_rect(x_sw, y_top + pad_y, x_sw + sw, y_top + row_h - pad_y, bands[b].color);
		}

		const uint32_t text_color = !coverage || present ? 0xFFE6E6E6 : 0xFF707070;
		char range[24];
		if (raw && b == 0)
			snprintf(range, sizeof(range), "<0%%");
		else if (raw && b == 1)
			snprintf(range, sizeof(range), "0-%d%%", (int)lroundf(bands[1].y_max * 100.0f));
		else
			fc_band_range(bands, b, range, sizeof(range));
		text_draw(&ts, range, x_range, y_mid, font_px, text_color, TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
		text_draw(&ts, bands[b].label, x_label, y_mid, font_px, text_color, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_MIDDLE);
		if (coverage) {
			char pct[16];
			snprintf(pct, sizeof(pct), "%.1f%%", share * 100.0f);
			text_draw(&ts, pct, x_right, y_mid, font_px, text_color, TEXT_ALIGN_RIGHT, TEXT_VALIGN_MIDDLE);
		}
	}

	// thin separators between rows
	for (int b = 1; b < n; b++)
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

		// the raw signal of a video source, when it matches the captured picture
		bool raw = false;
		if (fcs_raw_active(src)) {
			pthread_mutex_lock(&src->ext_mutex);
			if (src->ext_img && src->ext_w == cx && src->ext_h == cy) {
				if (src->ext_new || !src->ext_tex) {
					if (src->ext_tex && (src->ext_tex_w != cx || src->ext_tex_h != cy)) {
						gs_texture_destroy(src->ext_tex);
						src->ext_tex = NULL;
					}
					if (!src->ext_tex) {
						const uint8_t *d = src->ext_img;
						src->ext_tex = gs_texture_create(cx, cy, GS_BGRA, 1, &d, GS_DYNAMIC);
						src->ext_tex_w = cx;
						src->ext_tex_h = cy;
					} else {
						gs_texture_set_image(src->ext_tex, src->ext_img, cx * 4, false);
					}
					src->ext_new = false;
				}
				raw = src->ext_tex != NULL;
			}
			pthread_mutex_unlock(&src->ext_mutex);
		}
		const char *draw = draw_name(cm_get_colorspace(&src->cm), src->zb.is_falsecolor);
		if (src->zb.is_falsecolor && src->zb.gamut_mode) {
			// gamut view: grey inside, orange near the edge, red outside the chosen gamut
			const int cs = cm_get_colorspace(&src->cm);
			gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
			set_effect_params(&src->zb, true);
			gs_effect_set_float(gs_effect_get_param_by_name(e, "gamut_hdr"), cs == CM_CS_2100_PQ    ? 2.0f
											 : cs == CM_CS_2100_HLG ? 1.0f
														: 0.0f);
			gs_effect_set_float(gs_effect_get_param_by_name(e, "gamut_target"),
					    src->zb.gamut_mode == 2 ? 1.0f : 0.0f);
			while (gs_effect_loop(e, "DrawGamut"))
				gs_draw_sprite_subregion(tex, 0, 0, 0, cx, cy);
			const struct text_scale ts = text_scale_from_viewport((float)cx, (float)cy);
			const char *note = obs_module_text(!cm_cs_is_hdr(cs)         ? "FC.Gamut.SdrNote"
							   : src->zb.gamut_mode == 2 ? "FC.Gamut.LegendP3"
										     : "FC.Gamut.Legend709");
			const float tw = text_width(&ts, note, 11.0f);
			fill_rect(4.0f * ts.ux, cy - 22.0f * ts.uy, 12.0f * ts.ux + tw, cy - 4.0f * ts.uy, 0xB0000000);
			text_draw(&ts, note, 8.0f * ts.ux, cy - 13.0f * ts.uy, 11.0f, 0xFFE6E6EA, TEXT_ALIGN_LEFT,
				  TEXT_VALIGN_MIDDLE);
			PROFILE_END(prof_render_name);
			return;
		}
		if (raw && !src->zb.is_falsecolor) {
			// zebra: the picture as it is, the stripes from the raw luma
			gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
			gs_effect_set_texture(gs_effect_get_param_by_name(e, "raw"), src->ext_tex);
			set_effect_params(&src->zb, true);
			while (gs_effect_loop(e, "DrawZebraRaw"))
				gs_draw_sprite_subregion(tex, 0, 0, 0, cx, cy);
		} else if (raw) {
			gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), src->ext_tex);
			set_effect_params_raw(&src->zb, true);
			while (gs_effect_loop(e, "DrawFalseColorRaw"))
				gs_draw_sprite_subregion(src->ext_tex, 0, 0, 0, cx, cy);
		} else {
			gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
			set_effect_params(&src->zb, true);
			while (gs_effect_loop(e, draw))
				gs_draw_sprite_subregion(tex, 0, 0, 0, cx, cy);
		}

		// opacity: the picture shows through, false color times opacity plus the picture times the rest
		if (src->zb.is_falsecolor && src->zb.opacity < 0.995f) {
			const float k = 1.0f - fmaxf(src->zb.opacity, 0.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
			fill_rect(0.0f, 0.0f, (float)cx, (float)cy, (uint32_t)(k * 255.0f) << 24);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
			gs_effect_set_float(gs_effect_get_param_by_name(def, "multiplier"), k);
			while (gs_effect_loop(def, "DrawMultiply"))
				gs_draw_sprite_subregion(tex, 0, 0, 0, cx, cy);
			gs_blend_state_pop();
		}

		zb_render_key(&src->zb, draw, cx, cy, raw && src->zb.is_falsecolor);

		if (zb_has_table(&src->zb)) {
			const float tw = cy * 3.0f / 4.0f;
			const float *cov = src->has_coverage ? src->coverage[src->w_coverage ^ 1] : NULL;
			zb_render_table(&src->zb, (float)cx, tw, (float)cy, cx + tw, cov, raw && src->cov_raw);
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

	set_effect_params(&src->zb, false);

	gs_blend_state_push();
	gs_reset_blend_state();

	const char *draw = draw_name(src->colorspace, src->zb.is_falsecolor);
	obs_source_process_filter_tech_end(src->context, e, 0, 0, draw);

	obs_source_t *target = obs_filter_get_target(src->context);
	if (target) {
		uint32_t cx = obs_source_get_base_width(target);
		uint32_t cy = obs_source_get_base_height(target);
		zb_render_key(&src->zb, draw, cx, cy, false);
		if (zb_has_table(&src->zb)) {
			const float tw = cy * 3.0f / 4.0f;
			zb_render_table(&src->zb, (float)cx, tw, (float)cy, cx + tw, NULL, false);
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
	.get_defaults = fc_source_get_defaults,
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

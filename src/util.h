#pragma once

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

void property_list_add_sources(obs_property_t *prop, obs_source_t *self);
obs_property_t *properties_add_colorspace(obs_properties_t *props, const char *name, const char *description);

/* Color spaces of the analyzed signal. 1 and 2 are SDR R'G'B' with that matrix;
 * CM_CS_2100_HLG captures in linear float and encodes to Rec. 2100 HLG with Rec. 2020 primaries
 * and matrix, the same way OBS encodes its HLG output. */
#define CM_CS_601 1
#define CM_CS_709 2
#define CM_CS_2100_HLG 3
#define CM_CS_2100_PQ 4

/* HDR color spaces: captured in linear float, Rec. 2020 primaries and matrix. */
static inline bool cm_cs_is_hdr(int cs)
{
	return cs == CM_CS_2100_HLG || cs == CM_CS_2100_PQ;
}

int calc_colorspace(int);

/* PQ (SMPTE ST 2084) signal 0..1 to display light relative to 10000 nits. */
static inline float pq_to_linear(float e)
{
	const float m1 = 0.1593017578f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
	const float p = powf(e > 0.0f ? e : 0.0f, 1.0f / m2);
	const float num = p - c1 > 0.0f ? p - c1 : 0.0f;
	return powf(num / (c2 - c3 * p), 1.0f / m1);
}

/* HLG signal 0..1 to scene light 0..1 (inverse OETF). */
static inline float hlg_to_scene(float e)
{
	return e <= 0.5f ? e * e / 3.0f : (expf((e - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
}

gs_effect_t *create_effect_from_module_file(const char *basename);

/* An image file as a texture. OBS 33 replaced gs_image_file_t, so this uses
 * gs_texture_create_from_file, which every supported OBS version has. */
struct cm_image
{
	gs_texture_t *texture;
	uint32_t cx, cy;
	bool loaded;
};

/* Call with the graphics context entered. */
static inline void cm_image_load(struct cm_image *img, const char *file)
{
	img->texture = file ? gs_texture_create_from_file(file) : NULL;
	img->loaded = img->texture != NULL;
	img->cx = img->loaded ? gs_texture_get_width(img->texture) : 0;
	img->cy = img->loaded ? gs_texture_get_height(img->texture) : 0;
}

/* Call with the graphics context entered. */
static inline void cm_image_free(struct cm_image *img)
{
	gs_texture_destroy(img->texture);
	img->texture = NULL;
	img->cx = img->cy = 0;
	img->loaded = false;
}

/* Destroys a vertex buffer and makes sure the device no longer points at it.
 * libobs keeps the last loaded vertex buffer as current, and some of its own draws
 * (async video conversion) use the current one; a destroyed one there crashes the driver.
 * Call with the graphics context entered. */
static inline void vertexbuffer_destroy_safe(gs_vertbuffer_t *vb)
{
	if (!vb)
		return;
	gs_load_vertexbuffer(NULL);
	gs_vertexbuffer_destroy(vb);
}

#ifdef __cplusplus
}
#endif

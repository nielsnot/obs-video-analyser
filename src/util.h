#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void property_list_add_sources(obs_property_t *prop, obs_source_t *self);
obs_property_t *properties_add_colorspace(obs_properties_t *props, const char *name, const char *description);

int calc_colorspace(int);

gs_effect_t *create_effect_from_module_file(const char *basename);

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

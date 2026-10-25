#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A 3D LUT as RGBA floats, red changing fastest, then green, then blue. */
struct cube_lut
{
	float *data;
	int size; // points per axis
	float domain_min[3], domain_max[3];
};

bool cube_lut_load(const char *path, struct cube_lut *out);
void cube_lut_free(struct cube_lut *lut);

#ifdef __cplusplus
}
#endif

/*
 * .cube LUT files (Adobe / Resolve format): 3D LUTs, and 1D LUTs expanded to 3D.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "plugin-macros.generated.h"
#include "lut.h"

#define LUT_1D_EXPAND 33
#define LUT_MAX_3D 129
#define LUT_MAX_1D 65536

static bool starts_with(const char *line, const char *key)
{
	const size_t n = strlen(key);
	return strncmp(line, key, n) == 0 && (line[n] == ' ' || line[n] == '\t');
}

/* Reads a number with a decimal point whatever the locale; returns the end, or NULL. */
static const char *parse_float(const char *p, float *out)
{
	while (*p == ' ' || *p == '\t')
		p++;
	double sign = 1.0, v = 0.0;
	if (*p == '-' || *p == '+')
		sign = *p++ == '-' ? -1.0 : 1.0;
	bool digits = false;
	while (isdigit((unsigned char)*p)) {
		v = v * 10.0 + (*p++ - '0');
		digits = true;
	}
	if (*p == '.') {
		p++;
		double f = 0.1;
		while (isdigit((unsigned char)*p)) {
			v += (*p++ - '0') * f;
			f *= 0.1;
			digits = true;
		}
	}
	if (!digits)
		return NULL;
	if (*p == 'e' || *p == 'E') {
		p++;
		int es = 1, e = 0;
		if (*p == '-' || *p == '+')
			es = *p++ == '-' ? -1 : 1;
		while (isdigit((unsigned char)*p))
			e = e * 10 + (*p++ - '0');
		double m = 1.0;
		for (int i = 0; i < e; i++)
			m *= 10.0;
		v = es > 0 ? v * m : v / m;
	}
	*out = (float)(sign * v);
	return p;
}

static int parse_floats(const char *p, float *out, int n)
{
	int i = 0;
	for (; i < n && p; i++)
		p = parse_float(p, out + i);
	return p ? i : i - 1;
}

static float lerp_1d(const float *v, int n, float x, int c)
{
	if (x <= 0.0f)
		return v[c];
	if (x >= 1.0f)
		return v[(n - 1) * 3 + c];
	const float f = x * (n - 1);
	const int i = (int)f;
	const float t = f - i;
	return v[i * 3 + c] * (1.0f - t) + v[(i + 1) * 3 + c] * t;
}

bool cube_lut_load(const char *path, struct cube_lut *out)
{
	memset(out, 0, sizeof(*out));
	char *text = os_quick_read_utf8_file(path);
	if (!text) {
		blog(LOG_WARNING, "Cannot read LUT '%s'", path);
		return false;
	}

	int size3 = 0, size1 = 0;
	float dmin[3] = {0.0f, 0.0f, 0.0f}, dmax[3] = {1.0f, 1.0f, 1.0f};
	float *values = NULL;
	size_t n_values = 0, cap = 0;

	for (char *next = text, *line; next && *next;) {
		// split off one line (strtok_r is not available everywhere)
		line = next;
		char *eol = strpbrk(line, "\r\n");
		if (eol) {
			*eol = 0;
			next = eol + 1;
		} else {
			next = NULL;
		}
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#')
			continue;
		if (starts_with(line, "LUT_3D_SIZE")) {
			size3 = atoi(line + 11);
		} else if (starts_with(line, "LUT_1D_SIZE")) {
			size1 = atoi(line + 11);
		} else if (starts_with(line, "DOMAIN_MIN")) {
			parse_floats(line + 10, dmin, 3);
		} else if (starts_with(line, "DOMAIN_MAX")) {
			parse_floats(line + 10, dmax, 3);
		} else if (starts_with(line, "LUT_3D_INPUT_RANGE") || starts_with(line, "LUT_1D_INPUT_RANGE")) {
			float ab[2] = {0.0f, 1.0f};
			parse_floats(line + 18, ab, 2);
			const float a = ab[0], b = ab[1];
			dmin[0] = dmin[1] = dmin[2] = a;
			dmax[0] = dmax[1] = dmax[2] = b;
		} else if (isdigit((unsigned char)*line) || *line == '-' || *line == '.') {
			float rgb[3];
			if (parse_floats(line, rgb, 3) != 3)
				continue;
			const float r = rgb[0], g = rgb[1], b = rgb[2];
			if (n_values + 3 > cap) {
				cap = cap ? cap * 2 : 3 * 4096;
				values = brealloc(values, cap * sizeof(float));
			}
			values[n_values++] = r;
			values[n_values++] = g;
			values[n_values++] = b;
		}
		// TITLE and other keywords are ignored
	}
	bfree(text);

	bool ok = false;
	if (size3 >= 2 && size3 <= LUT_MAX_3D && n_values == (size_t)size3 * size3 * size3 * 3) {
		const size_t n = (size_t)size3 * size3 * size3;
		out->data = bmalloc(n * 4 * sizeof(float));
		for (size_t i = 0; i < n; i++) {
			out->data[i * 4 + 0] = values[i * 3 + 0];
			out->data[i * 4 + 1] = values[i * 3 + 1];
			out->data[i * 4 + 2] = values[i * 3 + 2];
			out->data[i * 4 + 3] = 1.0f;
		}
		out->size = size3;
		ok = true;
	} else if (size1 >= 2 && size1 <= LUT_MAX_1D && n_values == (size_t)size1 * 3) {
		// a 1D LUT works per channel; build the equivalent 3D LUT
		const int s = LUT_1D_EXPAND;
		out->data = bmalloc((size_t)s * s * s * 4 * sizeof(float));
		for (int b = 0; b < s; b++)
			for (int g = 0; g < s; g++)
				for (int r = 0; r < s; r++) {
					float *d = out->data + (((size_t)b * s + g) * s + r) * 4;
					d[0] = lerp_1d(values, size1, (float)r / (s - 1), 0);
					d[1] = lerp_1d(values, size1, (float)g / (s - 1), 1);
					d[2] = lerp_1d(values, size1, (float)b / (s - 1), 2);
					d[3] = 1.0f;
				}
		out->size = s;
		ok = true;
	} else {
		blog(LOG_WARNING, "LUT '%s': unsupported size (3D %d, 1D %d, %zu values)", path, size3, size1,
		     n_values / 3);
	}
	bfree(values);

	if (ok) {
		memcpy(out->domain_min, dmin, sizeof(dmin));
		memcpy(out->domain_max, dmax, sizeof(dmax));
		blog(LOG_INFO, "Loaded LUT '%s' (%d points per axis)", path, out->size);
	}
	return ok;
}

void cube_lut_free(struct cube_lut *lut)
{
	bfree(lut->data);
	lut->data = NULL;
	lut->size = 0;
}

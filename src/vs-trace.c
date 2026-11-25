#include <obs-module.h>
#include <math.h>
#include <string.h>
#include "util.h"
#include "vs-trace.h"

#define N (VST_SIZE * VST_SIZE)
#define DEG_RAD (3.14159265f / 180.0f)
#define R_OUTER 127.5f // the outer ring of a circle layout, in scope pixels

void vst_init(struct vst_state *s)
{
	memset(s, 0, sizeof(*s));
}

void vst_free(struct vst_state *s)
{
	bfree(s->cnt);
	bfree(s->r);
	bfree(s->g);
	bfree(s->b);
	bfree(s->wc);
	bfree(s->wr);
	bfree(s->wg);
	bfree(s->wb);
	bfree(s->tmp);
	bfree(s->rgba);
	bfree(s->bright);
	memset(s, 0, sizeof(*s));
}

static void ensure(struct vst_state *s)
{
	if (s->cnt)
		return;
	s->cnt = bzalloc(N * sizeof(float));
	s->r = bzalloc(N * sizeof(float));
	s->g = bzalloc(N * sizeof(float));
	s->b = bzalloc(N * sizeof(float));
	s->wc = bzalloc(N * sizeof(float));
	s->wr = bzalloc(N * sizeof(float));
	s->wg = bzalloc(N * sizeof(float));
	s->wb = bzalloc(N * sizeof(float));
	s->tmp = bzalloc(N * sizeof(float));
	s->rgba = bzalloc(N * sizeof(uint32_t));
	s->bright = bzalloc(N);
}

bool vst_is_plain(const struct vst_params *p)
{
	return p->projection == VST_CBCR && p->layout == VST_CIRCLE && fabsf(p->hue_rot) < 0.01f && !p->expand;
}

static void matrix(int colorspace, float *kr, float *kb)
{
	*kr = colorspace == CM_CS_601 ? 0.299f : cm_cs_is_hdr(colorspace) ? 0.2627f : 0.2126f;
	*kb = colorspace == CM_CS_601 ? 0.114f : cm_cs_is_hdr(colorspace) ? 0.0593f : 0.0722f;
}

/* The skin tone line of the projection, so the semicircle and the quadrant can put it in their middle. */
static float skin_dir(const struct vst_params *p)
{
	// once per projection and color space: vst_map runs for every pixel
	static float cache[3][CM_CS_2100_PQ + 1];
	static bool have[3][CM_CS_2100_PQ + 1];
	const int pr = p->projection >= 0 && p->projection < 3 ? p->projection : 0;
	const int cs = p->colorspace >= 0 && p->colorspace <= CM_CS_2100_PQ ? p->colorspace : CM_CS_709;
	if (!have[pr][cs]) {
		float a, r;
		struct vst_params q = *p;
		q.projection = pr;
		q.colorspace = cs;
		vst_polar_rgb(&q, 0.85f, 0.62f, 0.50f, &a, &r);
		cache[pr][cs] = a;
		have[pr][cs] = true;
	}
	return cache[pr][cs];
}

bool vst_map(const struct vst_params *p, float angle, float radius, float *x, float *y)
{
	if (radius < 0.0f)
		radius = 0.0f;
	if (p->expand)
		radius = sqrtf(radius);
	float a = angle + p->hue_rot * DEG_RAD;
	switch (p->layout) {
	case VST_SEMICIRCLE: {
		// the skin tone line straight up, the upper half twice as large
		a += 90.0f * DEG_RAD - skin_dir(p);
		const float cx = 127.5f, cy = 250.0f, rr = 245.0f;
		*x = cx + rr * radius * cosf(a);
		*y = cy - rr * radius * sinf(a);
		return sinf(a) >= -0.02f && radius <= 1.0f;
	}
	case VST_QUADRANT: {
		// the skin tone line on the diagonal of a quarter, four times as large
		a += 45.0f * DEG_RAD - skin_dir(p);
		const float cx = 6.0f, cy = 250.0f, rr = 244.0f;
		*x = cx + rr * radius * cosf(a);
		*y = cy - rr * radius * sinf(a);
		return cosf(a) >= -0.02f && sinf(a) >= -0.02f && radius <= 1.0f;
	}
	case VST_RECTANGLE: {
		// hue across (starting at the opposite of the skin tone line), saturation up
		a += 180.0f * DEG_RAD - skin_dir(p);
		a = fmodf(a, 2.0f * 3.14159265f);
		if (a < 0.0f)
			a += 2.0f * 3.14159265f;
		*x = 2.0f + a / (2.0f * 3.14159265f) * 252.0f;
		*y = 252.0f - fminf(radius, 1.0f) * 248.0f;
		return radius <= 1.0f;
	}
	case VST_CIRCLE:
	default:
		*x = 127.5f + R_OUTER * radius * cosf(a);
		*y = 127.5f - R_OUTER * radius * sinf(a);
		return radius <= 1.0f;
	}
}

void vst_unmap(const struct vst_params *p, float x, float y, float *angle, float *radius)
{
	float a, r;
	switch (p->layout) {
	case VST_SEMICIRCLE:
		a = atan2f(250.0f - y, x - 127.5f) - (90.0f * DEG_RAD - skin_dir(p));
		r = hypotf(x - 127.5f, 250.0f - y) / 245.0f;
		break;
	case VST_QUADRANT:
		a = atan2f(250.0f - y, x - 6.0f) - (45.0f * DEG_RAD - skin_dir(p));
		r = hypotf(x - 6.0f, 250.0f - y) / 244.0f;
		break;
	case VST_RECTANGLE:
		a = (x - 2.0f) / 252.0f * 2.0f * 3.14159265f - (180.0f * DEG_RAD - skin_dir(p));
		r = (252.0f - y) / 248.0f;
		break;
	case VST_CIRCLE:
	default:
		a = atan2f(127.5f - y, x - 127.5f);
		r = hypotf(x - 127.5f, 127.5f - y) / R_OUTER;
		break;
	}
	a -= p->hue_rot * DEG_RAD;
	if (r < 0.0f)
		r = 0.0f;
	if (p->expand)
		r = r * r;
	a = fmodf(a, 2.0f * 3.14159265f);
	if (a < 0.0f)
		a += 2.0f * 3.14159265f;
	*angle = a;
	*radius = r;
}

void vst_polar_rgb(const struct vst_params *p, float r, float g, float b, float *angle, float *radius)
{
	switch (p->projection) {
	case VST_HSV: {
		const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), c = mx - mn;
		float h = 0.0f;
		if (c > 1e-5f) {
			if (mx == r)
				h = fmodf((g - b) / c, 6.0f);
			else if (mx == g)
				h = (b - r) / c + 2.0f;
			else
				h = (r - g) / c + 4.0f;
		}
		// red where it is on a Cb/Cr vectorscope, the hues in the same order around
		*angle = 103.0f * DEG_RAD + h * 60.0f * DEG_RAD;
		*radius = mx > 1e-5f ? c / mx : 0.0f;
		break;
	}
	case VST_OPPONENT: {
		const float x = r - g, yv = 0.5f * (r + g) - b;
		*angle = atan2f(yv, x);
		*radius = hypotf(x, yv) / 1.118f; // full red is the farthest
		break;
	}
	case VST_CBCR:
	default: {
		float kr, kb;
		matrix(p->colorspace, &kr, &kb);
		const float yy = kr * r + (1.0f - kr - kb) * g + kb * b;
		// in 8-bit steps, so a circle layout is where the classic vectorscope has it
		const float u = 112.0f * (b - yy) / (1.0f - kb), v = 112.0f * (r - yy) / (1.0f - kr);
		*angle = atan2f(v, u);
		*radius = hypotf(u, v) / R_OUTER;
		break;
	}
	}
}

void vst_accumulate(struct vst_state *s, const struct vst_params *p, const uint8_t *yuv, uint32_t width,
		    uint32_t height, uint32_t linesize, float persistence)
{
	ensure(s);
	float kr, kb;
	matrix(p->colorspace, &kr, &kb);
	const float kg = 1.0f - kr - kb;
	const bool plain = vst_is_plain(p);

	// the new frame into the work buffers
	memset(s->wc, 0, N * sizeof(float));
	memset(s->wr, 0, N * sizeof(float));
	memset(s->wg, 0, N * sizeof(float));
	memset(s->wb, 0, N * sizeof(float));
	int max_d2 = 0;
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *vd = yuv + (size_t)y * linesize;
		for (uint32_t x = 0; x < width; x++, vd += 4) {
			const int u = vd[0], luma = vd[1], v = vd[2];
			if (!vd[3])
				continue; // outside the selection
			if ((p->luma == VST_LUMA_LOWS && luma >= 85) ||
			    (p->luma == VST_LUMA_MIDS && (luma < 85 || luma >= 170)) ||
			    (p->luma == VST_LUMA_HIGHS && luma < 170))
				continue;
			const int du = u - 128, dv = v - 128;
			if (du * du + dv * dv > max_d2)
				max_d2 = du * du + dv * dv;
			// R'G'B' of the pixel (video range Y'CbCr)
			const float yy = (luma - 16) / 219.0f, cb = du / 224.0f, cr = dv / 224.0f;
			const float r = yy + 2.0f * (1.0f - kr) * cr, b = yy + 2.0f * (1.0f - kb) * cb;
			const float g = (yy - kr * r - kb * b) / kg;
			int idx;
			if (plain) {
				idx = u + VST_SIZE * (255 - v); // exactly where the classic vectorscope puts it
			} else {
				float a, rad, px, py;
				if (p->projection == VST_CBCR) {
					a = atan2f((float)dv, (float)du);
					rad = sqrtf((float)(du * du + dv * dv)) / R_OUTER;
				} else {
					vst_polar_rgb(p, fminf(fmaxf(r, 0.0f), 1.0f), fminf(fmaxf(g, 0.0f), 1.0f),
						      fminf(fmaxf(b, 0.0f), 1.0f), &a, &rad);
				}
				if (!vst_map(p, a, rad, &px, &py))
					continue;
				const int ix = (int)px, iy = (int)py;
				if (ix < 0 || iy < 0 || ix >= VST_SIZE || iy >= VST_SIZE)
					continue;
				idx = ix + VST_SIZE * iy;
			}
			s->wc[idx] += 1.0f;
			s->wr[idx] += r;
			s->wg[idx] += g;
			s->wb[idx] += b;
		}
	}
	s->max_sat = sqrtf((float)max_d2) / 127.0f;

	// with persistence, a running average over the frames
	const float keep = s->has_data ? fminf(fmaxf(persistence, 0.0f), 0.95f) : 0.0f, add = 1.0f - keep;
	for (int i = 0; i < N; i++) {
		s->cnt[i] = s->cnt[i] * keep + s->wc[i] * add;
		s->r[i] = s->r[i] * keep + s->wr[i] * add;
		s->g[i] = s->g[i] * keep + s->wg[i] * add;
		s->b[i] = s->b[i] * keep + s->wb[i] * add;
	}
	s->has_data = true;
}

/* A box blur of the given radius, horizontally and vertically, from src into dst. */
static void blur(const float *src, float *dst, float *tmp, int rad)
{
	const float norm = 1.0f / (2 * rad + 1);
	for (int y = 0; y < VST_SIZE; y++) {
		const float *row = src + y * VST_SIZE;
		float sum = 0.0f;
		for (int x = -rad; x <= rad; x++)
			sum += row[x < 0 ? 0 : x];
		for (int x = 0; x < VST_SIZE; x++) {
			tmp[y * VST_SIZE + x] = sum * norm;
			const int out = x - rad, in = x + rad + 1;
			sum += row[in >= VST_SIZE ? VST_SIZE - 1 : in] - row[out < 0 ? 0 : out];
		}
	}
	for (int x = 0; x < VST_SIZE; x++) {
		float sum = 0.0f;
		for (int y = -rad; y <= rad; y++)
			sum += tmp[(y < 0 ? 0 : y) * VST_SIZE + x];
		for (int y = 0; y < VST_SIZE; y++) {
			dst[y * VST_SIZE + x] = sum * norm;
			const int out = y - rad, in = y + rad + 1;
			sum += tmp[(in >= VST_SIZE ? VST_SIZE - 1 : in) * VST_SIZE + x] -
			       tmp[(out < 0 ? 0 : out) * VST_SIZE + x];
		}
	}
}

static inline float curve_of(int curve, float x)
{
	switch (curve) {
	case VST_SOFT:
		return 1.0f - expf(-x);
	case VST_LOG:
		return logf(1.0f + 16.0f * x) / logf(17.0f);
	default:
		return x;
	}
}

void vst_build(struct vst_state *s, int curve, float intensity, int smooth, bool colored)
{
	ensure(s);
	const float *c = s->cnt, *r = s->r, *g = s->g, *b = s->b;
	if (smooth > 0) {
		if (smooth > 3)
			smooth = 3;
		blur(s->cnt, s->wc, s->tmp, smooth);
		blur(s->r, s->wr, s->tmp, smooth);
		blur(s->g, s->wg, s->tmp, smooth);
		blur(s->b, s->wb, s->tmp, smooth);
		c = s->wc, r = s->wr, g = s->wg, b = s->wb;
	}
	// a smoothed point spreads its count; scale up so a single point keeps about its brightness
	const float spread = smooth > 0 ? (float)((2 * smooth + 1) * (2 * smooth + 1)) * 0.5f : 1.0f;
	const float k = intensity / 255.0f * spread;
	for (int i = 0; i < N; i++) {
		float v = curve_of(curve, c[i] * k);
		if (v > 1.0f)
			v = 1.0f;
		s->bright[i] = (uint8_t)(v * 255.0f + 0.5f);
		float cr = 1.0f, cg = 1.0f, cb = 1.0f;
		if (colored && c[i] > 1e-6f) {
			cr = fmaxf(r[i] / c[i], 0.0f), cg = fmaxf(g[i] / c[i], 0.0f), cb = fmaxf(b[i] / c[i], 0.0f);
			const float mx = fmaxf(cr, fmaxf(cg, cb));
			if (mx > 1e-5f)
				cr /= mx, cg /= mx, cb /= mx;
			else
				cr = cg = cb = 1.0f;
			// a little lift, so dark hues stay visible
			cr = 0.2f + 0.8f * cr, cg = 0.2f + 0.8f * cg, cb = 0.2f + 0.8f * cb;
		}
		const uint32_t R = (uint32_t)(cr * v * 255.0f + 0.5f), G = (uint32_t)(cg * v * 255.0f + 0.5f),
			       B = (uint32_t)(cb * v * 255.0f + 0.5f);
		s->rgba[i] = 0xFF000000u | B << 16 | G << 8 | R;
	}
}

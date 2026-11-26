#include "autoselect.h"
#include <util/bmem.h>
#include <math.h>
#include <string.h>

/* Running mean and spread of colors. */
struct stats
{
	double s[3], ss[3];
	int n;
};

static void st_add(struct stats *st, const float *c)
{
	for (int k = 0; k < 3; k++) {
		st->s[k] += c[k];
		st->ss[k] += (double)c[k] * c[k];
	}
	st->n++;
}

static bool st_get(const struct stats *st, int min_n, float m[3], float *sd)
{
	if (st->n < min_n)
		return false;
	double var = 0.0;
	for (int k = 0; k < 3; k++) {
		const double mean = st->s[k] / st->n;
		m[k] = (float)mean;
		var += st->ss[k] / st->n - mean * mean;
	}
	*sd = (float)sqrt(fmax(var / 3.0, 0.0));
	return true;
}

static inline float dist3(const float *a, const float *b)
{
	const float dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
	return sqrtf(dr * dr + dg * dg + db * db);
}

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* Y', Cb and Cr (8-bit, full range) of an R'G'B' color. */
static inline void to_ycc(const float *c, float *y, float *cb, float *cr)
{
	*y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
	*cb = 128.0f + (c[2] - *y) * 0.5389f;
	*cr = 128.0f + (c[0] - *y) * 0.6350f;
}

struct rectf
{
	float x0, y0, x1, y1;
};

static inline bool in_rect(const struct rectf *r, float u, float v)
{
	return u >= r->x0 && u <= r->x1 && v >= r->y0 && v <= r->y1;
}

/* The parts of the picture around one face, normalized. */
struct face_regions
{
	float cx, cy, rx, ry; // the face ellipse
	struct rectf head;    // face and hair
	struct rectf torso;   // below the chin
	struct rectf hair_sample, cloth_sample, skin_sample;
};

static void face_regions_of(const struct face_box *f, struct face_regions *r)
{
	r->cx = f->x + f->w * 0.5f;
	r->cy = f->y + f->h * 0.5f;
	r->rx = f->w * 0.55f;
	r->ry = f->h * 0.62f;
	r->head = (struct rectf){f->x - f->w * 0.45f, f->y - f->h * 0.75f, f->x + f->w * 1.45f, f->y + f->h * 1.05f};
	r->torso = (struct rectf){r->cx - f->w * 1.7f, f->y + f->h * 1.05f, r->cx + f->w * 1.7f, f->y + f->h * 6.0f};
	r->hair_sample =
		(struct rectf){f->x + f->w * 0.2f, f->y - f->h * 0.35f, f->x + f->w * 0.8f, f->y - f->h * 0.05f};
	r->cloth_sample =
		(struct rectf){r->cx - f->w * 0.6f, f->y + f->h * 1.6f, r->cx + f->w * 0.6f, f->y + f->h * 2.6f};
	// cheeks and nose: skin without eyes, brows or beard
	r->skin_sample =
		(struct rectf){f->x + f->w * 0.25f, f->y + f->h * 0.35f, f->x + f->w * 0.75f, f->y + f->h * 0.75f};
}

static inline bool in_ellipse(const struct face_regions *r, float u, float v, float k)
{
	const float du = (u - r->cx) / (r->rx * k), dv = (v - r->cy) / (r->ry * k);
	return du * du + dv * dv <= 1.0f;
}

void auto_select_free(struct auto_select *as)
{
	bfree(as->map);
	bfree(as->score);
	bfree(as->cls);
	bfree(as->tmp);
	bfree(as->avg);
	memset(as, 0, sizeof(*as));
}

static void resize(struct auto_select *as, uint32_t w, uint32_t h)
{
	uint32_t block = w / 240;
	if (block < 2)
		block = 2;
	const uint32_t bw = (w + block - 1) / block, bh = (h + block - 1) / block;
	if (bw == as->bw && bh == as->bh && block == as->block && as->map)
		return;
	auto_select_free(as);
	as->bw = bw;
	as->bh = bh;
	as->block = block;
	const size_t n = (size_t)bw * bh;
	as->map = bzalloc(n);
	as->score = bzalloc(n);
	as->cls = bzalloc(n);
	as->tmp = bzalloc(n);
	as->avg = bzalloc(n * 3 * sizeof(float));
}

/* The mean R'G'B' of every block (every other pixel of a large block). */
static void block_means(struct auto_select *as, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t linesize)
{
	const uint32_t B = as->block, step = B >= 4 ? 2 : 1;
	for (uint32_t by = 0; by < as->bh; by++) {
		const uint32_t y0 = by * B, y1 = y0 + B < h ? y0 + B : h;
		for (uint32_t bx = 0; bx < as->bw; bx++) {
			const uint32_t x0 = bx * B, x1 = x0 + B < w ? x0 + B : w;
			uint32_t sr = 0, sg = 0, sb = 0, n = 0;
			for (uint32_t y = y0; y < y1; y += step) {
				const uint8_t *p = bgra + (size_t)linesize * y + (size_t)x0 * 4;
				for (uint32_t x = x0; x < x1; x += step, p += 4 * step) {
					sb += p[0];
					sg += p[1];
					sr += p[2];
					n++;
				}
			}
			float *c = as->avg + ((size_t)by * as->bw + bx) * 3;
			const float k = n ? 1.0f / (float)n : 0.0f;
			c[0] = sr * k;
			c[1] = sg * k;
			c[2] = sb * k;
		}
	}
}

#define MAX_REGIONS FACE_MAX

void auto_select_run(struct auto_select *as, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t linesize,
		     const struct face_box *faces, int n_faces, int classes, bool smooth, bool stable)
{
	if (!bgra || w < 8 || h < 8)
		return;
	resize(as, w, h);
	block_means(as, bgra, w, h, linesize);

	const uint32_t bw = as->bw, bh = as->bh;
	const size_t n = (size_t)bw * bh;
	if (n_faces > MAX_REGIONS)
		n_faces = MAX_REGIONS;
	struct face_regions reg[MAX_REGIONS];
	for (int i = 0; i < n_faces; i++)
		face_regions_of(faces + i, reg + i);

	// the skin color, learned from the cheeks of the faces found
	double s_cb = 0.0, s_cr = 0.0, s_y = 0.0, ss_cb = 0.0, ss_cr = 0.0;
	int n_skin = 0;
	for (uint32_t by = 0; by < bh && n_faces; by++) {
		const float v = (by + 0.5f) / bh;
		for (uint32_t bx = 0; bx < bw; bx++) {
			const float u = (bx + 0.5f) / bw;
			for (int i = 0; i < n_faces; i++) {
				if (!in_rect(&reg[i].skin_sample, u, v))
					continue;
				float y, cb, cr;
				to_ycc(as->avg + ((size_t)by * bw + bx) * 3, &y, &cb, &cr);
				s_cb += cb;
				s_cr += cr;
				s_y += y;
				ss_cb += (double)cb * cb;
				ss_cr += (double)cr * cr;
				n_skin++;
				break;
			}
		}
	}
	const bool adaptive = n_skin >= 4;
	float m_cb = 0.0f, m_cr = 0.0f, m_y = 0.0f, t_cb = 1.0f, t_cr = 1.0f;
	if (adaptive) {
		m_cb = (float)(s_cb / n_skin);
		m_cr = (float)(s_cr / n_skin);
		m_y = (float)(s_y / n_skin);
		const float sd_cb = (float)sqrt(fmax(ss_cb / n_skin - (double)m_cb * m_cb, 0.0));
		const float sd_cr = (float)sqrt(fmax(ss_cr / n_skin - (double)m_cr * m_cr, 0.0));
		t_cb = clampf(sd_cb * 2.5f, 6.0f, 18.0f);
		t_cr = clampf(sd_cr * 2.5f, 6.0f, 18.0f);
	}

	// per block: skin, and which part of a person it lies in (tmp bits)
	enum { B_SKIN = 1, B_ELLIPSE = 2, B_FACE = 4, B_CORE = 8, B_HEAD = 16, B_TORSO = 32 };
	struct stats st_bg = {0}, st_bg_all = {0}, st_hair = {0}, st_cloth = {0};
	for (uint32_t by = 0; by < bh; by++) {
		const float v = (by + 0.5f) / bh;
		for (uint32_t bx = 0; bx < bw; bx++) {
			const float u = (bx + 0.5f) / bw;
			const size_t i = (size_t)by * bw + bx;
			const float *c = as->avg + i * 3;
			float y, cb, cr;
			to_ycc(c, &y, &cb, &cr);
			bool skin;
			if (adaptive) {
				const float dcb = (cb - m_cb) / t_cb, dcr = (cr - m_cr) / t_cr;
				skin = dcb * dcb + dcr * dcr <= 1.0f && y > m_y * 0.3f &&
				       y < fminf(255.0f, m_y * 1.7f + 25.0f);
			} else {
				skin = cb >= 77.0f && cb <= 127.0f && cr >= 133.0f && cr <= 173.0f && y > 40.0f;
			}
			uint8_t bits = skin ? B_SKIN : 0;
			bool hair_s = false, cloth_s = false;
			for (int f = 0; f < n_faces; f++) {
				const struct face_regions *r = reg + f;
				if (in_ellipse(r, u, v, 1.0f))
					bits |= B_ELLIPSE;
				if (in_ellipse(r, u, v, 1.15f))
					bits |= B_FACE;
				if (in_ellipse(r, u, v, 0.85f))
					bits |= B_CORE;
				if (in_rect(&r->head, u, v))
					bits |= B_HEAD;
				if (in_rect(&r->torso, u, v))
					bits |= B_TORSO;
				hair_s = hair_s || in_rect(&r->hair_sample, u, v);
				cloth_s = cloth_s || in_rect(&r->cloth_sample, u, v);
			}
			as->tmp[i] = bits;
			if (!(bits & (B_HEAD | B_TORSO))) {
				st_add(&st_bg_all, c);
				if (u < 0.06f || u > 0.94f || v < 0.08f)
					st_add(&st_bg, c);
			}
			if (!skin && hair_s && !(bits & B_ELLIPSE))
				st_add(&st_hair, c);
			if (!skin && cloth_s)
				st_add(&st_cloth, c);
		}
	}
	float m_bg[3] = {0}, m_hair[3] = {0}, m_cloth[3] = {0}, sd_bg = 0.0f, sd_hair = 0.0f, sd_cloth = 0.0f;
	const bool has_bg = st_get(&st_bg, 8, m_bg, &sd_bg) || st_get(&st_bg_all, 8, m_bg, &sd_bg);
	const bool has_hair = st_get(&st_hair, 3, m_hair, &sd_hair);
	const bool has_cloth = st_get(&st_cloth, 3, m_cloth, &sd_cloth);
	const float t_bg = clampf(sd_bg * 2.5f, 40.0f, 90.0f);
	const float t_hair = clampf(sd_hair * 2.5f, 40.0f, 90.0f);
	const float t_cloth = clampf(sd_cloth * 2.5f, 45.0f, 100.0f);

	for (size_t i = 0; i < n; i++) {
		const uint8_t bits = as->tmp[i];
		const float *c = as->avg + i * 3;
		const bool skin = bits & B_SKIN;
		const float d_bg = has_bg ? dist3(c, m_bg) : 1e9f;
		bool hair = false, cloth = false;
		if (n_faces && has_hair && (bits & B_HEAD) && !skin && !(bits & B_CORE)) {
			const float d = dist3(c, m_hair);
			hair = d < t_hair && d < d_bg;
		}
		if (n_faces && (bits & B_TORSO) && !skin && !hair) {
			const float d = has_cloth ? dist3(c, m_cloth) : 1e9f;
			cloth = (d < t_cloth || (has_bg && d_bg > t_bg)) && (!has_bg || !has_cloth || d <= d_bg * 1.1f);
		}
		// skin anywhere counts as a person too: hands and arms are not background
		const bool person = skin || (n_faces && ((bits & B_ELLIPSE) || hair || cloth));
		uint8_t cls = 0;
		if (skin)
			cls |= AS_SKIN;
		if (skin && (bits & B_FACE))
			cls |= AS_FACE_SKIN;
		if (hair)
			cls |= AS_HAIR;
		if (cloth)
			cls |= AS_CLOTHING;
		if (!person)
			cls |= AS_BACKGROUND;
		as->cls[i] = cls;
		as->tmp[i] = (cls & classes) ? 1 : 0;
	}

	// smooth edges: a block follows the majority of its 3 x 3 neighbourhood
	for (uint32_t by = 0; by < bh; by++) {
		for (uint32_t bx = 0; bx < bw; bx++) {
			const size_t i = (size_t)by * bw + bx;
			bool sel = as->tmp[i];
			if (smooth) {
				int on = 0, all = 0;
				for (int dy = -1; dy <= 1; dy++) {
					const int yy = (int)by + dy;
					if (yy < 0 || yy >= (int)bh)
						continue;
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = (int)bx + dx;
						if (xx < 0 || xx >= (int)bw)
							continue;
						on += as->tmp[(size_t)yy * bw + xx] & 1;
						all++;
					}
				}
				sel = on * 2 > all;
			}
			// stabilize: a change shows after a few frames, so a block does not flicker
			const int target = sel ? 255 : 0;
			const int s = stable ? (as->score[i] * 5 + target * 3) / 8 : target;
			as->score[i] = (uint8_t)s;
			as->map[i] = s >= 128 ? 255 : 0;
		}
	}
}

/*
 * Raw input: a filter that keeps the latest Y'CbCr frame of a video source as it arrives, so the scopes can
 * measure the signal itself, including superwhite above 100 % and sub-black below 0 %, which OBS clips when it
 * converts the frame to 8-bit R'G'B'. The plugin adds the filter by itself to a video source a scope measures.
 */
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <string.h>
#include <math.h>
#include "plugin-macros.generated.h"
#include "raw-input.h"
#include "util.h"

struct raw_frame
{
	uint8_t *buf;
	size_t cap;
	uint8_t *data[MAX_AV_PLANES];
	uint32_t linesize[MAX_AV_PLANES];
	uint32_t width, height;
	enum video_format format;
	bool full_range, flip;
	float matrix[16];
	int trc; // enum video_trc: SDR (default or sRGB), PQ or HLG
	bool valid;
};

enum raw_range_e {
	RAW_RANGE_AUTO = 0,
	RAW_RANGE_VIDEO = 1,
	RAW_RANGE_FULL = 2,
};

struct raw_input
{
	obs_source_t *self;
	pthread_mutex_t mutex;
	struct raw_frame frame;
	volatile uint64_t last_use; // ns; frames are copied only while a scope uses them
	bool any_transfer;          // also when the source's transfer differs from the project's
	int range;                  // RAW_RANGE_*: as the source says, or video (legal) or full range
};

static const char *raw_input_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("RawInput");
}

static void raw_input_update(void *data, obs_data_t *settings)
{
	struct raw_input *r = data;
	r->any_transfer = obs_data_get_bool(settings, "any_transfer");
	r->range = (int)obs_data_get_int(settings, "range");
}

static void *raw_input_create(obs_data_t *settings, obs_source_t *source)
{
	struct raw_input *r = bzalloc(sizeof(struct raw_input));
	r->self = source;
	pthread_mutex_init(&r->mutex, NULL);
	raw_input_update(r, settings);
	return r;
}

static obs_properties_t *raw_input_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "range", obs_module_text("RawInput.Range"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("RawInput.Range.Auto"), RAW_RANGE_AUTO);
	obs_property_list_add_int(p, obs_module_text("RawInput.Range.Video"), RAW_RANGE_VIDEO);
	obs_property_list_add_int(p, obs_module_text("RawInput.Range.Full"), RAW_RANGE_FULL);
	obs_properties_add_bool(props, "any_transfer", obs_module_text("RawInput.AnyTransfer"));
	obs_properties_add_text(props, "note", obs_module_text("RawInput.Note"), OBS_TEXT_INFO);
	return props;
}

static void raw_input_destroy(void *data)
{
	struct raw_input *r = data;
	pthread_mutex_destroy(&r->mutex);
	bfree(r->frame.buf);
	bfree(r);
}

/* Planes of a format: their number and the height of each, for a picture of height h. */
static int plane_heights(enum video_format f, uint32_t h, uint32_t heights[MAX_AV_PLANES])
{
	const uint32_t h2 = (h + 1) / 2;
	switch (f) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I010:
		heights[0] = h, heights[1] = h2, heights[2] = h2;
		return 3;
	case VIDEO_FORMAT_I40A:
		heights[0] = h, heights[1] = h2, heights[2] = h2, heights[3] = h;
		return 4;
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_P010:
		heights[0] = h, heights[1] = h2;
		return 2;
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_I412:
		heights[0] = heights[1] = heights[2] = h;
		return 3;
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
		heights[0] = heights[1] = heights[2] = heights[3] = h;
		return 4;
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
		heights[0] = heights[1] = h;
		return 2;
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_AYUV:
	case VIDEO_FORMAT_V210:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_R10L:
		heights[0] = h;
		return 1;
	default:
		return 0; // R'G'B' or unknown: nothing to add to what OBS measures
	}
}

static const char *format_desc(enum video_format f)
{
	switch (f) {
	case VIDEO_FORMAT_I420:
		return "8-bit I420";
	case VIDEO_FORMAT_NV12:
		return "8-bit NV12";
	case VIDEO_FORMAT_YVYU:
		return "8-bit YVYU";
	case VIDEO_FORMAT_YUY2:
		return "8-bit YUY2";
	case VIDEO_FORMAT_UYVY:
		return "8-bit UYVY";
	case VIDEO_FORMAT_I444:
		return "8-bit I444";
	case VIDEO_FORMAT_I422:
		return "8-bit I422";
	case VIDEO_FORMAT_I40A:
		return "8-bit I40A";
	case VIDEO_FORMAT_I42A:
		return "8-bit I42A";
	case VIDEO_FORMAT_YUVA:
		return "8-bit YUVA";
	case VIDEO_FORMAT_AYUV:
		return "8-bit AYUV";
	case VIDEO_FORMAT_I010:
		return "10-bit I010";
	case VIDEO_FORMAT_P010:
		return "10-bit P010";
	case VIDEO_FORMAT_I210:
		return "10-bit I210";
	case VIDEO_FORMAT_I412:
		return "12-bit I412";
	case VIDEO_FORMAT_P216:
		return "16-bit P216";
	case VIDEO_FORMAT_P416:
		return "16-bit P416";
	case VIDEO_FORMAT_V210:
		return "10-bit V210";
	case VIDEO_FORMAT_BGRA:
		return "8-bit BGRA";
	case VIDEO_FORMAT_BGRX:
		return "8-bit BGRX";
	case VIDEO_FORMAT_RGBA:
		return "8-bit RGBA";
	case VIDEO_FORMAT_R10L:
		return "10-bit RGB";
	default:
		return "?";
	}
}

static struct obs_source_frame *raw_input_filter_video(void *data, struct obs_source_frame *frame)
{
	struct raw_input *r = data;
	if (!frame || os_gettime_ns() - r->last_use > 2000000000ULL)
		return frame; // no scope uses it: pass the frame on and do nothing

	uint32_t heights[MAX_AV_PLANES] = {0};
	const int n = plane_heights(frame->format, frame->height, heights);
	size_t size = 0;
	for (int i = 0; i < n; i++)
		size += (size_t)frame->linesize[i] * heights[i];

	pthread_mutex_lock(&r->mutex);
	struct raw_frame *f = &r->frame;
	f->valid = n > 0 && size > 0;
	if (f->valid) {
		if (f->cap < size) {
			bfree(f->buf);
			f->buf = bmalloc(size);
			f->cap = size;
		}
		uint8_t *p = f->buf;
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			f->data[i] = NULL;
			f->linesize[i] = 0;
		}
		for (int i = 0; i < n; i++) {
			const size_t len = (size_t)frame->linesize[i] * heights[i];
			memcpy(p, frame->data[i], len);
			f->data[i] = p;
			f->linesize[i] = frame->linesize[i];
			p += len;
		}
		f->width = frame->width;
		f->height = frame->height;
		f->format = frame->format;
		f->full_range = frame->full_range;
		f->flip = frame->flip;
		memcpy(f->matrix, frame->color_matrix, sizeof(f->matrix));
		f->trc = frame->trc;
	}
	pthread_mutex_unlock(&r->mutex);
	return frame;
}

static inline uint32_t rd16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One pixel as 16-bit values (the code shifted to the top of 16 bits): Y, Cb, Cr. */
static void get_pixel(const struct raw_frame *f, uint32_t x, uint32_t y, uint32_t *Y, uint32_t *U, uint32_t *V)
{
	const uint32_t x2 = x / 2, y2 = y / 2;
	const uint8_t *const *d = (const uint8_t *const *)f->data;
	const uint32_t *ls = f->linesize;
	switch (f->format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I40A:
		*Y = d[0][ls[0] * y + x] << 8;
		*U = d[1][ls[1] * y2 + x2] << 8;
		*V = d[2][ls[2] * y2 + x2] << 8;
		break;
	case VIDEO_FORMAT_NV12:
		*Y = d[0][ls[0] * y + x] << 8;
		*U = d[1][ls[1] * y2 + x2 * 2] << 8;
		*V = d[1][ls[1] * y2 + x2 * 2 + 1] << 8;
		break;
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I42A:
		*Y = d[0][ls[0] * y + x] << 8;
		*U = d[1][ls[1] * y + x2] << 8;
		*V = d[2][ls[2] * y + x2] << 8;
		break;
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_YUVA:
		*Y = d[0][ls[0] * y + x] << 8;
		*U = d[1][ls[1] * y + x] << 8;
		*V = d[2][ls[2] * y + x] << 8;
		break;
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_UYVY: {
		const uint8_t *p = d[0] + ls[0] * y + x2 * 4;
		if (f->format == VIDEO_FORMAT_UYVY) {
			*Y = p[1 + (x & 1) * 2] << 8;
			*U = p[0] << 8;
			*V = p[2] << 8;
		} else {
			*Y = p[(x & 1) * 2] << 8;
			const bool yvyu = f->format == VIDEO_FORMAT_YVYU;
			*U = p[yvyu ? 3 : 1] << 8;
			*V = p[yvyu ? 1 : 3] << 8;
		}
		break;
	}
	case VIDEO_FORMAT_AYUV: {
		const uint8_t *p = d[0] + ls[0] * y + x * 4;
		*Y = p[1] << 8;
		*U = p[2] << 8;
		*V = p[3] << 8;
		break;
	}
	case VIDEO_FORMAT_I010:
		// 10 bits in the low bits of 16
		*Y = rd16(d[0] + ls[0] * y + x * 2) << 6;
		*U = rd16(d[1] + ls[1] * y2 + x2 * 2) << 6;
		*V = rd16(d[2] + ls[2] * y2 + x2 * 2) << 6;
		break;
	case VIDEO_FORMAT_I210:
		*Y = rd16(d[0] + ls[0] * y + x * 2) << 6;
		*U = rd16(d[1] + ls[1] * y + x2 * 2) << 6;
		*V = rd16(d[2] + ls[2] * y + x2 * 2) << 6;
		break;
	case VIDEO_FORMAT_I412:
		*Y = rd16(d[0] + ls[0] * y + x * 2) << 4;
		*U = rd16(d[1] + ls[1] * y + x * 2) << 4;
		*V = rd16(d[2] + ls[2] * y + x * 2) << 4;
		break;
	case VIDEO_FORMAT_P010:
		// 10 bits in the high bits of 16
		*Y = rd16(d[0] + ls[0] * y + x * 2);
		*U = rd16(d[1] + ls[1] * y2 + x2 * 4);
		*V = rd16(d[1] + ls[1] * y2 + x2 * 4 + 2);
		break;
	case VIDEO_FORMAT_P216:
		*Y = rd16(d[0] + ls[0] * y + x * 2);
		*U = rd16(d[1] + ls[1] * y + x2 * 4);
		*V = rd16(d[1] + ls[1] * y + x2 * 4 + 2);
		break;
	case VIDEO_FORMAT_P416:
		*Y = rd16(d[0] + ls[0] * y + x * 2);
		*U = rd16(d[1] + ls[1] * y + x * 4);
		*V = rd16(d[1] + ls[1] * y + x * 4 + 2);
		break;
	case VIDEO_FORMAT_V210: {
		// 6 pixels in 16 bytes: Cb0 Y0 Cr0 | Y1 Cb2 Y2 | Cr2 Y3 Cb4 | Y4 Cr4 Y5, 10 bits each
		const uint8_t *g = d[0] + ls[0] * y + (x / 6) * 16;
		const uint32_t k = x % 6;
		const uint32_t iy = 2 * k + 1, iu = 4 * (k / 2), iv = iu + 2;
#define V210(i) ((rd32(g + ((i) / 3) * 4) >> (((i) % 3) * 10)) & 0x3FF)
		*Y = V210(iy) << 6;
		*U = V210(iu) << 6;
		*V = V210(iv) << 6;
#undef V210
		break;
	}
	default:
		*Y = 16 << 8, *U = *V = 128 << 8;
		break;
	}
}

static inline bool is_rgb_format(enum video_format f)
{
	return f == VIDEO_FORMAT_BGRA || f == VIDEO_FORMAT_BGRX || f == VIDEO_FORMAT_RGBA || f == VIDEO_FORMAT_R10L;
}

/* One pixel of an R'G'B' format as 16-bit values (the code shifted to the top of 16 bits). */
static void get_rgb(const struct raw_frame *f, uint32_t x, uint32_t y, uint32_t *R, uint32_t *G, uint32_t *B)
{
	const uint8_t *p = f->data[0] + f->linesize[0] * y + x * 4;
	switch (f->format) {
	case VIDEO_FORMAT_RGBA:
		*R = p[0] << 8, *G = p[1] << 8, *B = p[2] << 8;
		break;
	case VIDEO_FORMAT_R10L: {
		// little-endian 32 bits: R in 31..22, G in 21..12, B in 11..2 (Blackmagic 10-bit RGBXLE)
		const uint32_t w = rd32(p);
		*R = ((w >> 22) & 0x3FF) << 6;
		*G = ((w >> 12) & 0x3FF) << 6;
		*B = ((w >> 2) & 0x3FF) << 6;
		break;
	}
	default: // BGRA, BGRX
		*R = p[2] << 8, *G = p[1] << 8, *B = p[0] << 8;
		break;
	}
}

static inline uint8_t code8(uint32_t v16)
{
	const uint32_t c = (v16 + 128) >> 8;
	return (uint8_t)(c > 255 ? 255 : c);
}

static inline uint8_t to_code(float v)
{
	// 0..1 of the signal to an 8-bit legal code: 16 is 0 %, 235 is 100 %
	const float c = 16.0f + 219.0f * v + 0.5f;
	return (uint8_t)(c < 0.0f ? 0.0f : c > 255.0f ? 255.0f : c);
}

static const char *trc_desc(int trc)
{
	return trc == VIDEO_TRC_HLG ? "HLG" : trc == VIDEO_TRC_PQ ? "PQ" : "SDR";
}

bool raw_input_sample(obs_source_t *filter, float u, float v, int cols, int rows, struct raw_sample *out,
		      bool *rgb_source, bool *full_out, char *desc, size_t desc_size)
{
	struct raw_input *r = filter ? obs_obj_get_data(filter) : NULL;
	if (!r || cols <= 0 || rows <= 0)
		return false;
	r->last_use = os_gettime_ns();
	pthread_mutex_lock(&r->mutex);
	const struct raw_frame *f = &r->frame;
	if (!f->valid || !f->width || !f->height) {
		pthread_mutex_unlock(&r->mutex);
		return false;
	}
	const bool full = r->range == RAW_RANGE_AUTO ? f->full_range : r->range == RAW_RANGE_FULL;
	const bool rgb_fmt = is_rgb_format(f->format);
	const int cx = (int)(u * f->width), cy = (int)(v * f->height);
	const float *m = f->matrix;
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < cols; i++) {
			int x = cx + i - cols / 2, y = cy + j - rows / 2;
			x = x < 0 ? 0 : x >= (int)f->width ? (int)f->width - 1 : x;
			y = y < 0 ? 0 : y >= (int)f->height ? (int)f->height - 1 : y;
			const uint32_t fy = f->flip ? f->height - 1 - (uint32_t)y : (uint32_t)y;
			struct raw_sample *o = out + j * cols + i;
			uint32_t a, b, c;
			if (rgb_fmt) {
				get_rgb(f, (uint32_t)x, fy, &a, &b, &c);
				for (int k = 0; k < 3; k++) {
					const float n = (k == 0 ? a : k == 1 ? b : c) / 65535.0f;
					o->rgb[k] = full ? n : (n * 255.0f - 16.0f) / 219.0f;
				}
			} else {
				get_pixel(f, (uint32_t)x, fy, &a, &b, &c);
				const float ny = a / 65535.0f, nu = b / 65535.0f, nv = c / 65535.0f;
				o->rgb[0] = m[0] * ny + m[1] * nu + m[2] * nv + m[3];
				o->rgb[1] = m[4] * ny + m[5] * nu + m[6] * nv + m[7];
				o->rgb[2] = m[8] * ny + m[9] * nu + m[10] * nv + m[11];
			}
			o->code[0] = (uint16_t)(a >> 6);
			o->code[1] = (uint16_t)(b >> 6);
			o->code[2] = (uint16_t)(c >> 6);
		}
	}
	if (rgb_source)
		*rgb_source = rgb_fmt;
	if (full_out)
		*full_out = full;
	if (desc && desc_size)
		snprintf(desc, desc_size, "RAW %s %s%s", format_desc(f->format), trc_desc(f->trc), full ? " full" : "");
	pthread_mutex_unlock(&r->mutex);
	return true;
}

bool raw_input_describe(obs_source_t *filter, int colorspace, char *desc, size_t desc_size, bool *usable)
{
	struct raw_input *r = filter ? obs_obj_get_data(filter) : NULL;
	if (usable)
		*usable = false;
	if (!r)
		return false;
	r->last_use = os_gettime_ns(); // keep the frame fresh while the window is open
	pthread_mutex_lock(&r->mutex);
	const struct raw_frame *f = &r->frame;
	const bool valid = f->valid && f->width && f->height;
	if (valid) {
		const bool full = r->range == RAW_RANGE_AUTO ? f->full_range : r->range == RAW_RANGE_FULL;
		const int project = colorspace == CM_CS_2100_HLG  ? VIDEO_TRC_HLG
				    : colorspace == CM_CS_2100_PQ ? VIDEO_TRC_PQ
								  : VIDEO_TRC_DEFAULT;
		const int source = f->trc == VIDEO_TRC_HLG || f->trc == VIDEO_TRC_PQ ? f->trc : VIDEO_TRC_DEFAULT;
		if (usable)
			*usable = source == project || r->any_transfer;
		if (desc && desc_size)
			snprintf(desc, desc_size, "%s, %ux%u, %s%s, %s", format_desc(f->format), f->width, f->height,
				 full ? "full range" : "video range", r->range == RAW_RANGE_AUTO ? "" : " (set)",
				 trc_desc(f->trc));
	}
	pthread_mutex_unlock(&r->mutex);
	return valid;
}

bool raw_input_fill(obs_source_t *filter, uint8_t *ext, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0,
		    uint32_t src_w, uint32_t src_h, int colorspace, char *desc, size_t desc_size)
{
	struct raw_input *r = filter ? obs_obj_get_data(filter) : NULL;
	if (!r || !width || !height || !src_w || !src_h)
		return false;
	r->last_use = os_gettime_ns();

	pthread_mutex_lock(&r->mutex);
	const struct raw_frame *f = &r->frame;
	if (!f->valid) {
		pthread_mutex_unlock(&r->mutex);
		return false;
	}
	// The raw signal is only what the scopes show when the source has the transfer of the project: an SDR source
	// in an HLG project is converted by OBS (SDR white lands near 75 %), and so is HLG in an SDR project.
	const int project = colorspace == CM_CS_2100_HLG  ? VIDEO_TRC_HLG
			    : colorspace == CM_CS_2100_PQ ? VIDEO_TRC_PQ
							  : VIDEO_TRC_DEFAULT;
	const int source = f->trc == VIDEO_TRC_HLG || f->trc == VIDEO_TRC_PQ ? f->trc : VIDEO_TRC_DEFAULT;
	if (source != project && !r->any_transfer) {
		pthread_mutex_unlock(&r->mutex);
		return false;
	}
	const float kx = (float)f->width / src_w, ky = (float)f->height / src_h;
	const float *m = f->matrix;
	// the range the codes are in: as the source says, or as set on the filter
	const bool full = r->range == RAW_RANGE_AUTO ? f->full_range : r->range == RAW_RANGE_FULL;
	const bool rgb_fmt = is_rgb_format(f->format);
	// R'G'B' sources: luma with the weights of the transfer (Rec. 2020 for HDR, Rec. 709 otherwise)
	const bool wide = f->trc == VIDEO_TRC_HLG || f->trc == VIDEO_TRC_PQ;
	const float kr = wide ? 0.2627f : 0.2126f, kb = wide ? 0.0593f : 0.0722f, kg = 1.0f - kr - kb;
	for (uint32_t y = 0; y < height; y++) {
		uint32_t fy = (uint32_t)((y0 + y + 0.5f) * ky);
		if (fy >= f->height)
			fy = f->height - 1;
		if (f->flip)
			fy = f->height - 1 - fy;
		uint8_t *o = ext + (size_t)width * 4 * y;
		for (uint32_t x = 0; x < width; x++, o += 4) {
			uint32_t fx = (uint32_t)((x0 + x + 0.5f) * kx);
			if (fx >= f->width)
				fx = f->width - 1;
			if (rgb_fmt) {
				uint32_t R, G, B;
				get_rgb(f, fx, fy, &R, &G, &B);
				if (full) {
					o[0] = to_code(B / 65535.0f);
					o[1] = to_code(G / 65535.0f);
					o[2] = to_code(R / 65535.0f);
				} else {
					// video range R'G'B': the codes themselves
					o[0] = code8(B), o[1] = code8(G), o[2] = code8(R);
				}
				o[3] = (uint8_t)(kr * o[2] + kg * o[1] + kb * o[0] + 0.5f);
				continue;
			}
			uint32_t Y, U, V;
			get_pixel(f, fx, fy, &Y, &U, &V);
			// normalized as the shaders of OBS see the planes, so the matrix of the frame applies
			float ny = Y / 65535.0f, nu = U / 65535.0f, nv = V / 65535.0f;
			if (full != f->full_range) {
				// the filter says another range than the source: turn the codes into the source's
				// range first, so its matrix gives the colors of the range set on the filter
				if (!full) { // video range codes, the source says full
					ny = (ny - 16.0f / 255.0f) * 255.0f / 219.0f;
					nu = (nu - 128.0f / 255.0f) * 255.0f / 224.0f + 128.0f / 255.0f;
					nv = (nv - 128.0f / 255.0f) * 255.0f / 224.0f + 128.0f / 255.0f;
				} else { // full range codes, the source says video
					ny = ny * 219.0f / 255.0f + 16.0f / 255.0f;
					nu = (nu - 128.0f / 255.0f) * 224.0f / 255.0f + 128.0f / 255.0f;
					nv = (nv - 128.0f / 255.0f) * 224.0f / 255.0f + 128.0f / 255.0f;
				}
			}
			const float rr = m[0] * ny + m[1] * nu + m[2] * nv + m[3];
			const float gg = m[4] * ny + m[5] * nu + m[6] * nv + m[7];
			const float bb = m[8] * ny + m[9] * nu + m[10] * nv + m[11];
			o[0] = to_code(bb);
			o[1] = to_code(gg);
			o[2] = to_code(rr);
			if (full) {
				o[3] = to_code(Y / 65535.0f);
			} else {
				// the luma code itself, at 8 bits
				const uint32_t c = (Y + 128) >> 8;
				o[3] = (uint8_t)(c > 255 ? 255 : c);
			}
		}
	}
	if (desc && desc_size)
		snprintf(desc, desc_size, "RAW %s %s%s", format_desc(f->format), trc_desc(f->trc), full ? " full" : "");
	pthread_mutex_unlock(&r->mutex);
	return true;
}

struct find_ctx
{
	obs_source_t *found;
};

static void find_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct find_ctx *ctx = param;
	if (!ctx->found && !strcmp(obs_source_get_unversioned_id(child), RAW_INPUT_ID))
		ctx->found = obs_source_get_ref(child);
}

obs_source_t *raw_input_find(obs_source_t *parent)
{
	if (!parent)
		return NULL;
	struct find_ctx ctx = {NULL};
	obs_source_enum_filters(parent, find_cb, &ctx);
	return ctx.found;
}

static void ensure_task(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *parent = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!parent)
		return;
	obs_source_t *found = raw_input_find(parent);
	if (found) {
		obs_source_release(found);
	} else {
		obs_source_t *filter = obs_source_create(RAW_INPUT_ID, obs_module_text("RawInput"), NULL, NULL);
		if (filter) {
			obs_source_filter_add(parent, filter);
			blog(LOG_INFO, "[" PLUGIN_NAME "] added the raw input filter to '%s'",
			     obs_source_get_name(parent));
			obs_source_release(filter);
		}
	}
	obs_source_release(parent);
}

/* The sources the filter was added to in this session: when you remove it, it is not added again. */
static pthread_mutex_t added_mutex = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(char *) added;

void raw_input_ensure(obs_source_t *parent)
{
	if (!parent || (obs_source_get_output_flags(parent) & OBS_SOURCE_ASYNC_VIDEO) != OBS_SOURCE_ASYNC_VIDEO)
		return;
	if (obs_source_get_type(parent) != OBS_SOURCE_TYPE_INPUT)
		return;
	const char *uuid = obs_source_get_uuid(parent);
	if (!uuid)
		return;
	pthread_mutex_lock(&added_mutex);
	bool seen = false;
	for (size_t i = 0; i < added.num && !seen; i++)
		seen = !strcmp(added.array[i], uuid);
	if (!seen) {
		char *copy = bstrdup(uuid);
		da_push_back(added, &copy);
	}
	pthread_mutex_unlock(&added_mutex);
	if (seen)
		return;
	obs_queue_task(OBS_TASK_UI, ensure_task, obs_source_get_weak_source(parent), false);
}

void raw_input_free(void)
{
	pthread_mutex_lock(&added_mutex);
	for (size_t i = 0; i < added.num; i++)
		bfree(added.array[i]);
	da_free(added);
	pthread_mutex_unlock(&added_mutex);
}

const struct obs_source_info colormonitor_raw_input = {
	.id = RAW_INPUT_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name = raw_input_get_name,
	.create = raw_input_create,
	.destroy = raw_input_destroy,
	.filter_video = raw_input_filter_video,
	.update = raw_input_update,
	.get_properties = raw_input_get_properties,
};

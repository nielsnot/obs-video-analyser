#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The trace of a vectorscope: where every pixel lands for a projection and a layout, built up with optional
 * persistence and smoothing, and turned into an RGBA picture with a density curve. Shared by the vectorscope and
 * the color inspector. */

#define VST_SIZE 256

enum vst_projection {
	VST_CBCR = 0,     // the classic Cb/Cr plane
	VST_HSV = 1,      // HSV hue around, HSV saturation outwards
	VST_OPPONENT = 2, // R-G across, (R+G)/2-B up
};

enum vst_layout {
	VST_CIRCLE = 0,
	VST_SEMICIRCLE = 1, // half a circle twice as large, skin tones in the middle
	VST_RECTANGLE = 2,  // hue across, saturation up
	VST_QUADRANT = 3,   // a quarter four times as large, skin tones on the diagonal
};

enum vst_curve {
	VST_LINEAR = 0, // as many pixels, as bright, up to full
	VST_SOFT = 1,   // dense places come up gently, sparse ones stay faint
	VST_LOG = 2,    // sparse places stand out too
};

enum vst_luma {
	VST_LUMA_ALL = 0,
	VST_LUMA_LOWS = 1,
	VST_LUMA_MIDS = 2,
	VST_LUMA_HIGHS = 3,
};

struct vst_params
{
	int projection; // vst_projection
	int layout;     // vst_layout
	float hue_rot;  // degrees, turned counter-clockwise
	bool expand;    // low saturations larger (square root of the radius)
	int colorspace; // CM_CS_*
	int luma;       // vst_luma
};

struct vst_state
{
	float *cnt, *r, *g, *b;         // this frame (or with persistence, the running average)
	float *wc, *wr, *wg, *wb, *tmp; // work buffers for smoothing
	uint32_t *rgba;                 // the result, VST_SIZE x VST_SIZE, ABGR in memory order R, G, B, A
	uint8_t *bright;                // brightness only, for a snapshot reference
	float max_sat;                  // highest Cb/Cr saturation of the last picture, 0..1 of the outer ring
	bool has_data;
};

void vst_init(struct vst_state *s);
void vst_free(struct vst_state *s);

/* True when the parameters are the classic plain view (Cb/Cr, circle, no rotation, no expansion): then the
 * vectorscope's own graticule, markers and rings fit. */
bool vst_is_plain(const struct vst_params *p);

/* The skin tone direction of the projection (radians) for a skin color, and the place of a polar position
 * (angle in radians of the projection, radius 0..1) in the scope (0..VST_SIZE). False when outside the layout. */
bool vst_map(const struct vst_params *p, float angle, float radius, float *x, float *y);

/* The other way: the polar position (angle in radians of the projection, 0..2 pi, radius) at a place in the scope. */
void vst_unmap(const struct vst_params *p, float x, float y, float *angle, float *radius);

/* Polar position of an R'G'B' color (0..1) in the projection. */
void vst_polar_rgb(const struct vst_params *p, float r, float g, float b, float *angle, float *radius);

/* Adds the pixels of an 8-bit Y'CbCr capture (bytes U, Y, V, A per pixel; A 0 is outside the selection).
 * persistence 0..0.95 keeps that share of the previous frames. */
void vst_accumulate(struct vst_state *s, const struct vst_params *p, const uint8_t *yuv, uint32_t width,
		    uint32_t height, uint32_t linesize, float persistence);

/* Builds s->rgba (and s->bright) with a density curve, intensity 1..255 as before, smoothing 0..3 pixels.
 * colored: the average color of the pixels at each place, otherwise white. */
void vst_build(struct vst_state *s, int curve, float intensity, int smooth, bool colored);

#ifdef __cplusplus
}
#endif

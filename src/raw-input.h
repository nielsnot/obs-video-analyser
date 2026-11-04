#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAW_INPUT_ID ID_PREFIX "raw_input"

/* The raw input filter: sits on a video source (a capture card, camera, media file) and keeps the latest
 * Y'CbCr frame as the source delivered it, before OBS converts it to 8-bit R'G'B' and clips it. */

/* Finds the raw input filter on the source; a new reference, or NULL. */
obs_source_t *raw_input_find(obs_source_t *parent);

/* Adds the raw input filter to an async video source that does not have one yet (on the UI thread). */
void raw_input_ensure(obs_source_t *parent);

/* colorspace is the scope's (CM_CS_*): the raw signal is used only when the source has the same transfer (SDR,
 * HLG or PQ), unless the filter is set to measure it anyway.
 * Fills ext (width x height, 4 bytes per pixel: B, G, R, Y) with the raw signal as 8-bit legal codes, where
 * 16 is 0 % and 235 is 100 %, so values above 100 % and below 0 % are kept up to 109 % and down to -7 %.
 * The picture is the region (x0, y0) of a capture of src_w x src_h pixels of the source.
 * desc gets a short description such as "RAW 10-bit V210". Returns false when there is no usable frame. */
bool raw_input_fill(obs_source_t *filter, uint8_t *ext, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0,
		    uint32_t src_w, uint32_t src_h, int colorspace, char *desc, size_t desc_size);

/* One pixel of the raw signal for the data analyser. */
struct raw_sample
{
	uint16_t code[3]; // Y, Cb, Cr (or R, G, B for an R'G'B' source) as 10-bit codes
	float rgb[3];     // R', G', B' of the pixel, 0..1 of the signal (beyond it for superwhite and sub-black)
};

/* The cols x rows pixels of the frame around (u, v) (0..1 of the picture), one per frame pixel. *rgb_source is set
 * for an R'G'B' format, *full for full range. Returns false without a usable frame. */
bool raw_input_sample(obs_source_t *filter, float u, float v, int cols, int rows, struct raw_sample *out,
		      bool *rgb_source, bool *full, char *desc, size_t desc_size);

/* Frees what the module keeps; at unload. */
void raw_input_free(void);

#ifdef __cplusplus
}
#endif

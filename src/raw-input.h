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

/* Fills ext (width x height, 4 bytes per pixel: B, G, R, Y) with the raw signal as 8-bit legal codes, where
 * 16 is 0 % and 235 is 100 %, so values above 100 % and below 0 % are kept up to 109 % and down to -7 %.
 * The picture is the region (x0, y0) of a capture of src_w x src_h pixels of the source.
 * desc gets a short description such as "RAW 10-bit V210". Returns false when there is no usable frame. */
bool raw_input_fill(obs_source_t *filter, uint8_t *ext, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0,
		    uint32_t src_w, uint32_t src_h, char *desc, size_t desc_size);

#ifdef __cplusplus
}
#endif

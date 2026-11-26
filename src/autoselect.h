#pragma once

/* Automatic selection: the dock selection becomes the skin, the face skin, the hair, the clothing or the
 * background of the picture, found from the faces the face detector sees and the colors around them.
 * No model of its own: skin by its color (learned from the faces found), hair above the face, clothing below
 * it, and the background as the rest. Works on the analyzed copy, in blocks, on the pipeline thread. */

#include <stdint.h>
#include <stdbool.h>
#include "face.h"

#ifdef __cplusplus
extern "C" {
#endif

enum auto_select_class_e {
	AS_SKIN = 1,       // all skin in the picture (faces, hands, arms)
	AS_FACE_SKIN = 2,  // the skin of the faces only
	AS_HAIR = 4,       // above and around the faces
	AS_CLOTHING = 8,   // below the faces
	AS_BACKGROUND = 16 // everything that is not a person
};

struct auto_select
{
	uint32_t bw, bh, block; // the map: bw x bh blocks of block x block pixels
	uint8_t *map;           // 255 selected, 0 not
	uint8_t *score;         // smoothed over time (stabilize)
	uint8_t *cls;           // classes per block (AS_* bits), for the overlay and the log
	uint8_t *tmp;
	float *avg; // mean R'G'B' per block
};

void auto_select_free(struct auto_select *as);

/* Classifies one BGRA frame (w x h); faces normalized to the frame. Fills as->map (and as->cls).
 * smooth: a 3x3 majority filter on the map; stable: the map follows changes over a few frames. */
void auto_select_run(struct auto_select *as, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t linesize,
		     const struct face_box *faces, int n_faces, int classes, bool smooth, bool stable);

#ifdef __cplusplus
}
#endif

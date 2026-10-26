#pragma once

/* Face detection on a worker thread, with simple tracking.
 * Frames are handed in from the pipeline thread; the detector runs on a copy scaled to at most
 * 320 pixels wide (libfacedetection, a small CNN that needs no other libraries). */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_MAX 8

struct face_box
{
	int id;
	float x, y, w, h;    // normalized to the picture (0..1)
	float confidence;    // 0..1
	float landmarks[10]; // eyes, nose, mouth corners: x, y pairs, normalized
};

struct face_detector;

struct face_detector *face_detector_create(void);
void face_detector_destroy(struct face_detector *fd);

/* Hands a BGRA picture to the detector; skipped while it is still busy with the last one. */
void face_detector_submit(struct face_detector *fd, const uint8_t *bgra, uint32_t width, uint32_t height,
			  uint32_t linesize);

/* The tracked faces, largest first. Returns the count. */
int face_detector_get(struct face_detector *fd, struct face_box *out, int max);

#ifdef __cplusplus
}
#endif

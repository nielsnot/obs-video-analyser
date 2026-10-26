#pragma once

/* User markers drawn on top of the waveform and vectorscope.
 * Stored in the scope's settings as the array "markers"; each item has a name, a color and
 * a style, and either a level (waveform) or a hue angle and saturation (vectorscope). */

#include <obs.h>
#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CM_MARKERS_MAX 16

enum cm_marker_kind_e {
	CM_MARKER_LEVEL = 0, // waveform: horizontal line at a level
	CM_MARKER_POINT = 1, // vectorscope: a point at a hue and saturation
	CM_MARKER_LINE = 2,  // vectorscope: a line from the center towards a hue
};

enum cm_marker_style_e {
	CM_MARKER_SOLID = 0,
	CM_MARKER_DASHED = 1,
	CM_MARKER_DOTTED = 2,
};

enum cm_marker_shape_e {
	CM_MARKER_SHAPE_CIRCLE = 0,
	CM_MARKER_SHAPE_CROSS = 1,
	CM_MARKER_SHAPE_SQUARE = 2,
	CM_MARKER_SHAPE_DOT = 3,
};

struct cm_marker
{
	char name[64];
	int kind;       // cm_marker_kind_e
	float level;    // waveform level, 0..1 (may exceed 1)
	float hue;      // vectorscope angle in degrees, counter-clockwise from +U (3 o'clock)
	float sat;      // vectorscope distance from the center, 1 = the outer ring
	uint32_t color; // 0xAARRGGBB
	int style;      // cm_marker_style_e
	int shape;      // cm_marker_shape_e, for points
	float width;    // line width / point size in screen pixels
	bool show_label;
};

struct cm_markers
{
	pthread_mutex_t mutex;
	struct cm_marker items[CM_MARKERS_MAX];
	int n;
};

void cm_markers_init(struct cm_markers *m);
void cm_markers_free(struct cm_markers *m);
void cm_markers_load(struct cm_markers *m, obs_data_t *settings);
/* Copies the markers so rendering does not hold the lock. Returns the count. */
int cm_markers_copy(struct cm_markers *m, struct cm_marker *out);

/* Draws a line from (x0, y0) to (x1, y1) in source units. ux/uy are source units per screen pixel. */
void cm_marker_draw_line(float x0, float y0, float x1, float y1, uint32_t color, int style, float width_px, float ux,
			 float uy);
/* Draws a point marker centered at (x, y). */
void cm_marker_draw_point(float x, float y, uint32_t color, int shape, float size_px, float ux, float uy);

/* Editing in the scope (marker edit mode). They write the "markers" setting, which reloads the markers.
 * Positions: for a level marker a is the level (0..1); for points and lines a is the hue in degrees and
 * b the saturation (1 = the outer ring). Returns the index of the marker stored (a new one when add). */
int cm_markers_store(obs_source_t *src, int index, int kind, float a, float b, bool add);
void cm_markers_cycle_color(obs_source_t *src, int index);
void cm_markers_delete(obs_source_t *src, int index);

#ifdef __cplusplus
}
#endif

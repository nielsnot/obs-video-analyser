#pragma once

#include <util/darray.h>
#include "common.h"
#include "face.h"

#ifdef __cplusplus
extern "C" {
#endif

enum roi_shape_e {
	ROI_SHAPE_RECT = 0,
	ROI_SHAPE_ELLIPSE = 1,
};

enum roi_draw_mode_e {
	ROI_DRAW_NONE = 0,
	ROI_DRAW_RECT = 1,
	ROI_DRAW_ELLIPSE = 2,
	ROI_DRAW_PIN = 3, // color picker: a click places a pin that reads the color under it
};

#define ROI_MAX_PINS 8

enum roi_zoom_anchor_e {
	ROI_ZOOM_FACE = 0,    // middle of the width, a little above the middle of the height
	ROI_ZOOM_POINTER = 1, // around the mouse pointer
	ROI_ZOOM_CENTER = 2,
};

/* A color picker pin; position normalized to the captured frame, color read each frame. */
struct roi_pin
{
	float x, y;
	volatile uint8_t r, g, b;
	volatile bool valid;
};

/* One selection shape; coordinates are normalized to the captured frame (0..1),
 * so they stay valid when the scale changes. An ellipse is inscribed in its box. */
struct roi_shape
{
	int type; // roi_shape_e
	float x0, y0, x1, y1;
};

struct roi_source
{
	struct cm_source cm;
	int n_interleave, i_interleave;
	bool interleave_rendered;

	// drawing a new shape with the mouse
	volatile int draw_mode; // roi_draw_mode_e
	volatile bool dragging;
	volatile bool shift_down;
	int x_start, y_start;
	int x_mouse, y_mouse;

	// selection shapes; the scopes count pixels inside any of them
	pthread_mutex_t shapes_mutex;
	DARRAY(struct roi_shape) shapes;
	DARRAY(struct roi_pin) pins; // also guarded by shapes_mutex
	volatile bool shapes_dirty;

	// rasterized shapes, rebuilt in the graphics thread when shapes change
	gs_texture_t *mask_tex;
	uint8_t *mask_buf;
	uint32_t mask_w, mask_h;

	struct cm_mask mask;

	// view zoom: the picture is shown zoomed by `zoom` with view_x/view_y (picture pixels) at the top left
	float zoom, view_x, view_y;
	int zoom_anchor; // roi_zoom_anchor_e

	// guides over the picture: title and action safe, thirds, center cross (each can be turned off)
	bool guide_title, guide_action, guide_thirds, guide_center;

	// comparing with the snapshot: 0 wipe left | right, 1 wipe top / bottom, 2 blend
	int compare_mode;
	volatile float compare_pos; // where the wipe is, 0..1
	float compare_alpha;        // how much of the snapshot in a blend, 0..1
	volatile bool wipe_drag;
	float guide_title_pct, guide_action_pct;

	// follow faces: the selection is the faces found, instead of the drawn shapes
	volatile bool face_follow;
	struct face_detector *fd;
	struct face_box faces[FACE_MAX];
	int n_faces;
	// the face ellipses, for masking the analyzed pixels on the pipeline thread (shapes_mutex)
	struct roi_shape follow_shapes[FACE_MAX];
	int n_follow_shapes;
	volatile bool panning;
	int pan_mouse_x, pan_mouse_y; // view coordinates where the pan started
	float pan_view_x, pan_view_y;

	// snapshot reference: the frame at the moment of the snapshot, shown left of a split
	volatile bool want_snapshot;
	volatile bool compare;
	gs_texture_t *ref_tex;
	uint32_t ref_w, ref_h;
	char *ref_load_path; // a picture file to load as the reference on the next frame (shapes_mutex)

	pthread_mutex_t sources_mutex;
	DARRAY(struct cm_source *) sources;
};

struct roi_source *roi_from_source(obs_source_t *);
bool roi_target_render(struct roi_source *src);

void roi_register_source(struct roi_source *, struct cm_source *);
void roi_unregister_source(struct roi_source *, struct cm_source *);

/* Called from the UI (dock toolbar). */
void roi_set_draw_mode(struct roi_source *src, int mode);
int roi_get_draw_mode(const struct roi_source *src);
void roi_undo_shape(struct roi_source *src);
void roi_clear_shapes(struct roi_source *src);
size_t roi_get_shape_count(struct roi_source *src);
void roi_undo_pin(struct roi_source *src);
void roi_clear_pins(struct roi_source *src);
/* Loads a picture file (PNG, JPEG) as the snapshot reference, scaled to the ROI picture, on the next frame. */
void roi_load_reference(struct roi_source *src, const char *path);
/* Copies the snapshot reference as 8-bit BGRA, the way the view shows it (an HDR one through the display
 * conversion). Call inside obs_enter_graphics(). *out is allocated with bmalloc. */
bool roi_read_reference(struct roi_source *src, uint8_t **out, uint32_t *w, uint32_t *h);
/* The first color picker pin, normalized to the picture; false when there is none. */
bool roi_first_pin(struct roi_source *src, float *x, float *y);

#ifdef __cplusplus
}
#endif

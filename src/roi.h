#pragma once

#include <util/darray.h>
#include "common.h"

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
	volatile bool shapes_dirty;

	// rasterized shapes, rebuilt in the graphics thread when shapes change
	gs_texture_t *mask_tex;
	uint8_t *mask_buf;
	uint32_t mask_w, mask_h;

	struct cm_mask mask;

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

#ifdef __cplusplus
}
#endif

#pragma once

/* Small text renderer for labels and numbers drawn inside the scopes.
 * A font atlas is rasterized with Qt once on the UI thread (text_render_init) and
 * drawn as textured quads in the graphics thread. */

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

enum text_align_e {
	TEXT_ALIGN_LEFT = 0,
	TEXT_ALIGN_CENTER = 1,
	TEXT_ALIGN_RIGHT = 2,
};

enum text_valign_e {
	TEXT_VALIGN_TOP = 0,
	TEXT_VALIGN_MIDDLE = 1,
	TEXT_VALIGN_BOTTOM = 2,
};

/* Builds the font atlas; call once from the UI thread (module load). */
void text_render_init(void);
/* Frees graphics resources; call from module unload. */
void text_render_free(void);

/* Scope coordinates are often stretched to fill a tile. The scale gives source units per
 * screen pixel, so text keeps its pixel size and shape. Call inside video_render with the
 * size of the source's own coordinate space. */
struct text_scale
{
	float ux, uy; // source units per screen pixel
};
struct text_scale text_scale_from_viewport(float source_width, float source_height);

/* Draws UTF-8 text (ASCII plus the degree sign) at (x, y) in source units.
 * px_size is the text height in screen pixels; color is 0xAARRGGBB.
 * Returns the width drawn, in source units. */
float text_draw(const struct text_scale *scale, const char *text, float x, float y, float px_size, uint32_t color,
		int align, int valign);

/* Width the text would take, in source units. */
float text_width(const struct text_scale *scale, const char *text, float px_size);

/* Device pixels per point of the display being drawn (2 on Retina); text sizes are in points while it is set.
 * The dock sets it while it draws and puts it back to 1 afterwards. Graphics thread only. */
void text_set_ui_scale(float scale);

#ifdef __cplusplus
}
#endif

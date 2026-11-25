#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Small drawing helpers in the current coordinates, colors as 0xAARRGGBB. Graphics thread. */
void du_fill_rect(float x0, float y0, float x1, float y1, uint32_t argb);
void du_line(float x0, float y0, float x1, float y1, uint32_t argb);
void du_rect(float x0, float y0, float x1, float y1, uint32_t argb);
void du_circle(float cx, float cy, float r, uint32_t argb, bool filled);

/* 0xAARRGGBB from 0..1 components. */
uint32_t du_argb(float r, float g, float b, float a);

#ifdef __cplusplus
}
#endif

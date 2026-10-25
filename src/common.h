#pragma once

#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool is_program_name(const char *name)
{
	return name && name[0] == 0;
}

static inline bool is_mainview_name(const char *name)
{
	return name && name[0] == 0x01 && name[1] == 0;
}

static inline bool is_preview_name(const char *name)
{
	return name && name[0] == 0x10 && name[1] == 0;
}

/* Selection mask applied while converting the captured frame.
 * Pixels outside the mask get alpha 0, which the scopes skip. */
struct cm_mask
{
	gs_texture_t *tex;        // R8 shape mask over the captured frame; NULL means no shapes
	bool qualifier;           // hue/saturation/luma qualifier enabled
	float hue_center;         // degrees
	float hue_half_width;     // degrees; 180 or more passes every hue
	float sat_min, sat_max;   // 0..1
	float luma_min, luma_max; // 0..1
};

struct cm_surface_data
{
	uint8_t *rgb_data, *yuv_data;
	uint32_t linesize, width, height;
	int colorspace;
	gs_texture_t *tex; // for bypass mode
};

typedef void (*cm_surface_cb_t)(void *data, struct cm_surface_data *surface_data);

struct cm_surface_queue_item
{
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;
	uint32_t width, height, sheight;
	uint32_t flags; // RGB or YUV
	int colorspace;

	cm_surface_cb_t cb;
	void *cb_data;
};

#define CM_SURFACE_QUEUE_SIZE 3

struct cm_source
{
	obs_source_t *self;

	// graphics
	struct cm_surface_queue_item queue[CM_SURFACE_QUEUE_SIZE];
	volatile int i_write_queue, i_staging_queue;
	volatile int i_read_queue;
	int i_bypass_queue;
	gs_texrender_t *texrender;
	enum gs_color_format texrender_format; // GS_RGBA16F holds linear light for HDR
	uint32_t texrender_width, texrender_height;
	gs_effect_t *effect;
	bool rendered;
	int x0, x1, y0, y1; // for ROI

	// threading
	pthread_t pipeline_thread;
	pthread_mutex_t pipeline_mutex;
	pthread_cond_t pipeline_cond;
	volatile bool pipeline_thread_running;
	volatile bool request_exit;

	// upper layer
	cm_surface_cb_t callback;
	void *callback_data;

	bool enumerating; // not thread safe but I have no other idea.

	// target
	pthread_mutex_t target_update_mutex;
	obs_weak_source_t *weak_target;
	obs_source_t *roi_src;
	struct roi_source *roi;
	char *target_name;

	// optional selection mask, owned by the ROI source
	const struct cm_mask *mask;

	// optional 3D LUT on the analyzed signal
	gs_texture_t *lut_tex;
	int lut_size;
	float lut_dmin[3], lut_dmax[3];
	int lut_mode; // cm_lut_mode_e
	char *lut_path;

	// properties
	int target_scale;
	int colorspace; // get from ovi if auto
	uint32_t flags;
	bool bypass;
};

enum cm_lut_mode_e {
	CM_LUT_OFF = 0,
	CM_LUT_VIEW = 1,   // the picture in the ROI view only
	CM_LUT_SCOPES = 2, // the picture and everything the scopes measure
};

#define CM_FLAG_CONVERT_RGB 1
#define CM_FLAG_CONVERT_YUV 2
#define CM_FLAG_RAW_TEXTURE 4
#define CM_FLAG_ROI 8

void cm_create(struct cm_source *src, obs_data_t *settings, obs_source_t *source);
void cm_destroy(struct cm_source *src);
void cm_update(struct cm_source *src, obs_data_t *settings);
void cm_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param);
void cm_get_properties(struct cm_source *src, obs_properties_t *props);
void cm_render_target(struct cm_source *src);
bool cm_stagesurface_map(struct cm_source *src, uint8_t **video_data, uint32_t *video_linesize);
void cm_stagesurface_unmap(struct cm_source *src);
void cm_bypass_render(struct cm_source *src);
void cm_tick(void *data, float unused);

void cm_request(struct cm_source *src, cm_surface_cb_t callback, void *data);
void cm_set_mask_params(gs_effect_t *effect, const struct cm_mask *mask);
/* Sets the uniforms that encode the captured picture (hdr_mode, sdr_white_over_max). */
void cm_set_signal_params(gs_effect_t *effect, int colorspace);
/* Sets the LUT uniforms; for_view selects the ROI view, otherwise the analyzed signal. */
void cm_set_lut_params(gs_effect_t *effect, const struct cm_source *src, bool for_view);

/* The color space the scope's data is in: the ROI's when the scope follows the dock selection. */
int cm_get_colorspace(const struct cm_source *src);

uint32_t cm_bypass_get_width(struct cm_source *src);
uint32_t cm_bypass_get_height(struct cm_source *src);
gs_texture_t *cm_bypass_get_texture(struct cm_source *src);
static inline bool cm_is_roi(const struct cm_source *src)
{
	return src->roi_src && src->roi;
}

bool is_roi_source_name(const char *name);

#ifdef __cplusplus
}
#endif

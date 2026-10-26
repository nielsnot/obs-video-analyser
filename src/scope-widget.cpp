#include <obs-module.h>
#include <obs.h>
#include <obs-frontend-api.h>
#include <util/threading.h>
#include <util/dstr.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <QMenu>
#include <QAction>
#include <QMouseEvent>
#include <QToolButton>
#include <QHBoxLayout>
#include <QTabBar>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QInputDialog>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QTimer>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QPointer>
#include <QKeyEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QImage>
#include <QStandardPaths>
#include <QWidgetAction>
#include <QDockWidget>
#include <QShortcut>
#include <QColor>
#include <memory>
#include "plugin-macros.generated.h"
#include "scope-widget.hpp"
#include "scope-widget-properties.hpp"
#include "scope-dock.hpp"
#include "ScopeWidgetInteractiveEventFilter.hpp"
#include "roi.h"
#include "text-render.h"

/* ---------- scope types that can be added to a dock ---------- */

enum fit_e {
	FIT_FILL,         // stretches to the tile; reads best when wide (waveform, histogram)
	FIT_SQUARE,       // vectorscope
	FIT_ASPECT,       // keeps the picture's aspect ratio (zebra, false color)
	FIT_FOCUSPEAKING, // aspect, or actual size when that property is set
};

struct scope_type_s
{
	const char *id;       // source type id
	const char *key;      // saved in the dock settings
	const char *name_key; // locale key for menus and titles
	int fit;
};

static const scope_type_s scope_types[] = {
	{ID_PREFIX "waveform_source", "waveform", "Waveform", FIT_FILL},
	{ID_PREFIX "vectorscope_source", "vectorscope", "Vectorscope", FIT_SQUARE},
	{ID_PREFIX "histogram_source", "histogram", "Histogram", FIT_FILL},
	{ID_PREFIX "zebra_source", "zebra", "Zebra", FIT_ASPECT},
	{ID_PREFIX "falsecolor_source", "falsecolor", "False Color", FIT_ASPECT},
	{ID_PREFIX "focuspeaking_source", "focuspeaking", "FocusPeaking.Name", FIT_FOCUSPEAKING},
	{ID_PREFIX "diamond_source", "diamond", "Diamond", FIT_SQUARE},
	{ID_PREFIX "cie_source", "cie", "CIE", FIT_ASPECT},
	{ID_PREFIX "qc_source", "qc", "QC", FIT_ASPECT},
	{ID_PREFIX "satlum_source", "satlum", "SatLum", FIT_SQUARE},
	{ID_PREFIX "lineplot_source", "lineplot", "LinePlot", FIT_FILL},
	{ID_PREFIX "info_source", "info", "Info", FIT_ASPECT},
	{ID_PREFIX "audio_source", "audio", "Audio", FIT_ASPECT},
	{ID_PREFIX "cube_source", "cube", "Cube", FIT_SQUARE},
	{ID_PREFIX "faces_source", "faces", "Faces", FIT_ASPECT},
};
#define N_TYPES ((int)(sizeof(scope_types) / sizeof(scope_types[0])))
enum {
	TYPE_WAVEFORM = 0,
	TYPE_VECTORSCOPE,
	TYPE_HISTOGRAM,
	TYPE_ZEBRA,
	TYPE_FALSECOLOR,
	TYPE_FOCUSPEAKING,
	TYPE_DIAMOND
};

static int type_from_key(const char *key)
{
	for (int t = 0; key && t < N_TYPES; t++)
		if (strcmp(scope_types[t].key, key) == 0)
			return t;
	return -1;
}

// waveform settings used for the default dock
#define WV_COMP_RGB 0x07
#define WV_COMP_Y 0x20
#define WV_DISP_PARADE 2

#define ROI_ID ID_PREFIX "roi"
#define ROI_KEY "roi" // prefix of the ROI entries in the saved dock settings

/* ---------- widget state ---------- */

static void draw(void *param, uint32_t cx, uint32_t cy);

struct src_rect_s
{
	int x0 = INT_MIN, y0 = INT_MIN, x1 = INT_MIN, y1 = INT_MIN;
	int w = 0, h = 0;

	inline bool is_inside(int x, int y) const
	{
		return x0 <= x && x <= x1 && x0 < x1 && y0 <= y && y <= y1 && y0 < y1;
	}
	inline int x_from_widget(int x) const { return x1 > x0 ? (x - x0) * w / (x1 - x0) : 0; }
	inline int y_from_widget(int y) const { return y1 > y0 ? (y - y0) * h / (y1 - y0) : 0; }
	inline void clear() { *this = src_rect_s(); }

	// the cell the tile was given; the scope is fitted inside it (free layout, arranging)
	int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
	inline bool cell_inside(int x, int y) const
	{
		return cx0 <= x && x <= cx1 && cy0 <= y && y <= cy1 && cx0 < cx1;
	}
};

/* A tile's place in the free layout, as fractions of the dock; w <= 0 means not placed yet. */
struct free_rect_s
{
	float x = 0.0f, y = 0.0f, w = -1.0f, h = -1.0f;
};

enum aspect_e {
	ASPECT_AUTO = 0, // what suits the scope type
	ASPECT_FILL = 1,
	ASPECT_16_9 = 2,
	ASPECT_4_3 = 3,
	ASPECT_1_1 = 4,
};

struct scope_entry
{
	obs_source_t *src = nullptr;
	int type = 0;
	volatile bool actual_size = false; // focus peaking only
	src_rect_s rect;

	// name shown on the scope (guarded by the widget mutex; read while drawing)
	std::string label;
	bool show_label = true;

	// what the scope analyzes: the dock's ROI selection, or a source of its own
	bool use_roi = true;
	std::string target; // target_name when not using the ROI ("" is the program)

	int aspect = ASPECT_AUTO;
	free_rect_s free;
	uint32_t border = 0; // tile border color (0xAARRGGBB), 0 for none; groups the scopes of one camera
};

/* Tiles are numbered 0 for the ROI and 1..n for the added scopes. */
struct scope_widget_s
{
	pthread_mutex_t mutex;

	obs_source_t *roi = nullptr;
	volatile bool roi_shown = true;
	src_rect_s roi_rect;

	std::vector<scope_entry *> entries;

	int i_mouse_last = -1;
	int i_menu = -1;

	volatile int layout = 0; // scope_layout_e
	volatile int solo = -1;  // maximized tile, or -1

	// free layout
	free_rect_s roi_free;
	volatile bool arranging = false; // tiles are moved and resized with the mouse
	int drag_tile = -1;
	bool drag_resize = false;
	int drag_x = 0, drag_y = 0;
	free_rect_s drag_start;
	int last_cx = 0, last_cy = 0;

	// quick controls shown on the tile under the mouse
	volatile int hover_tile = -1;
	int qb_drag_tile = -1; // dragging the intensity slider
	float ui_scale = 1.0f; // device pixels per point, so the quick controls keep a readable size

	// Views: only the active one has live sources; the others are kept as saved settings.
	struct view_s
	{
		std::string name;
		OBSData state;
	};
	std::vector<view_s> views;
	int active_view = 0;
};

static inline int n_tiles(const scope_widget_s *data)
{
	return 1 + (int)data->entries.size();
}

static inline obs_source_t *tile_source(const scope_widget_s *data, int tile)
{
	if (tile == 0)
		return data->roi;
	if (tile > 0 && tile <= (int)data->entries.size())
		return data->entries[tile - 1]->src;
	return nullptr;
}

static inline src_rect_s *tile_rect(scope_widget_s *data, int tile)
{
	if (tile == 0)
		return &data->roi_rect;
	if (tile > 0 && tile <= (int)data->entries.size())
		return &data->entries[tile - 1]->rect;
	return nullptr;
}

static inline int tile_fit(const scope_widget_s *data, int tile, bool *actual_size)
{
	*actual_size = false;
	if (tile == 0)
		return FIT_ASPECT;
	const scope_entry *e = data->entries[tile - 1];
	*actual_size = e->actual_size;
	return scope_types[e->type].fit;
}

static void generate_source_name(std::string &result, const char *id)
{
	DStr name;
	dstr_init_copy(name, "dock-");
	dstr_cat(name, id);
	for (int inc = 0;;) {
		OBSSourceAutoRelease source = obs_get_source_by_name(name->array);
		if (!source) {
			result = name->array;
			return;
		}

		dstr_printf(name, "dock-%s-%d", id, inc++);
	}
}

static obs_source_t *create_source(const char *id, obs_data_t *settings, const char *name)
{
	const char *v_id = obs_get_latest_input_type_id(id);
	if (!v_id) {
		blog(LOG_ERROR, "create_source(id=%s): obs_get_latest_input_type_id failed", id);
		return NULL;
	}
	return obs_source_create(v_id, name, settings, NULL);
}

static void focuspeaking_update_cb(void *param, calldata_t *cd)
{
	auto *e = static_cast<scope_entry *>(param);
	auto *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	OBSDataAutoRelease settings = obs_source_get_settings(src);
	e->actual_size = obs_data_get_bool(settings, "actual_size");
}

static void release_entry(scope_entry *e)
{
	if (e->src) {
		if (e->type == TYPE_FOCUSPEAKING)
			signal_handler_disconnect(obs_source_get_signal_handler(e->src), "update",
						  focuspeaking_update_cb, e);
		obs_source_release(e->src);
	}
	delete e;
}

/* ---------- layout ---------- */

enum scope_layout_e {
	SCOPE_LAYOUT_AUTO = 0,
	SCOPE_LAYOUT_VERTICAL = 1,
	SCOPE_LAYOUT_HORIZONTAL = 2,
	SCOPE_LAYOUT_FREE = 3, // every tile where you put it
};

static inline free_rect_s *tile_free(scope_widget_s *data, int tile)
{
	if (tile == 0)
		return &data->roi_free;
	if (tile > 0 && tile <= (int)data->entries.size())
		return &data->entries[tile - 1]->free;
	return nullptr;
}

static void fit_source(int fit, bool actual_size, obs_source_t *s, int cell_w, int cell_h, int *w_out, int *h_out);

/* Fits a tile into its cell: the scope's own fit, or the aspect chosen for it. */
static void fit_tile(const scope_widget_s *data, int t, obs_source_t *s, int cell_w, int cell_h, int *w, int *h)
{
	const int aspect = t > 0 ? data->entries[t - 1]->aspect : ASPECT_AUTO;
	if (aspect == ASPECT_FILL) {
		*w = cell_w;
		*h = cell_h;
		return;
	}
	if (aspect == ASPECT_16_9 || aspect == ASPECT_4_3 || aspect == ASPECT_1_1) {
		const double r = aspect == ASPECT_16_9 ? 16.0 / 9.0 : aspect == ASPECT_4_3 ? 4.0 / 3.0 : 1.0;
		if ((double)cell_w / std::max(cell_h, 1) > r) {
			*h = cell_h;
			*w = (int)(cell_h * r);
		} else {
			*w = cell_w;
			*h = (int)(cell_w / r);
		}
		return;
	}
	bool actual_size;
	const int fit = tile_fit(data, t, &actual_size);
	fit_source(fit, actual_size, s, cell_w, cell_h, w, h);
}

#define SCOPE_GAP 2

static void fit_source(int fit, bool actual_size, obs_source_t *s, int cell_w, int cell_h, int *w_out, int *h_out)
{
	int w_src = (int)obs_source_get_width(s);
	int h_src = (int)obs_source_get_height(s);
	int w = cell_w;
	int h = cell_h;
	if (fit == FIT_FOCUSPEAKING)
		fit = actual_size ? FIT_FILL : FIT_ASPECT;
	switch (fit) {
	case FIT_ASPECT:
		if (w_src > 0 && h_src > 0) {
			if (w * h_src > h * w_src)
				w = h * w_src / h_src;
			else if (h * w_src > w * h_src)
				h = w * h_src / w_src;
		}
		break;
	case FIT_SQUARE:
		w = h = std::min(w, h);
		break;
	default:
		break;
	}
	*w_out = std::max(w, 0);
	*h_out = std::max(h, 0);
}

/* How useful a tile of this size is for the scope; used to pick the best grid. */
static double layout_score(int fit, bool actual_size, obs_source_t *s, int cell_w, int cell_h)
{
	int w, h;
	fit_source(fit, actual_size, s, cell_w, cell_h, &w, &h);
	if (w <= 0 || h <= 0)
		return 0.0;
	double area = (double)w * h;
	if (fit == FIT_FILL) {
		const double preferred_aspect = 1.5;
		double aspect = (double)w / h;
		if (aspect < preferred_aspect)
			area *= aspect / preferred_aspect;
	}
	return area;
}

static void choose_grid(const scope_widget_s *data, const std::vector<int> &tiles, int cx, int cy, int layout,
			int *cols_out, int *rows_out)
{
	const int n = (int)tiles.size();
	if (layout == SCOPE_LAYOUT_VERTICAL || n <= 1) {
		*cols_out = 1;
		*rows_out = std::max(n, 1);
		return;
	}
	if (layout == SCOPE_LAYOUT_HORIZONTAL) {
		*cols_out = n;
		*rows_out = 1;
		return;
	}

	double best = -1.0;
	int best_cols = 1;
	for (int cols = 1; cols <= n; cols++) {
		int rows = (n + cols - 1) / cols;
		if (cols > 1 && (rows - 1) * cols >= n)
			continue; // an entire empty row; a smaller grid covers it
		int cell_w = (cx - SCOPE_GAP * (cols - 1)) / cols;
		int cell_h = (cy - SCOPE_GAP * (rows - 1)) / rows;
		if (cell_w <= 0 || cell_h <= 0)
			continue;
		double score = 0.0;
		for (int t : tiles) {
			bool actual_size;
			int fit = tile_fit(data, t, &actual_size);
			score += layout_score(fit, actual_size, tile_source(data, t), cell_w, cell_h);
		}
		if (score > best * 1.0001) {
			best = score;
			best_cols = cols;
		}
	}
	*cols_out = best_cols;
	*rows_out = (n + best_cols - 1) / best_cols;
}

static void draw_quickbar(scope_widget_s *data, int tile);

static void draw(void *param, uint32_t cx_, uint32_t cy_)
{
	auto *data = (struct scope_widget_s *)param;
	const int cx = (int)cx_;
	const int cy = (int)cy_;

	if (pthread_mutex_trylock(&data->mutex))
		return;
	// text in points, so it reads the same on Retina and HiDPI screens
	text_set_ui_scale(data->ui_scale);

	const int total = n_tiles(data);
	for (int t = 0; t < total; t++)
		tile_rect(data, t)->clear();

	std::vector<int> tiles;
	const int solo = data->solo;
	if (solo >= 0 && solo < total && tile_source(data, solo)) {
		tiles.push_back(solo);
	} else {
		if (data->roi && data->roi_shown)
			tiles.push_back(0);
		for (int t = 1; t < total; t++)
			if (tile_source(data, t))
				tiles.push_back(t);
	}

	const int n = (int)tiles.size();
	int cols = 1, rows = 1;
	data->last_cx = cx;
	data->last_cy = cy;
	choose_grid(data, tiles, cx, cy, data->layout == SCOPE_LAYOUT_FREE ? SCOPE_LAYOUT_AUTO : data->layout, &cols,
		    &rows);
	const int cell_w = n ? (cx - SCOPE_GAP * (cols - 1)) / cols : 0;
	const int cell_h = n ? (cy - SCOPE_GAP * (rows - 1)) / rows : 0;

	gs_blend_state_push();
	gs_reset_blend_state();

	for (int k = 0; k < n && cell_w > 0 && cell_h > 0; k++) {
		const int t = tiles[k];
		obs_source_t *s = tile_source(data, t);
		int w_src = obs_source_get_width(s);
		int h_src = obs_source_get_height(s);
		if (w_src <= 0 || h_src <= 0)
			continue;

		const int row = k / cols;
		const int col = k % cols;
		const int cols_in_row = std::min(cols, n - row * cols);
		// Center a partly filled last row.
		int x_cell = col * (cell_w + SCOPE_GAP) + (cols - cols_in_row) * (cell_w + SCOPE_GAP) / 2;
		int y_cell = row * (cell_h + SCOPE_GAP);
		int w_cell = cell_w, h_cell = cell_h;

		if (data->layout == SCOPE_LAYOUT_FREE && solo < 0) {
			free_rect_s *f = tile_free(data, t);
			if (f->w <= 0.0f || f->h <= 0.0f) {
				// not placed yet: start where the grid would put it
				f->x = (float)x_cell / cx;
				f->y = (float)y_cell / cy;
				f->w = (float)cell_w / cx;
				f->h = (float)cell_h / cy;
			}
			x_cell = (int)(f->x * cx);
			y_cell = (int)(f->y * cy);
			w_cell = (int)(f->w * cx);
			h_cell = (int)(f->h * cy);
		}

		int w, h;
		fit_tile(data, t, s, w_cell, h_cell, &w, &h);
		if (w <= 0 || h <= 0)
			continue;
		int x = x_cell + (w_cell - w) / 2;
		int y = y_cell + (h_cell - h) / 2;

		gs_projection_push();
		gs_viewport_push();
		gs_set_viewport(x, y, w, h);
		gs_ortho(0.0f, (float)w_src, -1.0f, (float)h_src, -100.0f, 100.0f);

		src_rect_s *r = tile_rect(data, t);
		r->x0 = x;
		r->y0 = y;
		r->x1 = x + w;
		r->y1 = y + h;
		r->cx0 = x_cell;
		r->cy0 = y_cell;
		r->cx1 = x_cell + w_cell;
		r->cy1 = y_cell + h_cell;
		r->w = w_src;
		r->h = h_src;

		obs_source_video_render(s);

		if (t > 0 && data->entries[t - 1]->border) {
			// colored frame around the tile, in tile pixels
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
			gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), data->entries[t - 1]->border);
			const float b = 2.0f, fw = (float)w, fh = (float)h;
			while (gs_effect_loop(solid, "Solid")) {
				gs_render_start(false);
				// four thin quads as two triangles each
				const float q[4][4] = {
					{0, 0, fw, b}, {0, fh - b, fw, fh}, {0, 0, b, fh}, {fw - b, 0, fw, fh}};
				for (int i = 0; i < 4; i++) {
					gs_vertex2f(q[i][0], q[i][1]);
					gs_vertex2f(q[i][2], q[i][1]);
					gs_vertex2f(q[i][0], q[i][3]);
					gs_vertex2f(q[i][0], q[i][3]);
					gs_vertex2f(q[i][2], q[i][1]);
					gs_vertex2f(q[i][2], q[i][3]);
				}
				gs_render_stop(GS_TRIS);
			}
			gs_ortho(0.0f, (float)w_src, -1.0f, (float)h_src, -100.0f, 100.0f);
		}

		if (t > 0) {
			const scope_entry *e = data->entries[t - 1];
			if (e->show_label && !e->label.empty()) {
				// Draw in tile pixels so the label keeps its size whatever the scope's scale.
				gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
				const struct text_scale ts = text_scale_from_viewport((float)w, (float)h);
				const float px = std::min(16.0f, std::max(11.0f, h / 14.0f));
				const float tw = text_width(&ts, e->label.c_str(), px);
				gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
				gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x99000000);
				while (gs_effect_loop(solid, "Solid")) {
					gs_render_start(false);
					gs_vertex2f(4.0f, 4.0f);
					gs_vertex2f(tw + 14.0f, 4.0f);
					gs_vertex2f(4.0f, px + 8.0f);
					gs_vertex2f(tw + 14.0f, px + 8.0f);
					gs_render_stop(GS_TRISTRIP);
				}
				text_draw(&ts, e->label.c_str(), 9.0f, 6.0f, px, 0xE6FFFFFF, TEXT_ALIGN_LEFT,
					  TEXT_VALIGN_TOP);
			}
		}

		gs_viewport_pop();
		gs_projection_pop();
	}

	if (!data->arranging && data->hover_tile > 0 && data->hover_tile < total) {
		gs_projection_push();
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		draw_quickbar(data, data->hover_tile);
		gs_projection_pop();
	}

	// arranging: outline every cell with a resize handle at the bottom right
	if (data->arranging && data->layout == SCOPE_LAYOUT_FREE && solo < 0) {
		gs_projection_push();
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		const struct text_scale ts = text_scale_from_viewport((float)cx, (float)cy);
		for (int k = 0; k < n; k++) {
			const src_rect_s *r = tile_rect(data, tiles[k]);
			if (r->cx1 <= r->cx0)
				continue;
			const float x0 = (float)r->cx0, y0 = (float)r->cy0, x1 = (float)r->cx1 - 1.0f,
				    y1 = (float)r->cy1 - 1.0f;
			const bool active = tiles[k] == data->drag_tile;
			gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"),
					    active ? 0xFFFFFFFF : 0xFF6FCF86);
			while (gs_effect_loop(solid, "Solid")) {
				gs_render_start(false);
				gs_vertex2f(x0, y0);
				gs_vertex2f(x1, y0);
				gs_vertex2f(x1, y0);
				gs_vertex2f(x1, y1);
				gs_vertex2f(x1, y1);
				gs_vertex2f(x0, y1);
				gs_vertex2f(x0, y1);
				gs_vertex2f(x0, y0);
				gs_render_stop(GS_LINES);
				gs_render_start(false);
				gs_vertex2f(x1 - 12.0f, y1 - 12.0f);
				gs_vertex2f(x1, y1 - 12.0f);
				gs_vertex2f(x1 - 12.0f, y1);
				gs_vertex2f(x1, y1);
				gs_render_stop(GS_TRISTRIP);
			}
			const int t = tiles[k];
			const char *name = t == 0 ? obs_module_text("ROI") : data->entries[t - 1]->label.c_str();
			text_draw(&ts, name, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 14.0f, 0xFFFFFFFF, TEXT_ALIGN_CENTER,
				  TEXT_VALIGN_MIDDLE);
		}
		gs_projection_pop();
	}

	gs_blend_state_pop();

	// Leave no vertex buffer of ours current for libobs' own draws.
	gs_load_vertexbuffer(NULL);

	text_set_ui_scale(1.0f);
	pthread_mutex_unlock(&data->mutex);
}

struct grid_key_s
{
	const char *key;
	bool is_double;
};

/* Settings that draw a scope's graticule; 0 means no graticule. */
static const grid_key_s grid_keys_waveform[] = {{"graticule_lines", false}, {NULL, false}};
static const grid_key_s grid_keys_vectorscope[] = {{"graticule", false}, {NULL, false}};
static const grid_key_s grid_keys_histogram[] = {{"graticule_vertical_lines", false},
						 {"graticule_horizontal_step_fixed", true},
						 {"graticule_horizontal_step_ratio", true},
						 {NULL, false}};

static const grid_key_s *grid_keys_for(int type)
{
	switch (type) {
	case TYPE_WAVEFORM:
		return grid_keys_waveform;
	case TYPE_VECTORSCOPE:
		return grid_keys_vectorscope;
	case TYPE_HISTOGRAM:
		return grid_keys_histogram;
	default:
		return NULL;
	}
}

static bool grid_visible(obs_source_t *src, const grid_key_s *keys)
{
	OBSDataAutoRelease settings = obs_source_get_settings(src);
	for (const grid_key_s *k = keys; k->key; k++) {
		if (k->is_double ? obs_data_get_double(settings, k->key) != 0.0
				 : obs_data_get_int(settings, k->key) != 0)
			return true;
	}
	return false;
}

/* Hiding remembers the current values so showing brings back the same graticule. */
static void set_grid_visible(obs_source_t *src, const grid_key_s *keys, bool visible)
{
	OBSDataAutoRelease settings = obs_source_get_settings(src);
	OBSDataAutoRelease changes = obs_data_create();
	for (const grid_key_s *k = keys; k->key; k++) {
		std::string prev = std::string("grid_prev_") + k->key;
		if (k->is_double) {
			double cur = obs_data_get_double(settings, k->key);
			if (visible) {
				double v = obs_data_has_user_value(settings, prev.c_str())
						   ? obs_data_get_double(settings, prev.c_str())
						   : obs_data_get_default_double(settings, k->key);
				obs_data_set_double(changes, k->key, v);
			} else {
				if (cur != 0.0)
					obs_data_set_double(changes, prev.c_str(), cur);
				obs_data_set_double(changes, k->key, 0.0);
			}
		} else {
			long long cur = obs_data_get_int(settings, k->key);
			if (visible) {
				long long v = obs_data_has_user_value(settings, prev.c_str())
						      ? obs_data_get_int(settings, prev.c_str())
						      : obs_data_get_default_int(settings, k->key);
				obs_data_set_int(changes, k->key, v);
			} else {
				if (cur != 0)
					obs_data_set_int(changes, prev.c_str(), cur);
				obs_data_set_int(changes, k->key, 0);
			}
		}
	}
	obs_source_update(src, changes);
}

/* ---------- quick controls ---------- */

enum qb_kind_e {
	QB_INTENSITY,
	QB_GRID,
	QB_COLORS,
	QB_GAIN,
	QB_ASPECT,
	QB_MAX,
};

struct qb_item
{
	int kind;
	int x0, x1;
};

#define QB_HEIGHT 30

static bool type_has_intensity(int type)
{
	switch (type) {
	case TYPE_WAVEFORM:
	case TYPE_VECTORSCOPE:
	case TYPE_DIAMOND:
		return true;
	default:
		// CIE and Sat / Lum
		return type >= 0 && type < N_TYPES &&
		       (!strcmp(scope_types[type].key, "cie") || !strcmp(scope_types[type].key, "satlum"));
	}
}

static const char *aspect_label(int aspect)
{
	switch (aspect) {
	case ASPECT_FILL:
		return "Fill";
	case ASPECT_16_9:
		return "16:9";
	case ASPECT_4_3:
		return "4:3";
	case ASPECT_1_1:
		return "1:1";
	default:
		return "Auto";
	}
}

/* The text on a quick control button. */
static std::string qb_label(const scope_widget_s *data, const scope_entry *e, int kind)
{
	OBSDataAutoRelease s = obs_source_get_settings(e->src);
	switch (kind) {
	case QB_GRID:
		return "Grid";
	case QB_COLORS:
		return "Color";
	case QB_GAIN:
		return "Gain " + std::to_string(std::max(1LL, obs_data_get_int(s, "gain"))) + "x";
	case QB_ASPECT:
		return aspect_label(e->aspect);
	case QB_MAX:
		return data->solo >= 0 ? "Back" : "Full";
	default:
		return "Int";
	}
}

#define QB_TEXT_PX 13.0f

/* The bar at the bottom of a scope tile: which controls, and where (widget pixels). */
static std::vector<qb_item> qb_layout(const scope_widget_s *data, int tile, int *bar_y0, int *bar_y1)
{
	std::vector<qb_item> items;
	if (tile <= 0 || tile > (int)data->entries.size())
		return items;
	const scope_entry *e = data->entries[tile - 1];
	const src_rect_s &r = e->rect;
	const float k = std::max(1.0f, data->ui_scale);
	auto px = [k](int v) { return (int)std::lround(v * k); };
	if (r.x1 - r.x0 < px(140) || r.y1 - r.y0 < px(70))
		return items;
	*bar_y1 = r.y1 - px(4);
	*bar_y0 = *bar_y1 - px(QB_HEIGHT);
	int x = r.x0 + px(6);
	// buttons as wide as their text, so no letter is cut off
	const struct text_scale ts = {1.0f, 1.0f};
	auto add = [&](int kind, int min_w) {
		int w = px(min_w);
		if (kind != QB_INTENSITY) {
			const std::string label = qb_label(data, e, kind);
			const float tw = text_width(&ts, label.c_str(), QB_TEXT_PX * k);
			w = std::max(w, (int)std::ceil(tw + 16.0f * k));
		}
		if (x + w > r.x1 - px(6))
			return;
		items.push_back({kind, x, x + w});
		x += w + px(4);
	};
	if (type_has_intensity(e->type))
		add(QB_INTENSITY, 120);
	if (grid_keys_for(e->type))
		add(QB_GRID, 40);
	if (e->type == TYPE_VECTORSCOPE || e->type == TYPE_WAVEFORM)
		add(QB_COLORS, 40);
	if (e->type == TYPE_VECTORSCOPE || e->type == TYPE_WAVEFORM)
		add(QB_GAIN, 56);
	add(QB_ASPECT, 40);
	add(QB_MAX, 40);
	return items;
}

static void qb_fill(float x0, float y0, float x1, float y1, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(x0, y0);
		gs_vertex2f(x1, y0);
		gs_vertex2f(x0, y1);
		gs_vertex2f(x1, y1);
		gs_render_stop(GS_TRISTRIP);
	}
}

static float intensity_to_t(long long v)
{
	return (float)(std::log((double)std::max(v, 1LL)) / std::log(255.0));
}

/* Called from draw() with a projection in widget pixels. */
static void draw_quickbar(scope_widget_s *data, int tile)
{
	int y0 = 0, y1 = 0;
	const std::vector<qb_item> items = qb_layout(data, tile, &y0, &y1);
	if (items.empty())
		return;
	const scope_entry *e = data->entries[tile - 1];
	OBSDataAutoRelease s = obs_source_get_settings(e->src);
	const float k = std::max(1.0f, data->ui_scale);
	const struct text_scale ts = {1.0f, 1.0f};
	const float px = QB_TEXT_PX * k;
	qb_fill((float)items.front().x0 - 4.0f * k, (float)y0, (float)items.back().x1 + 4.0f * k, (float)y1,
		0xD0141418);
	for (const qb_item &it : items) {
		const float x0 = (float)it.x0, x1 = (float)it.x1, ym = (y0 + y1) * 0.5f;
		bool on = false;
		std::string label;
		switch (it.kind) {
		case QB_INTENSITY: {
			text_draw(&ts, "Int", x0 + 2.0f * k, ym, px, 0xFFD0D0D6, TEXT_ALIGN_LEFT, TEXT_VALIGN_MIDDLE);
			const float tx0 = x0 + 30.0f * k, tx1 = x1 - 6.0f * k;
			qb_fill(tx0, ym - 2.0f * k, tx1, ym + 2.0f * k, 0xFF3A3A40);
			const float t = intensity_to_t(obs_data_get_int(s, "intensity"));
			const float kx = tx0 + (tx1 - tx0) * t;
			qb_fill(tx0, ym - 2.0f * k, kx, ym + 2.0f * k, 0xFF6FCF86);
			qb_fill(kx - 5.0f * k, ym - 7.0f * k, kx + 5.0f * k, ym + 7.0f * k, 0xFFF0F0F2);
			continue;
		}
		case QB_GRID:
			on = grid_visible(e->src, grid_keys_for(e->type));
			break;
		case QB_COLORS:
			on = e->type == TYPE_VECTORSCOPE ? obs_data_get_int(s, "color_type") != 0
							 : obs_data_get_bool(s, "colorize");
			break;
		case QB_GAIN:
			on = obs_data_get_int(s, "gain") > 1;
			break;
		case QB_ASPECT:
			on = e->aspect != ASPECT_AUTO;
			break;
		}
		label = qb_label(data, e, it.kind);
		qb_fill(x0, (float)y0 + 3.0f * k, x1, (float)y1 - 3.0f * k, on ? 0xFF3D5C45 : 0xFF2A2A2F);
		text_draw(&ts, label.c_str(), (x0 + x1) * 0.5f, ym, px, 0xFFF4F4F6, TEXT_ALIGN_CENTER,
			  TEXT_VALIGN_MIDDLE);
	}
}

static void qb_set_intensity(obs_source_t *src, int x, const qb_item &it, float ui_scale)
{
	const float k = std::max(1.0f, ui_scale);
	const float tx0 = it.x0 + 30.0f * k, tx1 = it.x1 - 6.0f * k;
	const float t = std::clamp((x - tx0) / std::max(tx1 - tx0, 1.0f), 0.0f, 1.0f);
	const long long v = std::clamp((long long)std::lround(std::exp(t * std::log(255.0))), 1LL, 255LL);
	OBSDataAutoRelease d = obs_data_create();
	obs_data_set_int(d, "intensity", v);
	obs_source_update(src, d);
}

/* ---------- widget ---------- */

ScopeWidget::ScopeWidget(QWidget *parent) : NorisQTDisplay(parent)
{
	properties = NULL;
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

	setMouseTracking(true);
	QObject::installEventFilter(new ScopeWidgetInteractiveEventFilter(this));

	data = new scope_widget_s;
	data->ui_scale = (float)devicePixelRatioF();
	pthread_mutex_init(&data->mutex, NULL);

	connect(this, &NorisQTDisplay::DisplayCreated, this, &ScopeWidget::RegisterCallbackToDisplay);
}

ScopeWidget::~ScopeWidget()
{
	disconnect(this, &NorisQTDisplay::DisplayCreated, this, &ScopeWidget::RegisterCallbackToDisplay);

	scope_dock_deleted(this);

	if (data) {
		if (obs_display_t *display = GetDisplay())
			obs_display_remove_draw_callback(display, draw, data);

		pthread_mutex_lock(&data->mutex);
		std::vector<scope_entry *> entries;
		entries.swap(data->entries);
		obs_source_t *roi = data->roi;
		data->roi = nullptr;
		pthread_mutex_unlock(&data->mutex);

		for (scope_entry *e : entries)
			release_entry(e);
		obs_source_release(roi);

		pthread_mutex_destroy(&data->mutex);
		delete data;
	}
	data = NULL;
}

void ScopeWidget::RegisterCallbackToDisplay()
{
	obs_display_t *display = GetDisplay();
	obs_display_add_draw_callback(display, draw, data);
}

void ScopeWidget::closeEvent(QCloseEvent *)
{
	DestroyDisplay();
}

void ScopeWidget::RemoveDock()
{
	obs_frontend_remove_dock(name.c_str());
}

/* ---------- adding and removing scopes ---------- */

static std::string roi_name_of(const ScopeWidget *w)
{
	char roi_name[64];
	snprintf(roi_name, sizeof(roi_name), "dock-roi-%p", (const void *)w);
	return roi_name;
}

static void call_compare(obs_source_t *src, bool on);

void ScopeWidget::addScope(int type, obs_data_t *settings)
{
	insertScope(type, settings, nullptr, -1);
}

/* meta (may be NULL) holds the dock-side settings of the scope: label, show_label, use_roi, target. */
void ScopeWidget::insertScope(int type, obs_data_t *settings, obs_data_t *meta, int index)
{
	if (type < 0 || type >= N_TYPES)
		return;

	const scope_type_s &st = scope_types[type];

	auto *e = new scope_entry;
	e->type = type;
	e->label = obs_module_text(st.name_key);
	if (meta) {
		if (obs_data_has_user_value(meta, "label"))
			e->label = obs_data_get_string(meta, "label");
		if (obs_data_has_user_value(meta, "show_label"))
			e->show_label = obs_data_get_bool(meta, "show_label");
		if (obs_data_has_user_value(meta, "use_roi"))
			e->use_roi = obs_data_get_bool(meta, "use_roi");
		e->target = obs_data_get_string(meta, "target");
		e->aspect = (int)obs_data_get_int(meta, "aspect");
		e->border = (uint32_t)obs_data_get_int(meta, "border");
		if (obs_data_has_user_value(meta, "free-w")) {
			e->free.x = (float)obs_data_get_double(meta, "free-x");
			e->free.y = (float)obs_data_get_double(meta, "free-y");
			e->free.w = (float)obs_data_get_double(meta, "free-w");
			e->free.h = (float)obs_data_get_double(meta, "free-h");
		}
	}

	OBSDataAutoRelease prop = settings ? obs_data_create_from_json(obs_data_get_json(settings)) : obs_data_create();
	obs_data_set_string(prop, "target_name", e->use_roi ? roi_name_of(this).c_str() : e->target.c_str());

	std::string name;
	generate_source_name(name, st.id);
	obs_source_t *src = create_source(st.id, prop, name.c_str());
	if (!src) {
		delete e;
		return;
	}

	e->src = src;
	if (type == TYPE_FOCUSPEAKING) {
		OBSDataAutoRelease s = obs_source_get_settings(src);
		e->actual_size = obs_data_get_bool(s, "actual_size");
		signal_handler_connect(obs_source_get_signal_handler(src), "update", focuspeaking_update_cb, e);
	}

	if (comparing)
		call_compare(src, true);

	pthread_mutex_lock(&data->mutex);
	if (index < 0 || index > (int)data->entries.size())
		data->entries.push_back(e);
	else
		data->entries.insert(data->entries.begin() + index, e);
	data->solo = -1;
	pthread_mutex_unlock(&data->mutex);
}

static void entry_meta(const scope_entry *e, obs_data_t *meta)
{
	obs_data_set_string(meta, "label", e->label.c_str());
	obs_data_set_bool(meta, "show_label", e->show_label);
	obs_data_set_bool(meta, "use_roi", e->use_roi);
	obs_data_set_string(meta, "target", e->target.c_str());
	obs_data_set_int(meta, "aspect", e->aspect);
	obs_data_set_int(meta, "border", (long long)e->border);
	if (e->free.w > 0.0f) {
		obs_data_set_double(meta, "free-x", e->free.x);
		obs_data_set_double(meta, "free-y", e->free.y);
		obs_data_set_double(meta, "free-w", e->free.w);
		obs_data_set_double(meta, "free-h", e->free.h);
	}
}

void ScopeWidget::duplicateScope(int tile)
{
	if (tile <= 0 || tile > (int)data->entries.size())
		return;
	const scope_entry *e = data->entries[tile - 1];
	OBSDataAutoRelease prop = obs_source_get_settings(e->src);
	OBSDataAutoRelease meta = obs_data_create();
	entry_meta(e, meta);
	insertScope(e->type, prop, meta, tile);
}

void ScopeWidget::changeScopeType(int tile, int type)
{
	if (tile <= 0 || tile > (int)data->entries.size() || type < 0 || type >= N_TYPES)
		return;
	const scope_entry *e = data->entries[tile - 1];
	if (e->type == type)
		return;
	OBSDataAutoRelease meta = obs_data_create();
	entry_meta(e, meta);
	// A name that was just the old type's name follows the new type.
	if (e->label == obs_module_text(scope_types[e->type].name_key))
		obs_data_set_string(meta, "label", obs_module_text(scope_types[type].name_key));
	insertScope(type, nullptr, meta, tile); // lands right after the old one
	removeScope(tile);
}

void ScopeWidget::removeScope(int tile)
{
	if (tile <= 0 || tile > (int)data->entries.size())
		return;

	pthread_mutex_lock(&data->mutex);
	scope_entry *e = data->entries[tile - 1];
	data->entries.erase(data->entries.begin() + (tile - 1));
	data->solo = -1;
	data->i_mouse_last = -1;
	pthread_mutex_unlock(&data->mutex);

	if (properties && properties->getSource() == e->src)
		closeProperties();

	release_entry(e);
}

void ScopeWidget::fillAddScopeMenu(QMenu *menu)
{
	for (int t = 0; t < N_TYPES; t++) {
		if (!strcmp(scope_types[t].key, "info")) {
			// one line per tile: timecode, clock, date, video format, text, or all of them stacked
			QMenu *sub = menu->addMenu(QString::fromUtf8(obs_module_text(scope_types[t].name_key)));
			static const char *items[][2] = {
				{"timecode", "Info.Prop.Timecode"}, {"clock", "Info.Prop.Clock"},
				{"date", "Info.Prop.Date"},         {"video", "Info.Prop.Video"},
				{"text", "Info.Prop.Text"},         {"all", "Info.Prop.All"},
			};
			for (const auto &it : items) {
				QAction *act = sub->addAction(QString::fromUtf8(obs_module_text(it[1])));
				const std::string item = it[0];
				connect(act, &QAction::triggered, this, [this, t, item]() {
					OBSDataAutoRelease d = obs_data_create();
					obs_data_set_string(d, "item", item.c_str());
					addScope(t, d);
				});
			}
			continue;
		}
		QAction *act = menu->addAction(QString::fromUtf8(obs_module_text(scope_types[t].name_key)));
		connect(act, &QAction::triggered, this, [this, t]() { addScope(t); });
	}
}

/* ---------- toolbar ---------- */

static QToolButton *make_tool_button(QWidget *parent, const char *text_key, const char *tip_key, bool checkable)
{
	auto *b = new QToolButton(parent);
	b->setText(QString::fromUtf8(obs_module_text(text_key)));
	b->setToolTip(QString::fromUtf8(obs_module_text(tip_key)));
	b->setCheckable(checkable);
	b->setAutoRaise(true);
	b->setToolButtonStyle(Qt::ToolButtonTextOnly);
	b->setFocusPolicy(Qt::NoFocus);
	return b;
}

QWidget *ScopeWidget::createToolbar(QWidget *parent)
{
	toolbar = new QWidget(parent);
	auto *layout = new QHBoxLayout(toolbar);
	layout->setContentsMargins(2, 1, 2, 1);
	layout->setSpacing(2);

	auto *btnAdd = make_tool_button(toolbar, "dock.toolbar.add", "dock.toolbar.add.tip", false);
	auto *addMenu = new QMenu(btnAdd);
	fillAddScopeMenu(addMenu);
	btnAdd->setMenu(addMenu);
	btnAdd->setPopupMode(QToolButton::InstantPopup);

	btnRect = make_tool_button(toolbar, "dock.toolbar.rect", "dock.toolbar.rect.tip", true);
	btnEllipse = make_tool_button(toolbar, "dock.toolbar.ellipse", "dock.toolbar.ellipse.tip", true);
	btnPin = make_tool_button(toolbar, "dock.toolbar.pin", "dock.toolbar.pin.tip", true);
	auto *btnSnap = make_tool_button(toolbar, "dock.toolbar.snapshot", "dock.toolbar.snapshot.tip", false);
	btnCompare = make_tool_button(toolbar, "dock.toolbar.compare", "dock.toolbar.compare.tip", true);
	btnArrange = make_tool_button(toolbar, "dock.toolbar.arrange", "dock.toolbar.arrange.tip", true);
	auto *btnExport = make_tool_button(toolbar, "dock.toolbar.export", "dock.toolbar.export.tip", false);
	btnFull = make_tool_button(toolbar, "dock.toolbar.full", "dock.toolbar.full.tip", true);
	btnFaces = make_tool_button(toolbar, "dock.toolbar.faces", "dock.toolbar.faces.tip", true);
	auto *btnUndo = make_tool_button(toolbar, "dock.toolbar.undo", "dock.toolbar.undo.tip", false);
	auto *btnReset = make_tool_button(toolbar, "dock.toolbar.reset", "dock.toolbar.reset.tip", false);

	tabs = new QTabBar(toolbar);
	tabs->setDocumentMode(true);
	tabs->setDrawBase(false);
	tabs->setExpanding(false);
	tabs->setMovable(false);
	tabs->setFocusPolicy(Qt::NoFocus);
	tabs->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(tabs, &QTabBar::currentChanged, this, [this](int index) {
		if (!rebuildingTabs)
			switchView(index);
	});
	connect(tabs, &QTabBar::tabBarDoubleClicked, this, &ScopeWidget::renameView);
	connect(tabs, &QWidget::customContextMenuRequested, this, &ScopeWidget::openTabMenu);
	auto *btnView = make_tool_button(toolbar, "dock.toolbar.addview", "dock.toolbar.addview.tip", false);
	connect(btnView, &QToolButton::clicked, this, &ScopeWidget::addView);

	layout->addWidget(tabs);
	layout->addWidget(btnView);
	layout->addSpacing(8);
	layout->addWidget(btnAdd);
	layout->addSpacing(8);
	layout->addWidget(btnRect);
	layout->addWidget(btnEllipse);
	layout->addWidget(btnPin);
	layout->addWidget(btnUndo);
	layout->addWidget(btnReset);
	layout->addSpacing(8);
	layout->addWidget(btnSnap);
	layout->addWidget(btnCompare);
	layout->addWidget(btnFaces);
	layout->addSpacing(8);
	layout->addWidget(btnArrange);
	layout->addWidget(btnExport);
	layout->addWidget(btnFull);
	layout->addStretch(1);

	// The shape buttons behave like radio buttons that can also be switched off.
	connect(btnRect, &QToolButton::clicked, this,
		[this](bool checked) { setDrawMode(checked ? ROI_DRAW_RECT : ROI_DRAW_NONE); });
	connect(btnEllipse, &QToolButton::clicked, this,
		[this](bool checked) { setDrawMode(checked ? ROI_DRAW_ELLIPSE : ROI_DRAW_NONE); });
	connect(btnPin, &QToolButton::clicked, this,
		[this](bool checked) { setDrawMode(checked ? ROI_DRAW_PIN : ROI_DRAW_NONE); });
	connect(btnSnap, &QToolButton::clicked, this, &ScopeWidget::takeSnapshot);
	connect(btnCompare, &QToolButton::toggled, this, &ScopeWidget::setCompare);
	connect(btnArrange, &QToolButton::toggled, this, &ScopeWidget::setArranging);
	connect(btnExport, &QToolButton::clicked, this, &ScopeWidget::exportScreenshot);
	connect(btnFull, &QToolButton::toggled, this, &ScopeWidget::setFullScreen);
	// Esc leaves full screen
	auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), parent);
	esc->setContext(Qt::WidgetWithChildrenShortcut);
	connect(esc, &QShortcut::activated, this, [this]() {
		if (btnFull && btnFull->isChecked())
			btnFull->setChecked(false);
	});
	connect(btnFaces, &QToolButton::toggled, this, [this](bool on) {
		if (!data->roi)
			return;
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_bool(d, "face_follow", on);
		obs_source_update(data->roi, d);
	});
	connect(btnUndo, &QToolButton::clicked, this, &ScopeWidget::undoShape);
	connect(btnReset, &QToolButton::clicked, this, &ScopeWidget::resetShapes);

	toolbar->setVisible(toolbarVisible);
	return toolbar;
}

QDockWidget *ScopeWidget::findDock() const
{
	for (QWidget *p = parentWidget(); p; p = p->parentWidget()) {
		if (auto *d = qobject_cast<QDockWidget *>(p))
			return d;
	}
	return nullptr;
}

/* Full screen: the dock is taken out of the main window if needed and shown full screen on the screen it is on;
 * switching off puts it back where it was. */
void ScopeWidget::setFullScreen(bool on)
{
	QDockWidget *dock = findDock();
	if (!dock)
		return;
	if (on) {
		if (dock->isFullScreen())
			return;
		fullWasFloating = dock->isFloating();
		if (!fullWasFloating)
			dock->setFloating(true);
		fullRestore = dock->geometry();
		dock->showFullScreen();
		dock->raise();
		dock->activateWindow();
	} else {
		if (!dock->isFullScreen())
			return;
		dock->showNormal();
		if (fullWasFloating)
			dock->setGeometry(fullRestore);
		else
			dock->setFloating(false);
	}
}

static struct roi_source *widget_roi(struct scope_widget_s *data)
{
	return data->roi ? roi_from_source(data->roi) : NULL;
}

void ScopeWidget::setDrawMode(int mode)
{
	drawMode = mode;
	if (btnRect)
		btnRect->setChecked(mode == ROI_DRAW_RECT);
	if (btnEllipse)
		btnEllipse->setChecked(mode == ROI_DRAW_ELLIPSE);
	if (btnPin)
		btnPin->setChecked(mode == ROI_DRAW_PIN);
	setCursor(mode == ROI_DRAW_NONE ? Qt::ArrowCursor : Qt::CrossCursor);

	roi_set_draw_mode(widget_roi(data), mode);

	// Drawing needs the ROI view, so bring it back if it was hidden.
	if (mode != ROI_DRAW_NONE) {
		data->roi_shown = true;
		if (data->solo > 0)
			data->solo = -1;
	}
}

/* Undo and Reset work on the pins while the picker is active, otherwise on the shapes. */
void ScopeWidget::undoShape()
{
	if (drawMode == ROI_DRAW_PIN)
		roi_undo_pin(widget_roi(data));
	else
		roi_undo_shape(widget_roi(data));
}

void ScopeWidget::resetShapes()
{
	if (drawMode == ROI_DRAW_PIN)
		roi_clear_pins(widget_roi(data));
	else
		roi_clear_shapes(widget_roi(data));
}

static void call_compare(obs_source_t *src, bool on)
{
	if (!src)
		return;
	uint8_t stack[64];
	calldata_t cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_bool(&cd, "on", on);
	proc_handler_call(obs_source_get_proc_handler(src), "va_set_compare", &cd);
}

/* Freezes the current frame and every scope's trace as the reference to compare with. */
void ScopeWidget::takeSnapshot()
{
	auto snap = [](obs_source_t *src) {
		if (!src)
			return;
		uint8_t stack[64];
		calldata_t cd;
		calldata_init_fixed(&cd, stack, sizeof(stack));
		proc_handler_call(obs_source_get_proc_handler(src), "va_snapshot", &cd);
	};
	snap(data->roi);
	for (scope_entry *e : data->entries)
		snap(e->src);
	if (btnCompare && !btnCompare->isChecked())
		btnCompare->setChecked(true); // show what was just taken
}

/* Renders the whole dock as it is now (without the hover controls) into a PNG file. */
void ScopeWidget::exportScreenshot()
{
	const int cx = data->last_cx, cy = data->last_cy;
	if (cx <= 0 || cy <= 0)
		return;

	QImage image(cx, cy, QImage::Format_RGBA8888);
	image.fill(Qt::black);
	bool ok = false;

	const int hover = data->hover_tile;
	const bool arranging = data->arranging;
	data->hover_tile = -1;
	data->arranging = false;

	obs_enter_graphics();
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (tr && gs_texrender_begin(tr, (uint32_t)cx, (uint32_t)cy)) {
		struct vec4 black;
		vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		draw(data, (uint32_t)cx, (uint32_t)cy);
		gs_texrender_end(tr);

		gs_stagesurf_t *stage = gs_stagesurface_create((uint32_t)cx, (uint32_t)cy, GS_RGBA);
		if (stage) {
			gs_stage_texture(stage, gs_texrender_get_texture(tr));
			uint8_t *ptr;
			uint32_t linesize;
			if (gs_stagesurface_map(stage, &ptr, &linesize)) {
				for (int y = 0; y < cy; y++) {
					uint8_t *dst = image.scanLine(y);
					memcpy(dst, ptr + (size_t)linesize * y, (size_t)cx * 4);
					for (int x = 0; x < cx; x++)
						dst[x * 4 + 3] = 255; // opaque
				}
				gs_stagesurface_unmap(stage);
				ok = true;
			}
			gs_stagesurface_destroy(stage);
		}
	}
	gs_texrender_destroy(tr);
	obs_leave_graphics();

	data->hover_tile = hover;
	data->arranging = arranging;
	if (!ok)
		return;

	const QString name = QStringLiteral("Video Analyser %1.png")
				     .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")));
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	const QString file =
		QFileDialog::getSaveFileName(this, QString::fromUtf8(obs_module_text("dock.menu.export")).remove('&'),
					     dir + QStringLiteral("/") + name, QStringLiteral("PNG (*.png)"));
	if (file.isEmpty())
		return;
	if (!image.save(file, "PNG"))
		blog(LOG_WARNING, "Could not save the screenshot to '%s'", file.toUtf8().constData());
}

/* A click on one of the quick controls of a scope tile. */
void ScopeWidget::quickControl(int tile, const qb_item &it, int x)
{
	if (tile <= 0 || tile > (int)data->entries.size())
		return;
	scope_entry *e = data->entries[tile - 1];
	obs_source_t *src = e->src;
	OBSDataAutoRelease s = obs_source_get_settings(src);
	OBSDataAutoRelease d = obs_data_create();
	switch (it.kind) {
	case QB_INTENSITY:
		data->qb_drag_tile = tile;
		qb_set_intensity(src, x, it, data->ui_scale);
		return;
	case QB_GRID:
		set_grid_visible(src, grid_keys_for(e->type), !grid_visible(src, grid_keys_for(e->type)));
		return;
	case QB_COLORS:
		if (e->type == TYPE_VECTORSCOPE)
			obs_data_set_int(d, "color_type", obs_data_get_int(s, "color_type") ? 0 : 1);
		else
			obs_data_set_bool(d, "colorize", !obs_data_get_bool(s, "colorize"));
		break;
	case QB_GAIN: {
		const long long g = obs_data_get_int(s, "gain");
		obs_data_set_int(d, "gain", g >= 5 ? 1 : g >= 2 ? 5 : 2);
		break;
	}
	case QB_ASPECT:
		pthread_mutex_lock(&data->mutex);
		e->aspect = (e->aspect + 1) % 5;
		pthread_mutex_unlock(&data->mutex);
		return;
	case QB_MAX:
		toggleSolo(data->solo >= 0 ? -1 : tile);
		return;
	}
	obs_source_update(src, d);
}

/* Arranging: tiles are moved by dragging and resized at their bottom right corner, in the free layout. */
void ScopeWidget::setArranging(bool on)
{
	pthread_mutex_lock(&data->mutex);
	data->arranging = on;
	data->drag_tile = -1;
	if (on) {
		data->layout = SCOPE_LAYOUT_FREE;
		data->solo = -1;
	}
	pthread_mutex_unlock(&data->mutex);
	if (btnArrange && btnArrange->isChecked() != on) {
		btnArrange->blockSignals(true);
		btnArrange->setChecked(on);
		btnArrange->blockSignals(false);
	}
	if (!on)
		unsetCursor();
}

void ScopeWidget::setCompare(bool on)
{
	comparing = on;
	call_compare(data->roi, on);
	for (scope_entry *e : data->entries)
		call_compare(e->src, on);
}

void ScopeWidget::setToolbarVisible(bool visible)
{
	toolbarVisible = visible;
	if (toolbar)
		toolbar->setVisible(visible);
}

void ScopeWidget::toggleSolo(int tile)
{
	if (tile < 0 || data->solo == tile)
		data->solo = -1;
	else
		data->solo = tile;
}

/* ---------- mouse ---------- */

#define INTERACT_KEEP_SOURCE (1 << 30)

static obs_source_t *get_source_from_mouse(struct scope_widget_s *data, int x, int y, struct obs_mouse_event *event)
{
	const int total = n_tiles(data);
	int i_mouse = -1;
	if (event->modifiers &
	    (INTERACT_MOUSE_LEFT | INTERACT_MOUSE_MIDDLE | INTERACT_MOUSE_RIGHT | INTERACT_KEEP_SOURCE)) {
		i_mouse = data->i_mouse_last;
	} else {
		// the last drawn tile is on top when tiles overlap (free layout)
		for (int t = total - 1; t >= 0; t--) {
			if (tile_rect(data, t)->is_inside(x, y)) {
				i_mouse = t;
				break;
			}
		}
	}

	if (i_mouse < 0 || total <= i_mouse) {
		data->i_mouse_last = -1;
		return NULL;
	}
	const src_rect_s *r = tile_rect(data, i_mouse);
	event->x = r->x_from_widget(x);
	event->y = r->y_from_widget(y);
	data->i_mouse_last = i_mouse;
	return tile_source(data, i_mouse);
}

static int tile_at(scope_widget_s *data, int x, int y)
{
	const int total = n_tiles(data);
	for (int t = total - 1; t >= 0; t--)
		if (tile_rect(data, t)->is_inside(x, y))
			return t;
	return -1;
}

static int TranslateQtKeyboardEventModifiers(QInputEvent *event, bool mouseEvent)
{
	int obsModifiers = INTERACT_NONE;

	if (event->modifiers().testFlag(Qt::ShiftModifier))
		obsModifiers |= INTERACT_SHIFT_KEY;
	if (event->modifiers().testFlag(Qt::AltModifier))
		obsModifiers |= INTERACT_ALT_KEY;
#ifdef __APPLE__
	// Mac: Meta = Control, Control = Command
	if (event->modifiers().testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_COMMAND_KEY;
	if (event->modifiers().testFlag(Qt::MetaModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#else
	// Handle windows key? Can a browser even trap that key?
	if (event->modifiers().testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#endif

	if (!mouseEvent) {
		if (event->modifiers().testFlag(Qt::KeypadModifier))
			obsModifiers |= INTERACT_IS_KEY_PAD;
	}

	return obsModifiers;
}

static int TranslateQtMouseEventModifiers(QMouseEvent *event)
{
	int modifiers = TranslateQtKeyboardEventModifiers(event, true);

	if (event->buttons().testFlag(Qt::LeftButton))
		modifiers |= INTERACT_MOUSE_LEFT;
	if (event->buttons().testFlag(Qt::MiddleButton))
		modifiers |= INTERACT_MOUSE_MIDDLE;
	if (event->buttons().testFlag(Qt::RightButton))
		modifiers |= INTERACT_MOUSE_RIGHT;

	return modifiers;
}

static inline QPointF event_pos(QWidget *w, QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return event->position() * w->devicePixelRatio();
#else
	UNUSED_PARAMETER(w);
	return event->localPos();
#endif
}

bool ScopeWidget::HandleMouseClickEvent(QMouseEvent *event)
{
	data->ui_scale = (float)devicePixelRatioF();
	bool mouseUp = event->type() == QEvent::MouseButtonRelease;
	int clickCount = 1;
	if (event->type() == QEvent::MouseButtonDblClick)
		clickCount = 2;

	const QPointF qtPos = event_pos(this, event);

	// macOS: Control-click is a right click (Qt reports the Control key as Meta there)
	const bool control_click = event->button() == Qt::LeftButton &&
#ifdef __APPLE__
				   event->modifiers().testFlag(Qt::MetaModifier);
#else
				   false;
#endif

	if (event->button() == Qt::RightButton || control_click) {
		// macOS opens context menus on press (the release may not reach this view), others on release
#ifdef __APPLE__
		const bool open_now = !mouseUp && event->type() != QEvent::MouseButtonDblClick;
#else
		const bool open_now = mouseUp;
#endif
		if (open_now)
			openContextAt(qtPos, QCursor::pos());
		return true;
	}

	if (data->arranging && data->layout == SCOPE_LAYOUT_FREE && event->button() == Qt::LeftButton) {
		pthread_mutex_lock(&data->mutex);
		if (mouseUp) {
			data->drag_tile = -1;
		} else if (clickCount == 1) {
			const int x = (int)qtPos.x(), y = (int)qtPos.y();
			data->drag_tile = -1;
			for (int t = n_tiles(data) - 1; t >= 0; t--) {
				const src_rect_s *r = tile_rect(data, t);
				if (!r->cell_inside(x, y))
					continue;
				data->drag_tile = t;
				data->drag_resize = x >= r->cx1 - 16 && y >= r->cy1 - 16;
				data->drag_x = x;
				data->drag_y = y;
				data->drag_start = *tile_free(data, t);
				break;
			}
		}
		pthread_mutex_unlock(&data->mutex);
		return true;
	}

	// quick controls at the bottom of the tile under the mouse
	if (event->button() == Qt::LeftButton && !data->arranging) {
		const int x = (int)qtPos.x(), y = (int)qtPos.y();
		if (mouseUp && data->qb_drag_tile >= 0) {
			data->qb_drag_tile = -1;
			return true;
		}
		const int tile = tile_at(data, x, y);
		int by0 = 0, by1 = 0;
		pthread_mutex_lock(&data->mutex);
		const std::vector<qb_item> items = qb_layout(data, tile, &by0, &by1);
		const qb_item *hit = nullptr;
		for (const qb_item &it : items)
			if (y >= by0 && y <= by1 && x >= it.x0 && x <= it.x1)
				hit = &it;
		scope_entry *e = hit ? data->entries[tile - 1] : nullptr;
		pthread_mutex_unlock(&data->mutex);
		if (hit) {
			if (!mouseUp && clickCount == 1)
				quickControl(tile, *hit, x);
			return true;
		}
		UNUSED_PARAMETER(e);
	}

	if (clickCount == 2 && event->button() == Qt::LeftButton) {
		int hit = tile_at(data, (int)qtPos.x(), (int)qtPos.y());
		// The ROI uses clicks to draw shapes, so it is maximized from the menu instead.
		if (hit > 0) {
			toggleSolo(hit);
			return true;
		}
	}

	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);

	int32_t button = 0;

	switch (event->button()) {
	case Qt::LeftButton:
		button = MOUSE_LEFT;
		if (mouseUp)
			mouseEvent.modifiers |= INTERACT_KEEP_SOURCE; // Not to change i_mouse if released outside
		break;
	case Qt::MiddleButton:
		button = MOUSE_MIDDLE;
		break;
	default:
		return false;
	}

	obs_source_t *src = get_source_from_mouse(data, qtPos.x(), qtPos.y(), &mouseEvent);
	mouseEvent.modifiers &= ~INTERACT_KEEP_SOURCE;

	if (src)
		obs_source_send_mouse_click(src, &mouseEvent, button, mouseUp, clickCount);

	return true;
}

bool ScopeWidget::HandleMouseMoveEvent(QMouseEvent *event)
{
	data->ui_scale = (float)devicePixelRatioF();
	struct obs_mouse_event mouseEvent = {};

	bool mouseLeave = event->type() == QEvent::Leave;

	if (!mouseLeave)
		mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);

	const QPointF qtPos = event_pos(this, event);

	if (data->arranging && data->layout == SCOPE_LAYOUT_FREE) {
		const int x = (int)qtPos.x(), y = (int)qtPos.y();
		pthread_mutex_lock(&data->mutex);
		const int t = data->drag_tile;
		free_rect_s *f = t >= 0 ? tile_free(data, t) : nullptr;
		if (f && !mouseLeave && data->last_cx > 0 && data->last_cy > 0) {
			// snap to 1 % of the dock
			auto snap = [](float v) { return std::round(v * 100.0f) / 100.0f; };
			const float dx = (float)(x - data->drag_x) / data->last_cx;
			const float dy = (float)(y - data->drag_y) / data->last_cy;
			const free_rect_s &s0 = data->drag_start;
			if (data->drag_resize) {
				f->w = std::clamp(snap(s0.w + dx), 0.05f, 1.0f - s0.x);
				f->h = std::clamp(snap(s0.h + dy), 0.05f, 1.0f - s0.y);
			} else {
				f->x = std::clamp(snap(s0.x + dx), 0.0f, 1.0f - s0.w);
				f->y = std::clamp(snap(s0.y + dy), 0.0f, 1.0f - s0.h);
			}
		}
		// cursor: resize over a handle, move over a tile
		Qt::CursorShape shape = Qt::ArrowCursor;
		if (t >= 0) {
			shape = data->drag_resize ? Qt::SizeFDiagCursor : Qt::SizeAllCursor;
		} else {
			for (int k = n_tiles(data) - 1; k >= 0; k--) {
				const src_rect_s *r = tile_rect(data, k);
				if (r->cell_inside(x, y)) {
					shape = x >= r->cx1 - 16 && y >= r->cy1 - 16 ? Qt::SizeFDiagCursor
										     : Qt::SizeAllCursor;
					break;
				}
			}
		}
		pthread_mutex_unlock(&data->mutex);
		setCursor(shape);
		return true;
	}

	{
		const int x = (int)qtPos.x(), y = (int)qtPos.y();
		const int prev = data->hover_tile;
		data->hover_tile = mouseLeave ? -1 : tile_at(data, x, y);
		if (data->qb_drag_tile >= 0 && !mouseLeave) {
			int by0 = 0, by1 = 0;
			pthread_mutex_lock(&data->mutex);
			const std::vector<qb_item> items = qb_layout(data, data->qb_drag_tile, &by0, &by1);
			obs_source_t *src = tile_source(data, data->qb_drag_tile);
			pthread_mutex_unlock(&data->mutex);
			for (const qb_item &it : items)
				if (it.kind == QB_INTENSITY && src)
					qb_set_intensity(src, x, it, data->ui_scale);
			return true;
		}
		if (prev != data->hover_tile)
			update();
	}

	mouseEvent.modifiers |= INTERACT_KEEP_SOURCE;
	obs_source_t *src0 = get_source_from_mouse(data, qtPos.x(), qtPos.y(), &mouseEvent);
	mouseEvent.modifiers &= ~INTERACT_KEEP_SOURCE;

	struct obs_mouse_event mouseEvent1 = mouseEvent;
	obs_source_t *src1 = get_source_from_mouse(data, qtPos.x(), qtPos.y(), &mouseEvent1);

	if (src0 && src0 != src1)
		obs_source_send_mouse_move(src0, &mouseEvent, true);

	if (src1)
		obs_source_send_mouse_move(src1, &mouseEvent1, mouseLeave);

	return true;
}

bool ScopeWidget::HandleMouseWheelEvent(QWheelEvent *event)
{
	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtKeyboardEventModifiers(event, true);

	int xDelta = 0;
	int yDelta = 0;

	const QPoint angleDelta = event->angleDelta();
	if (!event->pixelDelta().isNull()) {
		if (angleDelta.x())
			xDelta = event->pixelDelta().x();
		else
			yDelta = event->pixelDelta().y();
	} else {
		if (angleDelta.x())
			xDelta = angleDelta.x();
		else
			yDelta = angleDelta.y();
	}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QPointF qtPos = event->position() * devicePixelRatio();
	const int x = qtPos.x();
	const int y = qtPos.y();
#elif QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
	const QPointF position = event->position();
	const int x = position.x();
	const int y = position.y();
#else
	const int x = event->x();
	const int y = event->y();
#endif

	obs_source_t *src = get_source_from_mouse(data, x, y, &mouseEvent);
	if (src)
		obs_source_send_mouse_wheel(src, &mouseEvent, xDelta, yDelta);

	return true;
}

/* Right click, Control-click and two-finger taps can each arrive as a mouse button and as a
 * context menu request; whichever comes first opens the menu and the other is ignored. */
void ScopeWidget::openContextAt(const QPointF &widgetPos, const QPoint &globalPos)
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (now - lastContextMs < 500)
		return;
	lastContextMs = now;

	data->i_menu = tile_at(data, (int)widgetPos.x(), (int)widgetPos.y());
	if (data->i_menu >= 0 && tile_source(data, data->i_menu))
		openOptions(data->i_menu, globalPos);
	else
		openMenu(nullptr);
	lastContextMs = QDateTime::currentMSecsSinceEpoch();
}

bool ScopeWidget::HandleContextMenuEvent(QContextMenuEvent *event)
{
	openContextAt(QPointF(event->pos()) * devicePixelRatio(), event->globalPos());
	event->accept();
	return true;
}

bool ScopeWidget::HandleKeyEvent(QKeyEvent *)
{
	// Implement if necessary
	return true;
}

/* ---------- quick settings in the context menu ---------- */

/* The dock always feeds its scopes from the ROI; these are handled elsewhere. */
static bool skip_quick_property(const char *name)
{
	return !strcmp(name, "target_name") || !strcmp(name, "target_scale") || !strcmp(name, "bypass") ||
	       !strncmp(name, "graticule", 9);
}

/* Adds a submenu per list setting and a check item per on/off setting, read from the scope itself. */
static void add_quick_properties(QWidget *owner, QMenu *menu, obs_source_t *src, obs_properties_t *props,
				 obs_data_t *settings)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		if (!obs_property_visible(p) || !obs_property_enabled(p))
			continue;
		const char *name = obs_property_name(p);
		const QString label = QString::fromUtf8(obs_property_description(p));
		if (!name || skip_quick_property(name))
			continue;

		switch (obs_property_get_type(p)) {
		case OBS_PROPERTY_BOOL: {
			QAction *act = menu->addAction(label);
			act->setCheckable(true);
			act->setChecked(obs_data_get_bool(settings, name));
			std::string key = name;
			QObject::connect(act, &QAction::toggled, owner, [src, key](bool checked) {
				OBSDataAutoRelease d = obs_data_create();
				obs_data_set_bool(d, key.c_str(), checked);
				obs_source_update(src, d);
			});
			break;
		}
		case OBS_PROPERTY_LIST: {
			const obs_combo_format fmt = obs_property_list_format(p);
			if (fmt != OBS_COMBO_FORMAT_INT && fmt != OBS_COMBO_FORMAT_FLOAT)
				break;
			const size_t n = obs_property_list_item_count(p);
			if (n < 2)
				break;
			QMenu *sub = menu->addMenu(label);
			std::string key = name;
			for (size_t i = 0; i < n; i++) {
				if (obs_property_list_item_disabled(p, i))
					continue;
				QAction *act = sub->addAction(QString::fromUtf8(obs_property_list_item_name(p, i)));
				act->setCheckable(true);
				if (fmt == OBS_COMBO_FORMAT_INT) {
					long long v = obs_property_list_item_int(p, i);
					act->setChecked(obs_data_get_int(settings, name) == v);
					QObject::connect(act, &QAction::triggered, owner, [src, key, v]() {
						OBSDataAutoRelease d = obs_data_create();
						obs_data_set_int(d, key.c_str(), v);
						obs_source_update(src, d);
					});
				} else {
					double v = obs_property_list_item_float(p, i);
					act->setChecked(obs_data_get_double(settings, name) == v);
					QObject::connect(act, &QAction::triggered, owner, [src, key, v]() {
						OBSDataAutoRelease d = obs_data_create();
						obs_data_set_double(d, key.c_str(), v);
						obs_source_update(src, d);
					});
				}
			}
			break;
		}
		case OBS_PROPERTY_GROUP: {
			obs_properties_t *content = obs_property_group_content(p);
			if (obs_property_group_type(p) == OBS_GROUP_CHECKABLE) {
				QAction *act = menu->addAction(label);
				act->setCheckable(true);
				act->setChecked(obs_data_get_bool(settings, name));
				std::string key = name;
				QObject::connect(act, &QAction::toggled, owner, [src, key](bool checked) {
					OBSDataAutoRelease d = obs_data_create();
					obs_data_set_bool(d, key.c_str(), checked);
					obs_source_update(src, d);
				});
			}
			if (content)
				add_quick_properties(owner, menu, src, content, settings);
			break;
		}
		default:
			break;
		}
	}
}

/* ---------- context menu ---------- */

bool ScopeWidget::openMenu(QMouseEvent *)
{
	QMenu popup(this);
	QAction *act;
	const int tile = data->i_menu;
	const bool on_tile = tile >= 0 && tile_source(data, tile);

	if (on_tile) {
		obs_source_t *src = tile_source(data, tile);

		// Quick settings of this scope, straight from its own properties.
		const int type = tile > 0 ? data->entries[tile - 1]->type : -1;
		if (const grid_key_s *keys = grid_keys_for(type)) {
			act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.grid")));
			act->setCheckable(true);
			act->setChecked(grid_visible(src, keys));
			connect(act, &QAction::toggled, this,
				[src, keys](bool checked) { set_grid_visible(src, keys, checked); });
		}
		{
			obs_properties_t *props = obs_source_properties(src);
			OBSDataAutoRelease settings = obs_source_get_settings(src);
			if (props) {
				add_quick_properties(this, &popup, src, props, settings);
				obs_properties_destroy(props);
			}
		}
		popup.addSeparator();

		act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.settings")));
		connect(act, &QAction::triggered, this, [this, tile]() { openProperties(tile); });

		if (data->solo >= 0) {
			act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.solo.exit")));
			connect(act, &QAction::triggered, this, [this]() { toggleSolo(-1); });
		} else {
			act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.solo")));
			connect(act, &QAction::triggered, this, [this, tile]() { toggleSolo(tile); });
		}

		act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.projector")));
		connect(act, &QAction::triggered, this, [this, tile]() {
			if (obs_source_t *s = tile_source(data, tile))
				obs_frontend_open_projector("Source", -1, "", obs_source_get_name(s));
		});

		if (tile > 0) {
			act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.move.earlier")));
			act->setEnabled(tile > 1);
			connect(act, &QAction::triggered, this, [this, tile]() { moveScope(tile, -1); });
			act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.move.later")));
			act->setEnabled(tile < (int)data->entries.size());
			connect(act, &QAction::triggered, this, [this, tile]() { moveScope(tile, +1); });

			act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.remove")));
			connect(
				act, &QAction::triggered, this, [this, tile]() { removeScope(tile); },
				Qt::QueuedConnection);
		}

		popup.addSeparator();
	} else if (data->solo >= 0) {
		act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.solo.exit")));
		connect(act, &QAction::triggered, this, [this]() { toggleSolo(-1); });
		popup.addSeparator();
	}

	QMenu *addMenu = popup.addMenu(QString::fromUtf8(obs_module_text("dock.menu.add")));
	fillAddScopeMenu(addMenu);

	act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.show.roi")));
	act->setCheckable(true);
	act->setChecked(data->roi_shown);
	connect(act, &QAction::toggled, this, [this](bool checked) {
		data->roi_shown = checked;
		if (!checked && data->solo == 0)
			data->solo = -1;
	});

	QMenu *layoutMenu = popup.addMenu(QString::fromUtf8(obs_module_text("dock.menu.layout")));
	const char *layout_text[] = {
		obs_module_text("dock.menu.layout.auto"),
		obs_module_text("dock.menu.layout.vertical"),
		obs_module_text("dock.menu.layout.horizontal"),
		obs_module_text("dock.menu.layout.free"),
	};
	for (int l = 0; l < 4; l++) {
		act = layoutMenu->addAction(QString::fromUtf8(layout_text[l]));
		act->setCheckable(true);
		act->setChecked(data->layout == l);
		connect(act, &QAction::triggered, this, [this, l]() {
			data->layout = l;
			if (l != SCOPE_LAYOUT_FREE)
				setArranging(false);
		});
	}

	act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.export")));
	connect(act, &QAction::triggered, this, &ScopeWidget::exportScreenshot, Qt::QueuedConnection);

	act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.toolbar")));
	act->setCheckable(true);
	act->setChecked(toolbarVisible);
	connect(act, &QAction::toggled, this, &ScopeWidget::setToolbarVisible);

	popup.addSeparator();

	act = popup.addAction(QString::fromUtf8(obs_module_text("dock.menu.close")));
	connect(act, &QAction::triggered, this, &ScopeWidget::RemoveDock, Qt::QueuedConnection);

	popup.exec(QCursor::pos());
	return true;
}

void ScopeWidget::closeProperties()
{
	if (properties) {
		auto *p = properties;
		properties = NULL;
		p->close();
	}
}

void ScopeWidget::openProperties(int tile)
{
	obs_source_t *s = tile_source(data, tile);
	if (!s)
		return;

	closeProperties();

	QString title;
	if (tile == 0)
		title = QString::fromUtf8(obs_module_text("ROI"));
	else
		title = QString::fromUtf8(obs_module_text(scope_types[data->entries[tile - 1]->type].name_key));

	properties = new ScopeWidgetProperties(this, s, title, tile == 0);
	properties->setAttribute(Qt::WA_DeleteOnClose, true);
	properties->Init();
}

/* ---------- options panel ---------- */

static const char *options_style = R"(
QFrame#ScopeOptions {
	background: #1c1c1f;
	border: 1px solid #3a3a40;
	border-radius: 10px;
}
QFrame#ScopeOptions QLabel { color: #d8d8dc; background: transparent; }
QFrame#ScopeOptions QLabel#Title { color: #f4f4f6; font-size: 13px; font-weight: 600; }
QFrame#ScopeOptions QLabel[section="true"] { color: #8d8d96; font-size: 10px; font-weight: 600; }
QFrame#ScopeOptions QComboBox, QFrame#ScopeOptions QLineEdit {
	background: #2a2a2f; color: #e8e8ec; border: 1px solid #3c3c43; border-radius: 6px;
	padding: 3px 8px; min-height: 20px;
}
QFrame#ScopeOptions QComboBox:hover, QFrame#ScopeOptions QLineEdit:focus { border-color: #6fcf86; }
QFrame#ScopeOptions QComboBox QAbstractItemView {
	background: #2a2a2f; color: #e8e8ec; selection-background-color: #3d5c45; border: 1px solid #3c3c43;
}
QFrame#ScopeOptions QPushButton {
	background: #2a2a2f; color: #e8e8ec; border: 1px solid #3c3c43; border-radius: 6px; padding: 4px 10px;
}
QFrame#ScopeOptions QPushButton:hover { background: #34343a; }
QFrame#ScopeOptions QPushButton#Segment { border-radius: 0px; }
QFrame#ScopeOptions QPushButton#SegmentLeft { border-top-right-radius: 0px; border-bottom-right-radius: 0px; }
QFrame#ScopeOptions QPushButton#SegmentRight { border-top-left-radius: 0px; border-bottom-left-radius: 0px; }
QFrame#ScopeOptions QPushButton#Delete { background: transparent; border: none; color: #e5655d; }
QFrame#ScopeOptions QPushButton#Delete:hover { color: #ff8078; }
QFrame#ScopeOptions QPushButton#Primary { background: #3b3b42; border-color: #4a4a52; }
QFrame#ScopeOptions QPushButton#Flat { background: transparent; border: none; color: #b8b8c0; padding: 2px 6px; }
QFrame#ScopeOptions QPushButton#Flat:hover { color: #ffffff; }
QFrame#ScopeOptions QCheckBox { color: #c9c9cf; background: transparent; }
QFrame#ScopeOptions QSlider::groove:horizontal { height: 4px; background: #3a3a40; border-radius: 2px; }
QFrame#ScopeOptions QSlider::sub-page:horizontal { background: #6fcf86; border-radius: 2px; }
QFrame#ScopeOptions QSlider::handle:horizontal {
	background: #f0f0f2; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px;
}
QFrame#ScopeOptions QFrame#Divider { background: #303036; max-height: 1px; min-height: 1px; border: none; }
QFrame#ScopeOptions QDoubleSpinBox {
	background: #2a2a2f; color: #e8e8ec; border: 1px solid #3c3c43; border-radius: 6px;
	padding: 2px 4px; min-height: 20px;
}
QFrame#ScopeOptions QDoubleSpinBox:focus { border-color: #6fcf86; }
QFrame#ScopeOptions QFrame#MarkerCard { background: #232327; border: 1px solid #333339; border-radius: 8px; }
QFrame#ScopeOptions QPushButton#Swatch { border: 1px solid #55555c; border-radius: 5px; padding: 0px; }
)";

static QLabel *section_label(QWidget *parent, const QString &text)
{
	auto *l = new QLabel(text.toUpper(), parent);
	l->setProperty("section", true);
	return l;
}

static QFrame *divider(QWidget *parent)
{
	auto *f = new QFrame(parent);
	f->setObjectName("Divider");
	return f;
}

static void update_one(obs_source_t *src, const std::function<void(obs_data_t *)> &set)
{
	OBSDataAutoRelease d = obs_data_create();
	set(d);
	obs_source_update(src, d);
}

/* Names of the visible properties, to notice when a change shows or hides other options. */
static std::string visible_signature(obs_properties_t *props)
{
	std::string sig;
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		if (!obs_property_visible(p))
			continue;
		sig += obs_property_name(p);
		sig += ';';
		if (obs_property_get_type(p) == OBS_PROPERTY_GROUP)
			if (obs_properties_t *c = obs_property_group_content(p))
				sig += visible_signature(c);
	}
	return sig;
}

/* Lays out a scope's own options: lists as dropdowns two to a row, sliders full width,
 * on/off options as check boxes. on_layout_change runs when a change shows or hides options. */
static void add_property_controls(QWidget *panel, QVBoxLayout *vbox, obs_source_t *src, obs_properties_t *props,
				  obs_data_t *settings, const std::function<void()> &on_layout_change)
{
	QGridLayout *grid = nullptr;
	int col = 0, row = 0;
	auto flush_grid = [&]() {
		grid = nullptr;
		col = row = 0;
	};
	auto check_layout = [src, on_layout_change](const std::string &before) {
		obs_properties_t *after = obs_source_properties(src);
		bool changed = after && visible_signature(after) != before;
		obs_properties_destroy(after);
		if (changed)
			on_layout_change();
	};
	const std::string sig = visible_signature(props);

	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		if (!obs_property_visible(p))
			continue;
		const char *name = obs_property_name(p);
		if (!name || !strcmp(name, "target_name") || !strcmp(name, "target_scale") || !strcmp(name, "bypass"))
			continue;
		const std::string key = name;
		const QString desc = QString::fromUtf8(obs_property_description(p));
		const bool enabled = obs_property_enabled(p);

		switch (obs_property_get_type(p)) {
		case OBS_PROPERTY_LIST: {
			const obs_combo_format fmt = obs_property_list_format(p);
			if (fmt != OBS_COMBO_FORMAT_INT && fmt != OBS_COMBO_FORMAT_FLOAT &&
			    fmt != OBS_COMBO_FORMAT_STRING)
				break;
			auto *combo = new QComboBox(panel);
			combo->setEnabled(enabled);
			const size_t n = obs_property_list_item_count(p);
			int current = -1;
			for (size_t i = 0; i < n; i++) {
				const QString item = QString::fromUtf8(obs_property_list_item_name(p, i));
				if (fmt == OBS_COMBO_FORMAT_STRING) {
					const char *v = obs_property_list_item_string(p, i);
					combo->addItem(item, QString::fromUtf8(v ? v : ""));
					if (v && strcmp(obs_data_get_string(settings, name), v) == 0)
						current = (int)i;
				} else if (fmt == OBS_COMBO_FORMAT_INT) {
					long long v = obs_property_list_item_int(p, i);
					combo->addItem(item, QVariant(v));
					if (obs_data_get_int(settings, name) == v)
						current = (int)i;
				} else {
					double v = obs_property_list_item_float(p, i);
					combo->addItem(item, QVariant(v));
					if (obs_data_get_double(settings, name) == v)
						current = (int)i;
				}
			}
			combo->setCurrentIndex(current);
			QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), panel,
					 [combo, src, key, fmt, sig, check_layout](int idx) {
						 const QVariant v = combo->itemData(idx);
						 update_one(src, [&](obs_data_t *d) {
							 if (fmt == OBS_COMBO_FORMAT_STRING)
								 obs_data_set_string(d, key.c_str(),
										     v.toString().toUtf8().constData());
							 else if (fmt == OBS_COMBO_FORMAT_INT)
								 obs_data_set_int(d, key.c_str(), v.toLongLong());
							 else
								 obs_data_set_double(d, key.c_str(), v.toDouble());
						 });
						 check_layout(sig);
					 });
			if (!grid) {
				grid = new QGridLayout();
				grid->setHorizontalSpacing(8);
				grid->setVerticalSpacing(3);
				vbox->addLayout(grid);
			}
			grid->addWidget(section_label(panel, desc), row * 2, col);
			grid->addWidget(combo, row * 2 + 1, col);
			if (++col == 2) {
				col = 0;
				row++;
			}
			break;
		}
		case OBS_PROPERTY_INT:
		case OBS_PROPERTY_FLOAT: {
			flush_grid();
			const bool is_int = obs_property_get_type(p) == OBS_PROPERTY_INT;
			double lo = is_int ? obs_property_int_min(p) : obs_property_float_min(p);
			double hi = is_int ? obs_property_int_max(p) : obs_property_float_max(p);
			double step = is_int ? obs_property_int_step(p) : obs_property_float_step(p);
			if (step <= 0 || hi <= lo || (hi - lo) / step > 100000)
				break;
			const double cur = is_int ? (double)obs_data_get_int(settings, name)
						  : obs_data_get_double(settings, name);
			auto *head = new QHBoxLayout();
			head->addWidget(section_label(panel, desc));
			head->addStretch(1);
			auto *value = new QLabel(QString::number(cur), panel);
			head->addWidget(value);
			vbox->addLayout(head);
			auto *slider = new QSlider(Qt::Horizontal, panel);
			slider->setEnabled(enabled);
			slider->setRange(0, (int)std::lround((hi - lo) / step));
			slider->setValue((int)std::lround((cur - lo) / step));
			QObject::connect(
				slider, &QSlider::valueChanged, panel, [src, key, is_int, lo, step, value](int pos) {
					const double v = lo + pos * step;
					value->setText(QString::number(v));
					update_one(src, [&](obs_data_t *d) {
						if (is_int)
							obs_data_set_int(d, key.c_str(), (long long)std::lround(v));
						else
							obs_data_set_double(d, key.c_str(), v);
					});
				});
			vbox->addWidget(slider);
			break;
		}
		case OBS_PROPERTY_BOOL: {
			flush_grid();
			auto *cb = new QCheckBox(desc, panel);
			cb->setEnabled(enabled);
			cb->setChecked(obs_data_get_bool(settings, name));
			QObject::connect(cb, &QCheckBox::toggled, panel, [src, key, sig, check_layout](bool on) {
				update_one(src, [&](obs_data_t *d) { obs_data_set_bool(d, key.c_str(), on); });
				check_layout(sig);
			});
			vbox->addWidget(cb);
			break;
		}
		case OBS_PROPERTY_GROUP: {
			flush_grid();
			if (obs_property_group_type(p) == OBS_GROUP_CHECKABLE) {
				auto *cb = new QCheckBox(desc, panel);
				cb->setChecked(obs_data_get_bool(settings, name));
				QObject::connect(cb, &QCheckBox::toggled, panel, [src, key](bool on) {
					update_one(src, [&](obs_data_t *d) { obs_data_set_bool(d, key.c_str(), on); });
				});
				vbox->addWidget(cb);
			} else {
				vbox->addWidget(section_label(panel, desc));
			}
			if (obs_properties_t *content = obs_property_group_content(p))
				add_property_controls(panel, vbox, src, content, settings, on_layout_change);
			break;
		}
		case OBS_PROPERTY_TEXT:
		case OBS_PROPERTY_PATH: {
			if (obs_property_get_type(p) == OBS_PROPERTY_TEXT &&
			    obs_property_text_type(p) != OBS_TEXT_DEFAULT)
				break;
			flush_grid();
			vbox->addWidget(section_label(panel, desc));
			auto *edit = new QLineEdit(QString::fromUtf8(obs_data_get_string(settings, name)), panel);
			edit->setEnabled(enabled);
			QObject::connect(edit, &QLineEdit::editingFinished, panel, [src, key, edit]() {
				const std::string v = edit->text().toStdString();
				update_one(src, [&](obs_data_t *d) { obs_data_set_string(d, key.c_str(), v.c_str()); });
			});
			if (obs_property_get_type(p) != OBS_PROPERTY_PATH) {
				vbox->addWidget(edit);
				break;
			}
			// a file dialog cannot open inside the popup panel: close it, then ask
			auto *row = new QHBoxLayout();
			row->addWidget(edit, 1);
			auto *browse = new QPushButton(QString::fromUtf8(obs_module_text("Options.Browse")), panel);
			const bool save = obs_property_path_type(p) == OBS_PATH_FILE_SAVE;
			const QString filter = QString::fromUtf8(obs_property_path_filter(p));
			QObject::connect(
				browse, &QPushButton::clicked, panel, [panel, src, key, desc, filter, save, edit]() {
					const QString start = edit->text();
					panel->window()->close();
					OBSWeakSourceAutoRelease weak = obs_source_get_weak_source(src);
					QTimer::singleShot(
						0, qApp,
						[w = OBSWeakSource(weak.Get()), key, desc, filter, save, start]() {
							const QString file =
								save ? QFileDialog::getSaveFileName(nullptr, desc,
												    start, filter)
								     : QFileDialog::getOpenFileName(nullptr, desc,
												    start, filter);
							OBSSourceAutoRelease s = obs_weak_source_get_source(w);
							if (file.isEmpty() || !s)
								return;
							const std::string v = file.toStdString();
							update_one(s, [&](obs_data_t *d) {
								obs_data_set_string(d, key.c_str(), v.c_str());
							});
						});
				});
			row->addWidget(browse);
			vbox->addLayout(row);
			break;
		}
		case OBS_PROPERTY_BUTTON: {
			flush_grid();
			auto *btn = new QPushButton(desc, panel);
			btn->setEnabled(enabled);
			QObject::connect(btn, &QPushButton::clicked, panel, [src, key]() {
				// the panel's properties are gone by now; look the button up again
				obs_properties_t *pp = obs_source_properties(src);
				if (obs_property_t *bp = pp ? obs_properties_get(pp, key.c_str()) : nullptr)
					obs_property_button_clicked(bp, src);
				obs_properties_destroy(pp);
			});
			vbox->addWidget(btn);
			break;
		}
		default:
			break;
		}
	}
}

#ifdef __APPLE__
/* On macOS a dropdown cannot open inside a Qt::Popup window, so the options panel is a
 * frameless tool window there. This closes it when another window takes over or on Escape,
 * but not while one of its own dropdown lists is open. */
class OptionsPanelCloser : public QObject {
	QWidget *panel;

public:
	explicit OptionsPanelCloser(QWidget *panel_) : QObject(panel_), panel(panel_) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		if (obj == panel && event->type() == QEvent::KeyPress &&
		    static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
			panel->close();
			return true;
		}
		if (obj == panel && event->type() == QEvent::WindowDeactivate) {
			QPointer<QWidget> p = panel;
			QTimer::singleShot(150, panel, [p]() {
				if (!p || QApplication::activePopupWidget())
					return;
				QWidget *active = QApplication::activeWindow();
				if (active == p || (active && p->isAncestorOf(active)))
					return;
				p->close();
			});
		}
		return false;
	}
};
#endif

/* ---------- markers ---------- */

// mirrors struct cm_marker (markers.h); percentages and degrees as shown in the panel
struct marker_ui
{
	std::string name;
	int kind = 0;        // 0 level (waveform), 1 point, 2 line (vectorscope)
	double level = 75.0; // %
	double hue = 123.0;  // degrees, counter-clockwise from +U
	double sat = 40.0;   // % of the outer ring
	uint32_t color = 0xFFFFD040;
	int style = 0;      // solid, dashed, dotted
	int shape = 0;      // circle, cross, square, dot
	double width = 1.5; // px
	bool show_label = true;
};

static const uint32_t marker_palette[] = {0xFFFFD040, 0xFF40E0F0, 0xFFFF5A5A, 0xFF6FCF86, 0xFFFF9F40,
					  0xFFB070FF, 0xFFFFFFFF, 0xFFFF70C0, 0xFF4090FF, 0xFFA0A0A8};

static std::vector<marker_ui> load_markers(obs_source_t *src)
{
	std::vector<marker_ui> v;
	OBSDataAutoRelease settings = obs_source_get_settings(src);
	OBSDataArrayAutoRelease array = obs_data_get_array(settings, "markers");
	const size_t n = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < n; i++) {
		OBSDataAutoRelease d = obs_data_array_item(array, i);
		marker_ui m;
		m.name = obs_data_get_string(d, "name");
		m.kind = (int)obs_data_get_int(d, "kind");
		m.level = obs_data_get_double(d, "level");
		m.hue = obs_data_get_double(d, "hue");
		m.sat = obs_data_get_double(d, "sat");
		if (obs_data_has_user_value(d, "color"))
			m.color = (uint32_t)obs_data_get_int(d, "color");
		m.style = (int)obs_data_get_int(d, "style");
		m.shape = (int)obs_data_get_int(d, "shape");
		if (obs_data_has_user_value(d, "width"))
			m.width = obs_data_get_double(d, "width");
		if (obs_data_has_user_value(d, "show_label"))
			m.show_label = obs_data_get_bool(d, "show_label");
		v.push_back(m);
	}
	return v;
}

static void save_markers(obs_source_t *src, const std::vector<marker_ui> &v)
{
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const marker_ui &m : v) {
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_string(d, "name", m.name.c_str());
		obs_data_set_int(d, "kind", m.kind);
		obs_data_set_double(d, "level", m.level);
		obs_data_set_double(d, "hue", m.hue);
		obs_data_set_double(d, "sat", m.sat);
		obs_data_set_int(d, "color", (long long)m.color);
		obs_data_set_int(d, "style", m.style);
		obs_data_set_int(d, "shape", m.shape);
		obs_data_set_double(d, "width", m.width);
		obs_data_set_bool(d, "show_label", m.show_label);
		obs_data_array_push_back(array, d);
	}
	update_one(src, [&](obs_data_t *d) { obs_data_set_array(d, "markers", array); });
}

static void set_swatch_color(QPushButton *b, uint32_t argb)
{
	const QColor c = QColor::fromRgba(argb);
	b->setStyleSheet(QStringLiteral("QPushButton#Swatch { background: %1; }").arg(c.name()));
}

/* Vectorscope angle (degrees) and distance (% of the outer ring) of an R'G'B' color. */
static void color_to_hue_sat(int colorspace, int r, int g, int b, double *hue, double *sat)
{
	const double kr = colorspace == 1 ? 0.299 : colorspace >= 3 ? 0.2627 : 0.2126;
	const double kb = colorspace == 1 ? 0.114 : colorspace >= 3 ? 0.0593 : 0.0722;
	const double R = r / 255.0, G = g / 255.0, B = b / 255.0;
	const double y = kr * R + (1.0 - kr - kb) * G + kb * B;
	const double u = 112.0 * (B - y) / (1.0 - kb);
	const double v = 112.0 * (R - y) / (1.0 - kr);
	double a = std::atan2(v, u) * 180.0 / M_PI;
	if (a < 0.0)
		a += 360.0;
	*hue = a;
	*sat = std::hypot(u, v) / 127.0 * 100.0;
}

static QDoubleSpinBox *marker_spin(QWidget *parent, double lo, double hi, double step, double value, const char *suffix,
				   int decimals = 1)
{
	auto *spin = new QDoubleSpinBox(parent);
	spin->setRange(lo, hi);
	spin->setSingleStep(step);
	spin->setDecimals(decimals);
	spin->setSuffix(QString::fromUtf8(suffix));
	spin->setValue(value);
	spin->setKeyboardTracking(false);
	return spin;
}

/* The MARKERS section of the options panel: a card per marker and an Add button. */
void ScopeWidget::addMarkerControls(QWidget *panel, QVBoxLayout *vbox, obs_source_t *src, bool vectorscope,
				    const std::function<void()> &reopen)
{
	auto markers = std::make_shared<std::vector<marker_ui>>(load_markers(src));
	auto save = [markers, src]() { save_markers(src, *markers); };

	vbox->addWidget(section_label(panel, QString::fromUtf8(obs_module_text("Markers"))));

	for (size_t i = 0; i < markers->size(); i++) {
		marker_ui &m = (*markers)[i];
		auto *card = new QFrame(panel);
		card->setObjectName("MarkerCard");
		auto *cv = new QVBoxLayout(card);
		cv->setContentsMargins(8, 6, 8, 6);
		cv->setSpacing(4);

		// color, name, delete
		{
			auto *row = new QHBoxLayout();
			auto *swatch = new QPushButton(card);
			swatch->setObjectName("Swatch");
			swatch->setFixedSize(22, 22);
			swatch->setToolTip(QString::fromUtf8(obs_module_text("Markers.Color")));
			set_swatch_color(swatch, m.color);
			connect(swatch, &QPushButton::clicked, this, [this, swatch, markers, i, save]() {
				QMenu menu(swatch);
				for (uint32_t c : marker_palette) {
					QPixmap pm(14, 14);
					pm.fill(QColor::fromRgba(c));
					QAction *a = menu.addAction(QIcon(pm), QColor::fromRgba(c).name().toUpper());
					connect(a, &QAction::triggered, this, [swatch, markers, i, save, c]() {
						(*markers)[i].color = c;
						set_swatch_color(swatch, c);
						save();
					});
				}
				menu.addSeparator();
				auto *hex =
					new QLineEdit(QColor::fromRgba((*markers)[i].color).name().toUpper(), &menu);
				hex->setPlaceholderText(QStringLiteral("#RRGGBB"));
				auto *wa = new QWidgetAction(&menu);
				wa->setDefaultWidget(hex);
				menu.addAction(wa);
				connect(hex, &QLineEdit::returnPressed, &menu,
					[&menu, hex, swatch, markers, i, save]() {
						const QColor c(hex->text().trimmed());
						if (c.isValid()) {
							(*markers)[i].color = 0xFF000000 | (c.rgb() & 0xFFFFFF);
							set_swatch_color(swatch, (*markers)[i].color);
							save();
						}
						menu.close();
					});
				menu.exec(swatch->mapToGlobal(QPoint(0, swatch->height())));
			});
			row->addWidget(swatch);

			auto *name = new QLineEdit(QString::fromStdString(m.name), card);
			name->setPlaceholderText(QString::fromUtf8(obs_module_text("Markers.Name")));
			connect(name, &QLineEdit::textEdited, this, [markers, i, save](const QString &t) {
				(*markers)[i].name = t.toStdString();
				save();
			});
			row->addWidget(name, 1);

			auto *del = new QPushButton(QStringLiteral("✕"), card);
			del->setObjectName("Flat");
			del->setToolTip(QString::fromUtf8(obs_module_text("Markers.Delete")));
			connect(del, &QPushButton::clicked, this, [markers, i, save, reopen]() {
				markers->erase(markers->begin() + (long)i);
				save();
				reopen();
			});
			row->addWidget(del);
			cv->addLayout(row);
		}

		auto add_style_combo = [&](QHBoxLayout *row) {
			auto *style = new QComboBox(card);
			style->addItem(QString::fromUtf8(obs_module_text("Markers.Solid")));
			style->addItem(QString::fromUtf8(obs_module_text("Markers.Dashed")));
			style->addItem(QString::fromUtf8(obs_module_text("Markers.Dotted")));
			style->setCurrentIndex(std::clamp(m.style, 0, 2));
			connect(style, QOverload<int>::of(&QComboBox::activated), this, [markers, i, save](int v) {
				(*markers)[i].style = v;
				save();
			});
			row->addWidget(style, 1);
		};
		auto add_width_spin = [&](QHBoxLayout *row) {
			auto *w = marker_spin(card, 0.5, 8.0, 0.5, m.width, " px");
			w->setToolTip(QString::fromUtf8(obs_module_text("Markers.Width")));
			connect(w, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
				[markers, i, save](double v) {
					(*markers)[i].width = v;
					save();
				});
			row->addWidget(w);
		};

		if (!vectorscope) {
			auto *row = new QHBoxLayout();
			auto *level = marker_spin(card, -7.0, 109.0, 0.5, m.level, " %");
			level->setToolTip(QString::fromUtf8(obs_module_text("Markers.Level")));
			connect(level, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
				[markers, i, save](double v) {
					(*markers)[i].level = v;
					save();
				});
			row->addWidget(level);
			add_style_combo(row);
			add_width_spin(row);
			cv->addLayout(row);
		} else {
			auto *row = new QHBoxLayout();
			auto *kind = new QComboBox(card);
			kind->addItem(QString::fromUtf8(obs_module_text("Markers.Point")), 1);
			kind->addItem(QString::fromUtf8(obs_module_text("Markers.Line")), 2);
			kind->setCurrentIndex(m.kind == 2 ? 1 : 0);
			connect(kind, QOverload<int>::of(&QComboBox::activated), this,
				[markers, i, save, reopen](int v) {
					(*markers)[i].kind = v == 1 ? 2 : 1;
					save();
					reopen();
				});
			row->addWidget(kind, 1);
			auto *hue = marker_spin(card, 0.0, 359.9, 1.0, m.hue, "°");
			hue->setWrapping(true);
			hue->setToolTip(QString::fromUtf8(obs_module_text("Markers.Angle")));
			connect(hue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
				[markers, i, save](double v) {
					(*markers)[i].hue = v;
					save();
				});
			row->addWidget(hue);
			QDoubleSpinBox *sat = nullptr;
			if (m.kind != 2) {
				sat = marker_spin(card, 0.0, 120.0, 1.0, m.sat, " %");
				sat->setToolTip(QString::fromUtf8(obs_module_text("Markers.Saturation")));
				connect(sat, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
					[markers, i, save](double v) {
						(*markers)[i].sat = v;
						save();
					});
				row->addWidget(sat);
			}
			cv->addLayout(row);

			auto *row2 = new QHBoxLayout();
			if (m.kind == 2) {
				add_style_combo(row2);
			} else {
				auto *shape = new QComboBox(card);
				shape->addItem(QString::fromUtf8(obs_module_text("Markers.Circle")));
				shape->addItem(QString::fromUtf8(obs_module_text("Markers.Cross")));
				shape->addItem(QString::fromUtf8(obs_module_text("Markers.Square")));
				shape->addItem(QString::fromUtf8(obs_module_text("Markers.Dot")));
				shape->setCurrentIndex(std::clamp(m.shape, 0, 3));
				connect(shape, QOverload<int>::of(&QComboBox::activated), this,
					[markers, i, save](int v) {
						(*markers)[i].shape = v;
						save();
					});
				row2->addWidget(shape, 1);
			}
			add_width_spin(row2);

			// put the marker on the color under a picker pin
			auto *pick = new QPushButton(QStringLiteral("⊕"), card);
			pick->setObjectName("Flat");
			pick->setToolTip(QString::fromUtf8(obs_module_text("Markers.FromPin")));
			connect(pick, &QPushButton::clicked, this, [this, pick, markers, i, save, hue, sat]() {
				struct roi_source *roi = widget_roi(data);
				QMenu menu(pick);
				bool any = false;
				if (roi) {
					pthread_mutex_lock(&roi->shapes_mutex);
					for (size_t p = 0; p < roi->pins.num; p++) {
						const struct roi_pin &pin = roi->pins.array[p];
						if (!pin.valid)
							continue;
						const int r = pin.r, g = pin.g, b = pin.b;
						QPixmap pm(14, 14);
						pm.fill(QColor(r, g, b));
						QAction *a = menu.addAction(
							QIcon(pm), QString::fromUtf8(obs_module_text("Markers.Pin"))
									   .arg((int)p + 1));
						const int cs = roi->cm.colorspace;
						connect(a, &QAction::triggered, this,
							[markers, i, save, hue, sat, cs, r, g, b]() {
								double h, sv;
								color_to_hue_sat(cs, r, g, b, &h, &sv);
								(*markers)[i].hue = h;
								(*markers)[i].sat = sv;
								hue->blockSignals(true);
								hue->setValue(h);
								hue->blockSignals(false);
								if (sat) {
									sat->blockSignals(true);
									sat->setValue(sv);
									sat->blockSignals(false);
								}
								save();
							});
						any = true;
					}
					pthread_mutex_unlock(&roi->shapes_mutex);
				}
				if (!any)
					menu.addAction(QString::fromUtf8(obs_module_text("Markers.NoPins")))
						->setEnabled(false);
				menu.exec(pick->mapToGlobal(QPoint(0, pick->height())));
			});
			row2->addWidget(pick);
			cv->addLayout(row2);
		}

		auto *label = new QCheckBox(QString::fromUtf8(obs_module_text("Markers.ShowLabel")), card);
		label->setChecked(m.show_label);
		connect(label, &QCheckBox::toggled, this, [markers, i, save](bool on) {
			(*markers)[i].show_label = on;
			save();
		});
		cv->addWidget(label);

		vbox->addWidget(card);
	}

	if (markers->size() < 16) {
		auto *add = new QPushButton(QString::fromUtf8(obs_module_text("Markers.Add")), panel);
		connect(add, &QPushButton::clicked, this, [markers, vectorscope, save, reopen]() {
			marker_ui m;
			const size_t n = markers->size();
			m.name = QString::fromUtf8(obs_module_text("Markers.Default")).arg((int)n + 1).toStdString();
			m.kind = vectorscope ? 1 : 0;
			m.color = marker_palette[n % (sizeof(marker_palette) / sizeof(marker_palette[0]))];
			markers->push_back(m);
			save();
			reopen();
		});
		vbox->addWidget(add);
	}
}

void ScopeWidget::openOptions(int tile, const QPoint &globalPos)
{
	obs_source_t *src = tile_source(data, tile);
	if (!src)
		return;
	const bool is_roi = tile == 0;
	scope_entry *e = is_roi ? nullptr : data->entries[tile - 1];

	// Translucent outer window so the panel can have rounded corners.
#ifdef __APPLE__
	auto *popup = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint);
	popup->installEventFilter(new OptionsPanelCloser(popup));
#else
	auto *popup = new QWidget(this, Qt::Popup | Qt::FramelessWindowHint);
#endif
	popup->setAttribute(Qt::WA_DeleteOnClose, true);
	popup->setAttribute(Qt::WA_TranslucentBackground, true);
	popup->setStyleSheet(options_style);
	auto *outer = new QVBoxLayout(popup);
	outer->setContentsMargins(0, 0, 0, 0);

	auto *panel = new QFrame(popup);
	panel->setObjectName("ScopeOptions");
	panel->setMinimumWidth(260);
	panel->setMaximumWidth(320);
	outer->addWidget(panel);

	auto *vbox = new QVBoxLayout(panel);
	vbox->setContentsMargins(14, 12, 14, 12);
	vbox->setSpacing(6);

	auto reopen = [this, tile, popup]() {
		const QPoint pos = popup->pos();
		popup->close();
		QTimer::singleShot(0, this, [this, tile, pos]() { openOptions(tile, pos); });
	};

	// header: title and the full dock menu
	{
		auto *head = new QHBoxLayout();
		const QString type_name = QString::fromUtf8(is_roi ? obs_module_text("ROI")
								   : obs_module_text(scope_types[e->type].name_key));
		auto *title = new QLabel(QString::fromUtf8(obs_module_text("Options.Title")).arg(type_name), panel);
		title->setObjectName("Title");
		head->addWidget(title);
		head->addStretch(1);
		auto *more = new QPushButton(QStringLiteral("⋯"), panel);
		more->setObjectName("Flat");
		more->setToolTip(QString::fromUtf8(obs_module_text("Options.DockMenu")));
		connect(more, &QPushButton::clicked, this, [this, popup]() {
			popup->close();
			QTimer::singleShot(0, this, [this]() { openMenu(nullptr); });
		});
		head->addWidget(more);
		vbox->addLayout(head);
	}

	// name shown on the scope
	if (e) {
		vbox->addWidget(section_label(panel, QString::fromUtf8(obs_module_text("Options.Name"))));
		auto *edit = new QLineEdit(QString::fromStdString(e->label), panel);
		connect(edit, &QLineEdit::textEdited, this, [this, e](const QString &text) {
			pthread_mutex_lock(&data->mutex);
			e->label = text.toStdString();
			pthread_mutex_unlock(&data->mutex);
		});
		vbox->addWidget(edit);
		auto *show = new QCheckBox(QString::fromUtf8(obs_module_text("Options.ShowName")), panel);
		show->setChecked(e->show_label);
		connect(show, &QCheckBox::toggled, this, [this, e](bool on) {
			pthread_mutex_lock(&data->mutex);
			e->show_label = on;
			pthread_mutex_unlock(&data->mutex);
		});
		vbox->addWidget(show);
	}

	// source: the dock's ROI selection, or a source of the scope's own
	{
		obs_properties_t *props = obs_source_properties(src);
		obs_property_t *tp = props ? obs_properties_get(props, "target_name") : nullptr;
		OBSDataAutoRelease settings = obs_source_get_settings(src);
		if (tp && obs_property_list_format(tp) == OBS_COMBO_FORMAT_STRING) {
			vbox->addWidget(section_label(panel, QString::fromUtf8(obs_module_text("Source"))));
			auto *combo = new QComboBox(panel);
			const std::string roi_name = roi_name_of(this);
			const QString roi_marker = QStringLiteral("\x01roi");
			if (e)
				combo->addItem(QString::fromUtf8(obs_module_text("Options.Source.Dock")), roi_marker);
			const char *cur = obs_data_get_string(settings, "target_name");
			int current = 0;
			const size_t n = obs_property_list_item_count(tp);
			for (size_t i = 0; i < n; i++) {
				const char *v = obs_property_list_item_string(tp, i);
				if (!v || v == roi_name || is_roi_source_name(v))
					continue; // the dock's own ROI is the first entry
				combo->addItem(QString::fromUtf8(obs_property_list_item_name(tp, i)),
					       QString::fromUtf8(v));
				if (!(e && e->use_roi) && cur && strcmp(cur, v) == 0)
					current = combo->count() - 1;
			}
			combo->setCurrentIndex(current);
			connect(combo, QOverload<int>::of(&QComboBox::activated), this,
				[this, combo, src, e, roi_marker, roi_name](int idx) {
					const QString v = combo->itemData(idx).toString();
					const bool use_roi = e && v == roi_marker;
					const std::string target = use_roi ? roi_name : v.toStdString();
					if (e) {
						e->use_roi = use_roi;
						e->target = use_roi ? std::string() : target;
					}
					update_one(src, [&](obs_data_t *d) {
						obs_data_set_string(d, "target_name", target.c_str());
					});
				});
			vbox->addWidget(combo);
		}
		obs_properties_destroy(props);
	}

	// type
	if (e) {
		vbox->addWidget(section_label(panel, QString::fromUtf8(obs_module_text("Options.Type"))));
		auto *combo = new QComboBox(panel);
		for (int t = 0; t < N_TYPES; t++)
			combo->addItem(QString::fromUtf8(obs_module_text(scope_types[t].name_key)), t);
		combo->setCurrentIndex(e->type);
		connect(combo, QOverload<int>::of(&QComboBox::activated), this, [this, tile, popup](int t) {
			const QPoint pos = popup->pos();
			popup->close();
			QTimer::singleShot(0, this, [this, tile, t, pos]() {
				changeScopeType(tile, t);
				openOptions(tile, pos);
			});
		});
		vbox->addWidget(combo);
	}

	// shape of the tile: what suits the scope, fill, or a fixed aspect ratio
	if (e) {
		vbox->addWidget(section_label(panel, QString::fromUtf8(obs_module_text("Options.Aspect"))));
		auto *combo = new QComboBox(panel);
		combo->addItem(QString::fromUtf8(obs_module_text("Options.Aspect.Auto")), ASPECT_AUTO);
		combo->addItem(QString::fromUtf8(obs_module_text("Options.Aspect.Fill")), ASPECT_FILL);
		combo->addItem(QStringLiteral("16:9"), ASPECT_16_9);
		combo->addItem(QStringLiteral("4:3"), ASPECT_4_3);
		combo->addItem(QStringLiteral("1:1"), ASPECT_1_1);
		combo->setCurrentIndex(std::clamp(e->aspect, 0, 4));
		connect(combo, QOverload<int>::of(&QComboBox::activated), this, [this, e, combo](int idx) {
			pthread_mutex_lock(&data->mutex);
			e->aspect = combo->itemData(idx).toInt();
			pthread_mutex_unlock(&data->mutex);
		});
		vbox->addWidget(combo);

		vbox->addWidget(section_label(panel, QString::fromUtf8(obs_module_text("Options.Border"))));
		auto *bcombo = new QComboBox(panel);
		static const struct
		{
			const char *name;
			uint32_t color;
		} borders[] = {{"Options.Border.None", 0},           {"Options.Border.Yellow", 0xFFE8B030},
			       {"Options.Border.Green", 0xFF6FCF86}, {"Options.Border.Pink", 0xFFE0508C},
			       {"Options.Border.Blue", 0xFF4AA8E8},  {"Options.Border.Purple", 0xFFA070E0},
			       {"Options.Border.White", 0xFFE8E8EC}};
		int cur = 0;
		for (size_t i = 0; i < sizeof(borders) / sizeof(borders[0]); i++) {
			bcombo->addItem(QString::fromUtf8(obs_module_text(borders[i].name)),
					QVariant((qulonglong)borders[i].color));
			if (e->border == borders[i].color)
				cur = (int)i;
		}
		bcombo->setCurrentIndex(cur);
		connect(bcombo, QOverload<int>::of(&QComboBox::activated), this, [this, e, bcombo](int idx) {
			pthread_mutex_lock(&data->mutex);
			e->border = (uint32_t)bcombo->itemData(idx).toULongLong();
			pthread_mutex_unlock(&data->mutex);
		});
		vbox->addWidget(bcombo);
	}

	vbox->addWidget(divider(panel));

	// graticule switch and the scope's own options
	if (e) {
		if (const grid_key_s *keys = grid_keys_for(e->type)) {
			auto *cb =
				new QCheckBox(QString::fromUtf8(obs_module_text("dock.menu.grid")).remove('&'), panel);
			cb->setChecked(grid_visible(src, keys));
			connect(cb, &QCheckBox::toggled, this,
				[src, keys](bool on) { set_grid_visible(src, keys, on); });
			vbox->addWidget(cb);
		}
	}
	{
		obs_properties_t *props = obs_source_properties(src);
		OBSDataAutoRelease settings = obs_source_get_settings(src);
		if (props) {
			add_property_controls(panel, vbox, src, props, settings, reopen);
			obs_properties_destroy(props);
		}
	}

	if (e && (e->type == TYPE_WAVEFORM || e->type == TYPE_VECTORSCOPE)) {
		vbox->addWidget(divider(panel));
		addMarkerControls(panel, vbox, src, e->type == TYPE_VECTORSCOPE, reopen);
	}

	vbox->addWidget(divider(panel));

	// order in the grid
	if (e) {
		vbox->addWidget(section_label(panel, QString::fromUtf8(obs_module_text("Options.Order"))));
		auto *seg = new QHBoxLayout();
		seg->setSpacing(0);
		auto *earlier = new QPushButton(QString::fromUtf8(obs_module_text("Options.Earlier")), panel);
		earlier->setObjectName("SegmentLeft");
		earlier->setEnabled(tile > 1);
		auto *later = new QPushButton(QString::fromUtf8(obs_module_text("Options.Later")), panel);
		later->setObjectName("SegmentRight");
		later->setEnabled(tile < (int)data->entries.size());
		connect(earlier, &QPushButton::clicked, this, [this, tile, popup]() {
			const QPoint pos = popup->pos();
			popup->close();
			moveScope(tile, -1);
			QTimer::singleShot(0, this, [this, tile, pos]() { openOptions(tile - 1, pos); });
		});
		connect(later, &QPushButton::clicked, this, [this, tile, popup]() {
			const QPoint pos = popup->pos();
			popup->close();
			moveScope(tile, +1);
			QTimer::singleShot(0, this, [this, tile, pos]() { openOptions(tile + 1, pos); });
		});
		seg->addWidget(earlier);
		seg->addWidget(later);
		vbox->addLayout(seg);
	}

	// view actions
	{
		auto *row = new QHBoxLayout();
		auto *solo = new QPushButton(
			QString::fromUtf8(obs_module_text(data->solo >= 0 ? "dock.menu.solo.exit" : "dock.menu.solo"))
				.remove('&'),
			panel);
		solo->setObjectName("Flat");
		connect(solo, &QPushButton::clicked, this, [this, tile, popup]() {
			popup->close();
			toggleSolo(data->solo >= 0 ? -1 : tile);
		});
		auto *proj =
			new QPushButton(QString::fromUtf8(obs_module_text("dock.menu.projector")).remove('&'), panel);
		proj->setObjectName("Flat");
		connect(proj, &QPushButton::clicked, this, [src, popup]() {
			popup->close();
			obs_frontend_open_projector("Source", -1, "", obs_source_get_name(src));
		});
		auto *all =
			new QPushButton(QString::fromUtf8(obs_module_text("dock.menu.settings")).remove('&'), panel);
		all->setObjectName("Flat");
		connect(all, &QPushButton::clicked, this, [this, tile, popup]() {
			popup->close();
			openProperties(tile);
		});
		row->addWidget(solo);
		row->addWidget(proj);
		row->addStretch(1);
		row->addWidget(all);
		vbox->addLayout(row);
	}

	// footer
	if (e) {
		auto *row = new QHBoxLayout();
		auto *del = new QPushButton(QString::fromUtf8(obs_module_text("Options.Delete")), panel);
		del->setObjectName("Delete");
		connect(del, &QPushButton::clicked, this, [this, tile, popup]() {
			popup->close();
			QTimer::singleShot(0, this, [this, tile]() { removeScope(tile); });
		});
		auto *dup = new QPushButton(QString::fromUtf8(obs_module_text("Options.Duplicate")), panel);
		dup->setObjectName("Primary");
		connect(dup, &QPushButton::clicked, this, [this, tile, popup]() {
			popup->close();
			duplicateScope(tile);
		});
		row->addWidget(del);
		row->addStretch(1);
		row->addWidget(dup);
		vbox->addLayout(row);
	}

	popup->adjustSize();

	// keep the panel on screen
	QPoint pos = globalPos;
	if (QScreen *screen = QGuiApplication::screenAt(globalPos)) {
		const QRect avail = screen->availableGeometry();
		if (pos.x() + popup->width() > avail.right())
			pos.setX(std::max(avail.left(), avail.right() - popup->width()));
		if (pos.y() + popup->height() > avail.bottom())
			pos.setY(std::max(avail.top(), avail.bottom() - popup->height()));
	}
	popup->move(pos);
	popup->show();
#ifdef __APPLE__
	popup->raise();
	popup->activateWindow();
#endif
}

/* ---------- arranging ---------- */

void ScopeWidget::moveScope(int tile, int delta)
{
	const int i = tile - 1;
	const int j = i + delta;
	if (i < 0 || j < 0 || i >= (int)data->entries.size() || j >= (int)data->entries.size())
		return;
	pthread_mutex_lock(&data->mutex);
	std::swap(data->entries[i], data->entries[j]);
	if (data->solo == tile)
		data->solo = j + 1;
	pthread_mutex_unlock(&data->mutex);
}

/* ---------- views ---------- */

/* Writes the active view (its scopes and layout) in the same shape loadActive() reads. */
static void serialize_active(scope_widget_s *data, obs_data_t *out)
{
	pthread_mutex_lock(&data->mutex);
	obs_data_set_int(out, "layout", data->layout);
	if (data->roi_free.w > 0.0f) {
		obs_data_set_double(out, "roi-free-x", data->roi_free.x);
		obs_data_set_double(out, "roi-free-y", data->roi_free.y);
		obs_data_set_double(out, "roi-free-w", data->roi_free.w);
		obs_data_set_double(out, "roi-free-h", data->roi_free.h);
	}
	obs_data_set_int(out, "solo", data->solo);
	obs_data_set_bool(out, "roi-shown", data->roi_shown);

	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (scope_entry *e : data->entries) {
		OBSDataAutoRelease item = obs_data_create();
		OBSDataAutoRelease prop = obs_source_get_settings(e->src);
		obs_data_set_string(item, "type", scope_types[e->type].key);
		obs_data_set_obj(item, "prop", prop);
		entry_meta(e, item);
		obs_data_array_push_back(array, item);
	}
	obs_data_set_array(out, "scopes", array);
	pthread_mutex_unlock(&data->mutex);
}

static void add_default_scopes(ScopeWidget *w)
{
	OBSDataAutoRelease rgb = obs_data_create();
	obs_data_set_int(rgb, "components", WV_COMP_RGB);
	obs_data_set_int(rgb, "display", WV_DISP_PARADE);
	w->addScope(TYPE_WAVEFORM, rgb);

	OBSDataAutoRelease luma = obs_data_create();
	obs_data_set_int(luma, "components", WV_COMP_Y);
	w->addScope(TYPE_WAVEFORM, luma);

	OBSDataAutoRelease vs = obs_data_create();
	obs_data_set_bool(vs, "ring_scale", true);
	w->addScope(TYPE_VECTORSCOPE, vs);
	w->addScope(TYPE_HISTOGRAM);
}

void ScopeWidget::storeActive()
{
	if (data->active_view < 0 || data->active_view >= (int)data->views.size())
		return;
	OBSDataAutoRelease state = obs_data_create();
	serialize_active(data, state);
	data->views[data->active_view].state = state.Get();
}

void ScopeWidget::clearActive()
{
	closeProperties();

	pthread_mutex_lock(&data->mutex);
	std::vector<scope_entry *> entries;
	entries.swap(data->entries);
	data->solo = -1;
	data->i_mouse_last = -1;
	pthread_mutex_unlock(&data->mutex);

	for (scope_entry *e : entries)
		release_entry(e);
}

void ScopeWidget::loadActive(obs_data_t *state)
{
	clearActive();

	pthread_mutex_lock(&data->mutex);
	data->layout = state ? (int)obs_data_get_int(state, "layout") : SCOPE_LAYOUT_AUTO;
	if (data->layout < SCOPE_LAYOUT_AUTO || data->layout > SCOPE_LAYOUT_FREE)
		data->layout = SCOPE_LAYOUT_AUTO;
	data->roi_free = free_rect_s();
	if (state && obs_data_has_user_value(state, "roi-free-w")) {
		data->roi_free.x = (float)obs_data_get_double(state, "roi-free-x");
		data->roi_free.y = (float)obs_data_get_double(state, "roi-free-y");
		data->roi_free.w = (float)obs_data_get_double(state, "roi-free-w");
		data->roi_free.h = (float)obs_data_get_double(state, "roi-free-h");
	}
	data->roi_shown = state && obs_data_has_user_value(state, "roi-shown") ? obs_data_get_bool(state, "roi-shown")
									       : true;
	pthread_mutex_unlock(&data->mutex);

	if (state && obs_data_has_user_value(state, "scopes")) {
		OBSDataArrayAutoRelease array = obs_data_get_array(state, "scopes");
		const size_t n = obs_data_array_count(array);
		for (size_t i = 0; i < n; i++) {
			OBSDataAutoRelease item = obs_data_array_item(array, i);
			int type = type_from_key(obs_data_get_string(item, "type"));
			if (type < 0)
				continue;
			OBSDataAutoRelease prop = obs_data_get_obj(item, "prop");
			insertScope(type, prop, item, -1);
		}
	} else {
		add_default_scopes(this);
	}

	// addScope() resets solo; restore the saved one if it still exists.
	int solo = state && obs_data_has_user_value(state, "solo") ? (int)obs_data_get_int(state, "solo") : -1;
	data->solo = (solo >= 0 && solo < n_tiles(data)) ? solo : -1;
}

void ScopeWidget::rebuildTabs()
{
	if (!tabs)
		return;
	rebuildingTabs = true;
	while (tabs->count())
		tabs->removeTab(0);
	for (const auto &v : data->views)
		tabs->addTab(QString::fromStdString(v.name));
	tabs->setCurrentIndex(data->active_view);
	tabs->setVisible(data->views.size() > 1);
	rebuildingTabs = false;
}

void ScopeWidget::switchView(int index)
{
	if (index < 0 || index >= (int)data->views.size() || index == data->active_view)
		return;
	storeActive();
	data->active_view = index;
	loadActive(data->views[index].state);
	rebuildTabs();
}

static std::string next_view_name(const scope_widget_s *data)
{
	for (int i = (int)data->views.size() + 1;; i++) {
		const std::string name =
			QString::fromUtf8(obs_module_text("dock.view.default_name")).arg(i).toStdString();
		bool used = false;
		for (const auto &v : data->views)
			used = used || v.name == name;
		if (!used)
			return name;
	}
}

void ScopeWidget::addView()
{
	storeActive();

	// A new view starts empty apart from the ROI, ready for scopes of your choice.
	OBSDataAutoRelease state = obs_data_create();
	OBSDataArrayAutoRelease empty = obs_data_array_create();
	obs_data_set_array(state, "scopes", empty);

	data->views.push_back({next_view_name(data), state.Get()});
	data->active_view = (int)data->views.size() - 1;
	loadActive(state);
	rebuildTabs();
}

void ScopeWidget::duplicateView(int index)
{
	if (index < 0 || index >= (int)data->views.size())
		return;
	storeActive();
	OBSDataAutoRelease copy = obs_data_create_from_json(obs_data_get_json(data->views[index].state));
	std::string name = data->views[index].name + " " + obs_module_text("dock.view.copy_suffix");
	data->views.insert(data->views.begin() + index + 1, {name, copy.Get()});
	data->active_view = index + 1;
	loadActive(copy);
	rebuildTabs();
}

void ScopeWidget::renameView(int index)
{
	if (index < 0 || index >= (int)data->views.size())
		return;
	bool ok = false;
	QString name = QInputDialog::getText(this, QString::fromUtf8(obs_module_text("dock.view.rename.title")),
					     QString::fromUtf8(obs_module_text("dock.view.rename.label")),
					     QLineEdit::Normal, QString::fromStdString(data->views[index].name), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;
	data->views[index].name = name.trimmed().toStdString();
	rebuildTabs();
}

void ScopeWidget::deleteView(int index)
{
	if (data->views.size() <= 1 || index < 0 || index >= (int)data->views.size())
		return;
	storeActive();
	data->views.erase(data->views.begin() + index);
	if (data->active_view >= index && data->active_view > 0)
		data->active_view--;
	loadActive(data->views[data->active_view].state);
	rebuildTabs();
}

void ScopeWidget::openTabMenu(const QPoint &pos)
{
	const int index = tabs ? tabs->tabAt(pos) : -1;
	QMenu menu(this);
	QAction *act;
	if (index >= 0) {
		act = menu.addAction(QString::fromUtf8(obs_module_text("dock.view.rename")));
		connect(act, &QAction::triggered, this, [this, index]() { renameView(index); });
		act = menu.addAction(QString::fromUtf8(obs_module_text("dock.view.duplicate")));
		connect(act, &QAction::triggered, this, [this, index]() { duplicateView(index); });
		act = menu.addAction(QString::fromUtf8(obs_module_text("dock.view.delete")));
		act->setEnabled(data->views.size() > 1);
		connect(act, &QAction::triggered, this, [this, index]() { deleteView(index); }, Qt::QueuedConnection);
		menu.addSeparator();
	}
	act = menu.addAction(QString::fromUtf8(obs_module_text("dock.view.add")));
	connect(act, &QAction::triggered, this, &ScopeWidget::addView);
	menu.exec(tabs->mapToGlobal(pos));
}

/* ---------- saving and loading ---------- */

void ScopeWidget::default_properties(obs_data_t *props)
{
	obs_data_set_default_bool(props, "toolbar", true);
}

void ScopeWidget::save_properties(obs_data_t *props)
{
	storeActive();

	obs_data_set_bool(props, "toolbar", toolbarVisible);
	obs_data_set_int(props, "active_view", data->active_view);

	if (data->roi) {
		OBSDataAutoRelease prop = obs_source_get_settings(data->roi);
		obs_data_set_obj(props, ROI_KEY "-prop", prop);
	}

	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const auto &v : data->views) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "name", v.name.c_str());
		obs_data_set_obj(item, "state", v.state);
		obs_data_array_push_back(array, item);
	}
	obs_data_set_array(props, "views", array);
}

void ScopeWidget::load_properties(obs_data_t *props)
{
	const std::string roi_name = roi_name_of(this);

	setToolbarVisible(obs_data_get_bool(props, "toolbar"));

	if (!data->roi) {
		OBSDataAutoRelease prop = obs_data_get_obj(props, ROI_KEY "-prop");
		if (!prop)
			prop = obs_data_create();
		obs_source_t *roi = create_source(ROI_ID, prop, roi_name.c_str());
		pthread_mutex_lock(&data->mutex);
		data->roi = roi;
		pthread_mutex_unlock(&data->mutex);
	}

	data->views.clear();
	OBSDataArrayAutoRelease array = obs_data_get_array(props, "views");
	const size_t n = obs_data_array_count(array);
	for (size_t i = 0; i < n; i++) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		OBSDataAutoRelease state = obs_data_get_obj(item, "state");
		const char *name = obs_data_get_string(item, "name");
		data->views.push_back({name && *name ? name : next_view_name(data), state.Get()});
	}

	if (data->views.empty()) {
		// A single view; docks from before views existed kept their scopes at the top level.
		OBSDataAutoRelease state = obs_data_has_user_value(props, "scopes")
						   ? obs_data_create_from_json(obs_data_get_json(props))
						   : nullptr;
		data->views.push_back({next_view_name(data), state.Get()});
	}

	int active = (int)obs_data_get_int(props, "active_view");
	data->active_view = (active >= 0 && active < (int)data->views.size()) ? active : 0;
	loadActive(data->views[data->active_view].state);
	rebuildTabs();
}

#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/threading.h>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <vector>
#include <cmath>
#include "plugin-macros.generated.h"
#include "text-render.h"

// Glyphs: printable ASCII 32..126 plus the degree sign in the last slot.
#define FIRST_CHAR 32
#define N_ASCII 95
#define DEGREE_SLOT N_ASCII
#define N_GLYPHS (N_ASCII + 1)
#define COLS 16
#define FONT_PX 32 // atlas resolution; drawn scaled to the requested size

namespace {

struct glyph_s
{
	float u0, v0, u1, v1; // atlas UV
	float advance;        // in atlas pixels
};

struct atlas_s
{
	std::vector<uint8_t> pixels; // BGRA
	int width = 0, height = 0;
	float cell_w = 0, cell_h = 0, ascent = 0;
	glyph_s glyphs[N_GLYPHS];
	bool ready = false;

	gs_texture_t *tex = nullptr;
	gs_effect_t *effect = nullptr;
	bool effect_failed = false;
};

atlas_s atlas;
pthread_mutex_t atlas_mutex = PTHREAD_MUTEX_INITIALIZER;

int glyph_index(const unsigned char *&p)
{
	unsigned char c = *p++;
	if (c >= FIRST_CHAR && c < FIRST_CHAR + N_ASCII)
		return c - FIRST_CHAR;
	if (c == 0xC2 && *p == 0xB0) { // UTF-8 degree sign
		p++;
		return DEGREE_SLOT;
	}
	// skip the rest of any other multi-byte character
	while ((*p & 0xC0) == 0x80)
		p++;
	return '?' - FIRST_CHAR;
}

bool ensure_graphics()
{
	if (!atlas.ready)
		return false;
	if (!atlas.tex) {
		const uint8_t *data = atlas.pixels.data();
		atlas.tex = gs_texture_create(atlas.width, atlas.height, GS_BGRA, 1, &data, 0);
	}
	if (!atlas.effect && !atlas.effect_failed) {
		char *f = obs_module_file("text.effect");
		char *errors = NULL;
		atlas.effect = f ? gs_effect_create_from_file(f, &errors) : NULL;
		if (!atlas.effect) {
			blog(LOG_ERROR, "Cannot load text.effect: %s", errors ? errors : "(no file)");
			atlas.effect_failed = true;
		}
		bfree(errors);
		bfree(f);
	}
	return atlas.tex && atlas.effect;
}

} // namespace

extern "C" void text_render_init(void)
{
	if (atlas.ready || !QGuiApplication::instance())
		return;

	QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
	font.setPixelSize(FONT_PX);
	font.setHintingPreference(QFont::PreferNoHinting);
	QFontMetricsF fm(font);

	const float pad = 2.0f;
	atlas.cell_h = std::ceil((float)fm.height()) + pad * 2;
	atlas.ascent = (float)fm.ascent() + pad;
	float max_adv = 0;
	for (int i = 0; i < N_GLYPHS; i++) {
		QChar ch = i == DEGREE_SLOT ? QChar(0x00B0) : QChar(FIRST_CHAR + i);
		max_adv = std::max(max_adv, (float)fm.horizontalAdvance(ch));
	}
	atlas.cell_w = std::ceil(max_adv) + pad * 2;

	const int rows = (N_GLYPHS + COLS - 1) / COLS;
	atlas.width = (int)atlas.cell_w * COLS;
	atlas.height = (int)atlas.cell_h * rows;

	QImage img(atlas.width, atlas.height, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	{
		QPainter painter(&img);
		painter.setRenderHint(QPainter::TextAntialiasing, true);
		painter.setFont(font);
		painter.setPen(Qt::white);
		for (int i = 0; i < N_GLYPHS; i++) {
			const int col = i % COLS, row = i / COLS;
			const float x0 = col * (float)(int)atlas.cell_w;
			const float y0 = row * (float)(int)atlas.cell_h;
			QChar ch = i == DEGREE_SLOT ? QChar(0x00B0) : QChar(FIRST_CHAR + i);
			painter.drawText(QPointF(x0 + pad, y0 + atlas.ascent), QString(ch));

			glyph_s &g = atlas.glyphs[i];
			g.advance = (float)fm.horizontalAdvance(ch);
			g.u0 = x0 / atlas.width;
			g.v0 = y0 / atlas.height;
			g.u1 = (x0 + g.advance + pad * 2) / atlas.width;
			g.v1 = (y0 + atlas.cell_h) / atlas.height;
		}
	}

	// Format_ARGB32 is laid out as BGRA bytes on little-endian machines, matching GS_BGRA.
	atlas.pixels.resize((size_t)atlas.width * atlas.height * 4);
	for (int y = 0; y < atlas.height; y++)
		memcpy(atlas.pixels.data() + (size_t)y * atlas.width * 4, img.constScanLine(y),
		       (size_t)atlas.width * 4);

	atlas.ready = true;
}

extern "C" void text_render_free(void)
{
	obs_enter_graphics();
	gs_texture_destroy(atlas.tex);
	atlas.tex = nullptr;
	gs_effect_destroy(atlas.effect);
	atlas.effect = nullptr;
	obs_leave_graphics();
}

static float ui_scale = 1.0f;

extern "C" void text_set_ui_scale(float scale)
{
	ui_scale = scale >= 1.0f && scale <= 4.0f ? scale : 1.0f;
}

extern "C" struct text_scale text_scale_from_viewport(float source_width, float source_height)
{
	struct gs_rect vp;
	gs_get_viewport(&vp);
	text_scale s;
	s.ux = (vp.cx > 0 ? source_width / (float)vp.cx : 1.0f) * ui_scale;
	s.uy = (vp.cy > 0 ? source_height / (float)vp.cy : 1.0f) * ui_scale;
	return s;
}

extern "C" float text_width(const struct text_scale *scale, const char *text, float px_size)
{
	if (!atlas.ready || !text)
		return 0.0f;
	const float k = px_size / atlas.cell_h; // atlas pixels -> screen pixels
	float w = 0.0f;
	const unsigned char *p = (const unsigned char *)text;
	while (*p)
		w += atlas.glyphs[glyph_index(p)].advance * k;
	return w * scale->ux;
}

extern "C" float text_draw(const struct text_scale *scale, const char *text, float x, float y, float px_size,
			   uint32_t color, int align, int valign)
{
	if (!text || !*text)
		return 0.0f;

	pthread_mutex_lock(&atlas_mutex);
	const bool ok = ensure_graphics();
	pthread_mutex_unlock(&atlas_mutex);
	if (!ok)
		return 0.0f;

	const float k = px_size / atlas.cell_h;
	const float pad = 2.0f * k * scale->ux;
	const float width = text_width(scale, text, px_size);
	const float height = px_size * scale->uy;

	if (align == TEXT_ALIGN_CENTER)
		x -= width * 0.5f;
	else if (align == TEXT_ALIGN_RIGHT)
		x -= width;
	if (valign == TEXT_VALIGN_MIDDLE)
		y -= height * 0.5f;
	else if (valign == TEXT_VALIGN_BOTTOM)
		y -= height;

	struct vec4 c;
	vec4_from_rgba(&c, ((color >> 16) & 0xFF) | (color & 0xFF00) | ((color & 0xFF) << 16) | (color & 0xFF000000));

	gs_effect_t *effect = atlas.effect;
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), atlas.tex);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &c);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(effect, "Draw")) {
		// OBS draws at most 512 vertices per immediate batch: 6 per glyph, so 80 glyphs a batch.
		const int glyphs_per_batch = 80;
		int in_batch = 0;
		gs_render_start(false); // immediate buffer; see vertexbuffer_destroy_safe
		float pen = x - pad;
		const unsigned char *p = (const unsigned char *)text;
		while (*p) {
			if (in_batch == glyphs_per_batch) {
				gs_render_stop(GS_TRIS);
				gs_render_start(false); // immediate buffer; see vertexbuffer_destroy_safe
				in_batch = 0;
			}
			in_batch++;
			const glyph_s &g = atlas.glyphs[glyph_index(p)];
			const float gw = (g.u1 - g.u0) * atlas.width * k * scale->ux;
			const float x0 = pen, x1 = pen + gw;
			const float y0 = y, y1 = y + height;
			gs_texcoord(g.u0, g.v0, 0);
			gs_vertex2f(x0, y0);
			gs_texcoord(g.u1, g.v0, 0);
			gs_vertex2f(x1, y0);
			gs_texcoord(g.u0, g.v1, 0);
			gs_vertex2f(x0, y1);
			gs_texcoord(g.u1, g.v0, 0);
			gs_vertex2f(x1, y0);
			gs_texcoord(g.u1, g.v1, 0);
			gs_vertex2f(x1, y1);
			gs_texcoord(g.u0, g.v1, 0);
			gs_vertex2f(x0, y1);
			pen += g.advance * k * scale->ux;
		}
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();

	return width;
}

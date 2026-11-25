/*
 * Test signal: a video source that sends 10-bit Y'CbCr 4:2:2 test patterns the way a capture card does, so the
 * whole chain can be checked: OBS's conversion, the raw signal (superwhite and sub-black), the scopes and HDR.
 * The A/V sync pattern flashes white with a beep once a second, for the A/V sync measurement.
 */

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <media-io/video-io.h>
#include <math.h>
#include <string.h>
#include "plugin-macros.generated.h"

enum ts_pattern {
	TS_RP219 = 0,    // HD color bars in the style of SMPTE RP 219, with superwhite and sub-black patches
	TS_BARS100 = 1,  // full (100 %) color bars
	TS_RAMP = 2,     // luma ramp from -7 % to 109 %
	TS_STEPS = 3,    // 11 grey steps 0..100 % and 105 / 109 %
	TS_PLUGE = 4,    // black level check
	TS_HLG_BARS = 5, // simplified BT.2111 HLG bars
	TS_PQ_BARS = 6,  // simplified BT.2111 PQ bars
	TS_AV_SYNC = 7,  // black with a white flash and a 1 kHz beep once a second
	TS_SKIN = 8,     // skin tones and a grey card
};

struct ts_source
{
	obs_source_t *self;
	pthread_t thread;
	bool thread_ok;
	os_event_t *stop;
	pthread_mutex_t mutex;

	// settings (mutex)
	int pattern;
	uint32_t width, height;
	int fps_num, fps_den;
	bool dirty;

	// the frame being sent (thread only)
	uint16_t *planes[3];
	uint32_t cur_w, cur_h;
	int cur_pattern;
};

static const char *ts_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("TestSignal");
}

/* ---------- drawing into the 10-bit planes ---------- */

struct canvas
{
	uint16_t *y, *u, *v;
	uint32_t w, h;
	float kr, kb; // matrix
};

static inline uint16_t code_y(float level)
{
	float c = 64.0f + 876.0f * level;
	return (uint16_t)(c < 4.0f ? 4.0f : c > 1019.0f ? 1019.0f : c + 0.5f);
}

static inline uint16_t code_c(float c)
{
	float v = 512.0f + 896.0f * c;
	return (uint16_t)(v < 4.0f ? 4.0f : v > 1019.0f ? 1019.0f : v + 0.5f);
}

/* An R'G'B' signal (0..1, beyond for excursions) as a rectangle; x and y in 0..1 of the picture. */
static void rect_rgb(struct canvas *c, float x0, float y0, float x1, float y1, float r, float g, float b)
{
	const float yy = c->kr * r + (1.0f - c->kr - c->kb) * g + c->kb * b;
	const float cb = (b - yy) / (2.0f * (1.0f - c->kb)), cr = (r - yy) / (2.0f * (1.0f - c->kr));
	const uint16_t Y = code_y(yy), U = code_c(cb), V = code_c(cr);
	const uint32_t ix0 = (uint32_t)(x0 * c->w + 0.5f), ix1 = (uint32_t)(x1 * c->w + 0.5f);
	const uint32_t iy0 = (uint32_t)(y0 * c->h + 0.5f), iy1 = (uint32_t)(y1 * c->h + 0.5f);
	for (uint32_t y = iy0; y < iy1 && y < c->h; y++) {
		for (uint32_t x = ix0; x < ix1 && x < c->w; x++)
			c->y[y * c->w + x] = Y;
		for (uint32_t x = ix0 / 2; x < (ix1 + 1) / 2 && x < c->w / 2; x++) {
			c->u[y * (c->w / 2) + x] = U;
			c->v[y * (c->w / 2) + x] = V;
		}
	}
}

static void rect_grey(struct canvas *c, float x0, float y0, float x1, float y1, float level)
{
	rect_rgb(c, x0, y0, x1, y1, level, level, level);
}

/* A horizontal luma ramp from l0 to l1, neutral. */
static void ramp(struct canvas *c, float x0, float y0, float x1, float y1, float l0, float l1)
{
	const uint32_t ix0 = (uint32_t)(x0 * c->w), ix1 = (uint32_t)(x1 * c->w);
	const uint32_t iy0 = (uint32_t)(y0 * c->h), iy1 = (uint32_t)(y1 * c->h);
	for (uint32_t y = iy0; y < iy1 && y < c->h; y++) {
		for (uint32_t x = ix0; x < ix1 && x < c->w; x++)
			c->y[y * c->w + x] = code_y(l0 + (l1 - l0) * (float)(x - ix0) / (float)(ix1 - ix0 - 1));
		for (uint32_t x = ix0 / 2; x < ix1 / 2 && x < c->w / 2; x++)
			c->u[y * (c->w / 2) + x] = c->v[y * (c->w / 2) + x] = 512;
	}
}

static const float bars7[7][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}};

static void draw_rp219(struct canvas *c)
{
	const float side = 240.0f / 1920.0f, bw = (1.0f - 2.0f * side) / 7.0f;
	const float h1 = 7.0f / 12.0f, h2 = h1 + 1.0f / 12.0f, h3 = h2 + 1.0f / 12.0f;
	// pattern 1: 40 % grey sides, 75 % bars
	rect_grey(c, 0, 0, side, h1, 0.4f);
	rect_grey(c, 1.0f - side, 0, 1, h1, 0.4f);
	for (int i = 0; i < 7; i++)
		rect_rgb(c, side + i * bw, 0, side + (i + 1) * bw, h1, 0.75f * bars7[i][0], 0.75f * bars7[i][1],
			 0.75f * bars7[i][2]);
	// pattern 2: 100 % cyan, 75 % white, 100 % blue
	rect_rgb(c, 0, h1, side, h2, 0, 1, 1);
	rect_grey(c, side, h1, 1.0f - side, h2, 0.75f);
	rect_rgb(c, 1.0f - side, h1, 1, h2, 0, 0, 1);
	// pattern 3: 100 % yellow, ramp, 100 % red
	rect_rgb(c, 0, h2, side, h3, 1, 1, 0);
	ramp(c, side, h2, 1.0f - side, h3, 0.0f, 1.0f);
	rect_rgb(c, 1.0f - side, h2, 1, h3, 1, 0, 0);
	// pattern 4: 15 % grey sides, black, 100 % and 109 % white, -7 % sub-black, PLUGE -2 / 0 / +2 / 0 / +4 %
	rect_grey(c, 0, h3, side, 1, 0.15f);
	rect_grey(c, 1.0f - side, h3, 1, 1, 0.15f);
	float x = side;
	const float seg = (1.0f - 2.0f * side) / 12.0f;
	rect_grey(c, x, h3, x + 1.5f * seg, 1, 0.0f), x += 1.5f * seg;
	rect_grey(c, x, h3, x + 1.5f * seg, 1, 1.0f), x += 1.5f * seg;
	rect_grey(c, x, h3, x + 1.0f * seg, 1, 1.09f), x += 1.0f * seg; // superwhite
	rect_grey(c, x, h3, x + 1.0f * seg, 1, 0.0f), x += 1.0f * seg;
	rect_grey(c, x, h3, x + 1.0f * seg, 1, -0.07f), x += 1.0f * seg; // sub-black
	rect_grey(c, x, h3, x + 1.0f * seg, 1, 0.0f), x += 1.0f * seg;
	rect_grey(c, x, h3, x + 0.67f * seg, 1, -0.02f), x += 0.67f * seg;
	rect_grey(c, x, h3, x + 0.66f * seg, 1, 0.0f), x += 0.66f * seg;
	rect_grey(c, x, h3, x + 0.67f * seg, 1, 0.02f), x += 0.67f * seg;
	rect_grey(c, x, h3, x + 0.5f * seg, 1, 0.0f), x += 0.5f * seg;
	rect_grey(c, x, h3, x + 0.5f * seg, 1, 0.04f), x += 0.5f * seg;
	rect_grey(c, x, h3, 1.0f - side, 1, 0.0f);
}

static void draw_bars(struct canvas *c, float level, bool with_black)
{
	const int n = with_black ? 8 : 7;
	for (int i = 0; i < n; i++) {
		const float r = i < 7 ? bars7[i][0] * level : 0.0f, g = i < 7 ? bars7[i][1] * level : 0.0f,
			    b = i < 7 ? bars7[i][2] * level : 0.0f;
		rect_rgb(c, (float)i / n, 0, (float)(i + 1) / n, 1, r, g, b);
	}
}

static void draw_steps(struct canvas *c)
{
	for (int i = 0; i <= 10; i++)
		rect_grey(c, i / 13.0f, 0, (i + 1) / 13.0f, 1, i / 10.0f);
	rect_grey(c, 11 / 13.0f, 0, 12 / 13.0f, 1, 1.05f);
	rect_grey(c, 12 / 13.0f, 0, 1, 1, 1.09f);
}

static void draw_pluge(struct canvas *c)
{
	rect_grey(c, 0, 0, 1, 1, 0.0f);
	rect_grey(c, 0.20f, 0.2f, 0.30f, 0.8f, -0.02f);
	rect_grey(c, 0.35f, 0.2f, 0.45f, 0.8f, 0.02f);
	rect_grey(c, 0.50f, 0.2f, 0.60f, 0.8f, 0.04f);
	rect_grey(c, 0.70f, 0.35f, 0.85f, 0.65f, 1.0f);
}

/* Simplified BT.2111: reference-level bars (75 % HLG or 58 % PQ, about 203 nits) on top, 100 % bars, a grey
 * ramp and steps below. The signal values are written directly in the HDR transfer. */
static void draw_hdr_bars(struct canvas *c, float ref_level)
{
	const float side = 1.0f / 8.0f, bw = (1.0f - 2.0f * side) / 7.0f;
	rect_grey(c, 0, 0, 1, 1, 0.0f);
	rect_grey(c, 0, 0, side, 0.5f, ref_level * 0.53f); // about 40 % in light
	rect_grey(c, 1.0f - side, 0, 1, 0.5f, ref_level * 0.53f);
	for (int i = 0; i < 7; i++)
		rect_rgb(c, side + i * bw, 0, side + (i + 1) * bw, 0.5f, ref_level * bars7[i][0],
			 ref_level * bars7[i][1], ref_level * bars7[i][2]);
	for (int i = 0; i < 7; i++)
		rect_rgb(c, side + i * bw, 0.5f, side + (i + 1) * bw, 0.62f, bars7[i][0], bars7[i][1], bars7[i][2]);
	ramp(c, 0, 0.62f, 1, 0.75f, -0.07f, 1.09f);
	for (int i = 0; i <= 10; i++)
		rect_grey(c, i / 11.0f, 0.75f, (i + 1) / 11.0f, 1, i / 10.0f);
}

static void draw_skin(struct canvas *c)
{
	// light to dark skin tones (sRGB-like values) on a 40 % grey, with a white and a black patch
	static const float skin[6][3] = {{0.95f, 0.80f, 0.70f}, {0.88f, 0.70f, 0.58f}, {0.78f, 0.58f, 0.45f},
					 {0.65f, 0.46f, 0.34f}, {0.50f, 0.34f, 0.25f}, {0.36f, 0.24f, 0.18f}};
	rect_grey(c, 0, 0, 1, 1, 0.4f);
	for (int i = 0; i < 6; i++)
		rect_rgb(c, 0.05f + i * 0.15f, 0.15f, 0.18f + i * 0.15f, 0.55f, skin[i][0], skin[i][1], skin[i][2]);
	rect_grey(c, 0.05f, 0.65f, 0.30f, 0.85f, 0.9f);
	rect_grey(c, 0.70f, 0.65f, 0.95f, 0.85f, 0.03f);
}

static bool is_hdr(int pattern)
{
	return pattern == TS_HLG_BARS || pattern == TS_PQ_BARS;
}

static void render_pattern(struct ts_source *s, int pattern, uint32_t w, uint32_t h, bool flash)
{
	struct canvas c = {s->planes[0], s->planes[1], s->planes[2], w, h, 0.2126f, 0.0722f};
	if (is_hdr(pattern))
		c.kr = 0.2627f, c.kb = 0.0593f;
	switch (pattern) {
	case TS_BARS100:
		draw_bars(&c, 1.0f, false);
		break;
	case TS_RAMP:
		ramp(&c, 0, 0, 1, 1, -0.07f, 1.09f);
		break;
	case TS_STEPS:
		draw_steps(&c);
		break;
	case TS_PLUGE:
		draw_pluge(&c);
		break;
	case TS_HLG_BARS:
		draw_hdr_bars(&c, 0.75f);
		break;
	case TS_PQ_BARS:
		draw_hdr_bars(&c, 0.58f);
		break;
	case TS_AV_SYNC:
		rect_grey(&c, 0, 0, 1, 1, flash ? 1.0f : 0.0f);
		if (!flash)
			rect_grey(&c, 0.47f, 0.45f, 0.53f, 0.55f, 0.25f); // a small grey square: the source is running
		break;
	case TS_SKIN:
		draw_skin(&c);
		break;
	case TS_RP219:
	default:
		draw_rp219(&c);
		break;
	}
}

/* ---------- the thread that sends frames and sound ---------- */

static void ensure_planes(struct ts_source *s, uint32_t w, uint32_t h)
{
	if (s->cur_w == w && s->cur_h == h && s->planes[0])
		return;
	for (int i = 0; i < 3; i++)
		bfree(s->planes[i]);
	s->planes[0] = bzalloc((size_t)w * h * 2);
	s->planes[1] = bzalloc((size_t)w / 2 * h * 2);
	s->planes[2] = bzalloc((size_t)w / 2 * h * 2);
	s->cur_w = w;
	s->cur_h = h;
	s->cur_pattern = -1;
}

static void send_frame(struct ts_source *s, int pattern, uint64_t ts)
{
	struct obs_source_frame f = {0};
	f.data[0] = (uint8_t *)s->planes[0];
	f.data[1] = (uint8_t *)s->planes[1];
	f.data[2] = (uint8_t *)s->planes[2];
	f.linesize[0] = s->cur_w * 2;
	f.linesize[1] = s->cur_w;
	f.linesize[2] = s->cur_w;
	f.width = s->cur_w;
	f.height = s->cur_h;
	f.format = VIDEO_FORMAT_I210;
	f.timestamp = ts;
	const enum video_colorspace cs = pattern == TS_HLG_BARS  ? VIDEO_CS_2100_HLG
					 : pattern == TS_PQ_BARS ? VIDEO_CS_2100_PQ
								 : VIDEO_CS_709;
	video_format_get_parameters_for_format(cs, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I210, f.color_matrix,
					       f.color_range_min, f.color_range_max);
	f.full_range = false;
	f.trc = pattern == TS_HLG_BARS ? VIDEO_TRC_HLG : pattern == TS_PQ_BARS ? VIDEO_TRC_PQ : VIDEO_TRC_DEFAULT;
	f.max_luminance = pattern == TS_PQ_BARS ? 1000 : 0;
	obs_source_output_video(s->self, &f);
}

static void send_audio(struct ts_source *s, uint64_t ts, uint32_t frames, bool beep, uint64_t *phase)
{
	float *buf = bzalloc(frames * sizeof(float) * 2);
	if (beep)
		for (uint32_t i = 0; i < frames; i++) {
			const float v = 0.5f * sinf(2.0f * 3.14159265f * 1000.0f * (float)(*phase + i) / 48000.0f);
			buf[i] = buf[frames + i] = v;
		}
	*phase += frames;
	struct obs_source_audio a = {0};
	a.data[0] = (uint8_t *)buf;
	a.data[1] = (uint8_t *)(buf + frames);
	a.frames = frames;
	a.speakers = SPEAKERS_STEREO;
	a.format = AUDIO_FORMAT_FLOAT_PLANAR;
	a.samples_per_sec = 48000;
	a.timestamp = ts;
	obs_source_output_audio(s->self, &a);
	bfree(buf);
}

static void *ts_thread(void *data)
{
	struct ts_source *s = data;
	os_set_thread_name("va-test-signal");
	uint64_t start = os_gettime_ns(), n = 0, phase = 0;
	int last_flash = -1;
	while (os_event_try(s->stop) == EAGAIN) {
		pthread_mutex_lock(&s->mutex);
		const int pattern = s->pattern;
		const uint32_t w = s->width & ~1u, h = s->height;
		const int num = s->fps_num > 0 ? s->fps_num : 25, den = s->fps_den > 0 ? s->fps_den : 1;
		const bool dirty = s->dirty;
		s->dirty = false;
		pthread_mutex_unlock(&s->mutex);
		if (dirty) {
			start = os_gettime_ns();
			n = 0;
		}

		const uint64_t interval = 1000000000ULL * (uint64_t)den / (uint64_t)num;
		const uint64_t ts = start + n * interval;
		os_sleepto_ns(ts);

		ensure_planes(s, w, h);
		// the A/V sync pattern flashes on the first frame of every second
		const uint64_t frames_per_s = ((uint64_t)num + den - 1) / den;
		const bool flash = pattern == TS_AV_SYNC && n % frames_per_s == 0;
		if (pattern != s->cur_pattern || (pattern == TS_AV_SYNC && (int)flash != last_flash)) {
			render_pattern(s, pattern, w, h, flash);
			s->cur_pattern = pattern;
			last_flash = flash;
		}
		send_frame(s, pattern, ts);
		if (pattern == TS_AV_SYNC) {
			const uint32_t samples = (uint32_t)(48000ULL * den / num);
			send_audio(s, ts, samples, flash, &phase);
		}
		n++;
	}
	return NULL;
}

/* ---------- source ---------- */

static void ts_update(void *data, obs_data_t *settings)
{
	struct ts_source *s = data;
	pthread_mutex_lock(&s->mutex);
	s->pattern = (int)obs_data_get_int(settings, "pattern");
	const int res = (int)obs_data_get_int(settings, "resolution");
	s->width = res == 720 ? 1280 : res == 2160 ? 3840 : 1920;
	s->height = res == 720 ? 720 : res == 2160 ? 2160 : 1080;
	const int fps = (int)obs_data_get_int(settings, "fps");
	switch (fps) {
	case 2997:
		s->fps_num = 30000, s->fps_den = 1001;
		break;
	case 5994:
		s->fps_num = 60000, s->fps_den = 1001;
		break;
	default:
		s->fps_num = fps > 0 ? fps : 25, s->fps_den = 1;
	}
	s->dirty = true;
	pthread_mutex_unlock(&s->mutex);
}

static void *ts_create(obs_data_t *settings, obs_source_t *source)
{
	struct ts_source *s = bzalloc(sizeof(struct ts_source));
	s->self = source;
	s->cur_pattern = -1;
	pthread_mutex_init(&s->mutex, NULL);
	ts_update(s, settings);
	if (os_event_init(&s->stop, OS_EVENT_TYPE_MANUAL) == 0)
		s->thread_ok = pthread_create(&s->thread, NULL, ts_thread, s) == 0;
	return s;
}

static void ts_destroy(void *data)
{
	struct ts_source *s = data;
	if (s->thread_ok) {
		os_event_signal(s->stop);
		pthread_join(s->thread, NULL);
	}
	os_event_destroy(s->stop);
	pthread_mutex_destroy(&s->mutex);
	for (int i = 0; i < 3; i++)
		bfree(s->planes[i]);
	bfree(s);
}

static void ts_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "pattern", TS_RP219);
	obs_data_set_default_int(settings, "resolution", 1080);
	obs_data_set_default_int(settings, "fps", 25);
}

static obs_properties_t *ts_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "pattern", obs_module_text("TestSignal.Pattern"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("TestSignal.RP219"), TS_RP219);
	obs_property_list_add_int(p, obs_module_text("TestSignal.Bars100"), TS_BARS100);
	obs_property_list_add_int(p, obs_module_text("TestSignal.Ramp"), TS_RAMP);
	obs_property_list_add_int(p, obs_module_text("TestSignal.Steps"), TS_STEPS);
	obs_property_list_add_int(p, obs_module_text("TestSignal.Pluge"), TS_PLUGE);
	obs_property_list_add_int(p, obs_module_text("TestSignal.Skin"), TS_SKIN);
	obs_property_list_add_int(p, obs_module_text("TestSignal.HLG"), TS_HLG_BARS);
	obs_property_list_add_int(p, obs_module_text("TestSignal.PQ"), TS_PQ_BARS);
	obs_property_list_add_int(p, obs_module_text("TestSignal.AVSync"), TS_AV_SYNC);
	p = obs_properties_add_list(props, "resolution", obs_module_text("TestSignal.Resolution"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "1280x720", 720);
	obs_property_list_add_int(p, "1920x1080", 1080);
	obs_property_list_add_int(p, "3840x2160", 2160);
	p = obs_properties_add_list(props, "fps", obs_module_text("TestSignal.Fps"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "25", 25);
	obs_property_list_add_int(p, "29.97", 2997);
	obs_property_list_add_int(p, "30", 30);
	obs_property_list_add_int(p, "50", 50);
	obs_property_list_add_int(p, "59.94", 5994);
	obs_property_list_add_int(p, "60", 60);
	obs_properties_add_text(props, "note", obs_module_text("TestSignal.Note"), OBS_TEXT_INFO);
	return props;
}

const struct obs_source_info colormonitor_testsignal = {
	.id = ID_PREFIX "test_signal",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = ts_get_name,
	.create = ts_create,
	.destroy = ts_destroy,
	.update = ts_update,
	.get_defaults = ts_defaults,
	.get_properties = ts_properties,
	.icon_type = OBS_ICON_TYPE_COLOR,
};

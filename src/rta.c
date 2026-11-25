/*
 * Audio spectrum (RTA): the frequency spectrum of the program mix or one source on a logarithmic scale from 20 Hz to
 * 20 kHz, with a peak hold line, and a waterfall below that scrolls the spectrum over time.
 */
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <media-io/audio-io.h>
#include <math.h>
#include <stdio.h>
#include "plugin-macros.generated.h"
#include "text-render.h"
#include "draw-util.h"

#define RTA_W 520
#define RTA_H 300
#define SPEC_H 170 // the spectrum on top, the waterfall below
#define FFT_N 4096
#define RING (FFT_N * 2)
#define N_BANDS 160
#define WF_W N_BANDS
#define WF_H 120
#define DB_MIN -90.0f

struct rta_source
{
	obs_source_t *self;
	pthread_mutex_t mutex;
	char *source_name;
	obs_weak_source_t *weak_src;
	bool mix_connected;
	uint32_t rate;

	float ring[2][RING]; // left and right (or the only channel)
	uint32_t ring_pos;
	size_t channels;

	float re[FFT_N], im[FFT_N], window[FFT_N];
	float band_db[2][N_BANDS]; // smoothed
	float hold_db[N_BANDS];
	uint64_t hold_ns[N_BANDS];
	uint32_t *wf_buf; // waterfall, newest row on top
	gs_texture_t *wf_tex;

	int mode; // 0 sum, 1 left and right
	bool waterfall;
	float speed; // smoothing: 0 fast .. 0.9 slow
};

static const char *rta_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("RTA");
}

static void process(struct rta_source *s, const struct audio_data *d, size_t channels)
{
	if (!d || !d->frames || !d->data[0])
		return;
	const float *l = (const float *)d->data[0];
	const float *r = channels > 1 && d->data[1] ? (const float *)d->data[1] : l;
	pthread_mutex_lock(&s->mutex);
	for (uint32_t i = 0; i < d->frames; i++) {
		s->ring[0][s->ring_pos] = l[i];
		s->ring[1][s->ring_pos] = r[i];
		s->ring_pos = (s->ring_pos + 1) % RING;
	}
	s->channels = channels;
	pthread_mutex_unlock(&s->mutex);
}

static void mix_cb(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	process(param, data, audio_output_get_channels(obs_get_audio()));
}

static void source_cb(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(muted);
	process(param, data, audio_output_get_channels(obs_get_audio()));
}

static void detach(struct rta_source *s)
{
	if (s->mix_connected) {
		obs_remove_raw_audio_callback(0, mix_cb, s);
		s->mix_connected = false;
	}
	if (s->weak_src) {
		obs_source_t *src = obs_weak_source_get_source(s->weak_src);
		if (src) {
			obs_source_remove_audio_capture_callback(src, source_cb, s);
			obs_source_release(src);
		}
		obs_weak_source_release(s->weak_src);
		s->weak_src = NULL;
	}
}

static void attach(struct rta_source *s)
{
	detach(s);
	s->rate = audio_output_get_sample_rate(obs_get_audio());
	if (!s->source_name || !*s->source_name) {
		obs_add_raw_audio_callback(0, NULL, mix_cb, s);
		s->mix_connected = true;
		return;
	}
	obs_source_t *src = obs_get_source_by_name(s->source_name);
	if (src) {
		obs_source_add_audio_capture_callback(src, source_cb, s);
		s->weak_src = obs_source_get_weak_source(src);
		obs_source_release(src);
	}
}

static void rta_update(void *data, obs_data_t *settings)
{
	struct rta_source *s = data;
	const char *name = obs_data_get_string(settings, "audio_source");
	s->mode = (int)obs_data_get_int(settings, "rta_mode");
	s->waterfall = obs_data_get_bool(settings, "rta_waterfall");
	s->speed = (float)obs_data_get_int(settings, "rta_speed") / 100.0f;
	if (!s->source_name || strcmp(name, s->source_name) != 0 || (!s->mix_connected && !s->weak_src)) {
		bfree(s->source_name);
		s->source_name = bstrdup(name);
		attach(s);
	}
}

static void *rta_create(obs_data_t *settings, obs_source_t *source)
{
	struct rta_source *s = bzalloc(sizeof(struct rta_source));
	s->self = source;
	pthread_mutex_init(&s->mutex, NULL);
	for (int i = 0; i < FFT_N; i++)
		s->window[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / (FFT_N - 1)); // Hann
	for (int c = 0; c < 2; c++)
		for (int b = 0; b < N_BANDS; b++)
			s->band_db[c][b] = DB_MIN;
	for (int b = 0; b < N_BANDS; b++)
		s->hold_db[b] = DB_MIN;
	s->wf_buf = bzalloc(WF_W * WF_H * 4);
	rta_update(s, settings);
	return s;
}

static void rta_destroy(void *data)
{
	struct rta_source *s = data;
	detach(s);
	obs_enter_graphics();
	gs_texture_destroy(s->wf_tex);
	obs_leave_graphics();
	bfree(s->wf_buf);
	bfree(s->source_name);
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static bool add_audio_source(void *param, obs_source_t *src)
{
	if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(src);
		obs_property_list_add_string(param, name, name);
	}
	return true;
}

static void rta_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_int(settings, "rta_mode", 0);
	obs_data_set_default_bool(settings, "rta_waterfall", true);
	obs_data_set_default_int(settings, "rta_speed", 60);
}

static obs_properties_t *rta_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "audio_source", obs_module_text("Audio.Prop.Source"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Audio.Prop.Mix"), "");
	obs_enum_sources(add_audio_source, p);
	p = obs_properties_add_list(props, "rta_mode", obs_module_text("RTA.Mode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("RTA.Mode.Sum"), 0);
	obs_property_list_add_int(p, obs_module_text("RTA.Mode.LR"), 1);
	p = obs_properties_add_int_slider(props, "rta_speed", obs_module_text("RTA.Speed"), 0, 95, 5);
	obs_property_int_set_suffix(p, " %");
	obs_properties_add_bool(props, "rta_waterfall", obs_module_text("RTA.Waterfall"));
	return props;
}

static uint32_t rta_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	return RTA_W;
}

static uint32_t rta_get_height(void *data)
{
	UNUSED_PARAMETER(data);
	return RTA_H;
}

/* In-place radix-2 FFT. */
static void fft(float *re, float *im, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i];
			re[i] = re[j], re[j] = t;
			t = im[i], im[i] = im[j], im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		const float ang = -2.0f * 3.14159265f / len;
		const float wr = cosf(ang), wi = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float cr = 1.0f, ci = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				const int a = i + k, b = i + k + len / 2;
				const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
				re[b] = re[a] - tr, im[b] = im[a] - ti;
				re[a] += tr, im[a] += ti;
				const float ncr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = ncr;
			}
		}
	}
}

static inline float band_freq(int b)
{
	return 20.0f * powf(1000.0f, (float)b / N_BANDS); // 20 Hz .. 20 kHz
}

/* The spectrum of one channel (or of the sum) in dBFS per band. */
static void analyse(struct rta_source *s, int ch, float *out_db)
{
	pthread_mutex_lock(&s->mutex);
	const uint32_t pos = s->ring_pos;
	for (int i = 0; i < FFT_N; i++) {
		const uint32_t k = (pos + RING - FFT_N + i) % RING;
		const float v = ch == 2 ? 0.5f * (s->ring[0][k] + s->ring[1][k]) : s->ring[ch][k];
		s->re[i] = v * s->window[i];
		s->im[i] = 0.0f;
	}
	pthread_mutex_unlock(&s->mutex);
	fft(s->re, s->im, FFT_N);
	const float rate = s->rate ? (float)s->rate : 48000.0f;
	const float bin_hz = rate / FFT_N;
	for (int b = 0; b < N_BANDS; b++) {
		const float f0 = band_freq(b), f1 = band_freq(b + 1);
		int k0 = (int)(f0 / bin_hz), k1 = (int)(f1 / bin_hz);
		if (k1 <= k0)
			k1 = k0 + 1;
		float p = 0.0f;
		for (int k = k0; k < k1 && k < FFT_N / 2; k++)
			p = fmaxf(p, s->re[k] * s->re[k] + s->im[k] * s->im[k]);
		// a full-scale sine reads about 0 dBFS (Hann window: coherent gain 0.5)
		const float amp = sqrtf(p) * 2.0f / (FFT_N * 0.5f);
		out_db[b] = fmaxf(20.0f * log10f(amp + 1e-9f), DB_MIN);
	}
}

static uint32_t heat(float t)
{
	// black, blue, green, yellow, red, white
	t = fminf(fmaxf(t, 0.0f), 1.0f);
	const float stops[6][3] = {{0, 0, 0}, {0, 0.2f, 0.8f}, {0, 0.8f, 0.4f}, {1, 0.9f, 0}, {1, 0.2f, 0}, {1, 1, 1}};
	const float x = t * 5.0f;
	const int i = x >= 5.0f ? 4 : (int)x;
	const float f = x - i;
	const float r = stops[i][0] + (stops[i + 1][0] - stops[i][0]) * f;
	const float g = stops[i][1] + (stops[i + 1][1] - stops[i][1]) * f;
	const float b = stops[i][2] + (stops[i + 1][2] - stops[i][2]) * f;
	return 0xFF000000u | (uint32_t)(b * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(r * 255);
}

static void rta_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct rta_source *s = data;
	if (!s->mix_connected && !s->weak_src && s->source_name && *s->source_name)
		attach(s); // the source may come later
	float db[2][N_BANDS];
	const int n_ch = s->mode == 1 ? 2 : 1;
	for (int c = 0; c < n_ch; c++)
		analyse(s, s->mode == 1 ? c : 2, db[c]);
	const float k = fminf(fmaxf(s->speed, 0.0f), 0.95f);
	const uint64_t now = os_gettime_ns();
	for (int c = 0; c < n_ch; c++)
		for (int b = 0; b < N_BANDS; b++) {
			float *v = &s->band_db[c][b];
			// rises at once, falls with the speed
			*v = db[c][b] > *v ? db[c][b] : *v * k + db[c][b] * (1.0f - k);
		}
	for (int b = 0; b < N_BANDS; b++) {
		const float v = n_ch == 2 ? fmaxf(s->band_db[0][b], s->band_db[1][b]) : s->band_db[0][b];
		if (v >= s->hold_db[b] || now - s->hold_ns[b] > 2000000000ULL) {
			s->hold_db[b] = v;
			s->hold_ns[b] = now;
		}
	}
	if (s->waterfall) {
		memmove(s->wf_buf + WF_W, s->wf_buf, (size_t)WF_W * (WF_H - 1) * 4);
		for (int b = 0; b < N_BANDS; b++) {
			const float v = n_ch == 2 ? fmaxf(db[0][b], db[1][b]) : db[0][b];
			s->wf_buf[b] = heat((v - DB_MIN) / -DB_MIN);
		}
	}
}

static void rta_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct rta_source *s = data;
	const struct text_scale ts = text_scale_from_viewport((float)RTA_W, (float)RTA_H);
	du_fill_rect(0, 0, RTA_W, RTA_H, 0xFF0C0C0E);
	const float x0 = 34.0f, x1 = RTA_W - 6.0f, y0 = 6.0f, y1 = s->waterfall ? SPEC_H : RTA_H - 16.0f;
	const float spec_h = y1 - y0;
	char buf[32];
	// dB lines every 10 dB
	for (int d = 0; d >= (int)DB_MIN; d -= 10) {
		const float y = y0 + spec_h * (float)d / DB_MIN;
		du_line(x0, y, x1, y, d == 0 ? 0x60FFFFFF : 0x25FFFFFF);
		if (d % 20 == 0) {
			snprintf(buf, sizeof(buf), "%d", d);
			text_draw(&ts, buf, x0 - 3.0f * ts.ux, y, 8.0f, 0x90FFFFFF, TEXT_ALIGN_RIGHT,
				  TEXT_VALIGN_MIDDLE);
		}
	}
	// frequency lines
	static const float freqs[] = {31.5f, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
	static const char *fnames[] = {"31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
	for (int i = 0; i < 10; i++) {
		const float x = x0 + (x1 - x0) * log10f(freqs[i] / 20.0f) / 3.0f;
		du_line(x, y0, x, y1, 0x25FFFFFF);
		text_draw(&ts, fnames[i], x, y1 + 2.0f * ts.uy, 8.0f, 0x90FFFFFF, TEXT_ALIGN_CENTER, TEXT_VALIGN_TOP);
	}
	// the spectrum as bars, peak hold as a line
	const float bw = (x1 - x0) / N_BANDS;
	const int n_ch = s->mode == 1 ? 2 : 1;
	for (int c = 0; c < n_ch; c++) {
		const uint32_t col = n_ch == 1 ? 0xC060D0FF : c == 0 ? 0xA060FF80 : 0xA0FF7060;
		for (int b = 0; b < N_BANDS; b++) {
			const float v = s->band_db[c][b];
			const float y = y0 + spec_h * fminf(v / DB_MIN, 1.0f);
			if (n_ch == 1)
				du_fill_rect(x0 + b * bw, y, x0 + (b + 1) * bw - 0.3f, y1, col);
			else
				du_line(x0 + b * bw, y, x0 + (b + 1) * bw, y, col | 0xFF000000);
		}
	}
	for (int b = 0; b < N_BANDS; b++) {
		const float y = y0 + spec_h * fminf(s->hold_db[b] / DB_MIN, 1.0f);
		du_line(x0 + b * bw, y, x0 + (b + 1) * bw, y, 0xB0FFFFFF);
	}
	// waterfall
	if (s->waterfall) {
		const uint8_t *buf8 = (const uint8_t *)s->wf_buf;
		if (!s->wf_tex)
			s->wf_tex = gs_texture_create(WF_W, WF_H, GS_RGBA, 1, &buf8, GS_DYNAMIC);
		else
			gs_texture_set_image(s->wf_tex, buf8, WF_W * 4, false);
		const float wy0 = SPEC_H + 14.0f, wy1 = RTA_H - 4.0f;
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), s->wf_tex);
		gs_matrix_push();
		gs_matrix_translate3f(x0, wy0, 0.0f);
		gs_matrix_scale3f((x1 - x0) / WF_W, (wy1 - wy0) / WF_H, 1.0f);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(s->wf_tex, 0, WF_W, WF_H);
		gs_matrix_pop();
	}
	text_draw(&ts, s->source_name && *s->source_name ? s->source_name : obs_module_text("Audio.Prop.Mix"),
		  x1 - 2.0f * ts.ux, y0 + 2.0f * ts.uy, 9.0f, 0xA0FFFFFF, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
}

const struct obs_source_info colormonitor_rta = {
	.id = ID_PREFIX "rta_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = rta_get_name,
	.create = rta_create,
	.destroy = rta_destroy,
	.update = rta_update,
	.get_defaults = rta_get_defaults,
	.get_properties = rta_get_properties,
	.get_width = rta_get_width,
	.get_height = rta_get_height,
	.video_render = rta_render,
	.video_tick = rta_tick,
};

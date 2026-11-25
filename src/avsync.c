/*
 * A/V sync: measures how much later (or earlier) the sound of a source comes than its picture, from a sync test
 * signal that flashes white with a beep (the Test Signal's A/V sync pattern, or a clip or generator that does the
 * same). The flash is found in the source's own frames (raw input filter), the beep in its own audio, both with
 * the source's timestamps, before OBS applies its Sync Offset.
 */
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <media-io/audio-io.h>
#include <math.h>
#include <stdio.h>
#include "plugin-macros.generated.h"
#include "util.h"
#include "text-render.h"
#include "draw-util.h"
#include "raw-input.h"

#define AVS_W 400
#define AVS_H 220
#define N_HIST 12

struct avs_source
{
	obs_source_t *self;
	pthread_mutex_t mutex;

	char *source_name;
	obs_weak_source_t *weak; // the measured source, with the audio callback
	uint64_t check_ns;

	// audio onsets (mutex)
	float env;
	uint64_t quiet_until; // the beep has to start after a quiet moment
	uint64_t beep_ts[16];
	int beep_n, beep_pos;

	// results
	float offsets[N_HIST]; // ms, audio minus video, newest first
	int n_offsets;
	uint64_t last_flash; // the flash measured last
	float threshold;     // beep level, linear
};

static const char *avs_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AVSync");
}

static void audio_cb(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(muted);
	struct avs_source *s = param;
	if (!audio || !audio->frames || !audio->data[0])
		return;
	struct obs_audio_info ai;
	const uint32_t rate = obs_get_audio_info(&ai) ? ai.samples_per_sec : 48000;
	const float *d = (const float *)audio->data[0];
	pthread_mutex_lock(&s->mutex);
	for (uint32_t i = 0; i < audio->frames; i++) {
		// an envelope of about 1 ms
		s->env += (fabsf(d[i]) - s->env) * 0.05f;
		const uint64_t ts = audio->timestamp + (uint64_t)i * 1000000000ULL / rate;
		if (s->env < s->threshold * 0.25f) {
			if (!s->quiet_until)
				s->quiet_until = ts + 100000000ULL; // 100 ms of quiet before the next beep
		} else if (s->env > s->threshold && s->quiet_until && ts > s->quiet_until) {
			// the beep starts about where the envelope began to rise: half a millisecond back
			s->beep_ts[s->beep_pos] = ts - 500000ULL;
			s->beep_pos = (s->beep_pos + 1) % 16;
			if (s->beep_n < 16)
				s->beep_n++;
			s->quiet_until = 0;
		} else if (s->env > s->threshold) {
			s->quiet_until = 0;
		}
	}
	pthread_mutex_unlock(&s->mutex);
}

static void detach(struct avs_source *s)
{
	obs_source_t *src = s->weak ? obs_weak_source_get_source(s->weak) : NULL;
	if (src) {
		obs_source_remove_audio_capture_callback(src, audio_cb, s);
		obs_source_release(src);
	}
	obs_weak_source_release(s->weak);
	s->weak = NULL;
}

static void avs_update(void *data, obs_data_t *settings)
{
	struct avs_source *s = data;
	const char *name = obs_data_get_string(settings, "avs_source");
	pthread_mutex_lock(&s->mutex);
	s->threshold = (float)pow(10.0, obs_data_get_double(settings, "avs_threshold") / 20.0);
	pthread_mutex_unlock(&s->mutex);
	if (!s->source_name || strcmp(s->source_name, name) != 0) {
		bfree(s->source_name);
		s->source_name = bstrdup(name);
		detach(s);
		s->n_offsets = 0;
		s->check_ns = 0;
	}
}

static void *avs_create(obs_data_t *settings, obs_source_t *source)
{
	struct avs_source *s = bzalloc(sizeof(struct avs_source));
	s->self = source;
	pthread_mutex_init(&s->mutex, NULL);
	avs_update(s, settings);
	return s;
}

static void avs_destroy(void *data)
{
	struct avs_source *s = data;
	detach(s);
	bfree(s->source_name);
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static void avs_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "avs_source", "");
	obs_data_set_default_double(settings, "avs_threshold", -30.0);
}

static obs_properties_t *avs_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "avs_source", obs_module_text("AVSync.Source"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	property_list_add_sources(p, NULL);
	p = obs_properties_add_float_slider(props, "avs_threshold", obs_module_text("AVSync.Threshold"), -60.0, -6.0,
					    1.0);
	obs_property_float_set_suffix(p, " dBFS");
	obs_properties_add_text(props, "avs_note", obs_module_text("AVSync.Note"), OBS_TEXT_INFO);
	return props;
}

static uint32_t avs_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	return AVS_W;
}

static uint32_t avs_get_height(void *data)
{
	UNUSED_PARAMETER(data);
	return AVS_H;
}

/* Keeps the source attached (audio callback, raw input filter) and pairs flashes with beeps. */
static void avs_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct avs_source *s = data;
	if (!s->source_name || !*s->source_name)
		return;
	obs_source_t *src = s->weak ? obs_weak_source_get_source(s->weak) : NULL;
	if (!src) {
		const uint64_t now = os_gettime_ns();
		if (now < s->check_ns)
			return;
		s->check_ns = now + 1000000000ULL;
		detach(s);
		src = obs_get_source_by_name(s->source_name);
		if (!src)
			return;
		s->weak = obs_source_get_weak_source(src);
		obs_source_add_audio_capture_callback(src, audio_cb, s);
	}
	raw_input_ensure(src);
	obs_source_t *filter = raw_input_find(src);
	if (filter) {
		raw_input_watch_flash(filter);
		uint64_t flashes[16];
		const int nf = raw_input_flashes(filter, flashes, 16);
		// the newest flash that has a beep within half a second, once
		if (nf > 0 && flashes[0] != s->last_flash) {
			const uint64_t f = flashes[0];
			pthread_mutex_lock(&s->mutex);
			int64_t best = INT64_MAX;
			for (int i = 0; i < s->beep_n; i++) {
				const int64_t d = (int64_t)(s->beep_ts[(s->beep_pos - 1 - i + 32) % 16] - f);
				if (llabs(d) < llabs(best))
					best = d;
			}
			pthread_mutex_unlock(&s->mutex);
			// wait until a beep after the flash could have come (up to half a second)
			if (best != INT64_MAX && llabs(best) < 500000000LL) {
				memmove(s->offsets + 1, s->offsets, sizeof(float) * (N_HIST - 1));
				s->offsets[0] = (float)best / 1e6f;
				if (s->n_offsets < N_HIST)
					s->n_offsets++;
				s->last_flash = f;
			}
		}
		obs_source_release(filter);
	}
	obs_source_release(src);
}

static void avs_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct avs_source *s = data;
	const struct text_scale ts = text_scale_from_viewport((float)AVS_W, (float)AVS_H);
	du_fill_rect(0, 0, AVS_W, AVS_H, 0xFF0C0C0E);
	char buf[128];
	text_draw(&ts, obs_module_text("AVSync"), 8.0f * ts.ux, 6.0f * ts.uy, 12.0f, 0xFFFFFFFF, TEXT_ALIGN_LEFT,
		  TEXT_VALIGN_TOP);
	if (!s->source_name || !*s->source_name) {
		text_draw(&ts, obs_module_text("AVSync.NoSource"), AVS_W / 2.0f, AVS_H / 2.0f, 11.0f, 0xC0FFFFFF,
			  TEXT_ALIGN_CENTER, TEXT_VALIGN_MIDDLE);
		return;
	}
	snprintf(buf, sizeof(buf), "%s", s->source_name);
	text_draw(&ts, buf, AVS_W - 8.0f * ts.ux, 6.0f * ts.uy, 10.0f, 0xA0FFFFFF, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
	if (!s->n_offsets) {
		text_draw(&ts, obs_module_text("AVSync.Waiting"), AVS_W / 2.0f, AVS_H / 2.0f, 11.0f, 0xC0FFFFFF,
			  TEXT_ALIGN_CENTER, TEXT_VALIGN_MIDDLE);
		return;
	}
	// the average of the measurements
	float avg = 0.0f, mn = 1e9f, mx = -1e9f;
	for (int i = 0; i < s->n_offsets; i++) {
		avg += s->offsets[i];
		mn = fminf(mn, s->offsets[i]);
		mx = fmaxf(mx, s->offsets[i]);
	}
	avg /= s->n_offsets;
	const bool ok = fabsf(avg) <= 20.0f;
	snprintf(buf, sizeof(buf), "%+.0f ms", avg);
	text_draw(&ts, buf, AVS_W / 2.0f, 70.0f, 34.0f, ok ? 0xFF60E060 : 0xFFFFB040, TEXT_ALIGN_CENTER,
		  TEXT_VALIGN_MIDDLE);
	const char *verdict = ok        ? obs_module_text("AVSync.InSync")
			      : avg > 0 ? obs_module_text("AVSync.AudioLate")
					: obs_module_text("AVSync.AudioEarly");
	text_draw(&ts, verdict, AVS_W / 2.0f, 104.0f, 12.0f, ok ? 0xFF80F080 : 0xFFFFC060, TEXT_ALIGN_CENTER,
		  TEXT_VALIGN_MIDDLE);
	// what to set: OBS delays the audio by its Sync Offset
	if (!ok) {
		snprintf(buf, sizeof(buf), "%s %+.0f ms", obs_module_text("AVSync.Suggest"), -avg);
		text_draw(&ts, buf, AVS_W / 2.0f, 124.0f, 10.0f, 0xD0FFFFFF, TEXT_ALIGN_CENTER, TEXT_VALIGN_MIDDLE);
	}
	// the last measurements as bars around 0
	const float bx = 20.0f, bw = AVS_W - 40.0f, by = 150.0f, bh = 44.0f, mid = by + bh / 2.0f;
	du_line(bx, mid, bx + bw, mid, 0x50FFFFFF);
	const float scale = bh / 2.0f / fmaxf(100.0f, fmaxf(fabsf(mn), fabsf(mx)));
	for (int i = 0; i < s->n_offsets; i++) {
		const float x = bx + bw - (i + 1) * bw / N_HIST + 4.0f, v = s->offsets[i];
		du_fill_rect(x, v > 0 ? mid - v * scale : mid, x + bw / N_HIST - 8.0f, v > 0 ? mid : mid - v * scale,
			     fabsf(v) <= 20.0f ? 0xC060E060 : 0xC0FFB040);
	}
	snprintf(buf, sizeof(buf), "%d %s, %.0f .. %.0f ms", s->n_offsets, obs_module_text("AVSync.Measurements"), mn,
		 mx);
	text_draw(&ts, buf, AVS_W / 2.0f, AVS_H - 6.0f * ts.uy, 9.0f, 0xA0FFFFFF, TEXT_ALIGN_CENTER,
		  TEXT_VALIGN_BOTTOM);
}

const struct obs_source_info colormonitor_avsync = {
	.id = ID_PREFIX "avsync_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = avs_get_name,
	.create = avs_create,
	.destroy = avs_destroy,
	.update = avs_update,
	.get_defaults = avs_get_defaults,
	.get_properties = avs_get_properties,
	.get_width = avs_get_width,
	.get_height = avs_get_height,
	.video_render = avs_render,
	.video_tick = avs_tick,
};

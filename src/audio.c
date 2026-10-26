/*
 * Audio meter: peak per channel with hold, EBU R128 loudness (momentary, short-term,
 * integrated), phase correlation and a goniometer (stereo vector scope).
 *
 * The audio comes from the program mix of OBS or from one source.
 * Loudness follows ITU-R BS.1770-4: K-weighting, channel weights, 400 ms blocks with
 * 75 % overlap, and the absolute and relative gates for the integrated value.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/darray.h>
#include <media-io/audio-io.h>
#include <math.h>
#include <float.h>
#include "plugin-macros.generated.h"
#include "text-render.h"
#include "util.h"

#define AU_W 520
#define AU_H 300
#define AU_TL_H 150 // loudness timeline below the meters
#define AU_HIST 400 // timeline buckets
#define AU_MAX_CH 8
#define GONIO 128
#define SUB_BLOCKS 30 // 100 ms sub-blocks: 4 for momentary, 30 for short-term

struct biquad
{
	double b0, b1, b2, a1, a2;
	double z1, z2;
};

static inline double biquad_run(struct biquad *f, double x)
{
	const double y = f->b0 * x + f->z1;
	f->z1 = f->b1 * x - f->a1 * y + f->z2;
	f->z2 = f->b2 * x - f->a2 * y;
	return y;
}

struct au_source
{
	obs_source_t *self;

	pthread_mutex_t mutex;
	char *source_name; // empty: program mix
	obs_weak_source_t *weak_src;
	bool mix_connected;

	uint32_t sample_rate;
	size_t channels;

	// K-weighting per channel: high shelf then high pass
	struct biquad shelf[AU_MAX_CH], hpass[AU_MAX_CH];

	// metering
	float peak[AU_MAX_CH];  // since the last render, linear
	float shown[AU_MAX_CH]; // with fall-back, dBFS
	float hold[AU_MAX_CH];  // dBFS
	uint64_t hold_ns[AU_MAX_CH];

	// loudness
	double sub_energy;   // weighted mean square sum of the current sub-block
	uint32_t sub_frames; // frames in the current sub-block
	double subs[SUB_BLOCKS];
	int n_subs, i_sub;
	DARRAY(double) blocks; // 400 ms block energies for the integrated loudness
	float momentary, short_term, integrated;
	float momentary_max, short_term_max;

	// loudness timeline: per bucket the highest momentary and short-term loudness
	struct
	{
		float m, s;
	} hist[AU_HIST];
	int hist_head;
	uint64_t bucket_start;
	double window; // seconds shown
	bool timeline;

	// stereo
	double sum_lr, sum_ll, sum_rr;
	float correlation;
	uint8_t gonio[GONIO * GONIO];
	gs_texture_t *gonio_tex;
	gs_effect_t *effect;

	volatile bool want_reset;
	int target_lufs;
};

static const char *au_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Audio");
}

static void kweight_init(struct au_source *src)
{
	const double fs = src->sample_rate ? src->sample_rate : 48000.0;
	for (size_t c = 0; c < AU_MAX_CH; c++) {
		// stage 1: high shelf
		double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
		double K = tan(M_PI * f0 / fs);
		double Vh = pow(10.0, G / 20.0), Vb = pow(Vh, 0.4996667741545416);
		double a0 = 1.0 + K / Q + K * K;
		struct biquad *s = src->shelf + c;
		memset(s, 0, sizeof(*s));
		s->b0 = (Vh + Vb * K / Q + K * K) / a0;
		s->b1 = 2.0 * (K * K - Vh) / a0;
		s->b2 = (Vh - Vb * K / Q + K * K) / a0;
		s->a1 = 2.0 * (K * K - 1.0) / a0;
		s->a2 = (1.0 - K / Q + K * K) / a0;
		// stage 2: high pass
		f0 = 38.13547087602444;
		Q = 0.5003270373238773;
		K = tan(M_PI * f0 / fs);
		a0 = 1.0 + K / Q + K * K;
		struct biquad *h = src->hpass + c;
		memset(h, 0, sizeof(*h));
		h->b0 = 1.0;
		h->b1 = -2.0;
		h->b2 = 1.0;
		h->a1 = 2.0 * (K * K - 1.0) / a0;
		h->a2 = (1.0 - K / Q + K * K) / a0;
	}
}

static void loudness_reset(struct au_source *src)
{
	src->sub_energy = 0.0;
	src->sub_frames = 0;
	src->n_subs = src->i_sub = 0;
	da_resize(src->blocks, 0);
	src->momentary = src->short_term = src->integrated = -INFINITY;
	src->momentary_max = src->short_term_max = -INFINITY;
	for (int i = 0; i < AU_HIST; i++)
		src->hist[i].m = src->hist[i].s = -INFINITY;
	src->hist_head = 0;
	src->bucket_start = 0;
	for (size_t c = 0; c < AU_MAX_CH; c++) {
		src->hold[c] = -INFINITY;
		src->shown[c] = -INFINITY;
	}
}

static inline float to_lufs(double energy)
{
	return energy > 0.0 ? (float)(-0.691 + 10.0 * log10(energy)) : -INFINITY;
}

static float integrated_loudness(const double *blocks, size_t n)
{
	// absolute gate at -70 LUFS, then relative gate 10 LU below the gated mean
	const double abs_gate = pow(10.0, (-70.0 + 0.691) / 10.0);
	double sum = 0.0;
	size_t cnt = 0;
	for (size_t i = 0; i < n; i++)
		if (blocks[i] > abs_gate) {
			sum += blocks[i];
			cnt++;
		}
	if (!cnt)
		return -INFINITY;
	const double rel_gate = sum / cnt * pow(10.0, -10.0 / 10.0);
	sum = 0.0;
	cnt = 0;
	for (size_t i = 0; i < n; i++)
		if (blocks[i] > abs_gate && blocks[i] > rel_gate) {
			sum += blocks[i];
			cnt++;
		}
	return cnt ? to_lufs(sum / cnt) : -INFINITY;
}

/* BS.1770 channel weights for the OBS channel order FL FR FC LFE RL RR (SL SR). */
static inline double channel_weight(size_t c, size_t channels)
{
	if (channels <= 2)
		return 1.0;
	if (c == 3)
		return 0.0; // LFE
	return c >= 4 ? 1.41 : 1.0;
}

static void process_audio(struct au_source *src, const struct audio_data *data, size_t channels)
{
	if (!data->frames)
		return;
	if (channels > AU_MAX_CH)
		channels = AU_MAX_CH;

	pthread_mutex_lock(&src->mutex);
	if (src->want_reset) {
		src->want_reset = false;
		loudness_reset(src);
	}
	src->channels = channels;
	const uint32_t sub_len = src->sample_rate / 10; // 100 ms

	const float *ch[AU_MAX_CH] = {0};
	for (size_t c = 0; c < channels; c++)
		ch[c] = (const float *)data->data[c];

	for (uint32_t i = 0; i < data->frames; i++) {
		double e = 0.0;
		for (size_t c = 0; c < channels; c++) {
			if (!ch[c])
				continue;
			const float v = ch[c][i];
			const float a = fabsf(v);
			if (a > src->peak[c])
				src->peak[c] = a;
			const double k = biquad_run(src->hpass + c, biquad_run(src->shelf + c, v));
			e += channel_weight(c, channels) * k * k;
		}
		src->sub_energy += e;

		if (channels >= 2 && ch[0] && ch[1]) {
			const float l = ch[0][i], r = ch[1][i];
			src->sum_lr += (double)l * r;
			src->sum_ll += (double)l * l;
			src->sum_rr += (double)r * r;
			// goniometer: side across, mid up; full scale fills the plot
			const float s = (l - r) * 0.7071f, m = (l + r) * 0.7071f;
			const int x = (int)((s * 0.5f + 0.5f) * (GONIO - 1));
			const int y = (int)((0.5f - m * 0.5f) * (GONIO - 1));
			if ((i & 1) == 0 && x >= 0 && x < GONIO && y >= 0 && y < GONIO) {
				uint8_t *p = src->gonio + y * GONIO + x;
				if (*p < 250)
					*p += 6;
			}
		}

		if (++src->sub_frames >= sub_len && sub_len) {
			src->subs[src->i_sub] = src->sub_energy / src->sub_frames;
			src->i_sub = (src->i_sub + 1) % SUB_BLOCKS;
			if (src->n_subs < SUB_BLOCKS)
				src->n_subs++;
			src->sub_energy = 0.0;
			src->sub_frames = 0;

			// momentary: last 4 sub-blocks (400 ms); short-term: last 30 (3 s)
			double m = 0.0, st = 0.0;
			for (int k = 0; k < src->n_subs; k++) {
				const double v = src->subs[(src->i_sub - 1 - k + SUB_BLOCKS) % SUB_BLOCKS];
				if (k < 4)
					m += v;
				st += v;
			}
			if (src->n_subs >= 4) {
				const double block = m / 4.0;
				src->momentary = to_lufs(block);
				da_push_back(src->blocks, &block);
				src->integrated = integrated_loudness(src->blocks.array, src->blocks.num);
			}
			src->short_term = src->n_subs >= SUB_BLOCKS ? to_lufs(st / src->n_subs) : -INFINITY;
			if (src->momentary > src->momentary_max)
				src->momentary_max = src->momentary;
			if (src->short_term > src->short_term_max)
				src->short_term_max = src->short_term;

			// timeline
			const uint64_t now = os_gettime_ns();
			const uint64_t len = (uint64_t)(src->window / AU_HIST * 1e9);
			if (!src->bucket_start)
				src->bucket_start = now;
			while (len && now - src->bucket_start >= len) {
				src->bucket_start += len;
				src->hist_head = (src->hist_head + 1) % AU_HIST;
				src->hist[src->hist_head].m = src->hist[src->hist_head].s = -INFINITY;
				if (now - src->bucket_start > len * AU_HIST)
					src->bucket_start = now;
			}
			if (src->momentary > src->hist[src->hist_head].m)
				src->hist[src->hist_head].m = src->momentary;
			if (src->short_term > src->hist[src->hist_head].s)
				src->hist[src->hist_head].s = src->short_term;

			const double den = sqrt(src->sum_ll * src->sum_rr);
			src->correlation = den > 1e-12 ? (float)(src->sum_lr / den) : 0.0f;
			src->sum_lr = src->sum_ll = src->sum_rr = 0.0;
		}
	}
	pthread_mutex_unlock(&src->mutex);
}

static void mix_cb(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct au_source *src = param;
	process_audio(src, data, audio_output_get_channels(obs_get_audio()));
}

static void source_cb(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(muted);
	struct au_source *src = param;
	process_audio(src, data, audio_output_get_channels(obs_get_audio()));
}

static void detach(struct au_source *src)
{
	if (src->mix_connected) {
		obs_remove_raw_audio_callback(0, mix_cb, src);
		src->mix_connected = false;
	}
	if (src->weak_src) {
		obs_source_t *s = obs_weak_source_get_source(src->weak_src);
		if (s) {
			obs_source_remove_audio_capture_callback(s, source_cb, src);
			obs_source_release(s);
		}
		obs_weak_source_release(src->weak_src);
		src->weak_src = NULL;
	}
}

static void attach(struct au_source *src)
{
	detach(src);
	src->sample_rate = audio_output_get_sample_rate(obs_get_audio());
	kweight_init(src);
	if (!src->source_name || !*src->source_name) {
		obs_add_raw_audio_callback(0, NULL, mix_cb, src);
		src->mix_connected = true;
		return;
	}
	obs_source_t *s = obs_get_source_by_name(src->source_name);
	if (s) {
		obs_source_add_audio_capture_callback(s, source_cb, src);
		src->weak_src = obs_source_get_weak_source(s);
		obs_source_release(s);
	}
}

static void au_update(void *data, obs_data_t *settings)
{
	struct au_source *src = data;
	const char *name = obs_data_get_string(settings, "audio_source");
	src->target_lufs = (int)obs_data_get_int(settings, "target_lufs");
	src->timeline = obs_data_get_bool(settings, "audio_timeline");
	const double window = (double)obs_data_get_int(settings, "audio_window");
	if (window != src->window) {
		pthread_mutex_lock(&src->mutex);
		src->window = window > 0.0 ? window : 300.0;
		for (int i = 0; i < AU_HIST; i++)
			src->hist[i].m = src->hist[i].s = -INFINITY;
		src->bucket_start = 0;
		pthread_mutex_unlock(&src->mutex);
	}
	if (!src->source_name || strcmp(name, src->source_name) != 0 || (!src->mix_connected && !src->weak_src)) {
		bfree(src->source_name);
		src->source_name = bstrdup(name);
		attach(src);
		src->want_reset = true;
	}
}

static void *au_create(obs_data_t *settings, obs_source_t *source)
{
	struct au_source *src = bzalloc(sizeof(struct au_source));
	src->self = source;
	pthread_mutex_init(&src->mutex, NULL);
	da_init(src->blocks);
	loudness_reset(src);
	obs_enter_graphics();
	src->effect = create_effect_from_module_file("vectorscope.effect");
	obs_leave_graphics();
	au_update(src, settings);
	return src;
}

static void au_destroy(void *data)
{
	struct au_source *src = data;
	detach(src);
	obs_enter_graphics();
	gs_texture_destroy(src->gonio_tex);
	obs_leave_graphics();
	da_free(src->blocks);
	pthread_mutex_destroy(&src->mutex);
	bfree(src->source_name);
	bfree(src);
}

static void au_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_int(settings, "target_lufs", -23);
	obs_data_set_default_bool(settings, "audio_timeline", true);
	obs_data_set_default_int(settings, "audio_window", 300);
}

static bool add_audio_source(void *param, obs_source_t *s)
{
	obs_property_t *prop = param;
	if (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(s);
		obs_property_list_add_string(prop, name, name);
	}
	return true;
}

static bool au_reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	((struct au_source *)data)->want_reset = true;
	return false;
}

static obs_properties_t *au_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_list(props, "audio_source", obs_module_text("Audio.Prop.Source"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Audio.Prop.Mix"), "");
	obs_enum_sources(add_audio_source, prop);
	prop = obs_properties_add_list(props, "target_lufs", obs_module_text("Audio.Prop.Target"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "EBU R128 (-23 LUFS)", -23);
	obs_property_list_add_int(prop, "ATSC A/85 (-24 LUFS)", -24);
	obs_property_list_add_int(prop, "Streaming (-16 LUFS)", -16);
	obs_property_list_add_int(prop, "Streaming (-14 LUFS)", -14);
	obs_properties_add_bool(props, "audio_timeline", obs_module_text("Audio.Prop.Timeline"));
	prop = obs_properties_add_list(props, "audio_window", obs_module_text("Audio.Prop.Window"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "30 s", 30);
	obs_property_list_add_int(prop, "1 min", 60);
	obs_property_list_add_int(prop, "5 min", 300);
	obs_property_list_add_int(prop, "15 min", 900);
	obs_property_list_add_int(prop, "30 min", 1800);
	obs_property_list_add_int(prop, "1 h", 3600);
	obs_property_list_add_int(prop, "1.5 h", 5400);
	obs_properties_add_button2(props, "audio_reset", obs_module_text("Audio.Reset"), au_reset_clicked, data);
	return props;
}

static uint32_t au_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	return AU_W;
}

static uint32_t au_get_height(void *data)
{
	const struct au_source *src = data;
	return src->timeline ? AU_H + AU_TL_H : AU_H;
}

static void au_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct au_source *src = data;
	// reattach when the chosen source appears later or was recreated
	if (src->source_name && *src->source_name) {
		obs_source_t *s = src->weak_src ? obs_weak_source_get_source(src->weak_src) : NULL;
		if (!s || obs_source_removed(s))
			attach(src);
		obs_source_release(s);
	}
}

static void fill(float x0, float y0, float x1, float y1, uint32_t color)
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

static const char *channel_name(size_t c, size_t n)
{
	static const char *stereo[] = {"L", "R"};
	static const char *surround[] = {"L", "R", "C", "LFE", "Ls", "Rs", "Lb", "Rb"};
	if (n == 1)
		return "M";
	return n == 2 ? stereo[c] : surround[c];
}

#define DB_MIN -60.0f

static float db_to_y(float db, float y0, float y1)
{
	if (db < DB_MIN)
		db = DB_MIN;
	if (db > 0.0f)
		db = 0.0f;
	return y1 - (db - DB_MIN) / -DB_MIN * (y1 - y0);
}

static void format_lufs(char *buf, size_t size, const char *label, float v)
{
	if (isfinite(v))
		snprintf(buf, size, "%s %.1f", label, v);
	else
		snprintf(buf, size, "%s  --", label);
}

/* Surround: every channel's level on its direction, as a polygon (L and R at 30 degrees,
 * the surrounds at 110 and the back channels at 150; LFE is left out). */
static void render_room_meter(struct au_source *src, const struct text_scale *ts, size_t n, float x0, float y0,
			      float size)
{
	static const float angles[AU_MAX_CH] = {-30.0f, 30.0f, 0.0f, 0.0f, -110.0f, 110.0f, -150.0f, 150.0f};
	const float cx = x0 + size * 0.5f, cy = y0 + size * 0.5f, r = size * 0.45f;
	fill(x0, y0, x0 + size, y0 + size, 0xFF0A0A0C);
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);

	float px[AU_MAX_CH], py[AU_MAX_CH];
	int idx[AU_MAX_CH], m = 0;
	for (size_t c = 0; c < n && c < AU_MAX_CH; c++) {
		if (c == 3)
			continue; // LFE
		idx[m++] = (int)c;
	}
	// sort by angle so the polygon goes round
	for (int i = 0; i < m; i++)
		for (int j = i + 1; j < m; j++)
			if (angles[idx[j]] < angles[idx[i]]) {
				const int t = idx[i];
				idx[i] = idx[j];
				idx[j] = t;
			}
	for (int i = 0; i < m; i++) {
		const int c = idx[i];
		float db = src->shown[c];
		if (!isfinite(db) || db < DB_MIN)
			db = DB_MIN;
		const float k = (db - DB_MIN) / -DB_MIN;
		const float a = angles[c] * (float)M_PI / 180.0f;
		px[i] = cx + r * k * sinf(a);
		py[i] = cy - r * k * cosf(a);
	}
	// rings at -40, -20 and 0 dBFS and the channel directions
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x40FFFFFF);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		for (int ring = 1; ring <= 3; ring++) {
			const float rr = r * ring / 3.0f;
			for (int i = 0; i < 48; i++) {
				const float a0 = i * 2.0f * (float)M_PI / 48, a1 = (i + 1) * 2.0f * (float)M_PI / 48;
				gs_vertex2f(cx + rr * sinf(a0), cy - rr * cosf(a0));
				gs_vertex2f(cx + rr * sinf(a1), cy - rr * cosf(a1));
			}
		}
		for (int i = 0; i < m; i++) {
			const float a = angles[idx[i]] * (float)M_PI / 180.0f;
			gs_vertex2f(cx, cy);
			gs_vertex2f(cx + r * sinf(a), cy - r * cosf(a));
		}
		gs_render_stop(GS_LINES);
	}
	// level polygon
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x9050D070);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		for (int i = 0; i < m; i++) {
			const int j = (i + 1) % m;
			gs_vertex2f(cx, cy);
			gs_vertex2f(px[i], py[i]);
			gs_vertex2f(px[j], py[j]);
		}
		gs_render_stop(GS_TRIS);
	}
	for (int i = 0; i < m; i++) {
		const int c = idx[i];
		const float a = angles[c] * (float)M_PI / 180.0f;
		text_draw(ts, channel_name((size_t)c, n), cx + (r + 8.0f * ts->ux) * sinf(a),
			  cy - (r + 8.0f * ts->uy) * cosf(a), 10.0f, 0xFFB8B8C0, TEXT_ALIGN_CENTER, TEXT_VALIGN_MIDDLE);
	}
}

/* Loudness over time: short-term as a filled area, momentary peaks as a line, the target dashed. */
static void render_loudness_timeline(struct au_source *src, const struct text_scale *ts, const void *hist_v, int head,
				     double window, float top, float height)
{
	const struct
	{
		float m, s;
	} *hist = hist_v;
	const float W = (float)AU_W;
	const float x0 = 40.0f * ts->ux, x1 = W - 8.0f * ts->ux;
	const float y0 = top + 18.0f * ts->uy, y1 = top + height - 16.0f * ts->uy;
	const float lo = -50.0f, hi = -5.0f; // LUFS shown
	fill(8.0f * ts->ux, top, W - 8.0f * ts->ux, top + 1.0f * ts->uy, 0xFF303036);
	char buf[48];
	if (window >= 3600.0)
		snprintf(buf, sizeof(buf), "LOUDNESS  %.1f h", window / 3600.0);
	else if (window >= 60.0)
		snprintf(buf, sizeof(buf), "LOUDNESS  %.0f min", window / 60.0);
	else
		snprintf(buf, sizeof(buf), "LOUDNESS  %.0f s", window);
	text_draw(ts, buf, 8.0f * ts->ux, top + 3.0f * ts->uy, 10.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	fill(x0, y0, x1, y1, 0xFF17171B);
	for (int l = -50; l <= -10; l += 10) {
		const float y = y1 - (y1 - y0) * (l - lo) / (hi - lo);
		fill(x0, y, x1, y + 1.0f * ts->uy, 0x30FFFFFF);
		snprintf(buf, sizeof(buf), "%d", l);
		text_draw(ts, buf, x0 - 4.0f * ts->ux, y, 10.0f, 0xFF8D8D96, TEXT_ALIGN_RIGHT, TEXT_VALIGN_MIDDLE);
	}
	const float yt = y1 - (y1 - y0) * ((float)src->target_lufs - lo) / (hi - lo);
	for (float x = x0; x < x1; x += 10.0f * ts->ux)
		fill(x, yt, fminf(x + 5.0f * ts->ux, x1), yt + 1.0f * ts->uy, 0xC06FCF86);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	const float bw = (x1 - x0) / AU_HIST;
	// short-term: filled bars, green near the target, orange above it
	for (int pass = 0; pass < 2; pass++) {
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), pass == 0 ? 0xA050C070 : 0xC03C8CFF);
		while (gs_effect_loop(solid, "Solid")) {
			int in_batch = 0;
			gs_render_start(false);
			for (int i = 0; i < AU_HIST; i++) {
				const float v = hist[(head + 1 + i) % AU_HIST].s;
				if (!isfinite(v))
					continue;
				const bool over = v > src->target_lufs + 1.0f;
				if ((pass == 1) != over)
					continue;
				const float y = y1 - (y1 - y0) * fminf(fmaxf((v - lo) / (hi - lo), 0.0f), 1.0f);
				const float x = x0 + bw * i;
				gs_vertex2f(x, y1);
				gs_vertex2f(x + bw, y1);
				gs_vertex2f(x, y);
				gs_vertex2f(x, y);
				gs_vertex2f(x + bw, y1);
				gs_vertex2f(x + bw, y);
				if ((in_batch += 6) >= 480) {
					gs_render_stop(GS_TRIS);
					gs_render_start(false);
					in_batch = 0;
				}
			}
			gs_render_stop(GS_TRIS);
		}
	}
	// momentary maximum as a thin line
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xC0F0F0F0);
	while (gs_effect_loop(solid, "Solid")) {
		int in_batch = 0;
		gs_render_start(false);
		for (int i = 1; i < AU_HIST; i++) {
			const float a = hist[(head + i) % AU_HIST].m, b = hist[(head + 1 + i) % AU_HIST].m;
			if (!isfinite(a) || !isfinite(b))
				continue;
			const float ya = y1 - (y1 - y0) * fminf(fmaxf((a - lo) / (hi - lo), 0.0f), 1.0f);
			const float yb = y1 - (y1 - y0) * fminf(fmaxf((b - lo) / (hi - lo), 0.0f), 1.0f);
			gs_vertex2f(x0 + bw * (i - 0.5f), ya);
			gs_vertex2f(x0 + bw * (i + 0.5f), yb);
			if ((in_batch += 2) >= 500) {
				gs_render_stop(GS_LINES);
				gs_render_start(false);
				in_batch = 0;
			}
		}
		gs_render_stop(GS_LINES);
	}
	text_draw(ts, "now", x1, y1 + 2.0f * ts->uy, 10.0f, 0xFF8D8D96, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
}

static void au_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct au_source *src = data;
	const float W = (float)AU_W, H = (float)AU_H;
	const float H_total = (float)au_get_height(src);
	const struct text_scale ts = text_scale_from_viewport(W, H_total);
	const float px = 11.0f;

	float peak[AU_MAX_CH];
	size_t channels;
	float m, st, in, corr;
	uint8_t gonio[GONIO * GONIO];
	pthread_mutex_lock(&src->mutex);
	channels = src->channels;
	memcpy(peak, src->peak, sizeof(peak));
	memset(src->peak, 0, sizeof(src->peak));
	m = src->momentary;
	st = src->short_term;
	in = src->integrated;
	corr = src->correlation;
	const float m_max = src->momentary_max, st_max = src->short_term_max;
	static struct
	{
		float m, s;
	} hist[AU_HIST];
	memcpy(hist, src->hist, sizeof(hist));
	const int head = src->hist_head;
	const double window = src->window;
	// persistence: fade the plot a little every frame
	for (int i = 0; i < GONIO * GONIO; i++) {
		gonio[i] = src->gonio[i];
		src->gonio[i] = (uint8_t)(src->gonio[i] * 7 / 8);
	}
	pthread_mutex_unlock(&src->mutex);

	fill(0.0f, 0.0f, W, H_total, 0xFF101013);

	// peak meters with a fall-back of 20 dB/s and a 2 s hold
	const uint64_t now = os_gettime_ns();
	const float meter_x0 = 34.0f * ts.ux, meter_x1 = W * 0.42f;
	const float y0 = 10.0f * ts.uy, y1 = H - 22.0f * ts.uy;
	const size_t n = channels ? channels : 2;
	const float bw = (meter_x1 - meter_x0) / (float)n;
	for (int db = 0; db >= (int)DB_MIN; db -= 6) {
		const float y = db_to_y((float)db, y0, y1);
		fill(meter_x0 - 4.0f * ts.ux, y, meter_x1, y + 1.0f * ts.uy, 0x40FFFFFF);
		char buf[8];
		snprintf(buf, sizeof(buf), "%d", db);
		text_draw(&ts, buf, meter_x0 - 6.0f * ts.ux, y, px - 1.0f, 0xFF8D8D96, TEXT_ALIGN_RIGHT,
			  TEXT_VALIGN_MIDDLE);
	}
	for (size_t c = 0; c < n && c < AU_MAX_CH; c++) {
		const float db = peak[c] > 0.0f ? 20.0f * log10f(peak[c]) : -INFINITY;
		float shown = src->shown[c] - 20.0f / 60.0f;
		if (!(shown > db))
			shown = db;
		src->shown[c] = shown;
		if (!(src->hold[c] > shown) || now - src->hold_ns[c] > 2000000000ull) {
			src->hold[c] = shown;
			src->hold_ns[c] = now;
		}
		const float x0 = meter_x0 + bw * c + 2.0f * ts.ux, x1 = meter_x0 + bw * (c + 1) - 2.0f * ts.ux;
		fill(x0, y0, x1, y1, 0xFF1E1E22);
		if (isfinite(shown)) {
			// green to -18, yellow to -6, red above
			const float ys = db_to_y(shown, y0, y1);
			const float y18 = db_to_y(-18.0f, y0, y1), y6 = db_to_y(-6.0f, y0, y1);
			fill(x0, fmaxf(ys, y18), x1, y1, 0xFF4CC870);
			if (ys < y18)
				fill(x0, fmaxf(ys, y6), x1, y18, 0xFFE6C840);
			if (ys < y6)
				fill(x0, ys, x1, y6, 0xFFE64C3C);
		}
		if (isfinite(src->hold[c])) {
			const float yh = db_to_y(src->hold[c], y0, y1);
			fill(x0, yh, x1, yh + 2.0f * ts.uy, src->hold[c] > -0.1f ? 0xFFFF3030 : 0xFFF0F0F0);
		}
		text_draw(&ts, channel_name(c, n), (x0 + x1) * 0.5f, H - 4.0f * ts.uy, px, 0xFFB8B8C0,
			  TEXT_ALIGN_CENTER, TEXT_VALIGN_BOTTOM);
	}

	// loudness figures
	const float tx = W * 0.46f;
	float ty = 10.0f * ts.uy;
	const float lh = (px + 7.0f) * ts.uy;
	char buf[64];
	text_draw(&ts, "LOUDNESS (LUFS)", tx, ty, px - 1.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	ty += lh;
	const float target = (float)src->target_lufs;
	const float vals[3] = {m, st, in};
	const char *labels[3] = {"M", "S", "I"};
	for (int k = 0; k < 3; k++) {
		format_lufs(buf, sizeof(buf), labels[k], vals[k]);
		uint32_t color = 0xFFE6E6EA;
		if (k > 0 && isfinite(vals[k]))
			color = fabsf(vals[k] - target) <= 1.0f ? 0xFF6FCF86
				: vals[k] > target              ? 0xFFFF5A50
								: 0xFFFFC040;
		text_draw(&ts, buf, tx, ty, px + 5.0f, color, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		if (k < 2) {
			const float mx = k == 0 ? m_max : st_max;
			char mb[32];
			if (isfinite(mx))
				snprintf(mb, sizeof(mb), "max %.1f", mx);
			else
				snprintf(mb, sizeof(mb), "max --");
			text_draw(&ts, mb, tx + 92.0f * ts.ux, ty + 4.0f * ts.uy, px - 1.0f, 0xFF8D8D96,
				  TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
		}
		ty += lh * 1.25f;
	}
	snprintf(buf, sizeof(buf), "target %d", src->target_lufs);
	text_draw(&ts, buf, tx, ty, px - 1.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	ty += lh;

	// phase correlation bar, -1 .. +1
	text_draw(&ts, "PHASE", tx, ty, px - 1.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	ty += lh;
	const float cx0 = tx, cx1 = W * 0.70f;
	fill(cx0, ty, cx1, ty + 8.0f * ts.uy, 0xFF1E1E22);
	fill((cx0 + cx1) * 0.5f, ty - 2.0f * ts.uy, (cx0 + cx1) * 0.5f + 1.0f * ts.ux, ty + 10.0f * ts.uy, 0x80FFFFFF);
	if (n >= 2) {
		const float xc = cx0 + (corr + 1.0f) * 0.5f * (cx1 - cx0);
		fill(xc - 2.0f * ts.ux, ty - 2.0f * ts.uy, xc + 2.0f * ts.ux, ty + 10.0f * ts.uy,
		     corr < 0.0f ? 0xFFFF5A50 : 0xFF6FCF86);
	}
	text_draw(&ts, "-1", cx0, ty + 10.0f * ts.uy, px - 2.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	text_draw(&ts, "+1", cx1, ty + 10.0f * ts.uy, px - 2.0f, 0xFF8D8D96, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);

	if (src->timeline)
		render_loudness_timeline(src, &ts, hist, head, window, H, (float)AU_TL_H);

	const float gs = fminf(W * 0.28f, H - 20.0f * ts.uy);
	const float gx0 = W - gs - 8.0f * ts.ux, gy0 = (H - gs) * 0.5f;
	if (n > 2) {
		render_room_meter(src, &ts, n, gx0, gy0, gs);
		return;
	}

	// goniometer
	fill(gx0, gy0, gx0 + gs, gy0 + gs, 0xFF0A0A0C);
	if (src->effect) {
		const uint8_t *p = gonio;
		if (!src->gonio_tex)
			src->gonio_tex = gs_texture_create(GONIO, GONIO, GS_R8, 1, &p, GS_DYNAMIC);
		else
			gs_texture_set_image(src->gonio_tex, p, GONIO, false);
		struct vec4 color;
		struct vec3 zero;
		vec4_set(&color, 0.45f, 1.0f, 0.55f, 1.0f);
		vec3_zero(&zero);
		gs_effect_t *e = src->effect;
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), src->gonio_tex);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "intensity"), 4.0f);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, "color"), &color);
		gs_effect_set_vec3(gs_effect_get_param_by_name(e, "color_u"), &zero);
		gs_effect_set_vec3(gs_effect_get_param_by_name(e, "color_v"), &zero);
		gs_matrix_push();
		gs_matrix_translate3f(gx0, gy0, 0.0f);
		while (gs_effect_loop(e, "Draw"))
			gs_draw_sprite(src->gonio_tex, 0, (uint32_t)gs, (uint32_t)gs);
		gs_matrix_pop();
	}
	// axes: M up, L and R on the diagonals
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x50FFFFFF);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(gx0 + gs * 0.5f, gy0);
		gs_vertex2f(gx0 + gs * 0.5f, gy0 + gs);
		gs_vertex2f(gx0, gy0);
		gs_vertex2f(gx0 + gs, gy0 + gs);
		gs_vertex2f(gx0 + gs, gy0);
		gs_vertex2f(gx0, gy0 + gs);
		gs_render_stop(GS_LINES);
	}
	text_draw(&ts, "M", gx0 + gs * 0.5f + 3.0f * ts.ux, gy0, px - 2.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT,
		  TEXT_VALIGN_TOP);
	text_draw(&ts, "L", gx0 + 2.0f * ts.ux, gy0, px - 2.0f, 0xFF8D8D96, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	text_draw(&ts, "R", gx0 + gs - 2.0f * ts.ux, gy0, px - 2.0f, 0xFF8D8D96, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
}

const struct obs_source_info colormonitor_audio = {
	.id = ID_PREFIX "audio_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = au_get_name,
	.create = au_create,
	.destroy = au_destroy,
	.update = au_update,
	.get_defaults = au_get_defaults,
	.get_properties = au_get_properties,
	.get_width = au_get_width,
	.get_height = au_get_height,
	.video_render = au_render,
	.video_tick = au_tick,
};

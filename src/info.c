/*
 * Info tile: clock, time-of-day timecode, date, the OBS video format and a text of your own.
 * By default a tile shows one of them as a single line that fills the tile; "All" stacks them.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <time.h>
#include <string.h>
#include "plugin-macros.generated.h"
#include "text-render.h"

#define INFO_W 480
#define INFO_H 200
#define INFO_LINE_H 72

enum info_item {
	INFO_ALL,
	INFO_TIMECODE,
	INFO_CLOCK,
	INFO_DATE,
	INFO_VIDEO,
	INFO_TEXT,
};

struct info_source
{
	obs_source_t *self;
	char *text;
	bool show_clock, show_timecode, show_date, show_video;
	float size;
	int item; // info_item
};

static const char *info_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Info");
}

static void info_update(void *data, obs_data_t *settings)
{
	struct info_source *src = data;
	bfree(src->text);
	src->text = bstrdup(obs_data_get_string(settings, "text"));
	src->show_clock = obs_data_get_bool(settings, "show_clock");
	src->show_timecode = obs_data_get_bool(settings, "show_timecode");
	src->show_date = obs_data_get_bool(settings, "show_date");
	src->show_video = obs_data_get_bool(settings, "show_video");
	src->size = (float)obs_data_get_int(settings, "text_size");
	const char *item = obs_data_get_string(settings, "item");
	src->item = !strcmp(item, "timecode") ? INFO_TIMECODE
		    : !strcmp(item, "clock")  ? INFO_CLOCK
		    : !strcmp(item, "date")   ? INFO_DATE
		    : !strcmp(item, "video")  ? INFO_VIDEO
		    : !strcmp(item, "text")   ? INFO_TEXT
					      : INFO_ALL;
}

static void *info_create(obs_data_t *settings, obs_source_t *source)
{
	struct info_source *src = bzalloc(sizeof(struct info_source));
	src->self = source;
	info_update(src, settings);
	return src;
}

static void info_destroy(void *data)
{
	struct info_source *src = data;
	bfree(src->text);
	bfree(src);
}

static void info_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "show_clock", false);
	obs_data_set_default_bool(settings, "show_timecode", true);
	obs_data_set_default_bool(settings, "show_date", true);
	obs_data_set_default_bool(settings, "show_video", true);
	obs_data_set_default_int(settings, "text_size", 28);
	obs_data_set_default_string(settings, "text", "");
	obs_data_set_default_string(settings, "item", "timecode");
}

static obs_properties_t *info_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_list(props, "item", obs_module_text("Info.Prop.Show"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Info.Prop.Timecode"), "timecode");
	obs_property_list_add_string(prop, obs_module_text("Info.Prop.Clock"), "clock");
	obs_property_list_add_string(prop, obs_module_text("Info.Prop.Date"), "date");
	obs_property_list_add_string(prop, obs_module_text("Info.Prop.Video"), "video");
	obs_property_list_add_string(prop, obs_module_text("Info.Prop.Text"), "text");
	obs_property_list_add_string(prop, obs_module_text("Info.Prop.All"), "all");
	obs_properties_add_text(props, "text", obs_module_text("Info.Prop.Text"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, "show_timecode", obs_module_text("Info.Prop.Timecode"));
	obs_properties_add_bool(props, "show_clock", obs_module_text("Info.Prop.Clock"));
	obs_properties_add_bool(props, "show_date", obs_module_text("Info.Prop.Date"));
	obs_properties_add_bool(props, "show_video", obs_module_text("Info.Prop.Video"));
	obs_properties_add_int_slider(props, "text_size", obs_module_text("Info.Prop.Size"), 10, 72, 1);
	return props;
}

static uint32_t info_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	return INFO_W;
}

static uint32_t info_get_height(void *data)
{
	const struct info_source *src = data;
	return src->item == INFO_ALL ? INFO_H : INFO_LINE_H;
}

static const char *colorspace_name(enum video_colorspace cs)
{
	switch (cs) {
	case VIDEO_CS_601:
		return "Rec. 601";
	case VIDEO_CS_SRGB:
		return "sRGB";
	case VIDEO_CS_2100_PQ:
		return "Rec. 2100 PQ";
	case VIDEO_CS_2100_HLG:
		return "Rec. 2100 HLG";
	default:
		return "Rec. 709";
	}
}

static void info_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct info_source *src = data;
	const float W = (float)INFO_W, H = (float)info_get_height(src);
	const struct text_scale ts = text_scale_from_viewport(W, H);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF101013);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(0.0f, 0.0f);
		gs_vertex2f(W, 0.0f);
		gs_vertex2f(0.0f, H);
		gs_vertex2f(W, H);
		gs_render_stop(GS_TRISTRIP);
	}

	struct obs_video_info ovi = {0};
	obs_get_video_info(&ovi);
	const double fps = ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 30.0;

	const uint64_t ns = os_gettime_ns();
	time_t now = time(NULL);
	struct tm tm;
#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	// frames within the second, from the monotonic clock
	const int frames = (int)((double)(ns % 1000000000ull) / 1e9 * fps);

	char buf[256];
	if (src->item != INFO_ALL) {
		// one line, as large as the tile allows
		uint32_t color = 0xFFF0F0F0;
		switch (src->item) {
		case INFO_TIMECODE:
			snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec, frames);
			color = 0xFF6FCF86;
			break;
		case INFO_CLOCK:
			snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
			break;
		case INFO_DATE:
			snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
			break;
		case INFO_VIDEO:
			snprintf(buf, sizeof(buf), "%ux%u  %.3g fps  %s", ovi.base_width, ovi.base_height, fps,
				 colorspace_name(ovi.colorspace));
			break;
		default:
			snprintf(buf, sizeof(buf), "%s", src->text ? src->text : "");
			break;
		}
		const float h = (float)INFO_LINE_H;
		float px = h * 0.62f / ts.uy; // text height in screen pixels for 62 % of the tile
		const float w = text_width(&ts, buf, px);
		if (w > W * 0.94f)
			px *= W * 0.94f / w;
		text_draw(&ts, buf, W * 0.5f, h * 0.5f, px, color, TEXT_ALIGN_CENTER, TEXT_VALIGN_MIDDLE);
		return;
	}

	const float big = src->size, small = src->size * 0.5f > 11.0f ? src->size * 0.5f : 11.0f;
	float y = 8.0f * ts.uy;
	const float x = W * 0.5f;

	if (src->text && *src->text) {
		text_draw(&ts, src->text, x, y, small * 1.2f, 0xFFF0F0F0, TEXT_ALIGN_CENTER, TEXT_VALIGN_TOP);
		y += (small * 1.2f + 6.0f) * ts.uy;
	}
	if (src->show_timecode) {
		snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec, frames);
		text_draw(&ts, buf, x, y, big, 0xFF6FCF86, TEXT_ALIGN_CENTER, TEXT_VALIGN_TOP);
		y += (big + 6.0f) * ts.uy;
	}
	if (src->show_clock) {
		snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
		text_draw(&ts, buf, x, y, big, 0xFFF0F0F0, TEXT_ALIGN_CENTER, TEXT_VALIGN_TOP);
		y += (big + 6.0f) * ts.uy;
	}
	if (src->show_date) {
		snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
		text_draw(&ts, buf, x, y, small, 0xFFB8B8C0, TEXT_ALIGN_CENTER, TEXT_VALIGN_TOP);
		y += (small + 5.0f) * ts.uy;
	}
	if (src->show_video) {
		snprintf(buf, sizeof(buf), "%ux%u  %.3g fps  %s", ovi.base_width, ovi.base_height, fps,
			 colorspace_name(ovi.colorspace));
		text_draw(&ts, buf, x, y, small, 0xFFB8B8C0, TEXT_ALIGN_CENTER, TEXT_VALIGN_TOP);
	}
}

const struct obs_source_info colormonitor_info = {
	.id = ID_PREFIX "info_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = info_get_name,
	.create = info_create,
	.destroy = info_destroy,
	.update = info_update,
	.get_defaults = info_get_defaults,
	.get_properties = info_get_properties,
	.get_width = info_get_width,
	.get_height = info_get_height,
	.video_render = info_render,
};

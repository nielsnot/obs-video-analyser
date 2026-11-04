#include <obs-module.h>
#include <util/platform.h>
#include <QStandardPaths>
#include "plugin-macros.generated.h"
#include "va-config.hpp"

static obs_data_t *config = nullptr;

static std::string config_file()
{
	return va_config_dir() + "config.json";
}

static void set_defaults(obs_data_t *d)
{
	const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	const std::string dir = (pictures + QStringLiteral("/Video Analyser")).toStdString();
	obs_data_set_default_string(d, "snap_dir", dir.c_str());
	obs_data_set_default_string(d, "snap_template", "{source} {date} {time}");
	obs_data_set_default_bool(d, "snap_autosave", false);
}

std::string va_config_dir()
{
	char *path = obs_module_config_path("");
	std::string dir = path ? path : "";
	bfree(path);
	if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
		dir += '/';
	if (!dir.empty())
		os_mkdirs(dir.c_str());
	return dir;
}

obs_data_t *va_config()
{
	if (!config) {
		const std::string file = config_file();
		config = obs_data_create_from_json_file_safe(file.c_str(), "bak");
		if (!config)
			config = obs_data_create();
		set_defaults(config);
	}
	return config;
}

void va_config_save()
{
	if (!config)
		return;
	const std::string file = config_file();
	if (!obs_data_save_json_safe(config, file.c_str(), "tmp", "bak"))
		blog(LOG_WARNING, "Could not save '%s'", file.c_str());
}

void va_config_replace(obs_data_t *data)
{
	obs_data_release(config);
	config = data ? obs_data_create_from_json(obs_data_get_json(data)) : obs_data_create();
	set_defaults(config);
	va_config_save();
}

void va_config_release()
{
	obs_data_release(config);
	config = nullptr;
}

obs_data_array_t *va_layouts()
{
	obs_data_array_t *a = obs_data_get_array(va_config(), "layouts");
	return a ? a : obs_data_array_create();
}

void va_layout_store(const char *name, obs_data_t *state)
{
	if (!name || !*name || !state)
		return;
	obs_data_array_t *a = va_layouts();
	const size_t n = obs_data_array_count(a);
	bool replaced = false;
	for (size_t i = 0; i < n && !replaced; i++) {
		obs_data_t *item = obs_data_array_item(a, i);
		if (strcmp(obs_data_get_string(item, "name"), name) == 0) {
			obs_data_set_obj(item, "state", state);
			replaced = true;
		}
		obs_data_release(item);
	}
	if (!replaced) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", name);
		obs_data_set_obj(item, "state", state);
		obs_data_array_push_back(a, item);
		obs_data_release(item);
	}
	obs_data_set_array(va_config(), "layouts", a);
	obs_data_array_release(a);
	va_config_save();
}

void va_layout_delete(const char *name)
{
	obs_data_array_t *a = va_layouts();
	for (size_t i = obs_data_array_count(a); i-- > 0;) {
		obs_data_t *item = obs_data_array_item(a, i);
		if (strcmp(obs_data_get_string(item, "name"), name) == 0)
			obs_data_array_erase(a, i);
		obs_data_release(item);
	}
	obs_data_set_array(va_config(), "layouts", a);
	obs_data_array_release(a);
	va_config_save();
}

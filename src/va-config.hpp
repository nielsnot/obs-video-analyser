#pragma once

#include <obs.h>
#include <string>

/* Settings of the plugin that belong to no scene collection: the layout library and the snapshot library.
 * They are kept in config.json in the plugin's config folder. */

/* The settings; not a new reference. Changes are written by va_config_save(). */
obs_data_t *va_config();
void va_config_save();
/* Replaces all of them (import), or clears them to the defaults with NULL (factory reset). */
void va_config_replace(obs_data_t *data);
void va_config_release();

/* The plugin's config folder, with a trailing slash; created when missing. */
std::string va_config_dir();

/* Layout library: named views (scopes, settings and arrangement) that every dock can open. */
obs_data_array_t *va_layouts(); // a new reference
void va_layout_store(const char *name, obs_data_t *state);
void va_layout_delete(const char *name);

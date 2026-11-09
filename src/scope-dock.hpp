#pragma once

extern "C" void scope_dock_add(const char *name, obs_data_t *props, bool show);
extern "C" void scope_dock_deleted(class ScopeWidget *);
extern "C" void scope_docks_init();
/* Shows the pause state on every dock's toolbar. */
void scope_docks_update_pause();
/* Factory reset of the whole plugin, after asking and with a backup first. */
void scope_docks_factory_reset_dialog(class QWidget *parent);

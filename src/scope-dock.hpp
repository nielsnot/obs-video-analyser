#pragma once

extern "C" void scope_dock_add(const char *name, obs_data_t *props, bool show);
extern "C" void scope_dock_deleted(class ScopeWidget *);
extern "C" void scope_docks_init();
/* Shows the pause state on every dock's toolbar. */
void scope_docks_update_pause();

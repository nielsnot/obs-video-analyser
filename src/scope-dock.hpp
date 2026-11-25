#pragma once

extern "C" void scope_dock_add(const char *name, obs_data_t *props, bool show);
extern "C" void scope_dock_deleted(class ScopeWidget *);
extern "C" void scope_docks_init();
/* Shows the pause state on every dock's toolbar. */
void scope_docks_update_pause();
/* Window mode: the dock out of OBS and filling a screen (by name; empty for the one it is on) in one step;
 * again puts it back where it was. */
void scope_dock_window_toggle(class QDockWidget *d, const class QString &screen);
bool scope_dock_in_window_mode(class QDockWidget *d);
/* All scope docks, for moving scopes from one to another. */
#include <vector>
std::vector<class ScopeWidget *> scope_docks_all();
/* The dock used last, for the hotkeys that act on one dock. */
void scope_dock_set_active(class ScopeWidget *w);
/* Factory reset of the whole plugin, after asking and with a backup first. */
void scope_docks_factory_reset_dialog(class QWidget *parent);

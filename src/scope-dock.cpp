#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAction>
#include <QMainWindow>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QDialog>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QToolButton>
#include <QStyle>
#include <QScreen>
#include <QMouseEvent>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <util/platform.h>
#include "common.h"
#include "va-config.hpp"
#include "plugin-macros.generated.h"
#include "scope-dock.hpp"
#include "scope-dock-new-dialog.hpp"
#include "scope-widget.hpp"

#define SAVE_DATA_NAME PLUGIN_NAME "-dock"
#define OBJ_NAME_SUFFIX "_scope_dock"

static std::vector<ScopeWidget *> *docks;

static inline bool is_program_dock(obs_data_t *props)
{
	bool ret = true;

	obs_data_t *roi_prop = obs_data_get_obj(props, "roi-prop");
	const char *target_name = obs_data_get_string(roi_prop, "target_name");
	if (target_name && *target_name)
		ret = false; // not program
	obs_data_release(roi_prop);

	return ret;
}

static void scope_dock_show(ScopeWidget *w);

/* ---------- a floating dock: maximize and restore like any window ---------- */

static void toggle_maximized(QDockWidget *d)
{
	auto *btn = d->titleBarWidget() ? d->titleBarWidget()->findChild<QToolButton *>("VideoAnalyserMaximize")
					: nullptr;
	if (!d->property("va_maximized").toBool()) {
		QScreen *screen = d->screen();
		if (!screen)
			return;
		d->setProperty("va_normal_geometry", d->geometry());
		d->setGeometry(screen->availableGeometry());
		d->setProperty("va_maximized", true);
		if (btn) {
			btn->setIcon(d->style()->standardIcon(QStyle::SP_TitleBarNormalButton, nullptr, d));
			btn->setToolTip(QString::fromUtf8(obs_module_text("dock.window.restore")));
		}
	} else {
		const QRect g = d->property("va_normal_geometry").toRect();
		if (g.isValid())
			d->setGeometry(g);
		d->setProperty("va_maximized", false);
		if (btn) {
			btn->setIcon(d->style()->standardIcon(QStyle::SP_TitleBarMaxButton, nullptr, d));
			btn->setToolTip(QString::fromUtf8(obs_module_text("dock.window.maximize")));
		}
	}
}

/* Double click on the title of a floating dock maximizes it, as on any window (instead of docking it again). */
class TitleDoubleClick : public QObject {
public:
	explicit TitleDoubleClick(QDockWidget *dock) : QObject(dock), d(dock) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		if (event->type() == QEvent::MouseButtonDblClick) {
			toggle_maximized(d);
			return true;
		}
		return QObject::eventFilter(obj, event);
	}

private:
	QDockWidget *d;
};

/* While floating, the dock gets a title bar with maximize, dock and close buttons; docked, OBS's own again. */
static void floating_title(QDockWidget *d, bool floating)
{
	QWidget *old = d->titleBarWidget();
	if (!floating) {
		d->setProperty("va_maximized", false);
		if (old) {
			d->setTitleBarWidget(nullptr);
			old->deleteLater();
		}
		return;
	}
	if (old)
		return;
	auto T = [](const char *k) { return QString::fromUtf8(obs_module_text(k)); };
	auto *bar = new QWidget(d);
	bar->setObjectName("VideoAnalyserFloatingTitle");
	auto *h = new QHBoxLayout(bar);
	h->setContentsMargins(8, 2, 2, 2);
	h->setSpacing(2);
	auto *title = new QLabel(d->windowTitle(), bar);
	QObject::connect(d, &QDockWidget::windowTitleChanged, title, &QLabel::setText);
	h->addWidget(title, 1);
	auto button = [&](QStyle::StandardPixmap px, const char *tip, const char *object) {
		auto *b = new QToolButton(bar);
		b->setObjectName(object);
		b->setIcon(d->style()->standardIcon(px, nullptr, d));
		b->setAutoRaise(true);
		b->setToolTip(T(tip));
		h->addWidget(b);
		return b;
	};
	QToolButton *max = button(QStyle::SP_TitleBarMaxButton, "dock.window.maximize", "VideoAnalyserMaximize");
	QToolButton *dock = button(QStyle::SP_TitleBarShadeButton, "dock.window.dock", "VideoAnalyserDockBack");
	QToolButton *close = button(QStyle::SP_TitleBarCloseButton, "dock.window.close", "VideoAnalyserClose");
	QObject::connect(max, &QToolButton::clicked, d, [d]() { toggle_maximized(d); });
	QObject::connect(dock, &QToolButton::clicked, d, [d]() {
		if (d->property("va_maximized").toBool())
			toggle_maximized(d);
		d->setFloating(false);
	});
	QObject::connect(close, &QToolButton::clicked, d, &QDockWidget::close);
	bar->installEventFilter(new TitleDoubleClick(d));
	title->installEventFilter(new TitleDoubleClick(d));
	d->setTitleBarWidget(bar);
}

static void window_buttons(QWidget *container)
{
	QMetaObject::invokeMethod(
		container,
		[container]() {
			QDockWidget *d = nullptr;
			for (QWidget *p = container->parentWidget(); p && !d; p = p->parentWidget())
				d = qobject_cast<QDockWidget *>(p);
			if (!d || d->property("va_window_buttons").toBool())
				return;
			d->setProperty("va_window_buttons", true);
			QObject::connect(d, &QDockWidget::topLevelChanged, d,
					 [d](bool floating) { floating_title(d, floating); });
			floating_title(d, d->isFloating());
		},
		Qt::QueuedConnection);
}

void scope_dock_add(const char *name, obs_data_t *props, bool show)
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	// The dock holds a slim selection toolbar above the scope display.
	QWidget *container = new QWidget(main_window);
	container->setObjectName("VideoAnalyserDock");
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	ScopeWidget *w = new ScopeWidget(container);
	w->name = name;
	layout->addWidget(w->createToolbar(container));
	layout->addWidget(w, 1);
	w->load_properties(props);
	if (!obs_frontend_add_dock_by_id(name, name, container)) {
		delete container;
		return;
	}

	if (docks)
		docks->push_back(w);
	window_buttons(container);

	if (show)
		scope_dock_show(w);
}

static void scope_dock_show(ScopeWidget *w)
{
	QMetaObject::invokeMethod(
		w,
		[w]() {
			auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (!main_window)
				return;
			QList<QDockWidget *> dd = main_window->findChildren<QDockWidget *>();
			for (QDockWidget *d : dd) {
				if (d->widget() == w || d->widget() == w->parentWidget()) {
					d->setVisible(true);
				}
			}
		},
		Qt::QueuedConnection);
}

static void close_all_docks()
{
	if (docks && docks->size()) {
		blog(LOG_INFO, "Closing %d remaining scope docks...", (int)docks->size());
		while (docks->size()) {
			ScopeWidget *w = (*docks)[docks->size() - 1];
			QWidget *container = w->parentWidget();
			w->close();
			// Deleting the container also deletes the scope widget, which removes itself from docks.
			if (container)
				delete container;
			else
				delete w;
		}
		blog(LOG_INFO, "Closed all remaining scope docks.");
	}
}

static void hotkeys_save(obs_data_t *props);
static void hotkeys_load(obs_data_t *props);

static void save_load_scope_docks(obs_data_t *save_data, bool saving, void *)
{
	blog(LOG_INFO, "save_load_scope_docks saving=%d", (int)saving);
	if (!docks)
		return;
	if (saving) {
		obs_data_t *props = obs_data_create();
		obs_data_array_t *array = obs_data_array_create();
		for (size_t i = 0; i < docks->size(); i++) {
			ScopeWidget *w = (*docks)[i];
			const char *name = w->name.c_str();
			obs_data_t *obj = obs_data_create();
			w->save_properties(obj);
			obs_data_set_string(obj, "name", name);
			obs_data_array_push_back(array, obj);
			obs_data_release(obj);
		}
		obs_data_set_array(props, "docks", array);
		hotkeys_save(props);
		obs_data_set_obj(save_data, SAVE_DATA_NAME, props);
		obs_data_array_release(array);
		obs_data_release(props);
	}

	else /* loading */ {
		close_all_docks();

		obs_data_t *props = obs_data_get_obj(save_data, SAVE_DATA_NAME);
		if (!props) {
			blog(LOG_INFO, "save_load_scope_docks: creating default properties");
			props = obs_data_create();
		}

		hotkeys_load(props);
		obs_data_array_t *array = obs_data_get_array(props, "docks");
		size_t count = obs_data_array_count(array);
		blog(LOG_INFO, "save_load_scope_docks: loading %d docks", (int)count);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *obj = obs_data_array_item(array, i);
			ScopeWidget::default_properties(obj);
			const char *name = obs_data_get_string(obj, "name");
			if (!name)
				name = "Video Analyser";
			scope_dock_add(name, obj, false);
			obs_data_release(obj);
		}
		obs_data_array_release(array);
		obs_data_release(props);
	}
}

static void scope_docks_release();

/* ---------- hotkeys: they act on the scopes of every dock (also from a Stream Deck through OBS) ---------- */

struct va_hotkey
{
	const char *name, *desc_key, *proc, *cmd;
	obs_hotkey_id id;
};

static va_hotkey hotkeys[] = {
	{"VideoAnalyser.ZoomBlacks", "Hotkey.ZoomBlacks", "va_waveform", "zoom_blacks", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.ZoomWhites", "Hotkey.ZoomWhites", "va_waveform", "zoom_whites", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.ZoomReset", "Hotkey.ZoomReset", "va_waveform", "zoom_reset", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.Peaks", "Hotkey.Peaks", "va_waveform", "peaks", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.PeakHold", "Hotkey.PeakHold", "va_waveform", "hold", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.PeakHoldReset", "Hotkey.PeakHoldReset", "va_waveform", "hold_reset", OBS_INVALID_HOTKEY_ID},
	// dock commands (no proc)
	{"VideoAnalyser.ViewNext", "Hotkey.ViewNext", nullptr, "view_next", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.ViewPrev", "Hotkey.ViewPrev", nullptr, "view_prev", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.View1", "Hotkey.View1", nullptr, "view_1", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.View2", "Hotkey.View2", nullptr, "view_2", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.View3", "Hotkey.View3", nullptr, "view_3", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.View4", "Hotkey.View4", nullptr, "view_4", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.View5", "Hotkey.View5", nullptr, "view_5", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.Snapshot", "Hotkey.Snapshot", nullptr, "snapshot", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.Compare", "Hotkey.Compare", nullptr, "compare", OBS_INVALID_HOTKEY_ID},
	{"VideoAnalyser.Pause", "Hotkey.Pause", nullptr, "pause", OBS_INVALID_HOTKEY_ID},
};

static void hotkey_pressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	// the docks live on the UI thread
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			const va_hotkey *h = static_cast<const va_hotkey *>(param);
			if (!docks)
				return;
			if (!h->proc && strcmp(h->cmd, "pause") == 0) {
				cm_set_paused(!cm_get_paused());
				scope_docks_update_pause();
				return;
			}
			for (ScopeWidget *w : *docks) {
				if (h->proc)
					w->sendCommand(h->proc, h->cmd);
				else
					w->dockCommand(h->cmd);
			}
		},
		data, false);
}

static void hotkeys_register()
{
	for (va_hotkey &h : hotkeys)
		h.id = obs_hotkey_register_frontend(h.name, obs_module_text(h.desc_key), hotkey_pressed, &h);
}

static void hotkeys_unregister()
{
	for (va_hotkey &h : hotkeys) {
		if (h.id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(h.id);
		h.id = OBS_INVALID_HOTKEY_ID;
	}
}

static void hotkeys_save(obs_data_t *props)
{
	obs_data_t *keys = obs_data_create();
	for (va_hotkey &h : hotkeys) {
		obs_data_array_t *a = obs_hotkey_save(h.id);
		obs_data_set_array(keys, h.name, a);
		obs_data_array_release(a);
	}
	obs_data_set_obj(props, "hotkeys", keys);
	obs_data_release(keys);
}

static void hotkeys_load(obs_data_t *props)
{
	obs_data_t *keys = obs_data_get_obj(props, "hotkeys");
	for (va_hotkey &h : hotkeys) {
		obs_data_array_t *a = keys ? obs_data_get_array(keys, h.name) : NULL;
		obs_hotkey_load(h.id, a);
		obs_data_array_release(a);
	}
	obs_data_release(keys);
}

void scope_docks_update_pause()
{
	if (!docks)
		return;
	for (ScopeWidget *w : *docks)
		w->updatePause();
}

/* ---------- export, import, factory reset ---------- */

/* Everything: the docks with their views and settings, the hotkeys, the layout and snapshot libraries. */
static obs_data_t *export_all()
{
	obs_data_t *all = obs_data_create();
	obs_data_set_int(all, "video-analyser-backup", 1);
	obs_data_set_string(all, "version", PLUGIN_VERSION);
	obs_data_set_string(all, "date", QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData());
	obs_data_t *props = obs_data_create();
	save_load_scope_docks(props, true, nullptr);
	obs_data_t *dock_data = obs_data_get_obj(props, SAVE_DATA_NAME);
	obs_data_set_obj(all, "docks", dock_data);
	obs_data_release(dock_data);
	obs_data_release(props);
	obs_data_set_obj(all, "config", va_config());
	return all;
}

static void remove_all_docks()
{
	if (!docks)
		return;
	std::vector<std::string> names;
	for (ScopeWidget *w : *docks)
		names.push_back(w->name);
	for (const std::string &n : names)
		obs_frontend_remove_dock(n.c_str()); // deletes the dock and its scope widget
	close_all_docks();                           // any left
}

static void import_all(obs_data_t *all)
{
	obs_data_t *dock_data = obs_data_get_obj(all, "docks");
	obs_data_t *config_data = obs_data_get_obj(all, "config");
	va_config_replace(config_data);
	remove_all_docks();
	obs_data_t *wrap = obs_data_create();
	if (dock_data)
		obs_data_set_obj(wrap, SAVE_DATA_NAME, dock_data);
	save_load_scope_docks(wrap, false, nullptr);
	obs_data_release(wrap);
	obs_data_release(config_data);
	obs_data_release(dock_data);
	if (docks)
		for (ScopeWidget *w : *docks)
			scope_dock_show(w);
}

static std::string backup_dir()
{
	const std::string dir = va_config_dir() + "backups/";
	os_mkdirs(dir.c_str());
	return dir;
}

/* Writes a backup of everything to the backups folder; returns its path, or empty on failure. */
static QString write_backup(const char *why)
{
	obs_data_t *all = export_all();
	const QString file = QString::fromStdString(backup_dir()) +
			     QStringLiteral("backup %1 %2.json")
				     .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")))
				     .arg(QString::fromUtf8(why));
	const bool ok = obs_data_save_json_pretty_safe(all, file.toUtf8().constData(), "tmp", "bak");
	obs_data_release(all);
	return ok ? file : QString();
}

static void factory_reset()
{
	remove_all_docks();
	for (va_hotkey &h : hotkeys) {
		obs_data_array_t *empty = obs_data_array_create();
		obs_hotkey_load(h.id, empty);
		obs_data_array_release(empty);
	}
	va_config_replace(nullptr);
	cm_set_paused(false);
}

/* The dialogs hang on the main window: a dock that asks is removed by the reset itself. */
void scope_docks_factory_reset_dialog(QWidget *)
{
	auto *parent = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto T = [](const char *k) { return QString::fromUtf8(obs_module_text(k)); };
	if (QMessageBox::warning(parent, T("Settings.Title"), T("Settings.Reset.Confirm"),
				 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
		return;
	const QString file = write_backup("before reset");
	if (file.isEmpty() &&
	    QMessageBox::warning(parent, T("Settings.Title"), T("Settings.Reset.NoBackup"),
				 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
		return;
	factory_reset();
	QMessageBox::information(parent, T("Settings.Title"),
				 T("Settings.Reset.Done").arg(QDir::toNativeSeparators(file)));
}

static void open_settings_dialog()
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dlg = new QDialog(main_window);
	dlg->setAttribute(Qt::WA_DeleteOnClose, true);
	dlg->setWindowTitle(QString::fromUtf8(obs_module_text("Settings.Title")));
	auto *v = new QVBoxLayout(dlg);
	auto *info = new QLabel(QString::fromUtf8(obs_module_text("Settings.Info")), dlg);
	info->setWordWrap(true);
	v->addWidget(info);
	auto T = [](const char *k) { return QString::fromUtf8(obs_module_text(k)); };
	auto *btnExport = new QPushButton(T("Settings.Export"), dlg);
	auto *btnImport = new QPushButton(T("Settings.Import"), dlg);
	auto *btnBackups = new QPushButton(T("Settings.Backups"), dlg);
	auto *btnReset = new QPushButton(T("Settings.Reset"), dlg);
	auto *btnClose = new QPushButton(T("Close"), dlg);
	for (QPushButton *b : {btnExport, btnImport, btnBackups, btnReset})
		v->addWidget(b);
	v->addStretch(1);
	v->addWidget(btnClose);

	QObject::connect(btnExport, &QPushButton::clicked, dlg, [dlg, T]() {
		const QString name = QStringLiteral("Video Analyser settings %1.json")
					     .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")));
		const QString file = QFileDialog::getSaveFileName(
			dlg, T("Settings.Export"),
			QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/") +
				name,
			QStringLiteral("JSON (*.json)"));
		if (file.isEmpty())
			return;
		obs_data_t *all = export_all();
		const bool ok = obs_data_save_json_pretty_safe(all, file.toUtf8().constData(), "tmp", "bak");
		obs_data_release(all);
		if (!ok)
			QMessageBox::warning(dlg, T("Settings.Title"), T("Settings.Failed"));
	});
	QObject::connect(btnImport, &QPushButton::clicked, dlg, [dlg, T]() {
		const QString file = QFileDialog::getOpenFileName(
			dlg, T("Settings.Import"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
			QStringLiteral("JSON (*.json)"));
		if (file.isEmpty())
			return;
		obs_data_t *all = obs_data_create_from_json_file(file.toUtf8().constData());
		if (!all || !obs_data_get_int(all, "video-analyser-backup")) {
			obs_data_release(all);
			QMessageBox::warning(dlg, T("Settings.Title"), T("Settings.NotBackup"));
			return;
		}
		if (QMessageBox::question(dlg, T("Settings.Title"), T("Settings.Import.Confirm")) != QMessageBox::Yes) {
			obs_data_release(all);
			return;
		}
		write_backup("before import");
		import_all(all);
		obs_data_release(all);
	});
	QObject::connect(btnBackups, &QPushButton::clicked, dlg, []() {
		QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(backup_dir())));
	});
	QObject::connect(btnReset, &QPushButton::clicked, dlg, [dlg]() { scope_docks_factory_reset_dialog(dlg); });
	QObject::connect(btnClose, &QPushButton::clicked, dlg, &QDialog::close);
	dlg->resize(420, 300);
	dlg->show();
}

static void frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		close_all_docks();

		if (event == OBS_FRONTEND_EVENT_EXIT)
			scope_docks_release();
		break;
	default:
		break;
	}
}

void scope_docks_init()
{
	docks = new std::vector<ScopeWidget *>;
	hotkeys_register();
	obs_frontend_add_save_callback(save_load_scope_docks, NULL);
	obs_frontend_add_event_callback(frontend_event, nullptr);

	QAction *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("New Scope Dock...")));
	action->setObjectName("actionScopeDockNew");
	auto cb = [] {
		obs_frontend_push_ui_translation(obs_module_get_string);
		auto *dialog = new ScopeDockNewDialog(static_cast<QMainWindow *>(obs_frontend_get_main_window()));
		dialog->show();
		dialog->setAttribute(Qt::WA_DeleteOnClose, true);
		obs_frontend_pop_ui_translation();
	};
	QAction::connect(action, &QAction::triggered, cb);

	QAction *settings = static_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction(obs_module_text("Video Analyser Settings and Backup...")));
	settings->setObjectName("actionVideoAnalyserSettings");
	QAction::connect(settings, &QAction::triggered, [] { open_settings_dialog(); });
}

void scope_docks_release()
{
	hotkeys_unregister();
	va_config_release();
	delete docks;
	docks = NULL;

	obs_frontend_remove_save_callback(save_load_scope_docks, NULL);
	obs_frontend_remove_event_callback(frontend_event, nullptr);
}

void scope_dock_deleted(class ScopeWidget *widget)
{
	if (!docks)
		return;

	for (size_t i = 0; i < docks->size(); i++) {
		if ((*docks)[i] != widget)
			continue;

		docks->erase(docks->begin() + i);
		break;
	}
}

#pragma once

#include <functional>

#include <obs.h>
#include <QWidget>
#include <QRect>
#include <memory>
#include <string>
#include <NorisQTDisplay.hpp>

class ScopeWidget : public NorisQTDisplay {
	Q_OBJECT

	struct scope_widget_s *data;
	class ScopeWidgetProperties *properties;

	// toolbar
	QWidget *toolbar = nullptr;
	class QToolButton *btnRect = nullptr;
	class QToolButton *btnEllipse = nullptr;
	class QToolButton *btnPin = nullptr;
	class QToolButton *btnCompare = nullptr;
	class QToolButton *btnArrange = nullptr;
	class QToolButton *btnFaces = nullptr;
	class QToolButton *btnFull = nullptr;
	class QToolButton *btnPause = nullptr;
	class QDialog *snapshotDialog = nullptr;
	class QDialog *signalDialog = nullptr;
	std::function<void()> snapshotRefresh;
	bool fullWasFloating = false;
	QRect fullRestore;
	bool comparing = false;
	bool toolbarVisible = true;
	int drawMode = 0;
	class QTabBar *tabs = nullptr;
	bool rebuildingTabs = false;

public:
	std::string name;

private:
	void closeEvent(QCloseEvent *event) override;
	void RegisterCallbackToDisplay();

	// for interactions
	bool HandleMouseClickEvent(QMouseEvent *event);
	bool HandleMouseMoveEvent(QMouseEvent *event);
	bool HandleMouseWheelEvent(QWheelEvent *event);
	bool HandleKeyEvent(QKeyEvent *event);
	bool HandleContextMenuEvent(class QContextMenuEvent *event);
	void openContextAt(const QPointF &widgetPos, const QPoint &globalPos);
	qint64 lastContextMs = 0;
	bool openMenu(QMouseEvent *event);
	void fillAddScopeMenu(class QMenu *menu);
	void closeProperties();
	void clearActive();
	void loadActive(obs_data_t *state);
	void storeActive();
	void rebuildTabs();
	void openTabMenu(const QPoint &pos);
	void openOptions(int tile, const QPoint &globalPos);
	void setArranging(bool on);
	void exportScreenshot();
	void setFullScreen(bool on);
	void updateScreenScale();
	class QDockWidget *findDock() const;
	void quickControl(int tile, const struct qb_item &it, int x);
	void addMarkerControls(class QWidget *panel, class QVBoxLayout *vbox, obs_source_t *src, bool vectorscope,
			       const std::function<void()> &reopen);
	void insertScope(int type, obs_data_t *prop, obs_data_t *meta, int index);
	void fillLayoutMenu(class QMenu *menu);
	QString snapshotName() const;
	bool saveSnapshotPicture(const QString &file);

public slots:
	void RemoveDock();
	void toggleSolo(int tile);
	void openProperties(int tile);
	void addScope(int type, obs_data_t *settings = nullptr);
	void removeScope(int tile);
	void setDrawMode(int mode);
	void undoShape();
	void resetShapes();
	void setToolbarVisible(bool visible);
	void takeSnapshot();
	void setCompare(bool on);
	void moveScope(int tile, int delta);
	void duplicateScope(int tile);
	void changeScopeType(int tile, int type);
	void switchView(int index);
	void addView();
	void renameView(int index);
	void duplicateView(int index);
	void deleteView(int index);
	void equalizeTiles(bool widths);
	void saveLayout();
	void openLayout(const QString &name);
	void openSnapshotLibrary();
	void openSignalPath();
	void loadReference(const QString &file);
	void clearDock();

public:
	ScopeWidget(QWidget *parent);
	~ScopeWidget();
	QWidget *createToolbar(QWidget *parent);
	static void default_properties(obs_data_t *);
	void save_properties(obs_data_t *);
	/* Calls proc(cmd) on every scope of the dock that has it, for example va_waveform("zoom_blacks"). */
	void sendCommand(const char *proc, const char *cmd);
	void load_properties(obs_data_t *);
	/* A dock command from a hotkey: view_next, view_prev, view_1..view_9, snapshot, compare. */
	void dockCommand(const char *cmd);
	/* Shows the pause state (cm_get_paused()) on the toolbar button. */
	void updatePause();
	/* The signal path of the dock as HTML: source, raw signal, project, capture, LUT, selection. */
	QString signalPathHtml();

	bool helpTipShown = false;
	void showHelp(int type);

	friend class ScopeWidgetProperties;
	friend class ScopeWidgetInteractiveEventFilter;
};

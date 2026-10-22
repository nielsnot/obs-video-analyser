#pragma once

#include <obs.h>
#include <QWidget>
#include <QDialog>
#include <QAbstractButton>
#include <QDialogButtonBox>
#include <obs.hpp>
#include "scope-widget.hpp"

/* Settings of one scope in a dock. Holds a reference, so it stays valid even if the scope is removed. */
class ScopeWidgetProperties : public QDialog {
	Q_OBJECT

private:
	OBSSource source;
	class OBSPropertiesView *view;
	class QDialogButtonBox *buttonBox;

private slots:
	void on_buttonBox_clicked(QAbstractButton *button);

public:
	ScopeWidgetProperties(QWidget *parent, obs_source_t *source, const QString &title, bool is_roi);
	~ScopeWidgetProperties();

	obs_source_t *getSource() const { return source; }
	void Init();
};

#include <obs-module.h>
#include <obs.h>
#include <obs-frontend-api.h>
#include <QPushButton>
#include <QLayout>
#include <QVBoxLayout>
#include "plugin-macros.generated.h"
#include "scope-widget-properties.hpp"
#include "properties-view.hpp"

/* The scopes in a dock always analyze the dock's ROI, so hide the target settings. */
static obs_properties_t *scopewidget_properties(const obs_source_t *source)
{
	obs_properties_t *props = obs_source_properties(source);
	if (!props)
		return NULL;
	obs_property_set_visible(obs_properties_get(props, "target_name"), false);
	obs_property_set_visible(obs_properties_get(props, "target_scale"), false);
	obs_property_set_visible(obs_properties_get(props, "bypass"), false);
	return props;
}

ScopeWidgetProperties::ScopeWidgetProperties(QWidget *parent, obs_source_t *source_, const QString &title,
					     bool is_roi)
	: QDialog(parent),
	  source(source_)
{
	setWindowTitle(title);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumWidth(360);
	setMinimumHeight(300);

	buttonBox = new QDialogButtonBox(this);
	buttonBox->setObjectName(QStringLiteral("buttonBox"));
	buttonBox->setStandardButtons(QDialogButtonBox::Ok);

	QMetaObject::connectSlotsByName(this);

	OBSDataAutoRelease settings = obs_source_get_settings(source);

	PropertiesReloadCallback prop_cb = is_roi ? (PropertiesReloadCallback)obs_source_properties
						  : (PropertiesReloadCallback)scopewidget_properties;

	PropertiesUpdateCallback update_cb = [](void *vp, obs_data_t *new_settings) {
		obs_source_t *source = static_cast<obs_source_t *>(vp);
		obs_source_update(source, new_settings);
	};

	view = new OBSPropertiesView(settings.Get(), source.Get(), prop_cb, update_cb);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(view, 1);
	layout->addWidget(buttonBox);
	setLayout(layout);
}

ScopeWidgetProperties::~ScopeWidgetProperties()
{
	auto *w = static_cast<ScopeWidget *>(parent());
	if (w && w->properties == this)
		w->properties = NULL;
}

void ScopeWidgetProperties::Init()
{
	show();
}

void ScopeWidgetProperties::on_buttonBox_clicked(QAbstractButton *)
{
	close();
}

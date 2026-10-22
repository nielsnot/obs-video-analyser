/* Common code among my plugins for OBS Studio
 * Copyright (C) 2025  Norihiro Kamae
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * - Copied from obs-studio/frontend/widgets/OBSQTDisplay.cpp
 * - Added checking display before use.
 * - Removed `destroying` so that a dock can be shown again.
 * - See NorisQTDisplay.hpp for other modifications.
 */

#include "obs.hpp"
#include "NorisQTDisplay.hpp"

#include <display-helpers.hpp>
#include <SurfaceEventFilter.hpp>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QWindow>
#ifdef ENABLE_WAYLAND
#include <QApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
#include <qpa/qplatformnativeinterface.h>
#endif
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "moc_NorisQTDisplay.cpp"

#define DEFAULT_BACKGROUND_COLOR 0xFF000000

struct private_data_s {
	OBSDisplay display;
};

static inline long long color_to_int(const QColor &color)
{
	auto shift = [&](unsigned val, int shift) {
		return ((val & 0xff) << shift);
	};

	return shift(color.red(), 0) | shift(color.green(), 8) | shift(color.blue(), 16) | shift(color.alpha(), 24);
}

static inline QColor rgba_to_color(uint32_t rgba)
{
	return QColor::fromRgb(rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 24) & 0xFF);
}

static bool QTToGSWindow(QWindow *window, gs_window &gswindow)
{
	bool success = true;

#ifdef _WIN32
	gswindow.hwnd = (HWND)window->winId();
#elif __APPLE__
	gswindow.view = (id)window->winId();
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gswindow.id = window->winId();
		gswindow.display = obs_get_nix_platform_display();
		break;
#ifdef ENABLE_WAYLAND
	case OBS_NIX_PLATFORM_WAYLAND: {
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
		QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
		gswindow.display = native->nativeResourceForWindow("surface", window);
#else
		gswindow.display = (void *)window->winId();
#endif
		success = gswindow.display != nullptr;
		break;
	}
#endif
	default:
		success = false;
		break;
	}
#endif
	return success;
}

NorisQTDisplay::NorisQTDisplay(QWidget *parent, Qt::WindowFlags flags)
	: QWidget(parent, flags),
	  priv(*new private_data_s())
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

	auto windowVisible = [this](bool visible) {
		if (!visible) {
#if !defined(_WIN32) && !defined(__APPLE__)
			priv.display = nullptr;
#endif
			return;
		}

		if (!priv.display) {
			CreateDisplay();
		} else {
			QSize size = GetPixelSize(this);
			obs_display_resize(priv.display, size.width(), size.height());
		}
	};

	auto screenChanged = [this](QScreen *) {
		CreateDisplay();

		if (!priv.display)
			return;
		QSize size = GetPixelSize(this);
		obs_display_resize(priv.display, size.width(), size.height());
	};

	connect(windowHandle(), &QWindow::visibleChanged, windowVisible);
	connect(windowHandle(), &QWindow::screenChanged, screenChanged);

	windowHandle()->installEventFilter(new NorisQTDisplay_SurfaceEventFilter(this));
}

NorisQTDisplay::~NorisQTDisplay()
{
	delete &priv;
}

void NorisQTDisplay::DestroyDisplay()
{
	priv.display = nullptr;
}

obs_display_t *NorisQTDisplay::GetDisplay() const
{
	return priv.display;
}

void NorisQTDisplay::CreateDisplay()
{
	if (priv.display)
		return;

	if (!windowHandle()->isExposed())
		return;

	QSize size = GetPixelSize(this);

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

	if (!QTToGSWindow(windowHandle(), info.window))
		return;

	priv.display = obs_display_create(&info, DEFAULT_BACKGROUND_COLOR);

	emit DisplayCreated(this);
}

void NorisQTDisplay::paintEvent(QPaintEvent *event)
{
	CreateDisplay();

	QWidget::paintEvent(event);
}

void NorisQTDisplay::moveEvent(QMoveEvent *event)
{
	QWidget::moveEvent(event);

	OnMove();
}

bool NorisQTDisplay::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_DISPLAYCHANGE:
		OnDisplayChange();
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

void NorisQTDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	CreateDisplay();

	if (isVisible() && priv.display) {
		QSize size = GetPixelSize(this);
		obs_display_resize(priv.display, size.width(), size.height());
	}

	emit DisplayResized();
}

QPaintEngine *NorisQTDisplay::paintEngine() const
{
	return nullptr;
}

void NorisQTDisplay::OnMove()
{
	if (priv.display)
		obs_display_update_color_space(priv.display);
}

void NorisQTDisplay::OnDisplayChange()
{
	if (priv.display)
		obs_display_update_color_space(priv.display);
}

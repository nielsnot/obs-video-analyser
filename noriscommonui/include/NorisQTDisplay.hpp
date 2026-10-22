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
 * - Copied from obs-studio/frontend/widgets/OBSQTDisplay.hpp
 * - Renamed OBSQTDisplay to NorisQTDisplay
 * - Moved inline function definitions to NorisQTDisplay.cpp.
 * - Moved private data into a hidden structure.
 */

#pragma once

#include <obs.h>
#include <QWidget>
#include <QtCore/QtGlobal>

#ifdef NORISCOMMONUI_STATIC
#define NORISCOMMONUI_EXPORT
#elif defined(NORISCOMMONUI_LIBRARY)
#define NORISCOMMONUI_EXPORT Q_DECL_EXPORT
#else
#define NORISCOMMONUI_EXPORT Q_DECL_IMPORT
#endif

class NORISCOMMONUI_EXPORT NorisQTDisplay : public QWidget {
	Q_OBJECT

	struct private_data_s &priv;

	virtual void paintEvent(QPaintEvent *event) override;
	virtual void moveEvent(QMoveEvent *event) override;
	virtual void resizeEvent(QResizeEvent *event) override;
	virtual bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
	void DisplayCreated(NorisQTDisplay *window);
	void DisplayResized();

public:
	NorisQTDisplay(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	~NorisQTDisplay();

	virtual QPaintEngine *paintEngine() const override;

	obs_display_t *GetDisplay() const;

	void CreateDisplay();
	void DestroyDisplay();

	void OnMove();
	void OnDisplayChange();
};

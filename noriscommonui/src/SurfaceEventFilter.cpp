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
 * - Copied from obs-studio/frontend/utility/SurfaceEventFilter.hpp
 * - Separated the implementation of eventFilter.
 */

#include <NorisQTDisplay.hpp>
#include <SurfaceEventFilter.hpp>
#include "moc_SurfaceEventFilter.cpp"

bool NorisQTDisplay_SurfaceEventFilter::eventFilter(QObject *obj, QEvent *event)
{
	bool result = QObject::eventFilter(obj, event);
	QPlatformSurfaceEvent *surfaceEvent;

	switch (event->type()) {
	case QEvent::PlatformSurface:
		surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);

		switch (surfaceEvent->surfaceEventType()) {
		case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
			display->DestroyDisplay();
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return result;
}

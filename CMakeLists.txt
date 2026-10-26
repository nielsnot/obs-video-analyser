cmake_minimum_required(VERSION 3.12)

project(obs-video-analyser VERSION 0.7.6)

set(PLUGIN_AUTHOR "Niels Notenboom, based on Color Monitor by Norihiro Kamae")

set(MACOS_BUNDLEID "io.github.obs-video-analyser")
set(ID_PREFIX "obs-video-analyser.")
set(MACOS_PACKAGE_UUID "17C7FC7C-3669-4232-93EE-BFBE2AF23DB8")
set(MACOS_INSTALLER_UUID "5F24DF5A-CC8E-4E1A-A02A-C855CDEBDCC2")
set(PLUGIN_URL "https://github.com/nielsnot/obs-video-analyser")

set(LINUX_MAINTAINER_EMAIL "notenboom.niels@gmail.com")

option(ENABLE_PROFILE "Enable profiling to see calculation time" OFF)
option(SHOW_ROI "Show ROI source to users" OFF)
option(ENABLE_COVERAGE "Enable coverage option for GCC" OFF)
option(NORISCOMMONUI_SUBMODULE "Use noriscommonui from submodule" ON)

set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# In case you need C++
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	find_package(libobs REQUIRED)
	find_package(obs-frontend-api REQUIRED)
	include(cmake/ObsPluginHelpers.cmake)
	add_library(OBS::frontend-api ALIAS OBS::obs-frontend-api)
	find_qt(VERSION ${QT_VERSION} COMPONENTS Widgets Core Gui)
endif()

if(NORISCOMMONUI_SUBMODULE)
	add_subdirectory(noriscommonui)
	add_library(noriscommonui::noriscommonui ALIAS noriscommonui)
else()
	find_package(NorisCommonUI REQUIRED)
endif()

configure_file(
	src/plugin-macros.h.in
	plugin-macros.generated.h
)

set(PLUGIN_SOURCES
	src/plugin-main.c
	src/diamond.c
	src/cie.c
	src/markers.c
	src/qc.c
	src/satlum.c
	src/lineplot.c
	src/info.c
	src/lut.c
	src/audio.c
	src/cube.c
	src/face.cpp
	src/faces.c
	src/vectorscope.c
	src/waveform.c
	src/histogram.c
	src/zebra.c
	src/focuspeaking.c
	src/roi.c
	src/common.c
	src/util.c
	src/util-cpp.cc
	src/obs-convenience.c
	src/scope-dock.cpp
	src/scope-dock-new-dialog.cpp
	src/scope-widget.cpp
	src/text-render.cpp
	src/scope-widget-properties.cpp
	src/ScopeWidgetInteractiveEventFilter.cpp
	src-obsstudio/properties-view.cpp
	src-obsstudio/vertical-scroll-area.cpp
	src-obsstudio/combobox-ignorewheel.cpp
	src-obsstudio/spinbox-ignorewheel.cpp
	src-obsstudio/double-slider.cpp
	src-obsstudio/slider-ignorewheel.cpp
	src-obsstudio/qt-wrappers.cpp
)

add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

# libfacedetection (BSD 3-clause): a small face detection CNN with its weights, no other libraries.
# Built without SIMD so it runs on every CPU; it works on a picture scaled to 320 pixels wide.
add_library(facedetection STATIC
	deps/libfacedetection/facedetectcnn.cpp
	deps/libfacedetection/facedetectcnn-model.cpp
	deps/libfacedetection/facedetectcnn-data.cpp
)
set_target_properties(facedetection PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(facedetection PUBLIC deps/libfacedetection)
if(MSVC)
	target_compile_options(facedetection PRIVATE /O2 /wd4068 /wd4244 /wd4267)
else()
	target_compile_options(facedetection PRIVATE -O2 -w)
endif()
target_link_libraries(${PROJECT_NAME} facedetection)

target_link_libraries(${PROJECT_NAME}
	OBS::libobs
	OBS::frontend-api
	Qt::Core
	Qt::Widgets
	Qt::Gui
	noriscommonui::noriscommonui
)

target_include_directories(${PROJECT_NAME}
	PRIVATE
	src-obsstudio/
	${CMAKE_CURRENT_BINARY_DIR}
)

if(OS_WINDOWS)
	# Enable Multicore Builds and disable FH4 (to not depend on VCRUNTIME140_1.DLL when building with VS2019)
	if (MSVC)
		add_definitions(/MP /d2FH4-)
	endif()

	target_link_libraries(${PROJECT_NAME} OBS::w32-pthreads)
endif()

if(OS_LINUX)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
	target_link_options(${PROJECT_NAME} PRIVATE -Wl,-z,defs)
	if(ENABLE_COVERAGE)
		target_compile_options(${PROJECT_NAME} PRIVATE -coverage)
		target_link_options(${PROJECT_NAME} PRIVATE -coverage)
		if(NORISCOMMONUI_SUBMODULE)
			target_compile_options(noriscommonui PRIVATE -coverage)
			target_link_options(noriscommonui PRIVATE -coverage)
		endif()
	endif()
endif()

if(APPLE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fvisibility=default")

	set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
	set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
	set(MACOSX_PLUGIN_BUNDLE_VERSION "${PROJECT_VERSION}")
	set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")

	if(ENABLE_COVERAGE)
		target_compile_options(${PROJECT_NAME} PRIVATE -coverage)
		target_link_options(${PROJECT_NAME} PRIVATE --coverage)
		if(NORISCOMMONUI_SUBMODULE)
			target_compile_options(noriscommonui PRIVATE -coverage)
			target_link_options(noriscommonui PRIVATE --coverage)
		endif()
	endif()
endif()

file(GENERATE OUTPUT .gitignore CONTENT "*\n")

setup_plugin_target(${PROJECT_NAME})

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	configure_file(
		installer/installer-Windows.iss.in
		installer-Windows.generated.iss
	)

	configure_file(
		ci/ci_includes.sh.in
		ci/ci_includes.generated.sh
	)
	configure_file(
		ci/ci_includes.cmd.in
		ci/ci_includes.generated.cmd
	)

	configure_file(
		installer/installer-macOS.pkgproj.in
		installer-macOS.generated.pkgproj
	)
endif()

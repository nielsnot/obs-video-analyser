cmake_minimum_required(VERSION 3.13)

project(noriscommonui VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

if(NOT TARGET OBS::libobs)
	find_package(libobs REQUIRED)
endif()
if(NOT TARGET OBS::frontend-api)
	find_package(obs-frontend-api REQUIRED)
	add_library(OBS::frontend-api ALIAS OBS::obs-frontend-api)
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)
if(UNIX AND NOT APPLE)
	# NixOS requires to find GuiPrivate.
	find_package(Qt6 QUIET COMPONENTS GuiPrivate)
endif()

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	add_library(${PROJECT_NAME})

	target_compile_definitions(${PROJECT_NAME} PRIVATE NORISCOMMONUI_LIBRARY)

	set_target_properties(${PROJECT_NAME} PROPERTIES SOVERSION 0)
	include(cmake/install-noriscommonui.cmake)

	file(GENERATE OUTPUT .gitignore CONTENT "*\n")
else()
	add_library(${PROJECT_NAME} STATIC)

	target_compile_definitions(${PROJECT_NAME} PUBLIC NORISCOMMONUI_STATIC)

	set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()


target_sources(
	${PROJECT_NAME}
	PRIVATE
	src/NorisQTDisplay.cpp
	src/SurfaceEventFilter.cpp
	include/NorisQTDisplay.hpp
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	src
)

target_include_directories(
	${PROJECT_NAME}
	PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}>
)

target_link_libraries(${PROJECT_NAME}
	OBS::libobs
	OBS::frontend-api
	Qt::Widgets
)

if(UNIX AND NOT APPLE)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
	target_link_options(${PROJECT_NAME} PRIVATE -Wl,-z,defs)
	target_link_libraries(${PROJECT_NAME} Qt::GuiPrivate)
endif()

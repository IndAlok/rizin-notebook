cmake_minimum_required(VERSION 3.16)
project(CutterNotebookPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Cutter REQUIRED)
find_package(Rizin REQUIRED COMPONENTS Core)
find_package(Qt6 COMPONENTS Core Widgets Network WebEngineWidgets REQUIRED)

set(CUTTER_INSTALL_PLUGDIR "${Cutter_USER_PLUGINDIR}" CACHE STRING "Cutter user native plugin directory")

add_library(CutterNotebookPlugin MODULE
    src/CutterNotebookPlugin.cpp
    src/CutterNotebookPlugin.h
    src/NotebookDockWidget.cpp
    src/NotebookDockWidget.h
    src/NotebookSettingsDialog.cpp
    src/NotebookSettingsDialog.h
)

target_link_libraries(CutterNotebookPlugin PRIVATE
    Cutter::Cutter
    Rizin::Core
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::WebEngineWidgets
)

install(TARGETS CutterNotebookPlugin DESTINATION "${CUTTER_INSTALL_PLUGDIR}")

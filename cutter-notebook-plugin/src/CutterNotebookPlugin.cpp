#include "CutterNotebookPlugin.h"

#include <MainWindow.h>

#include "NotebookDockWidget.h"

void CutterNotebookPlugin::setupPlugin() {}

void CutterNotebookPlugin::setupInterface(MainWindow *main) {
    if (widget != nullptr) {
        return;
    }

    widget = new NotebookDockWidget(main);
    main->addPluginDockWidget(widget);
}

void CutterNotebookPlugin::registerDecompilers() {}

void CutterNotebookPlugin::terminate() {
    if (widget != nullptr) {
        widget->shutdownServer();
        widget = nullptr;
    }
}

QString CutterNotebookPlugin::getName() {
    return "Notebook";
}

QString CutterNotebookPlugin::getAuthor() {
    return "rizin-notebook";
}

QString CutterNotebookPlugin::getDescription() {
    return "Embeds the rizin-notebook server in a Cutter dock widget.";
}

QString CutterNotebookPlugin::getVersion() {
    return "1.0.0";
}

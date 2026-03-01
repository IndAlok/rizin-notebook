#pragma once

#include <QObject>

#include <CutterPlugin.h>

class MainWindow;
class NotebookDockWidget;

class CutterNotebookPlugin final : public QObject, public CutterPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "re.rizin.cutter.plugins.CutterPlugin")
    Q_INTERFACES(CutterPlugin)

public:
    void setupPlugin() override;
    void setupInterface(MainWindow *main) override;
    void registerDecompilers() override;
    void terminate() override;

    QString getName() override;
    QString getAuthor() override;
    QString getDescription() override;
    QString getVersion() override;

private:
    NotebookDockWidget *widget = nullptr;
};

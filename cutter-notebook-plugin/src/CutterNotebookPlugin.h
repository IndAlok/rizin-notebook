#pragma once

#include <QObject>
#include <CutterPlugin.h>

class QMenu;
class QDockWidget;
class QLabel;
class MainWindow;

class CutterNotebookPlugin final : public QObject, public CutterPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "re.rizin.cutter.plugins.CutterPlugin")
    Q_INTERFACES(CutterPlugin)

public:
    void setupPlugin() override;
    void setupInterface(MainWindow *main) override;
    void terminate() override;

    QString getName() const override    { return "Notebook"; }
    QString getAuthor() const override  { return "rizin-notebook"; }
    QString getDescription() const override {
        return "Integrates the rizin-notebook server with Cutter via NB commands.";
    }
    QString getVersion() const override { return "1.0.0"; }

private:
    QString runCmd(const QString &cmd, bool *ok = nullptr);
    bool ensureNotebookReady(QString *statusOut = nullptr, bool showDialog = true);
    void refreshDockStatus();

    void onServerStatus();
    void onListPages();
    void onNewPage();
    void onOpenBrowser();
    void onSetUrl();

    MainWindow *mainWindow = nullptr;
    QDockWidget *dockWidget = nullptr;
    QLabel *statusLabel = nullptr;
};

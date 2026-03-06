#pragma once

#include <QObject>
#include <CutterPlugin.h>

class QMenu;
class QDockWidget;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QTabWidget;
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
        return "Provides a native Cutter client for the shared rizin-notebook server.";
    }
    QString getVersion() const override { return "1.0.0"; }

private:
    QString runCmd(const QString &cmd, bool *ok = nullptr);
    QString notebookBaseUrl(bool *fromNBPlugin = nullptr);
    void setNotebookBaseUrl(const QString &url);
    bool ensureNotebookReady(QString *statusOut = nullptr, bool showDialog = true);
    QByteArray sendJsonRequest(const QString &method,
                               const QString &path,
                               const QByteArray &body = QByteArray(),
                               int *statusCode = nullptr,
                               QString *errorOut = nullptr);
    bool populatePages(bool keepSelection = true);
    bool loadPage(const QString &pageId, bool focusView = false);
    void renderPage(const QVariantMap &page);
    QString selectedPageId() const;
    void updateComposeLabels(const QVariantMap &page = QVariantMap());
    void updateEditorPlaceholder();
    void refreshDockStatus();
    void ensureDockVisible();

    void onServerStatus();
    void onListPages();
    void onNewPage();
    void onDeletePage();
    void onOpenPipe();
    void onClosePipe();
    void onSubmitEditor();
    void onReloadSelectedPage();
    void onOpenBrowser();
    void onSetUrl();

    MainWindow *mainWindow = nullptr;
    QDockWidget *dockWidget = nullptr;
    QLabel *statusLabel = nullptr;
    QLabel *currentPageLabel = nullptr;
    QLabel *pipeLabel = nullptr;
    QListWidget *pageListWidget = nullptr;
    QPlainTextEdit *pageView = nullptr;
    QPlainTextEdit *editor = nullptr;
    QComboBox *editorMode = nullptr;
    QTabWidget *dockTabs = nullptr;
    QString currentServerUrl;
    QString currentPageId;
};

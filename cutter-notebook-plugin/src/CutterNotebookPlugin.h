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
    QString getVersion() const override { return "2.0.0"; }

private slots:
    void onConnect();
    void onSpawn();
    void onServerStatus();
    void onListPages();
    void onNewPage();
    void onDeletePage();
    void onAttachBinary();
    void onOpenPipe();
    void onClosePipe();
    void onSubmitEditor();
    void onReloadSelectedPage();
    void onOpenBrowser();
    void onSetUrl();
    void onExportPage();
    void onImportPage();

private:
    QString runCmd(const QString &cmd, bool *ok = nullptr);
    QString notebookBaseUrl(bool *fromNBPlugin = nullptr);
    void setNotebookBaseUrl(const QString &url);
    bool ensureNotebookReady(bool showDialog = true);
    QByteArray sendJsonRequest(const QString &method,
                               const QString &path,
                               const QByteArray &body = QByteArray(),
                               int *statusCode = nullptr,
                               QString *errorOut = nullptr);
    QByteArray sendRawGetRequest(const QString &path,
                                 int *statusCode = nullptr,
                                 QString *contentDisposition = nullptr,
                                 QString *errorOut = nullptr);
    QByteArray sendMultipartPost(const QString &path,
                                 const QString &fieldName,
                                 const QString &fileName,
                                 const QByteArray &fileData,
                                 int *statusCode = nullptr,
                                 QString *errorOut = nullptr);
    bool populatePages(bool keepSelection = true);
    bool loadPage(const QString &pageId, bool focusView = false);
    bool attachBinaryToPage(const QString &pageId, const QString &filePath = QString());
    void renderPage(const QVariantMap &page);
    QString selectedPageId() const;
    void setActivePage(const QString &pageId);
    void updateComposeLabels(const QVariantMap &page = QVariantMap());
    void updateEditorPlaceholder();
    void refreshDockStatus();
    void ensureDockVisible();
    void updateConnectionUI();
    QString computeFileHash(const QString &filePath);
    void verifyBinaryHash(const QString &serverHash);

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
    QString activePageId;
    bool connected = false;
};

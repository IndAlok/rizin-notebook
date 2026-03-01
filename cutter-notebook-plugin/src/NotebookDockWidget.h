#pragma once

#include <QHash>
#include <QPointer>
#include <QUrl>

#include <CutterDockWidget.h>

class QAction;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QToolBar;
class QTimer;
class QWebEngineView;
class MainWindow;

#include "NotebookSettingsDialog.h"

class NotebookDockWidget final : public CutterDockWidget {
    Q_OBJECT

public:
    explicit NotebookDockWidget(MainWindow *mainWindow);
    ~NotebookDockWidget() override;

    void shutdownServer();

private slots:
    void startOrStopServer();
    void openSettings();
    void openInBrowser();
    void reloadPage();
    void syncCurrentBinary();

    void onProcessReadyReadStdOut();
    void onProcessReadyReadStdErr();
    void onProcessFinished(int exitCode);

private:
    void buildUi();
    void loadSettings();
    void saveSettings();

    QString resolveNotebookBinary() const;
    QString resolveDataDirectory() const;

    int pickServerPort() const;
    bool startServer();
    void beginReadinessChecks();
    void updateStatus(const QString &text, bool isError = false);

    bool isServerRunning() const;
    QUrl serverUrl(const QString &path = QString()) const;
    QString currentFilePathFromCore() const;

    void createPageForFile(const QString &filePath, bool focusView);
    void openMappedOrCreatePage();

    QToolBar *toolbar = nullptr;
    QWebEngineView *webView = nullptr;

    QAction *startStopAction = nullptr;
    QAction *settingsAction = nullptr;
    QAction *externalAction = nullptr;
    QAction *reloadAction = nullptr;
    QAction *syncAction = nullptr;

    QLabel *statusLabel = nullptr;

    QProcess *serverProcess = nullptr;
    QNetworkAccessManager *network = nullptr;
    QTimer *readinessTimer = nullptr;

    NotebookPluginSettings pluginSettings;
    int currentPort = -1;
    int readinessRemaining = 0;

    QHash<QString, QString> fileToPageMap;
};

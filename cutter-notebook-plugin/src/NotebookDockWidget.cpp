#include "NotebookDockWidget.h"

#include <QAction>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QWebEngineView>

#include <MainWindow.h>
#include <core/Cutter.h>

namespace {
constexpr const char *kSettingsGroup = "NotebookPlugin";
constexpr int kReadinessAttempts = 80;
constexpr int kReadinessIntervalMs = 250;
}

NotebookDockWidget::NotebookDockWidget(MainWindow *mainWindow)
    : CutterDockWidget(mainWindow), network(new QNetworkAccessManager(this)), readinessTimer(new QTimer(this)) {
    setObjectName("NotebookDockWidget");
    setWindowTitle("Notebook");

    buildUi();
    loadSettings();

    readinessTimer->setSingleShot(false);
    readinessTimer->setInterval(kReadinessIntervalMs);
    connect(readinessTimer, &QTimer::timeout, this, [this]() {
        if (readinessRemaining <= 0) {
            readinessTimer->stop();
            updateStatus("Notebook server did not become ready in time", true);
            return;
        }
        readinessRemaining--;

        QNetworkRequest request(serverUrl("/about"));
        QNetworkReply *reply = network->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            const bool ok = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();
            if (!ok) {
                return;
            }

            readinessTimer->stop();
            updateStatus(QString("Notebook server ready on 127.0.0.1:%1").arg(currentPort));
            webView->setUrl(serverUrl("/"));
            openMappedOrCreatePage();
            startStopAction->setText("Stop");
        });
    });

    if (pluginSettings.autoStart) {
        startServer();
    }

    connect(Core(), &CutterCore::refreshAll, this, &NotebookDockWidget::syncCurrentBinary);
}

NotebookDockWidget::~NotebookDockWidget() {
    shutdownServer();
}

void NotebookDockWidget::shutdownServer() {
    readinessTimer->stop();
    if (serverProcess == nullptr) {
        return;
    }

    if (serverProcess->state() != QProcess::NotRunning) {
        serverProcess->terminate();
        if (!serverProcess->waitForFinished(4000)) {
            serverProcess->kill();
            serverProcess->waitForFinished(2000);
        }
    }

    serverProcess->deleteLater();
    serverProcess = nullptr;
    startStopAction->setText("Start");
}

void NotebookDockWidget::startOrStopServer() {
    if (isServerRunning()) {
        shutdownServer();
        updateStatus("Notebook server stopped");
        return;
    }

    startServer();
}

void NotebookDockWidget::openSettings() {
    NotebookSettingsDialog dialog(this);
    dialog.setSettings(pluginSettings);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const bool wasRunning = isServerRunning();
    pluginSettings = dialog.settings();
    saveSettings();

    if (wasRunning) {
        shutdownServer();
        startServer();
    }
}

void NotebookDockWidget::openInBrowser() {
    if (currentPort < 1) {
        return;
    }
    QDesktopServices::openUrl(serverUrl("/"));
}

void NotebookDockWidget::reloadPage() {
    webView->reload();
}

void NotebookDockWidget::syncCurrentBinary() {
    if (!isServerRunning()) {
        return;
    }
    openMappedOrCreatePage();
}

void NotebookDockWidget::onProcessReadyReadStdOut() {
    if (serverProcess == nullptr) {
        return;
    }
    const QString chunk = QString::fromLocal8Bit(serverProcess->readAllStandardOutput()).trimmed();
    if (!chunk.isEmpty()) {
        updateStatus(chunk);
    }
}

void NotebookDockWidget::onProcessReadyReadStdErr() {
    if (serverProcess == nullptr) {
        return;
    }
    const QString chunk = QString::fromLocal8Bit(serverProcess->readAllStandardError()).trimmed();
    if (!chunk.isEmpty()) {
        updateStatus(chunk, true);
    }
}

void NotebookDockWidget::onProcessFinished(int exitCode) {
    Q_UNUSED(exitCode)
    readinessTimer->stop();
    startStopAction->setText("Start");
    updateStatus("Notebook server exited", true);
}

void NotebookDockWidget::buildUi() {
    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    toolbar = new QToolBar(container);
    toolbar->setMovable(false);

    startStopAction = toolbar->addAction("Start");
    settingsAction = toolbar->addAction("Settings");
    syncAction = toolbar->addAction("Sync Binary");
    reloadAction = toolbar->addAction("Reload");
    externalAction = toolbar->addAction("Open in Browser");

    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    statusLabel = new QLabel("Notebook server not running", toolbar);
    statusLabel->setMinimumWidth(220);
    toolbar->addWidget(statusLabel);

    webView = new QWebEngineView(container);
    webView->setUrl(QUrl("about:blank"));

    layout->addWidget(toolbar);
    layout->addWidget(webView, 1);
    setWidget(container);

    connect(startStopAction, &QAction::triggered, this, &NotebookDockWidget::startOrStopServer);
    connect(settingsAction, &QAction::triggered, this, &NotebookDockWidget::openSettings);
    connect(externalAction, &QAction::triggered, this, &NotebookDockWidget::openInBrowser);
    connect(reloadAction, &QAction::triggered, this, &NotebookDockWidget::reloadPage);
    connect(syncAction, &QAction::triggered, this, &NotebookDockWidget::syncCurrentBinary);
}

void NotebookDockWidget::loadSettings() {
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    pluginSettings.notebookBinaryPath = settings.value("binaryPath").toString();
    pluginSettings.notebookDataDirectory = settings.value("dataDirectory").toString();
    pluginSettings.autoPort = settings.value("autoPort", true).toBool();
    pluginSettings.fixedPort = settings.value("fixedPort", 8000).toInt();
    pluginSettings.autoStart = settings.value("autoStart", true).toBool();

    const auto mapObj = settings.value("fileToPageMap").toJsonObject();
    for (auto it = mapObj.begin(); it != mapObj.end(); ++it) {
        fileToPageMap.insert(it.key(), it.value().toString());
    }

    settings.endGroup();
}

void NotebookDockWidget::saveSettings() {
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    settings.setValue("binaryPath", pluginSettings.notebookBinaryPath);
    settings.setValue("dataDirectory", pluginSettings.notebookDataDirectory);
    settings.setValue("autoPort", pluginSettings.autoPort);
    settings.setValue("fixedPort", pluginSettings.fixedPort);
    settings.setValue("autoStart", pluginSettings.autoStart);

    QJsonObject mapObj;
    for (auto it = fileToPageMap.constBegin(); it != fileToPageMap.constEnd(); ++it) {
        mapObj.insert(it.key(), it.value());
    }
    settings.setValue("fileToPageMap", mapObj);

    settings.endGroup();
}

QString NotebookDockWidget::resolveNotebookBinary() const {
    if (!pluginSettings.notebookBinaryPath.isEmpty()) {
        return pluginSettings.notebookBinaryPath;
    }

    const QString fromEnv = qEnvironmentVariable("RIZIN_NOTEBOOK_PATH");
    if (!fromEnv.isEmpty()) {
        return fromEnv;
    }

#ifdef Q_OS_WIN
    const QString exe = QStandardPaths::findExecutable("rizin-notebook.exe");
#else
    const QString exe = QStandardPaths::findExecutable("rizin-notebook");
#endif
    if (!exe.isEmpty()) {
        return exe;
    }

#ifdef Q_OS_WIN
    const QString bundledName = "rizin-notebook.exe";
#else
    const QString bundledName = "rizin-notebook";
#endif

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList bundledCandidates = {
        QDir(appDir).filePath(bundledName),
        QDir(appDir).filePath(QString("plugins/native/%1").arg(bundledName)),
        QDir(appDir).filePath(QString("../plugins/native/%1").arg(bundledName))
    };
    for (const QString &candidate : bundledCandidates) {
        if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    return QString();
}

QString NotebookDockWidget::resolveDataDirectory() const {
    if (!pluginSettings.notebookDataDirectory.isEmpty()) {
        return pluginSettings.notebookDataDirectory;
    }
    return QDir::homePath() + "/.rizin-notebook";
}

int NotebookDockWidget::pickServerPort() const {
    if (!pluginSettings.autoPort) {
        QTcpServer fixed;
        if (fixed.listen(QHostAddress::LocalHost, static_cast<quint16>(pluginSettings.fixedPort))) {
            fixed.close();
            return pluginSettings.fixedPort;
        }
        return -1;
    }

    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return -1;
    }
    const int port = static_cast<int>(probe.serverPort());
    probe.close();
    return port;
}

bool NotebookDockWidget::startServer() {
    const QString binary = resolveNotebookBinary();
    if (binary.isEmpty() || !QFileInfo::exists(binary)) {
        updateStatus("Notebook binary not found. Configure it in Settings.", true);
        openSettings();
        return false;
    }

    const QString dataDir = resolveDataDirectory();
    QDir().mkpath(dataDir);

    const int port = pickServerPort();
    if (port < 1) {
        updateStatus("Unable to allocate requested port", true);
        return false;
    }

    shutdownServer();

    currentPort = port;
    serverProcess = new QProcess(this);
    serverProcess->setProgram(binary);
    serverProcess->setArguments({"-bind", QString("127.0.0.1:%1").arg(port), "-root", "/", "-notebook", dataDir});

    connect(serverProcess, &QProcess::readyReadStandardOutput, this, &NotebookDockWidget::onProcessReadyReadStdOut);
    connect(serverProcess, &QProcess::readyReadStandardError, this, &NotebookDockWidget::onProcessReadyReadStdErr);
    connect(serverProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onProcessFinished(exitCode); });

    serverProcess->start();
    if (!serverProcess->waitForStarted(4000)) {
        updateStatus("Failed to start notebook server", true);
        shutdownServer();
        return false;
    }

    updateStatus(QString("Starting notebook server on 127.0.0.1:%1").arg(port));
    beginReadinessChecks();
    return true;
}

void NotebookDockWidget::beginReadinessChecks() {
    readinessRemaining = kReadinessAttempts;
    readinessTimer->start();
}

void NotebookDockWidget::updateStatus(const QString &text, bool isError) {
    if (statusLabel == nullptr) {
        return;
    }
    statusLabel->setText(text.simplified());
    statusLabel->setStyleSheet(isError ? "color:#e85600;" : "");
}

bool NotebookDockWidget::isServerRunning() const {
    return serverProcess != nullptr && serverProcess->state() != QProcess::NotRunning;
}

QUrl NotebookDockWidget::serverUrl(const QString &path) const {
    return QUrl(QString("http://127.0.0.1:%1%2").arg(currentPort).arg(path));
}

QString NotebookDockWidget::currentFilePathFromCore() const {
    const QString jsonOpen = Core()->cmd("oj").trimmed();
    const QJsonDocument doc = QJsonDocument::fromJson(jsonOpen.toUtf8());
    if (doc.isArray()) {
        const QJsonArray arr = doc.array();
        for (const QJsonValue &value : arr) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject obj = value.toObject();
            QString p = obj.value("uri").toString();
            if (p.isEmpty()) {
                p = obj.value("name").toString();
            }
            if (p.isEmpty()) {
                p = obj.value("file").toString();
            }
            if (p.isEmpty()) {
                continue;
            }
            if (p.startsWith("file://")) {
                p = QUrl(p).toLocalFile();
            }
            if (!p.isEmpty() && QFileInfo::exists(p)) {
                return p;
            }
        }
    }

    QString fallback = Core()->cmd("o.").trimmed();
    if (fallback.startsWith("file://")) {
        fallback = QUrl(fallback).toLocalFile();
    }
    if (QFileInfo::exists(fallback)) {
        return fallback;
    }
    return QString();
}

void NotebookDockWidget::createPageForFile(const QString &filePath, bool focusView) {
    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile()) {
        updateStatus("Cannot sync current binary: file not found", true);
        return;
    }

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart uniquePart;
    uniquePart.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"unique\"");
    uniquePart.setBody("");
    multiPart->append(uniquePart);

    QHttpPart titlePart;
    titlePart.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"title\"");
    titlePart.setBody(fi.fileName().toUtf8());
    multiPart->append(titlePart);

    auto *file = new QFile(filePath, multiPart);
    if (!file->open(QIODevice::ReadOnly)) {
        updateStatus("Cannot open binary for upload", true);
        multiPart->deleteLater();
        return;
    }

    QHttpPart binaryPart;
    binaryPart.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    binaryPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                         QString("form-data; name=\"binary\"; filename=\"%1\"").arg(fi.fileName()));
    binaryPart.setBodyDevice(file);
    multiPart->append(binaryPart);

    QNetworkRequest request(serverUrl("/edit"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = network->post(request, multiPart);
    multiPart->setParent(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, filePath, focusView]() {
        const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError || !redirect.isValid()) {
            updateStatus(QString("Failed to create page (%1)").arg(status), true);
            reply->deleteLater();
            return;
        }

        const QUrl resolved = serverUrl("/").resolved(redirect);
        const QStringList parts = resolved.path().split('/', Qt::SkipEmptyParts);
        if (parts.size() >= 2 && parts.first() == "view") {
            fileToPageMap.insert(filePath, parts[1]);
            saveSettings();
            if (focusView) {
                webView->setUrl(resolved);
            }
            updateStatus(QString("Synced page %1").arg(parts[1]));
        } else {
            updateStatus("Unexpected server redirect while creating page", true);
        }

        reply->deleteLater();
    });
}

void NotebookDockWidget::openMappedOrCreatePage() {
    const QString filePath = currentFilePathFromCore();
    if (filePath.isEmpty()) {
        return;
    }

    const QString mapped = fileToPageMap.value(filePath);
    if (mapped.isEmpty()) {
        createPageForFile(filePath, true);
        return;
    }

    QNetworkRequest request(serverUrl(QString("/view/%1").arg(mapped)));
    QNetworkReply *reply = network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, filePath, mapped]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (status == 200) {
            webView->setUrl(serverUrl(QString("/view/%1").arg(mapped)));
            updateStatus(QString("Loaded page %1").arg(mapped));
            return;
        }

        fileToPageMap.remove(filePath);
        saveSettings();
        createPageForFile(filePath, true);
    });
}

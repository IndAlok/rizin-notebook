#include "CutterNotebookPlugin.h"

#include <MainWindow.h>
#include <Cutter.h>

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QUrl>
#include <QVariantMap>
#include <QVBoxLayout>
#include <QWidget>
#include <QEventLoop>
#include <QTimer>
#include <QProcess>
#include <QThread>

static QString normalizeBaseUrl(QString url)
{
    url = url.trimmed();
    if (url.isEmpty()) {
        return QStringLiteral("http://127.0.0.1:8000");
    }
    while (url.endsWith('/')) {
        url.chop(1);
    }
    return url;
}

static bool pingServer(const QString &url, int timeoutMs = 1200)
{
    QNetworkAccessManager mgr;
    QNetworkRequest req{QUrl(url)};
    QNetworkReply *reply = mgr.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        reply->abort();
        loop.quit();
    });
    timer.start(timeoutMs);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return false;
    }
    const QByteArray body = reply->readAll().trimmed();
    reply->deleteLater();
    return body == "pong";
}

static bool tryStartServer(const QString &baseUrl, bool showDialog, int waitMs = 6000)
{
    bool started = QProcess::startDetached("rizin-notebook.exe");
    if (!started) {
        if (showDialog) {
            QMessageBox::warning(nullptr, "Notebook", "Failed to start rizin-notebook.exe. Make sure it is installed and on PATH.");
        }
        return false;
    }

    const QString pingUrl = normalizeBaseUrl(baseUrl) + QStringLiteral("/api/v1/ping");
    const int step = 300;
    int waited = 0;
    while (waited < waitMs) {
        QThread::msleep(step);
        if (pingServer(pingUrl, 600)) {
            return true;
        }
        waited += step;
    }
    if (showDialog) {
        QMessageBox::warning(nullptr, "Notebook", "Server did not respond after starting rizin-notebook.exe.");
    }
    return false;
}

/* ── Internal helpers ─────────────────────────────────────────────────── */

QString CutterNotebookPlugin::runCmd(const QString &cmd, bool *ok)
{
    QString out = Core()->cmdRaw(cmd).trimmed();

    // The command is "not available" if rizin says so, or if output is empty
    // for an expected-output command.  We must NOT match partial words —
    // e.g. "could not be started" should NOT trigger a false "not found".
    bool cmdMissing = out.contains("unknown command", Qt::CaseInsensitive)
                   || out.contains("invalid command", Qt::CaseInsensitive)
                   || out.startsWith("Command not found", Qt::CaseInsensitive);
    if (ok) {
        *ok = !cmdMissing;
    }
    return out;
}

QString CutterNotebookPlugin::notebookBaseUrl(bool *fromNBPlugin)
{
    bool ok = false;
    QString configured = normalizeBaseUrl(runCmd("NBu", &ok));
    if (fromNBPlugin) {
        *fromNBPlugin = ok && !configured.isEmpty();
    }
    if (ok && !configured.isEmpty() && configured.startsWith("http")) {
        setNotebookBaseUrl(configured);
        return configured;
    }

    if (!currentServerUrl.isEmpty()) {
        return currentServerUrl;
    }

    QSettings settings(QStringLiteral("rizinorg"), QStringLiteral("rizin-notebook"));
    setNotebookBaseUrl(settings.value(QStringLiteral("serverUrl"), QStringLiteral("http://127.0.0.1:8000")).toString());
    return currentServerUrl;
}

void CutterNotebookPlugin::setNotebookBaseUrl(const QString &url)
{
    currentServerUrl = normalizeBaseUrl(url);
    QSettings settings(QStringLiteral("rizinorg"), QStringLiteral("rizin-notebook"));
    settings.setValue(QStringLiteral("serverUrl"), currentServerUrl);
}

bool CutterNotebookPlugin::ensureNotebookReady(QString *statusOut, bool showDialog)
{
    const QString baseUrl = notebookBaseUrl();
    const QString pingUrl = baseUrl + QStringLiteral("/api/v1/ping");
    QString out = QStringLiteral("Server URL: %1").arg(baseUrl);
    if (statusOut) {
        *statusOut = out;
    }

    if (pingServer(pingUrl, 1000)) {
        return true;
    }

    if (tryStartServer(baseUrl, false)) {
        return true;
    }

    if (showDialog) {
        QMessageBox::warning(nullptr,
                             "Notebook",
                             QStringLiteral("The notebook server could not be reached at %1.\n"
                                            "Make sure rizin-notebook.exe is running, or set the correct server URL.")
                                 .arg(baseUrl));
    }
    return false;
}

QByteArray CutterNotebookPlugin::sendJsonRequest(const QString &method,
                                                 const QString &path,
                                                 const QByteArray &body,
                                                 int *statusCode,
                                                 QString *errorOut)
{
    const QString url = notebookBaseUrl() + path;
    QNetworkAccessManager mgr;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkReply *reply = nullptr;
    if (method == QLatin1String("GET")) {
        reply = mgr.get(req);
    } else if (method == QLatin1String("POST")) {
        reply = mgr.post(req, body);
    } else if (method == QLatin1String("DELETE")) {
        reply = mgr.deleteResource(req);
    } else {
        if (errorOut) {
            *errorOut = QStringLiteral("Unsupported request method: %1").arg(method);
        }
        return {};
    }

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (errorOut) {
            *errorOut = QStringLiteral("Request timed out: %1").arg(url);
        }
        reply->abort();
        loop.quit();
    });
    timer.start(2500);
    loop.exec();

    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode) {
        *statusCode = code;
    }
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && data.isEmpty() && errorOut && errorOut->isEmpty()) {
        *errorOut = reply->errorString();
    }
    reply->deleteLater();
    return data;
}

QString CutterNotebookPlugin::selectedPageId() const
{
    if (!pageListWidget || !pageListWidget->currentItem()) {
        return {};
    }
    return pageListWidget->currentItem()->data(Qt::UserRole).toString();
}

void CutterNotebookPlugin::updateComposeLabels(const QVariantMap &page)
{
    const QString title = page.value(QStringLiteral("title")).toString();
    if (currentPageLabel) {
        currentPageLabel->setText(title.isEmpty()
                                      ? QStringLiteral("Current page: none selected")
                                      : QStringLiteral("Current page: %1").arg(title));
    }
    if (pipeLabel) {
        const bool pipeOpen = page.value(QStringLiteral("pipe")).toBool();
        pipeLabel->setText(title.isEmpty()
                               ? QStringLiteral("Pipe: idle")
                               : QStringLiteral("Pipe: %1").arg(pipeOpen ? QStringLiteral("open") : QStringLiteral("closed")));
    }
}

void CutterNotebookPlugin::updateEditorPlaceholder()
{
    if (!editor || !editorMode) {
        return;
    }
    const QString mode = editorMode->currentData().toString();
    if (mode == QLatin1String("markdown")) {
        editor->setPlaceholderText(QStringLiteral("Write markdown notes for the selected page..."));
    } else if (mode == QLatin1String("command")) {
        editor->setPlaceholderText(QStringLiteral("Add a command cell, for example: afl"));
    } else if (mode == QLatin1String("script")) {
        editor->setPlaceholderText(QStringLiteral("Add a JavaScript cell executed by the notebook runtime..."));
    } else if (mode == QLatin1String("exec-command")) {
        editor->setPlaceholderText(QStringLiteral("Run a Rizin command now against the shared pipe..."));
    } else {
        editor->setPlaceholderText(QStringLiteral("Run a JavaScript snippet now against the shared pipe..."));
    }
}

void CutterNotebookPlugin::renderPage(const QVariantMap &page)
{
    currentPageId = page.value(QStringLiteral("id")).toString();
    updateComposeLabels(page);

    if (!pageView) {
        return;
    }

    QStringList lines;
    lines << QStringLiteral("Title: %1").arg(page.value(QStringLiteral("title")).toString());
    lines << QStringLiteral("Page ID: %1").arg(currentPageId);
    lines << QStringLiteral("Filename: %1").arg(page.value(QStringLiteral("filename")).toString().isEmpty()
                                                    ? QStringLiteral("(none)")
                                                    : page.value(QStringLiteral("filename")).toString());
    lines << QStringLiteral("Pipe: %1").arg(page.value(QStringLiteral("pipe")).toBool() ? QStringLiteral("open") : QStringLiteral("closed"));
    lines << QStringLiteral("");

    const QVariantList cells = page.value(QStringLiteral("cells")).toList();
    if (cells.isEmpty()) {
        lines << QStringLiteral("No cells yet.");
    }

    for (int i = 0; i < cells.size(); ++i) {
        const QVariantMap cell = cells.at(i).toMap();
        lines << QStringLiteral("[%1] %2").arg(i + 1).arg(cell.value(QStringLiteral("type")).toString());
        lines << QStringLiteral("----------------------------------------");
        lines << cell.value(QStringLiteral("content")).toString();
        const QString output = cell.value(QStringLiteral("output")).toString();
        if (!output.trimmed().isEmpty()) {
            lines << QStringLiteral("");
            lines << QStringLiteral("Output:");
            lines << output;
        }
        lines << QStringLiteral("");
    }

    pageView->setPlainText(lines.join('\n'));
}

bool CutterNotebookPlugin::loadPage(const QString &pageId, bool focusView)
{
    if (pageId.isEmpty()) {
        return false;
    }

    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("GET"),
                                            QStringLiteral("/api/v1/json/pages/%1").arg(pageId),
                                            {},
                                            &statusCode,
                                            &error);
    if (data.isEmpty()) {
        if (!error.isEmpty()) {
            QMessageBox::warning(nullptr, QStringLiteral("Notebook"), error);
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), QStringLiteral("Invalid server response while loading page."));
        return false;
    }

    const QJsonObject obj = doc.object();
    if (statusCode >= 400) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             obj.value(QStringLiteral("error")).toString(QStringLiteral("Failed to load page.")));
        return false;
    }

    renderPage(obj.value(QStringLiteral("page")).toObject().toVariantMap());
    if (focusView && dockTabs) {
        dockTabs->setCurrentIndex(0);
    }
    refreshDockStatus();
    return true;
}

bool CutterNotebookPlugin::attachBinaryToPage(const QString &pageId, const QString &filePath)
{
    if (pageId.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select a page first."));
        return false;
    }

    QString chosenPath = filePath;
    if (chosenPath.isEmpty()) {
        chosenPath = QFileDialog::getOpenFileName(nullptr,
                                                  QStringLiteral("Select Binary File"),
                                                  QString(),
                                                  QStringLiteral("All Files (*)"));
    }
    if (chosenPath.isEmpty()) {
        return false;
    }

    QFile file(chosenPath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             QStringLiteral("Failed to read binary file: %1").arg(chosenPath));
        return false;
    }

    QJsonObject req;
    req.insert(QStringLiteral("filename"), QFileInfo(chosenPath).fileName());
    req.insert(QStringLiteral("binary_base64"), QString::fromLatin1(file.readAll().toBase64()));

    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("POST"),
                                            QStringLiteral("/api/v1/json/pages/%1/binary").arg(pageId),
                                            QJsonDocument(req).toJson(QJsonDocument::Compact),
                                            &statusCode,
                                            &error);
    if (data.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             error.isEmpty() ? QStringLiteral("Failed to attach binary.") : error);
        return false;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (statusCode >= 400) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             obj.value(QStringLiteral("error")).toString(QStringLiteral("Failed to attach binary.")));
        return false;
    }

    loadPage(pageId, true);
    return true;
}

bool CutterNotebookPlugin::populatePages(bool keepSelection)
{
    if (!pageListWidget) {
        return false;
    }

    const QString previousSelection = keepSelection ? selectedPageId() : QString();
    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("GET"), QStringLiteral("/api/v1/json/pages"), {}, &statusCode, &error);
    if (data.isEmpty()) {
        if (!error.isEmpty()) {
            QMessageBox::warning(nullptr, QStringLiteral("Notebook"), error);
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), QStringLiteral("Invalid server response while listing pages."));
        return false;
    }

    const QJsonObject obj = doc.object();
    if (statusCode >= 400) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             obj.value(QStringLiteral("error")).toString(QStringLiteral("Failed to list pages.")));
        return false;
    }

    const QJsonArray pages = obj.value(QStringLiteral("pages")).toArray();
    pageListWidget->clear();

    QString selected = previousSelection;
    if (selected.isEmpty() && !currentPageId.isEmpty()) {
        selected = currentPageId;
    }

    for (const QJsonValue &value : pages) {
        const QJsonObject page = value.toObject();
        QString label = page.value(QStringLiteral("title")).toString();
        const int cells = page.value(QStringLiteral("cells_count")).toInt();
        if (cells > 0) {
            label += QStringLiteral(" (%1)").arg(cells);
        }
        if (page.value(QStringLiteral("pipe")).toBool()) {
            label += QStringLiteral(" • pipe");
        }
        auto *item = new QListWidgetItem(label, pageListWidget);
        item->setData(Qt::UserRole, page.value(QStringLiteral("id")).toString());
    }

    for (int i = 0; i < pageListWidget->count(); ++i) {
        auto *item = pageListWidget->item(i);
        if (item->data(Qt::UserRole).toString() == selected) {
            pageListWidget->setCurrentItem(item);
            break;
        }
    }

    if (!pageListWidget->currentItem() && pageListWidget->count() > 0) {
        pageListWidget->setCurrentRow(0);
    }

    if (pageListWidget->currentItem()) {
        loadPage(pageListWidget->currentItem()->data(Qt::UserRole).toString(), false);
    } else {
        currentPageId.clear();
        pageView->setPlainText(QStringLiteral("No notebook pages found."));
        updateComposeLabels();
    }

    if (statusLabel) {
        statusLabel->setText(QStringLiteral("Notebook: %1 page(s)").arg(pages.size()));
    }
    return true;
}

void CutterNotebookPlugin::refreshDockStatus()
{
    if (!statusLabel) {
        return;
    }

    ensureDockVisible();

    const QString baseUrl = notebookBaseUrl();
    if (!pingServer(baseUrl + QStringLiteral("/api/v1/ping"), 900)) {
        statusLabel->setText(QStringLiteral("Notebook: Offline (%1)").arg(baseUrl));
        statusLabel->setToolTip(QStringLiteral("Server unreachable."));
        return;
    }

    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("GET"), QStringLiteral("/api/v1/json/status"), {}, &statusCode, &error);
    if (data.isEmpty()) {
        statusLabel->setText(QStringLiteral("Notebook: Online (%1)").arg(baseUrl));
        statusLabel->setToolTip(error);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        statusLabel->setText(QStringLiteral("Notebook: Online (%1)").arg(baseUrl));
        return;
    }

    const QJsonObject obj = doc.object();
    statusLabel->setText(QStringLiteral("Notebook: %1 pages, %2 open pipe(s)")
                             .arg(obj.value(QStringLiteral("pages")).toInt())
                             .arg(obj.value(QStringLiteral("open_pipes")).toInt()));
    statusLabel->setToolTip(QStringLiteral("Server: %1\nRizin: %2\nStorage: %3")
                                .arg(baseUrl)
                                .arg(obj.value(QStringLiteral("rizin_version")).toString())
                                .arg(obj.value(QStringLiteral("storage")).toString()));
}

void CutterNotebookPlugin::ensureDockVisible()
{
    if (!dockWidget || !mainWindow) {
        return;
    }
    if (!dockWidget->isVisible()) {
        dockWidget->show();
    }
    dockWidget->raise();
}

/* ── Plugin lifecycle ──────────────────────────────────────────────────── */

void CutterNotebookPlugin::setupPlugin() {}

void CutterNotebookPlugin::setupInterface(MainWindow *main)
{
    if (dockWidget) {
        mainWindow = main;
        ensureDockVisible();
        refreshDockStatus();
        return;
    }

    mainWindow = main;

    QMenu *pluginsMenu = main->getMenuByType(MainWindow::MenuType::Plugins);
    // Reuse an existing "Notebook" menu if present to avoid duplicates.
    QMenu *nbMenu = nullptr;
    for (QAction *a : pluginsMenu->actions()) {
        if (a && a->text() == QLatin1String("Notebook") && a->menu()) {
            nbMenu = a->menu();
            break;
        }
    }
    if (!nbMenu) {
        nbMenu = pluginsMenu->addMenu("Notebook");
    }

    auto ensureAction = [this, nbMenu](const QString &text, const char *name, auto slot) {
        for (QAction *action : nbMenu->actions()) {
            if (action && action->objectName() == QLatin1String(name)) {
                return action;
            }
        }
        QAction *action = nbMenu->addAction(text);
        action->setObjectName(QLatin1String(name));
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    ensureAction("Server Status", "NotebookServerStatusAction", &CutterNotebookPlugin::onServerStatus);
    ensureAction("List Pages", "NotebookListPagesAction", &CutterNotebookPlugin::onListPages);
    ensureAction("New Page...", "NotebookNewPageAction", &CutterNotebookPlugin::onNewPage);
    ensureAction("Delete Page", "NotebookDeletePageAction", &CutterNotebookPlugin::onDeletePage);
    ensureAction("Attach Binary...", "NotebookAttachBinaryAction", &CutterNotebookPlugin::onAttachBinary);

    bool hasSeparator = false;
    for (QAction *action : nbMenu->actions()) {
        if (action && action->isSeparator()) {
            hasSeparator = true;
            break;
        }
    }
    if (!hasSeparator) {
        nbMenu->addSeparator();
    }

    ensureAction("Open in Browser", "NotebookOpenBrowserAction", &CutterNotebookPlugin::onOpenBrowser);
    ensureAction("Set Server URL...", "NotebookSetUrlAction", &CutterNotebookPlugin::onSetUrl);

    dockWidget = new QDockWidget("Notebook", main);
    dockWidget->setObjectName("NotebookDock");
    dockWidget->setAllowedAreas(Qt::BottomDockWidgetArea);
    dockWidget->setFeatures(QDockWidget::DockWidgetClosable |
                            QDockWidget::DockWidgetMovable |
                            QDockWidget::DockWidgetFloatable);

    auto *container = new QWidget(dockWidget);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(8, 6, 8, 6);

    auto *topRow = new QHBoxLayout();
    statusLabel = new QLabel("Notebook: Initializing...", container);
    topRow->addWidget(statusLabel, 1);

    auto *btnStatus = new QPushButton("Status", container);
    auto *btnRefresh = new QPushButton("Refresh", container);
    auto *btnBrowser = new QPushButton("Browser", container);
    auto *btnUrl = new QPushButton("URL", container);

    topRow->addWidget(btnStatus);
    topRow->addWidget(btnRefresh);
    topRow->addWidget(btnBrowser);
    topRow->addWidget(btnUrl);
    layout->addLayout(topRow);

    dockTabs = new QTabWidget(container);
    layout->addWidget(dockTabs, 1);

    auto *pagesTab = new QWidget(dockTabs);
    auto *pagesLayout = new QVBoxLayout(pagesTab);
    pagesLayout->setContentsMargins(0, 0, 0, 0);

    auto *pagesButtons = new QHBoxLayout();
    auto *btnPages = new QPushButton("Refresh Pages", pagesTab);
    auto *btnNew = new QPushButton("New Page", pagesTab);
    auto *btnDelete = new QPushButton("Delete Page", pagesTab);
    auto *btnAttachBinary = new QPushButton("Attach Binary", pagesTab);
    auto *btnOpenPipe = new QPushButton("Open Pipe", pagesTab);
    auto *btnClosePipe = new QPushButton("Close Pipe", pagesTab);
    auto *btnReloadPage = new QPushButton("Reload Page", pagesTab);
    pagesButtons->addWidget(btnPages);
    pagesButtons->addWidget(btnNew);
    pagesButtons->addWidget(btnDelete);
    pagesButtons->addWidget(btnAttachBinary);
    pagesButtons->addWidget(btnOpenPipe);
    pagesButtons->addWidget(btnClosePipe);
    pagesButtons->addWidget(btnReloadPage);
    pagesButtons->addStretch(1);
    pagesLayout->addLayout(pagesButtons);

    auto *splitter = new QSplitter(Qt::Horizontal, pagesTab);
    pageListWidget = new QListWidget(splitter);
    pageListWidget->setMinimumWidth(220);
    pageView = new QPlainTextEdit(splitter);
    pageView->setReadOnly(true);
    pageView->setPlaceholderText(QStringLiteral("Select a page to inspect its cells and output."));
    splitter->addWidget(pageListWidget);
    splitter->addWidget(pageView);
    splitter->setStretchFactor(1, 1);
    pagesLayout->addWidget(splitter, 1);
    dockTabs->addTab(pagesTab, QStringLiteral("Pages"));

    auto *composeTab = new QWidget(dockTabs);
    auto *composeLayout = new QVBoxLayout(composeTab);
    composeLayout->setContentsMargins(0, 0, 0, 0);
    currentPageLabel = new QLabel(QStringLiteral("Current page: none selected"), composeTab);
    pipeLabel = new QLabel(QStringLiteral("Pipe: idle"), composeTab);
    composeLayout->addWidget(currentPageLabel);
    composeLayout->addWidget(pipeLabel);

    editorMode = new QComboBox(composeTab);
    editorMode->addItem(QStringLiteral("Markdown note"), QStringLiteral("markdown"));
    editorMode->addItem(QStringLiteral("Command cell"), QStringLiteral("command"));
    editorMode->addItem(QStringLiteral("Script cell"), QStringLiteral("script"));
    editorMode->addItem(QStringLiteral("Run command now"), QStringLiteral("exec-command"));
    editorMode->addItem(QStringLiteral("Run script now"), QStringLiteral("exec-script"));
    composeLayout->addWidget(editorMode);

    editor = new QPlainTextEdit(composeTab);
    composeLayout->addWidget(editor, 1);

    auto *composeButtons = new QHBoxLayout();
    auto *btnSubmit = new QPushButton("Submit", composeTab);
    auto *btnComposeReload = new QPushButton("Reload Page", composeTab);
    composeButtons->addWidget(btnSubmit);
    composeButtons->addWidget(btnComposeReload);
    composeButtons->addStretch(1);
    composeLayout->addLayout(composeButtons);
    dockTabs->addTab(composeTab, QStringLiteral("Compose"));

    connect(btnStatus, &QPushButton::clicked, this, &CutterNotebookPlugin::onServerStatus);
    connect(btnPages, &QPushButton::clicked, this, &CutterNotebookPlugin::onListPages);
    connect(btnNew, &QPushButton::clicked, this, &CutterNotebookPlugin::onNewPage);
    connect(btnDelete, &QPushButton::clicked, this, &CutterNotebookPlugin::onDeletePage);
    connect(btnAttachBinary, &QPushButton::clicked, this, &CutterNotebookPlugin::onAttachBinary);
    connect(btnOpenPipe, &QPushButton::clicked, this, &CutterNotebookPlugin::onOpenPipe);
    connect(btnClosePipe, &QPushButton::clicked, this, &CutterNotebookPlugin::onClosePipe);
    connect(btnReloadPage, &QPushButton::clicked, this, &CutterNotebookPlugin::onReloadSelectedPage);
    connect(btnComposeReload, &QPushButton::clicked, this, &CutterNotebookPlugin::onReloadSelectedPage);
    connect(btnUrl, &QPushButton::clicked, this, &CutterNotebookPlugin::onSetUrl);
    connect(btnBrowser, &QPushButton::clicked, this, &CutterNotebookPlugin::onOpenBrowser);
    connect(btnRefresh, &QPushButton::clicked, this, &CutterNotebookPlugin::onListPages);
    connect(btnSubmit, &QPushButton::clicked, this, &CutterNotebookPlugin::onSubmitEditor);
    connect(editorMode, &QComboBox::currentIndexChanged, this, [this]() { updateEditorPlaceholder(); });
    connect(pageListWidget, &QListWidget::currentTextChanged, this, [this]() {
        const QString pageId = selectedPageId();
        if (!pageId.isEmpty()) {
            loadPage(pageId, false);
        }
    });

    container->setLayout(layout);
    dockWidget->setWidget(container);
    main->addDockWidget(Qt::BottomDockWidgetArea, dockWidget);

    QAction *showDockAction = nullptr;
    for (QAction *action : nbMenu->actions()) {
        if (action && action->objectName() == QLatin1String("NotebookShowDockAction")) {
            showDockAction = action;
            break;
        }
    }
    if (!showDockAction) {
        showDockAction = dockWidget->toggleViewAction();
        showDockAction->setObjectName(QStringLiteral("NotebookShowDockAction"));
        showDockAction->setText(QStringLiteral("Show Dock"));
        nbMenu->addAction(showDockAction);
    }
    Q_UNUSED(showDockAction);

    ensureDockVisible();
    updateEditorPlaceholder();
    refreshDockStatus();
    if (ensureNotebookReady(nullptr, false)) {
        populatePages(false);
    }
}

void CutterNotebookPlugin::terminate()
{
    if (dockWidget) {
        dockWidget->deleteLater();
        dockWidget = nullptr;
        statusLabel = nullptr;
    }
}

/* ── Menu actions ──────────────────────────────────────────────────────── */

void CutterNotebookPlugin::onServerStatus()
{
    ensureDockVisible();
    if (!ensureNotebookReady(nullptr, true)) {
        refreshDockStatus();
        return;
    }

    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("GET"), QStringLiteral("/api/v1/json/status"), {}, &statusCode, &error);
    if (data.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), error.isEmpty() ? QStringLiteral("Unable to query server status.") : error);
        refreshDockStatus();
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    const QString summary = QStringLiteral("Server: %1\nVersion: %2\nRizin: %3\nStorage: %4\nPages: %5\nOpen pipes: %6")
                                .arg(notebookBaseUrl())
                                .arg(obj.value(QStringLiteral("version")).toString())
                                .arg(obj.value(QStringLiteral("rizin_version")).toString())
                                .arg(obj.value(QStringLiteral("storage")).toString())
                                .arg(obj.value(QStringLiteral("pages")).toInt())
                                .arg(obj.value(QStringLiteral("open_pipes")).toInt());
    QMessageBox::information(nullptr, QStringLiteral("Notebook – Server Status"), summary);
    refreshDockStatus();
}

void CutterNotebookPlugin::onListPages()
{
    ensureDockVisible();
    if (!ensureNotebookReady(nullptr, true)) {
        refreshDockStatus();
        return;
    }
    populatePages(true);
    if (dockTabs) {
        dockTabs->setCurrentIndex(0);
    }
    refreshDockStatus();
}

void CutterNotebookPlugin::onNewPage()
{
    ensureDockVisible();
    bool ok = false;
    QString title = QInputDialog::getText(nullptr, "New Notebook Page",
                                          "Page title:", QLineEdit::Normal,
                                          QString(), &ok);
    if (!ok || title.isEmpty()) {
        return;
    }
    if (!ensureNotebookReady(nullptr, true)) {
        refreshDockStatus();
        return;
    }

    QJsonObject req;
    req.insert(QStringLiteral("title"), title);
    const QString binaryPath = QFileDialog::getOpenFileName(nullptr,
                                                            QStringLiteral("Optional Binary File"),
                                                            QString(),
                                                            QStringLiteral("All Files (*)"));
    if (!binaryPath.isEmpty()) {
        QFile file(binaryPath);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Notebook"),
                                 QStringLiteral("Failed to read binary file: %1").arg(binaryPath));
            return;
        }
        req.insert(QStringLiteral("filename"), QFileInfo(binaryPath).fileName());
        req.insert(QStringLiteral("binary_base64"), QString::fromLatin1(file.readAll().toBase64()));
    }
    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("POST"),
                                            QStringLiteral("/api/v1/json/pages"),
                                            QJsonDocument(req).toJson(QJsonDocument::Compact),
                                            &statusCode,
                                            &error);
    if (data.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), error.isEmpty() ? QStringLiteral("Failed to create page.") : error);
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (statusCode >= 400) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             obj.value(QStringLiteral("error")).toString(QStringLiteral("Failed to create page.")));
        return;
    }

    populatePages(false);
    const QString newPageId = obj.value(QStringLiteral("page")).toObject().value(QStringLiteral("id")).toString();
    if (!newPageId.isEmpty()) {
        loadPage(newPageId, true);
    }
    refreshDockStatus();
}

void CutterNotebookPlugin::onAttachBinary()
{
    ensureDockVisible();
    const QString pageId = selectedPageId().isEmpty() ? currentPageId : selectedPageId();
    attachBinaryToPage(pageId);
}

void CutterNotebookPlugin::onDeletePage()
{
    ensureDockVisible();
    const QString pageId = selectedPageId();
    if (pageId.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select a page first."));
        return;
    }
    if (QMessageBox::question(nullptr,
                              QStringLiteral("Notebook"),
                              QStringLiteral("Delete the selected page and all of its cells?")) != QMessageBox::Yes) {
        return;
    }

    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("DELETE"),
                                            QStringLiteral("/api/v1/json/pages/%1").arg(pageId),
                                            {},
                                            &statusCode,
                                            &error);
    if (data.isEmpty() && !error.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), error);
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (statusCode >= 400) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             obj.value(QStringLiteral("error")).toString(QStringLiteral("Failed to delete page.")));
        return;
    }
    currentPageId.clear();
    populatePages(false);
    refreshDockStatus();
}

void CutterNotebookPlugin::onOpenPipe()
{
    const QString pageId = selectedPageId();
    if (pageId.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select a page first."));
        return;
    }
    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("POST"),
                                            QStringLiteral("/api/v1/json/pages/%1/pipe/open").arg(pageId),
                                            QByteArrayLiteral("{}"),
                                            &statusCode,
                                            &error);
    if (data.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), error.isEmpty() ? QStringLiteral("Failed to open pipe.") : error);
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (!obj.value(QStringLiteral("open")).toBool()) {
        const QString serverError = obj.value(QStringLiteral("error")).toString();
        if (serverError.contains(QStringLiteral("no binary attached"), Qt::CaseInsensitive)) {
            if (QMessageBox::question(nullptr,
                                      QStringLiteral("Notebook"),
                                      QStringLiteral("This page has no binary attached yet. Select one now?")) == QMessageBox::Yes) {
                if (attachBinaryToPage(pageId)) {
                    onOpenPipe();
                }
            }
            return;
        }
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             serverError.isEmpty() ? QStringLiteral("Failed to open pipe.") : serverError);
        return;
    }
    loadPage(pageId, true);
}

void CutterNotebookPlugin::onClosePipe()
{
    const QString pageId = selectedPageId();
    if (pageId.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select a page first."));
        return;
    }
    sendJsonRequest(QStringLiteral("POST"),
                    QStringLiteral("/api/v1/json/pages/%1/pipe/close").arg(pageId),
                    QByteArrayLiteral("{}"));
    loadPage(pageId, true);
}

void CutterNotebookPlugin::onSubmitEditor()
{
    if (!editor || !editorMode) {
        return;
    }
    const QString pageId = selectedPageId().isEmpty() ? currentPageId : selectedPageId();
    if (pageId.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select or create a page first."));
        return;
    }
    const QString content = editor->toPlainText().trimmed();
    if (content.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Enter some content first."));
        return;
    }

    QString path;
    QJsonObject req;
    const QString mode = editorMode->currentData().toString();
    if (mode == QLatin1String("markdown") || mode == QLatin1String("command") || mode == QLatin1String("script")) {
        path = QStringLiteral("/api/v1/json/pages/%1/cells").arg(pageId);
        req.insert(QStringLiteral("type"), mode);
        req.insert(QStringLiteral("content"), content);
    } else if (mode == QLatin1String("exec-command")) {
        path = QStringLiteral("/api/v1/json/pages/%1/exec").arg(pageId);
        req.insert(QStringLiteral("command"), content);
    } else {
        path = QStringLiteral("/api/v1/json/pages/%1/script").arg(pageId);
        req.insert(QStringLiteral("script"), content);
    }

    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("POST"),
                                            path,
                                            QJsonDocument(req).toJson(QJsonDocument::Compact),
                                            &statusCode,
                                            &error);
    if (data.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), error.isEmpty() ? QStringLiteral("Notebook request failed.") : error);
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (statusCode >= 400 || (obj.contains(QStringLiteral("success")) && !obj.value(QStringLiteral("success")).toBool())) {
        const QString serverError = obj.value(QStringLiteral("error")).toString();
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             serverError.isEmpty() ? QStringLiteral("Notebook request failed.") : serverError);
        return;
    }

    editor->clear();
    loadPage(pageId, true);
    refreshDockStatus();
}

void CutterNotebookPlugin::onReloadSelectedPage()
{
    const QString pageId = selectedPageId().isEmpty() ? currentPageId : selectedPageId();
    if (pageId.isEmpty()) {
        populatePages(true);
        return;
    }
    loadPage(pageId, true);
}

void CutterNotebookPlugin::onOpenBrowser()
{
    ensureDockVisible();
    const QString url = notebookBaseUrl();
    QUrl qurl = QUrl::fromUserInput(url);
    if (!qurl.isValid() || qurl.scheme().isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), QStringLiteral("Invalid server URL: %1").arg(url));
        return;
    }
    QDesktopServices::openUrl(qurl);
    refreshDockStatus();
}

void CutterNotebookPlugin::onSetUrl()
{
    ensureDockVisible();
    QString current = notebookBaseUrl();
    bool ok = false;
    QString url = QInputDialog::getText(nullptr, "Set Notebook Server URL",
                                        "URL:", QLineEdit::Normal,
                                        current, &ok);
    if (!ok || url.isEmpty()) {
        return;
    }

    QUrl parsed = QUrl::fromUserInput(url);
    if (!parsed.isValid() || parsed.scheme().isEmpty()) {
        QMessageBox::warning(nullptr, "Notebook", "Please enter a valid URL (example: http://127.0.0.1:8000)");
        return;
    }

    QString safeUrl = parsed.toString(QUrl::FullyEncoded);
    setNotebookBaseUrl(safeUrl);
    bool hasNBPlugin = false;
    notebookBaseUrl(&hasNBPlugin);
    if (hasNBPlugin) {
        runCmd(QString("NBu %1").arg(safeUrl));
    }
    populatePages(false);
    refreshDockStatus();
}

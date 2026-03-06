#include "CutterNotebookPlugin.h"

#include <MainWindow.h>
#include <Cutter.h>

#include <QAction>
#include <QDockWidget>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QProcess>
#include <QThread>

// Helper: perform a synchronous HTTP GET to `url` and expect body "pong".
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

// Try starting the bundled server by invoking `rizin-notebook.exe` (relies on it
// being on PATH or in working directory). Waits up to `waitMs` for ping to succeed.
static bool tryStartServer(bool showDialog, int waitMs = 6000)
{
    bool started = QProcess::startDetached("rizin-notebook.exe");
    if (!started) {
        if (showDialog) {
            QMessageBox::warning(nullptr, "Notebook", "Failed to start rizin-notebook.exe. Make sure it is installed and on PATH.");
        }
        return false;
    }

    const QString pingUrl = QStringLiteral("http://127.0.0.1:8000/api/v1/ping");
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

bool CutterNotebookPlugin::ensureNotebookReady(QString *statusOut, bool showDialog)
{
    bool ok = false;
    QString out = runCmd("NBs", &ok);
    if (statusOut) {
        *statusOut = out;
    }

    if (!ok) {
        // NB commands are not available at all — rz_notebook plugin not loaded.
        if (showDialog) {
            QMessageBox::warning(
                nullptr,
                "Notebook",
                "The NB commands are not available.  The rz_notebook plugin is "
                "probably not installed in Cutter's rizin plugin directory.\n\n"
                "Please copy rz_notebook.dll (and protobuf-c.dll) into one of:\n"
                "  • %APPDATA%\\rizin\\plugins\\\n"
                "  • <Cutter-install>\\lib\\plugins\\\n\n"
                "Command output:\n" + out);
        }
        return false;
    }

    if (!out.contains("Notebook Server Status")) {
        // NB commands exist but server is not reachable.
        if (showDialog) {
            QMessageBox::warning(
                nullptr,
                "Notebook",
                "The notebook server could not be reached.\n"
                "Make sure rizin-notebook.exe is placed next to rz_notebook.dll "
                "so the plugin can auto-start it, or start it manually.\n\n"
                "Command output:\n" + out);
        }
        return false;
    }

    return true;
}

void CutterNotebookPlugin::refreshDockStatus()
{
    if (!statusLabel) {
        return;
    }

    QString out;
    bool ok = false;
    QString nbOut = runCmd("NBs", &ok);
    if (ok && nbOut.contains("Notebook Server Status")) {
        QString url = runCmd("NBu").trimmed();
        statusLabel->setText(QString("Notebook: Online (%1)").arg(url.isEmpty() ? "(no URL)" : url));
        statusLabel->setToolTip(nbOut.trimmed());
        return;
    }

    // Fallback: try HTTP ping to detect a running server even if rz_notebook plugin
    // isn't installed in this rizin instance.
    const QString defaultUrl = QStringLiteral("http://127.0.0.1:8000/api/v1/ping");
    if (pingServer(defaultUrl, 1200)) {
        statusLabel->setText(QString("Notebook: Online (no rz_notebook plugin)"));
        statusLabel->setToolTip("Server reachable but rz_notebook plugin not loaded in this rizin instance.");
        return;
    }

    statusLabel->setText("Notebook: Offline / NB commands unavailable");
    statusLabel->setToolTip(nbOut.trimmed());
}

/* ── Plugin lifecycle ──────────────────────────────────────────────────── */

void CutterNotebookPlugin::setupPlugin() {}

void CutterNotebookPlugin::setupInterface(MainWindow *main)
{
    if (dockWidget) {
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

    // Bottom dock for quick status and actions.
    dockWidget = new QDockWidget("Notebook", main);
    dockWidget->setObjectName("NotebookDock");
    dockWidget->setAllowedAreas(Qt::BottomDockWidgetArea);

    auto *container = new QWidget(dockWidget);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(8, 4, 8, 4);

    statusLabel = new QLabel("Notebook: Initializing...", container);
    layout->addWidget(statusLabel, 1);

    auto *btnStatus = new QPushButton("Status", container);
    auto *btnPages = new QPushButton("Pages", container);
    auto *btnNew = new QPushButton("New", container);
    auto *btnUrl = new QPushButton("URL", container);
    auto *btnOpen = new QPushButton("Open", container);

    layout->addWidget(btnStatus);
    layout->addWidget(btnPages);
    layout->addWidget(btnNew);
    layout->addWidget(btnUrl);
    layout->addWidget(btnOpen);

    connect(btnStatus, &QPushButton::clicked, this, &CutterNotebookPlugin::onServerStatus);
    connect(btnPages, &QPushButton::clicked, this, &CutterNotebookPlugin::onListPages);
    connect(btnNew, &QPushButton::clicked, this, &CutterNotebookPlugin::onNewPage);
    connect(btnUrl, &QPushButton::clicked, this, &CutterNotebookPlugin::onSetUrl);
    connect(btnOpen, &QPushButton::clicked, this, &CutterNotebookPlugin::onOpenBrowser);

    container->setLayout(layout);
    dockWidget->setWidget(container);
    main->addDockWidget(Qt::BottomDockWidgetArea, dockWidget);

    refreshDockStatus();
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
    bool ok = false;
    QString result = runCmd("NBs", &ok);
    if (ok && result.contains("Notebook Server Status")) {
        QMessageBox::information(nullptr, "Notebook – Server Status", result.trimmed());
        refreshDockStatus();
        return;
    }

    // If NB commands not available, try HTTP ping to see if a server is running.
    const QString pingUrl = QStringLiteral("http://127.0.0.1:8000/api/v1/ping");
    if (pingServer(pingUrl, 1000)) {
        QMessageBox::information(nullptr, "Notebook – Server Status",
                                 "A notebook server is reachable at http://127.0.0.1:8000, but the rz_notebook plugin is not loaded in this rizin instance.");
        refreshDockStatus();
        return;
    }

    // Offer to start the server when the user asks for status.
    auto choice = QMessageBox::question(nullptr, "Notebook",
                                        "NB commands are not available and no server was detected.\nStart the local notebook server now?",
                                        QMessageBox::Yes | QMessageBox::No);
    if (choice == QMessageBox::Yes) {
        if (tryStartServer(true)) {
            QMessageBox::information(nullptr, "Notebook", "Server started successfully.");
        }
    }
    refreshDockStatus();
}

void CutterNotebookPlugin::onListPages()
{
    if (!ensureNotebookReady()) {
        refreshDockStatus();
        return;
    }
    QString result = runCmd("NBl");
    QMessageBox::information(nullptr, "Notebook – Pages", result.trimmed());
    refreshDockStatus();
}

void CutterNotebookPlugin::onNewPage()
{
    bool ok = false;
    QString title = QInputDialog::getText(nullptr, "New Notebook Page",
                                          "Page title:", QLineEdit::Normal,
                                          QString(), &ok);
    if (!ok || title.isEmpty()) {
        return;
    }
    if (!ensureNotebookReady()) {
        refreshDockStatus();
        return;
    }
    QString safeTitle = title;
    safeTitle.replace("\"", "\\\"");
    QString result = runCmd(QString("NBn \"%1\"").arg(safeTitle));
    QMessageBox::information(nullptr, "Notebook – New Page", result.trimmed());
    refreshDockStatus();
}

void CutterNotebookPlugin::onOpenBrowser()
{
    // Prefer configured URL via NB plugin if available.
    bool ok = false;
    QString url = runCmd("NBu", &ok).trimmed();
    if (ok && !url.isEmpty()) {
        QUrl qurl = QUrl::fromUserInput(url);
        if (!qurl.isValid() || qurl.scheme().isEmpty()) {
            QMessageBox::warning(nullptr, "Notebook", "Invalid server URL: " + url);
            return;
        }
        QDesktopServices::openUrl(qurl);
        refreshDockStatus();
        return;
    }

    // Fallback: open default server URL if reachable.
    const QString defaultUrl = QStringLiteral("http://127.0.0.1:8000");
    if (pingServer(defaultUrl + "/api/v1/ping", 800)) {
        QDesktopServices::openUrl(QUrl(defaultUrl));
    } else {
        QMessageBox::warning(nullptr, "Notebook", "No server detected and rz_notebook plugin appears unavailable.");
    }
    refreshDockStatus();
}

void CutterNotebookPlugin::onSetUrl()
{
    QString current = runCmd("NBu").trimmed();
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
    QString out = runCmd(QString("NBu %1").arg(safeUrl));
    QMessageBox::information(nullptr, "Notebook", "Server URL set to:\n" + out.trimmed());
    refreshDockStatus();
}

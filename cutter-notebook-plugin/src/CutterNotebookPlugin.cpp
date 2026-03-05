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

/* ── Internal helpers ─────────────────────────────────────────────────── */

QString CutterNotebookPlugin::runCmd(const QString &cmd, bool *ok)
{
    QString out = Core()->cmdRaw(cmd).trimmed();
    bool success = !out.contains("unknown command", Qt::CaseInsensitive)
                && !out.contains("invalid command", Qt::CaseInsensitive)
                && !out.contains("not found", Qt::CaseInsensitive);
    if (ok) {
        *ok = success;
    }
    return out;
}

bool CutterNotebookPlugin::ensureNotebookReady(QString *statusOut)
{
    bool ok = false;
    QString out = runCmd("NBs", &ok);
    if (statusOut) {
        *statusOut = out;
    }
    if (!ok || !out.contains("Notebook Server Status")) {
        QMessageBox::warning(
            nullptr,
            "Notebook",
            "Notebook backend is not ready.\n"
            "Make sure `rz_notebook` is installed in rizin plugin path and the server can start.\n\n"
            "Command output:\n" + out);
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
    if (ensureNotebookReady(&out)) {
        QString url = runCmd("NBu").trimmed();
        statusLabel->setText(QString("Notebook: Online (%1)").arg(url));
    } else {
        statusLabel->setText("Notebook: Offline / NB commands unavailable");
    }
}

/* ── Plugin lifecycle ──────────────────────────────────────────────────── */

void CutterNotebookPlugin::setupPlugin() {}

void CutterNotebookPlugin::setupInterface(MainWindow *main)
{
    mainWindow = main;

    QMenu *pluginsMenu = main->getMenuByType(MainWindow::MenuType::Plugins);
    QMenu *nbMenu = pluginsMenu->addMenu("Notebook");

    auto *actStatus = nbMenu->addAction("Server Status");
    connect(actStatus, &QAction::triggered, this, &CutterNotebookPlugin::onServerStatus);

    auto *actList = nbMenu->addAction("List Pages");
    connect(actList, &QAction::triggered, this, &CutterNotebookPlugin::onListPages);

    auto *actNew = nbMenu->addAction("New Page...");
    connect(actNew, &QAction::triggered, this, &CutterNotebookPlugin::onNewPage);

    nbMenu->addSeparator();

    auto *actOpen = nbMenu->addAction("Open in Browser");
    connect(actOpen, &QAction::triggered, this, &CutterNotebookPlugin::onOpenBrowser);

    auto *actUrl = nbMenu->addAction("Set Server URL...");
    connect(actUrl, &QAction::triggered, this, &CutterNotebookPlugin::onSetUrl);

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
    QMessageBox::information(nullptr, "Notebook – Server Status", result.trimmed());
    Q_UNUSED(ok);
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
    if (!ensureNotebookReady()) {
        refreshDockStatus();
        return;
    }

    QString url = runCmd("NBu").trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(nullptr, "Notebook", "No server URL configured.");
        return;
    }

    QUrl qurl = QUrl::fromUserInput(url);
    if (!qurl.isValid() || qurl.scheme().isEmpty()) {
        QMessageBox::warning(nullptr, "Notebook", "Invalid server URL: " + url);
        return;
    }
    QDesktopServices::openUrl(qurl);
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

#include "CutterNotebookPlugin.h"

#include <MainWindow.h>
#include <Cutter.h>

#include <QAction>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QUrl>

/* ── Plugin lifecycle ──────────────────────────────────────────────────── */

void CutterNotebookPlugin::setupPlugin() {}

void CutterNotebookPlugin::setupInterface(MainWindow *main)
{
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
}

void CutterNotebookPlugin::terminate() {}

/* ── Menu actions ──────────────────────────────────────────────────────── */

void CutterNotebookPlugin::onServerStatus()
{
    QString result = Core()->cmdRaw("NBs");
    QMessageBox::information(nullptr, "Notebook – Server Status", result.trimmed());
}

void CutterNotebookPlugin::onListPages()
{
    QString result = Core()->cmdRaw("NBl");
    QMessageBox::information(nullptr, "Notebook – Pages", result.trimmed());
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
    QString result = Core()->cmdRaw(QString("NBn %1").arg(title));
    QMessageBox::information(nullptr, "Notebook – New Page", result.trimmed());
}

void CutterNotebookPlugin::onOpenBrowser()
{
    QString url = Core()->cmdRaw("NBu").trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(nullptr, "Notebook", "No server URL configured.");
        return;
    }
    QDesktopServices::openUrl(QUrl(url));
}

void CutterNotebookPlugin::onSetUrl()
{
    QString current = Core()->cmdRaw("NBu").trimmed();
    bool ok = false;
    QString url = QInputDialog::getText(nullptr, "Set Notebook Server URL",
                                        "URL:", QLineEdit::Normal,
                                        current, &ok);
    if (!ok || url.isEmpty()) {
        return;
    }
    Core()->cmdRaw(QString("NBu %1").arg(url));
}

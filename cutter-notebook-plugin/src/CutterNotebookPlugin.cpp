#include "CutterNotebookPlugin.h"

#include <MainWindow.h>
#include <Cutter.h>

#include <QAction>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDockWidget>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHttpMultiPart>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTabWidget>
#include <QToolTip>
#include <QUrl>
#include <QVariantMap>
#include <QVBoxLayout>
#include <QWidget>
#include <QEventLoop>
#include <QKeyEvent>
#include <QTimer>
#include <QProcess>
#include <QThread>
#include <QScreen>

/* ── Static helpers ──────────────────────────────────────────────────── */

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

/* ── Internal helpers ─────────────────────────────────────────────────── */

QString CutterNotebookPlugin::runCmd(const QString &cmd, bool *ok)
{
    QString out = Core()->cmdRaw(cmd).trimmed();
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

bool CutterNotebookPlugin::ensureNotebookReady(bool showDialog)
{
    const QString baseUrl = notebookBaseUrl();
    const QString pingUrl = baseUrl + QStringLiteral("/api/v1/ping");

    if (pingServer(pingUrl, 1000)) {
        connected = true;
        updateConnectionUI();
        return true;
    }

    connected = false;
    updateConnectionUI();

    if (showDialog) {
        QMessageBox::warning(nullptr,
                             "Notebook",
                             QStringLiteral("The notebook server could not be reached at %1.\n"
                                            "Use Connect or Spawn to establish a connection.")
                                 .arg(baseUrl));
    }
    return false;
}

void CutterNotebookPlugin::updateConnectionUI()
{
    if (!statusLabel) {
        return;
    }
    if (connected) {
        statusLabel->setText(QStringLiteral("Notebook: Connected (%1)").arg(notebookBaseUrl()));
    } else {
        statusLabel->setText(QStringLiteral("Notebook: Disconnected"));
    }
}

QString CutterNotebookPlugin::computeFileHash(const QString &filePath)
{
    if (filePath.isEmpty()) {
        return {};
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

void CutterNotebookPlugin::verifyBinaryHash(const QString &serverHash)
{
    if (serverHash.isEmpty()) {
        return;
    }

    QString localPath = currentBinaryPath();
    if (localPath.isEmpty()) {
        return;
    }

    const QString localHash = computeFileHash(localPath);
    if (localHash.isEmpty()) {
        return;
    }

    if (localHash.compare(serverHash, Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook – Binary Mismatch"),
                             QStringLiteral("The binary hash of the file currently loaded in Cutter "
                                            "does not match the binary hash stored on the notebook page.\n\n"
                                            "Local:  %1\nServer: %2\n\n"
                                            "You may be analyzing a different file than the one this page was created for.")
                                 .arg(localHash, serverHash));
    }
}

QString CutterNotebookPlugin::currentBinaryPath()
{
    // Use ij (info JSON) to reliably get the file path of the loaded binary.
    // This is stable across all rizin versions and always available in Cutter.
    QString ijOutput = Core()->cmdRaw(QStringLiteral("ij")).trimmed();
    if (!ijOutput.isEmpty() && ijOutput.startsWith(QChar('{'))) {
        QJsonDocument doc = QJsonDocument::fromJson(ijOutput.toUtf8());
        if (doc.isObject()) {
            QString file = doc.object()
                .value(QStringLiteral("core")).toObject()
                .value(QStringLiteral("file")).toString();
            if (!file.isEmpty()) {
                return file;
            }
        }
    }
    // Fallback: parse o. command output
    QString raw = Core()->cmdRaw(QStringLiteral("o.")).trimmed();
    if (!raw.isEmpty()
        && !raw.contains(QStringLiteral("unknown"), Qt::CaseInsensitive)
        && !raw.contains(QStringLiteral("invalid"), Qt::CaseInsensitive)
        && !raw.contains(QStringLiteral("Usage"), Qt::CaseInsensitive)) {
        return raw;
    }
    return {};
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
    // Use longer timeout for POST requests that may carry binary data
    timer.start(method == QLatin1String("POST") && body.size() > 100 ? 30000 : 5000);
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

QByteArray CutterNotebookPlugin::sendRawGetRequest(const QString &path,
                                                    int *statusCode,
                                                    QString *contentDisposition,
                                                    QString *errorOut)
{
    const QString url = notebookBaseUrl() + path;
    QNetworkAccessManager mgr;
    QNetworkRequest req{QUrl(url)};

    QNetworkReply *reply = mgr.get(req);

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
    timer.start(10000);
    loop.exec();

    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode) {
        *statusCode = code;
    }
    if (contentDisposition) {
        *contentDisposition = QString::fromUtf8(reply->rawHeader("Content-Disposition"));
    }
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && data.isEmpty() && errorOut && errorOut->isEmpty()) {
        *errorOut = reply->errorString();
    }
    reply->deleteLater();
    return data;
}

QByteArray CutterNotebookPlugin::sendMultipartPost(const QString &path,
                                                     const QString &fieldName,
                                                     const QString &fileName,
                                                     const QByteArray &fileData,
                                                     int *statusCode,
                                                     QString *errorOut)
{
    const QString url = notebookBaseUrl() + path;
    QNetworkAccessManager mgr;
    QNetworkRequest req{QUrl(url)};

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"%1\"; filename=\"%2\"")
                           .arg(fieldName, fileName));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    filePart.setBody(fileData);
    multiPart->append(filePart);

    QNetworkReply *reply = mgr.post(req, multiPart);
    multiPart->setParent(reply);

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
    timer.start(10000);
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

void CutterNotebookPlugin::setActivePage(const QString &pageId)
{
    activePageId = pageId;
    if (!m_populatingPages) {
        populatePages(true);
    }
}

void CutterNotebookPlugin::updateComposeLabels(const QVariantMap &page)
{
    const QString title = page.value(QStringLiteral("title")).toString();
    if (currentPageLabel) {
        if (activePageId.isEmpty()) {
            currentPageLabel->setText(QStringLiteral("Active page: none selected"));
        } else {
            currentPageLabel->setText(QStringLiteral("Active page: %1").arg(
                title.isEmpty() ? activePageId : title));
        }
    }
    if (pipeLabel) {
        const bool pipeOpen = page.value(QStringLiteral("pipe")).toBool();
        pipeLabel->setText(activePageId.isEmpty()
                               ? QStringLiteral("Pipe: idle")
                               : QStringLiteral("Pipe: %1").arg(pipeOpen ? QStringLiteral("open") : QStringLiteral("closed")));
    }
}

/* ── Autocomplete helpers ─────────────────────────────────────────────── */

void CutterNotebookPlugin::fetchCommandsForCompleter()
{
    auto fetchByPath = [this](const QString &path, QByteArray *outData, int *outStatus) -> bool {
        QString error;
        const QByteArray data = sendJsonRequest(QStringLiteral("GET"),
                                                path,
                                                QByteArray(),
                                                outStatus,
                                                &error);
        if (data.isEmpty() || *outStatus >= 400) {
            return false;
        }
        *outData = data;
        return true;
    };

    QByteArray data;
    int statusCode = 0;
    const bool fetched = fetchByPath(QStringLiteral("/api/v1/json/commands"), &data, &statusCode) ||
                         fetchByPath(QStringLiteral("/api/v1/commands"), &data, &statusCode);
    if (!fetched) {
        // Fallback: get commands directly from the current Cutter core.
        // This keeps autocomplete functional even when the connected notebook
        // server is an older build without /api/v1/json/commands.
        const QString localJson = Core() ? Core()->cmdRaw(QStringLiteral("?*j")).trimmed() : QString();
        if (!localJson.isEmpty()) {
            QJsonParseError localParseError;
            const QJsonDocument localDoc = QJsonDocument::fromJson(localJson.toUtf8(), &localParseError);
            if (localParseError.error == QJsonParseError::NoError && localDoc.isObject()) {
                const QJsonObject localObj = localDoc.object();
                QStringList cmds;
                cmds.reserve(localObj.size());
                for (auto it = localObj.begin(); it != localObj.end(); ++it) {
                    cmds.append(it.key());
                }
                cmds.sort();
                cmdCommandList = cmds;
                if (editor && editor->hasFocus()) {
                    updateAutocomplete();
                }
                return;
            }
        }

        // Server/core might still be loading commands. Retry after a short delay.
        if (cmdCommandList.isEmpty()) {
            QTimer::singleShot(3000, this, [this]() {
                if (cmdCommandList.isEmpty()) {
                    fetchCommandsForCompleter();
                }
            });
        }
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (cmdCommandList.isEmpty()) {
            QTimer::singleShot(3000, this, [this]() {
                if (cmdCommandList.isEmpty() && connected) {
                    fetchCommandsForCompleter();
                }
            });
        }
        return;
    }

    const QJsonObject obj = doc.object();
    if (obj.isEmpty() && cmdCommandList.isEmpty()) {
        // Server returned empty commands (still loading). Retry after 3 seconds.
        QTimer::singleShot(3000, this, [this]() {
            if (cmdCommandList.isEmpty() && connected) {
                fetchCommandsForCompleter();
            }
        });
        return;
    }

    QStringList cmds;
    cmds.reserve(obj.size());
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        cmds.append(it.key());
    }
    cmds.sort();
    cmdCommandList = cmds;

    if (editor && editor->hasFocus()) {
        updateAutocomplete();
    }
}

void CutterNotebookPlugin::fetchAutocompleteSettings()
{
    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("GET"),
                                            QStringLiteral("/api/v1/json/settings"),
                                            QByteArray(),
                                            &statusCode,
                                            &error);
    if (data.isEmpty() || statusCode >= 400) {
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();

    auto parseIntSetting = [&obj](const QString &key, int minVal, int maxVal, int current) {
        if (!obj.contains(key)) {
            return current;
        }
        const QJsonValue valNode = obj.value(key);
        bool ok = false;
        int val = 0;
        if (valNode.isString()) {
            val = valNode.toString().toInt(&ok);
        } else if (valNode.isDouble()) {
            val = valNode.toInt();
            ok = true;
        }
        if (!ok) {
            return current;
        }
        if (val < minVal) {
            val = minVal;
        } else if (val > maxVal) {
            val = maxVal;
        }
        return val;
    };

    maxCompleterResults = parseIntSetting(QStringLiteral("ac:max_results"), 1, 100, maxCompleterResults);
    minCompleterChars = parseIntSetting(QStringLiteral("ac:min_chars"), 1, 10, minCompleterChars);
}

void CutterNotebookPlugin::acceptAutocomplete()
{
    if (!acPopup || !acPopup->currentItem() || !editor) {
        return;
    }
    const QString completion = acPopup->currentItem()->text();
    acPopup->hide();

    QTextCursor tc = editor->textCursor();
    tc.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    tc.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    const QString line = tc.selectedText();

    int spaceIdx = line.indexOf(' ');
    if (spaceIdx < 0) {
        tc.insertText(completion);
    } else {
        tc.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        tc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, spaceIdx);
        tc.insertText(completion);
    }
    editor->setTextCursor(tc);
}

void CutterNotebookPlugin::updateAutocomplete()
{
    if (!editor || !acPopup || !editorMode) {
        return;
    }

    const QString mode = editorMode->currentData().toString();
    const bool isCommandMode = (mode == QLatin1String("exec-command") || mode == QLatin1String("command"));
    if (!isCommandMode) {
        acPopup->hide();
        return;
    }

    if (cmdCommandList.isEmpty()) {
        if (connected) {
            fetchCommandsForCompleter();
        }
        acPopup->hide();
        return;
    }

    QTextCursor tc = editor->textCursor();
    tc.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    tc.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    const QString line = tc.selectedText();

    int spaceIdx = line.indexOf(' ');
    QString prefix = (spaceIdx < 0) ? line.trimmed() : line.left(spaceIdx).trimmed();

    if (prefix.isEmpty() || prefix.length() < minCompleterChars) {
        acPopup->hide();
        return;
    }

    // Filter commands, preferring prefix matches over generic contains matches.
    acPopup->clear();
    int count = 0;
    const QString lowerPrefix = prefix.toLower();
    QStringList prefixMatches;
    QStringList containsMatches;
    for (const QString &cmd : cmdCommandList) {
        const QString lowerCmd = cmd.toLower();
        if (lowerCmd.startsWith(lowerPrefix)) {
            prefixMatches.append(cmd);
        } else if (lowerCmd.contains(lowerPrefix)) {
            containsMatches.append(cmd);
        }
    }

    const QStringList matches = prefixMatches + containsMatches;
    for (const QString &cmd : matches) {
        if (!cmd.isEmpty()) {
            acPopup->addItem(cmd);
            if (++count >= maxCompleterResults) {
                break;
            }
        }
    }

    if (count == 0) {
        acPopup->hide();
        return;
    }

    // Hide if the only match is exactly what the user typed.
    if (count == 1 && acPopup->item(0)->text().compare(prefix, Qt::CaseInsensitive) == 0) {
        acPopup->hide();
        return;
    }

    acPopup->setCurrentRow(0);

    int rowH = acPopup->sizeHintForRow(0);
    if (rowH < 1) {
        rowH = 20;
    }
    const int popupW = qMax(280, editor->viewport()->width() / 2);
    const int popupH = qMin(count * rowH + 6, 300);
    acPopup->setFixedWidth(popupW);
    acPopup->setFixedHeight(popupH);

    // Map cursor rect to global screen coordinates (popup is top-level).
    const QRect cr = editor->cursorRect();
    QPoint globalPos = editor->viewport()->mapToGlobal(
        QPoint(cr.left(), cr.bottom() + 2));

    // Keep popup on-screen.
    QScreen *screen = editor->screen();
    if (screen) {
        const QRect sr = screen->availableGeometry();
        if (globalPos.y() + popupH > sr.bottom()) {
            globalPos.setY(editor->viewport()->mapToGlobal(
                QPoint(0, cr.top())).y() - popupH - 2);
        }
        if (globalPos.x() + popupW > sr.right()) {
            globalPos.setX(qMax(sr.left(), sr.right() - popupW));
        }
    }

    acPopup->move(globalPos);
    acPopup->show();
    acPopup->raise();
}

bool CutterNotebookPlugin::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == editor && event->type() == QEvent::KeyPress && acPopup && acPopup->isVisible()) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Down: {
            int row = acPopup->currentRow();
            if (row < acPopup->count() - 1) {
                acPopup->setCurrentRow(row + 1);
            }
            return true;
        }
        case Qt::Key_Up: {
            int row = acPopup->currentRow();
            if (row > 0) {
                acPopup->setCurrentRow(row - 1);
            }
            return true;
        }
        case Qt::Key_Tab:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            acceptAutocomplete();
            return true;
        case Qt::Key_Escape:
            acPopup->hide();
            return true;
        default:
            break;
        }
    }
    return QObject::eventFilter(obj, event);
}

void CutterNotebookPlugin::updateEditorPlaceholder()
{
    if (!editor || !editorMode) {
        return;
    }
    const QString mode = editorMode->currentData().toString();
    if (mode == QLatin1String("markdown")) {
        editor->setPlaceholderText(QStringLiteral("Write markdown notes for the active page..."));
    } else if (mode == QLatin1String("command")) {
        editor->setPlaceholderText(QStringLiteral("Add a command cell, for example: afl"));
    } else if (mode == QLatin1String("script")) {
        editor->setPlaceholderText(QStringLiteral("Add a JavaScript cell executed by the notebook runtime..."));
    } else if (mode == QLatin1String("exec-command")) {
        editor->setPlaceholderText(QStringLiteral("Run a Rizin command locally and record to the active page..."));
    } else {
        editor->setPlaceholderText(QStringLiteral("Run a JavaScript snippet on the server..."));
    }
}

void CutterNotebookPlugin::renderPage(const QVariantMap &page)
{
    activePageId = page.value(QStringLiteral("id")).toString();
    updateComposeLabels(page);

    if (!pageView) {
        return;
    }

    QStringList lines;
    lines << QStringLiteral("Title: %1").arg(page.value(QStringLiteral("title")).toString());
    lines << QStringLiteral("Page ID: %1").arg(activePageId);
    lines << QStringLiteral("Filename: %1").arg(page.value(QStringLiteral("filename")).toString().isEmpty()
                                                    ? QStringLiteral("(none)")
                                                    : page.value(QStringLiteral("filename")).toString());
    const QString binaryHash = page.value(QStringLiteral("binary_hash")).toString();
    lines << QStringLiteral("Binary Hash: %1").arg(binaryHash.isEmpty()
                                                        ? QStringLiteral("(none)")
                                                        : binaryHash);
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

    verifyBinaryHash(binaryHash);
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
    if (m_populatingPages) {
        return false;
    }
    m_populatingPages = true;

    const QString previousSelection = keepSelection ? selectedPageId() : QString();
    int statusCode = 0;
    QString error;
    const QByteArray data = sendJsonRequest(QStringLiteral("GET"), QStringLiteral("/api/v1/json/pages"), {}, &statusCode, &error);
    if (data.isEmpty()) {
        if (!error.isEmpty()) {
            QMessageBox::warning(nullptr, QStringLiteral("Notebook"), error);
        }
        m_populatingPages = false;
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"), QStringLiteral("Invalid server response while listing pages."));
        m_populatingPages = false;
        return false;
    }

    const QJsonObject obj = doc.object();
    if (statusCode >= 400) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Notebook"),
                             obj.value(QStringLiteral("error")).toString(QStringLiteral("Failed to list pages.")));
        m_populatingPages = false;
        return false;
    }

    const QJsonArray pages = obj.value(QStringLiteral("pages")).toArray();
    pageListWidget->clear();

    QString selected = previousSelection;
    if (selected.isEmpty() && !activePageId.isEmpty()) {
        selected = activePageId;
    }

    for (const QJsonValue &value : pages) {
        const QJsonObject page = value.toObject();
        const QString pageId = page.value(QStringLiteral("id")).toString();
        QString label;
        if (pageId == activePageId) {
            label = QStringLiteral("\u2605 ");
        }
        label += page.value(QStringLiteral("title")).toString();
        const int cells = page.value(QStringLiteral("cells_count")).toInt();
        if (cells > 0) {
            label += QStringLiteral(" (%1)").arg(cells);
        }
        if (page.value(QStringLiteral("pipe")).toBool()) {
            label += QStringLiteral(" \u2022 pipe");
        }
        auto *item = new QListWidgetItem(label, pageListWidget);
        item->setData(Qt::UserRole, pageId);
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
        activePageId.clear();
        if (pageView) {
            pageView->setPlainText(QStringLiteral("No notebook pages found."));
        }
        updateComposeLabels();
    }

    if (statusLabel) {
        statusLabel->setText(QStringLiteral("Notebook: %1 page(s) – %2")
                                 .arg(pages.size())
                                 .arg(connected ? QStringLiteral("Connected") : QStringLiteral("Disconnected")));
    }
    m_populatingPages = false;
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
        connected = false;
        statusLabel->setText(QStringLiteral("Notebook: Offline (%1)").arg(baseUrl));
        statusLabel->setToolTip(QStringLiteral("Server unreachable."));
        return;
    }

    connected = true;
    if (cmdCommandList.isEmpty()) {
        fetchCommandsForCompleter();
        fetchAutocompleteSettings();
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

    const QJsonObject sobj = doc.object();
    statusLabel->setText(QStringLiteral("Notebook: %1 pages, %2 open pipe(s) – Connected")
                             .arg(sobj.value(QStringLiteral("pages")).toInt())
                             .arg(sobj.value(QStringLiteral("open_pipes")).toInt()));
    statusLabel->setToolTip(QStringLiteral("Server: %1\nRizin: %2\nStorage: %3")
                                .arg(baseUrl)
                                .arg(sobj.value(QStringLiteral("rizin_version")).toString())
                                .arg(sobj.value(QStringLiteral("storage")).toString()));
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

    ensureAction("Connect...", "NotebookConnectAction", &CutterNotebookPlugin::onConnect);
    ensureAction("Spawn Server", "NotebookSpawnAction", &CutterNotebookPlugin::onSpawn);
    ensureAction("Server Status", "NotebookServerStatusAction", &CutterNotebookPlugin::onServerStatus);
    ensureAction("List Pages", "NotebookListPagesAction", &CutterNotebookPlugin::onListPages);
    QAction *newPageAction = ensureAction("New Page...", "NotebookNewPageAction", &CutterNotebookPlugin::onNewPage);
    ensureAction("Delete Page", "NotebookDeletePageAction", &CutterNotebookPlugin::onDeletePage);
    ensureAction("Attach Binary...", "NotebookAttachBinaryAction", &CutterNotebookPlugin::onAttachBinary);
    ensureAction("Export Page...", "NotebookExportPageAction", &CutterNotebookPlugin::onExportPage);
    ensureAction("Import Page...", "NotebookImportPageAction", &CutterNotebookPlugin::onImportPage);

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

    QAction *browserAction = ensureAction("Open in Browser", "NotebookOpenBrowserAction", &CutterNotebookPlugin::onOpenBrowser);
    ensureAction("Set Server URL...", "NotebookSetUrlAction", &CutterNotebookPlugin::onSetUrl);

    newPageAction->setShortcut(QKeySequence());
    newPageAction->setShortcutContext(Qt::ApplicationShortcut);
    browserAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    browserAction->setShortcutContext(Qt::ApplicationShortcut);

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
    statusLabel = new QLabel("Notebook: Disconnected", container);
    topRow->addWidget(statusLabel, 1);

    auto *btnConnect = new QPushButton("Connect", container);
    auto *btnSpawn = new QPushButton("Spawn", container);
    auto *btnStatus = new QPushButton("Status", container);
    auto *btnRefresh = new QPushButton("Refresh", container);
    auto *btnBrowser = new QPushButton("Browser", container);
    auto *btnUrl = new QPushButton("URL", container);

    topRow->addWidget(btnConnect);
    topRow->addWidget(btnSpawn);
    topRow->addWidget(btnStatus);
    topRow->addWidget(btnRefresh);
    topRow->addWidget(btnBrowser);
    topRow->addWidget(btnUrl);
    layout->addLayout(topRow);

    dockTabs = new QTabWidget(container);
    layout->addWidget(dockTabs, 1);

    /* ── Pages tab ──────────────────────────────────────────────── */
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
    auto *btnExport = new QPushButton("Export", pagesTab);
    auto *btnImport = new QPushButton("Import", pagesTab);
    pagesButtons->addWidget(btnPages);
    pagesButtons->addWidget(btnNew);
    pagesButtons->addWidget(btnDelete);
    pagesButtons->addWidget(btnAttachBinary);
    pagesButtons->addWidget(btnOpenPipe);
    pagesButtons->addWidget(btnClosePipe);
    pagesButtons->addWidget(btnReloadPage);
    pagesButtons->addWidget(btnExport);
    pagesButtons->addWidget(btnImport);
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

    /* ── Compose tab ────────────────────────────────────────────── */
    auto *composeTab = new QWidget(dockTabs);
    auto *composeLayout = new QVBoxLayout(composeTab);
    composeLayout->setContentsMargins(0, 0, 0, 0);
    currentPageLabel = new QLabel(QStringLiteral("Active page: none selected"), composeTab);
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

    // --- command autocomplete via top-level tool window ---
    // The popup MUST be a top-level window, NOT a child of the editor
    // viewport.  QPlainTextEdit's viewport repaints over child widgets
    // on every keystroke, making embedded child popups invisible.
    acPopup = new QListWidget();
    acPopup->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint
                            | Qt::WindowStaysOnTopHint
                            | Qt::WindowDoesNotAcceptFocus);
    acPopup->setAttribute(Qt::WA_ShowWithoutActivating);
    acPopup->hide();
    acPopup->setFocusPolicy(Qt::NoFocus);
    acPopup->setMouseTracking(true);
    acPopup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    acPopup->setSelectionMode(QAbstractItemView::SingleSelection);
    acPopup->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    acPopup->setStyleSheet(QStringLiteral(
        "QListWidget { border: 1px solid #888; font-family: monospace; font-size: 12px;"
        "  background-color: palette(base); color: palette(text); }"
        "QListWidget::item { padding: 2px 6px; }"
        "QListWidget::item:selected { background: #3080d0; color: white; }"));
    connect(acPopup, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
        acceptAutocomplete();
    });
    editor->installEventFilter(this);
    connect(editor, &QPlainTextEdit::textChanged,
            this, &CutterNotebookPlugin::updateAutocomplete);
    connect(editorMode, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { updateAutocomplete(); });

    auto *composeButtons = new QHBoxLayout();
    auto *btnSubmit = new QPushButton("Submit", composeTab);
    auto *btnCancelCompose = new QPushButton("Cancel", composeTab);
    auto *btnComposeReload = new QPushButton("Reload Page", composeTab);
    composeButtons->addWidget(btnSubmit);
    composeButtons->addWidget(btnCancelCompose);
    composeButtons->addWidget(btnComposeReload);
    composeButtons->addStretch(1);
    composeLayout->addLayout(composeButtons);
    dockTabs->addTab(composeTab, QStringLiteral("Compose"));

    btnNew->setToolTip(QStringLiteral("New Page (%1)").arg(QKeySequence(QStringLiteral("Ctrl+N")).toString(QKeySequence::NativeText)));
    btnOpenPipe->setToolTip(QStringLiteral("Open Pipe (%1)").arg(QKeySequence(QStringLiteral("Ctrl+O")).toString(QKeySequence::NativeText)));
    btnClosePipe->setToolTip(QStringLiteral("Close Pipe (%1)").arg(QKeySequence(QStringLiteral("Ctrl+O")).toString(QKeySequence::NativeText)));
    btnBrowser->setToolTip(QStringLiteral("Open Browser (%1)").arg(QKeySequence(QStringLiteral("Ctrl+B")).toString(QKeySequence::NativeText)));
    btnSubmit->setToolTip(QStringLiteral("Submit current content (%1)").arg(QKeySequence(Qt::CTRL | Qt::Key_Return).toString(QKeySequence::NativeText)));
    editorMode->setToolTip(QStringLiteral("Compose shortcuts: Ctrl+E = Execute Command, Ctrl+M = Markdown"));

    auto bindShortcut = [this, main](const QString &sequence, auto handler) {
        auto *shortcut = new QShortcut(QKeySequence(sequence), main);
        shortcut->setContext(Qt::ApplicationShortcut);
        shortcut->setAutoRepeat(false);
        connect(shortcut, &QShortcut::activated, this, handler);
        return shortcut;
    };

    // Ctrl+N: New Page.
    // Connect to BOTH activated AND activatedAmbiguously so it fires even when another
    // part of Cutter also claims Ctrl+N (Qt would otherwise fire neither signal).
    {
        auto *ctrlNShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_N), main);
        ctrlNShortcut->setContext(Qt::ApplicationShortcut);
        ctrlNShortcut->setAutoRepeat(false);
        auto ctrlNHandler = [this]() {
            ensureDockVisible();
            onNewPage();
        };
        connect(ctrlNShortcut, &QShortcut::activated,            this, ctrlNHandler);
        connect(ctrlNShortcut, &QShortcut::activatedAmbiguously, this, ctrlNHandler);
    }
    bindShortcut(QStringLiteral("Ctrl+O"), [this]() {
        ensureDockVisible();
        const QString pageId = selectedPageId().isEmpty() ? activePageId : selectedPageId();
        if (pageId.isEmpty()) {
            QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select or create a page first."));
            return;
        }
        if (pipeLabel && pipeLabel->text().contains(QStringLiteral("open"), Qt::CaseInsensitive)) {
            onClosePipe();
            return;
        }
        onOpenPipe();
    });
    bindShortcut(QStringLiteral("Ctrl+E"), [this]() {
        ensureDockVisible();
        if (dockTabs) {
            dockTabs->setCurrentIndex(1);
        }
        if (editorMode) {
            int index = editorMode->findData(QStringLiteral("exec-command"));
            if (index >= 0) {
                editorMode->setCurrentIndex(index);
            }
        }
        if (editor) {
            editor->setFocus();
        }
    });
    bindShortcut(QStringLiteral("Ctrl+M"), [this]() {
        ensureDockVisible();
        if (dockTabs) {
            dockTabs->setCurrentIndex(1);
        }
        if (editorMode) {
            int index = editorMode->findData(QStringLiteral("markdown"));
            if (index >= 0) {
                editorMode->setCurrentIndex(index);
            }
        }
        if (editor) {
            editor->setFocus();
        }
    });

    /* ── Connections ─────────────────────────────────────────────── */
    connect(btnConnect, &QPushButton::clicked, this, &CutterNotebookPlugin::onConnect);
    connect(btnSpawn, &QPushButton::clicked, this, &CutterNotebookPlugin::onSpawn);
    connect(btnStatus, &QPushButton::clicked, this, &CutterNotebookPlugin::onServerStatus);
    connect(btnPages, &QPushButton::clicked, this, &CutterNotebookPlugin::onListPages);
    connect(btnNew, &QPushButton::clicked, this, &CutterNotebookPlugin::onNewPage);
    connect(btnDelete, &QPushButton::clicked, this, &CutterNotebookPlugin::onDeletePage);
    connect(btnAttachBinary, &QPushButton::clicked, this, &CutterNotebookPlugin::onAttachBinary);
    connect(btnOpenPipe, &QPushButton::clicked, this, &CutterNotebookPlugin::onOpenPipe);
    connect(btnClosePipe, &QPushButton::clicked, this, &CutterNotebookPlugin::onClosePipe);
    connect(btnReloadPage, &QPushButton::clicked, this, &CutterNotebookPlugin::onReloadSelectedPage);
    connect(btnExport, &QPushButton::clicked, this, &CutterNotebookPlugin::onExportPage);
    connect(btnImport, &QPushButton::clicked, this, &CutterNotebookPlugin::onImportPage);
    connect(btnComposeReload, &QPushButton::clicked, this, &CutterNotebookPlugin::onReloadSelectedPage);
    connect(btnUrl, &QPushButton::clicked, this, &CutterNotebookPlugin::onSetUrl);
    connect(btnBrowser, &QPushButton::clicked, this, &CutterNotebookPlugin::onOpenBrowser);
    connect(btnRefresh, &QPushButton::clicked, this, &CutterNotebookPlugin::onListPages);
    connect(btnSubmit, &QPushButton::clicked, this, &CutterNotebookPlugin::onSubmitEditor);
    connect(btnCancelCompose, &QPushButton::clicked, this, [this]() {
        if (!editor) {
            return;
        }
        editor->clear();
        editor->setFocus();
    });

    /* Ctrl+Enter submits from the compose tab (works even inside the editor) */
    auto *submitShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), composeTab);
    submitShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(submitShortcut, &QShortcut::activated, this, &CutterNotebookPlugin::onSubmitEditor);

    /* Escape clears the compose editor */
    auto *cancelShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), composeTab);
    cancelShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(cancelShortcut, &QShortcut::activated, this, [this]() {
        if (editor) {
            editor->clear();
            editor->setFocus();
        }
    });
    connect(editorMode, &QComboBox::currentIndexChanged, this, [this]() { updateEditorPlaceholder(); });
    connect(pageListWidget, &QListWidget::currentTextChanged, this, [this]() {
        if (m_populatingPages) {
            return;
        }
        const QString pageId = selectedPageId();
        if (!pageId.isEmpty()) {
            setActivePage(pageId);
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
    updateConnectionUI();
}

void CutterNotebookPlugin::terminate()
{
    if (acPopup) {
        acPopup->hide();
        delete acPopup;
        acPopup = nullptr;
    }
    if (dockWidget) {
        dockWidget->deleteLater();
        dockWidget = nullptr;
        statusLabel = nullptr;
    }
}

/* ── Connect / Spawn ──────────────────────────────────────────────────── */

void CutterNotebookPlugin::onConnect()
{
    ensureDockVisible();
    QString current = notebookBaseUrl();
    bool ok = false;
    QString url = QInputDialog::getText(nullptr, "Connect to Notebook Server",
                                        "Server URL:", QLineEdit::Normal,
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

    const QString pingUrl = normalizeBaseUrl(safeUrl) + QStringLiteral("/api/v1/ping");
    if (pingServer(pingUrl, 2000)) {
        connected = true;
        updateConnectionUI();
        populatePages(false);
        refreshDockStatus();
        fetchCommandsForCompleter();
        fetchAutocompleteSettings();
        QMessageBox::information(nullptr, "Notebook", QStringLiteral("Connected to %1").arg(safeUrl));
    } else {
        connected = false;
        updateConnectionUI();
        QMessageBox::warning(nullptr, "Notebook",
                             QStringLiteral("Could not reach server at %1.\nMake sure it is running.").arg(safeUrl));
    }
}

void CutterNotebookPlugin::onSpawn()
{
    ensureDockVisible();

    bool started = QProcess::startDetached(QStringLiteral("rizin-notebook.exe"));
    if (!started) {
        QMessageBox::warning(nullptr, "Notebook",
                             "Failed to start rizin-notebook.exe. Make sure it is installed and on PATH.");
        return;
    }

    statusLabel->setText(QStringLiteral("Notebook: Starting server..."));

    const QString baseUrl = notebookBaseUrl();
    const QString pingUrl = baseUrl + QStringLiteral("/api/v1/ping");
    const int waitMs = 6000;
    const int step = 300;
    int waited = 0;
    bool alive = false;
    while (waited < waitMs) {
        QThread::msleep(step);
        if (pingServer(pingUrl, 600)) {
            alive = true;
            break;
        }
        waited += step;
    }

    if (alive) {
        connected = true;
        updateConnectionUI();
        populatePages(false);
        refreshDockStatus();
        fetchCommandsForCompleter();
        fetchAutocompleteSettings();
        QMessageBox::information(nullptr, "Notebook", "Server spawned and connected.");
    } else {
        connected = false;
        updateConnectionUI();
        QMessageBox::warning(nullptr, "Notebook",
                             "rizin-notebook.exe was started but the server did not respond in time.");
    }
}

/* ── Menu / button actions ─────────────────────────────────────────────── */

void CutterNotebookPlugin::onServerStatus()
{
    ensureDockVisible();
    if (!ensureNotebookReady(true)) {
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
    QMessageBox::information(nullptr, QStringLiteral("Notebook \u2013 Server Status"), summary);
    refreshDockStatus();
}

void CutterNotebookPlugin::onListPages()
{
    ensureDockVisible();
    if (!ensureNotebookReady(true)) {
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
    QString title = QInputDialog::getText(mainWindow, "New Notebook Page",
                                          "Page title:", QLineEdit::Normal,
                                          QString(), &ok);
    if (!ok || title.isEmpty()) {
        return;
    }
    if (!ensureNotebookReady(true)) {
        refreshDockStatus();
        return;
    }

    QJsonObject req;
    req.insert(QStringLiteral("title"), title);

    // Auto-detect and attach the binary currently loaded in Cutter.
    // This mirrors the rizin plugin (NBn) which automatically reads
    // the binary from the currently analyzed file — no file dialog needed.
    const QString binaryPath = currentBinaryPath();
    if (!binaryPath.isEmpty()) {
        QFile file(binaryPath);
        if (file.open(QIODevice::ReadOnly)) {
            req.insert(QStringLiteral("filename"), QFileInfo(binaryPath).fileName());
            req.insert(QStringLiteral("binary_base64"), QString::fromLatin1(file.readAll().toBase64()));
        }
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

    const QString newPageId = obj.value(QStringLiteral("page")).toObject().value(QStringLiteral("id")).toString();
    if (!newPageId.isEmpty()) {
        activePageId = newPageId;
    }
    populatePages(false);
    if (!newPageId.isEmpty()) {
        loadPage(newPageId, true);
    }
    refreshDockStatus();
}

void CutterNotebookPlugin::onAttachBinary()
{
    ensureDockVisible();
    const QString pageId = selectedPageId().isEmpty() ? activePageId : selectedPageId();
    if (pageId.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select a page first."));
        return;
    }
    // Offer to attach the binary currently loaded in Cutter
    const QString cutterBin = currentBinaryPath();
    if (!cutterBin.isEmpty()) {
        auto choice = QMessageBox::question(nullptr,
            QStringLiteral("Notebook"),
            QStringLiteral("Attach the currently loaded binary?\n\n%1\n\n"
                           "Select 'No' to browse for a different file.").arg(cutterBin),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (choice == QMessageBox::Cancel) {
            return;
        }
        if (choice == QMessageBox::Yes) {
            attachBinaryToPage(pageId, cutterBin);
            return;
        }
    }
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
    if (activePageId == pageId) {
        activePageId.clear();
    }
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
            const QString cutterBin = currentBinaryPath();
            if (!cutterBin.isEmpty()) {
                if (QMessageBox::question(nullptr,
                                          QStringLiteral("Notebook"),
                                          QStringLiteral("This page has no binary attached. "
                                                         "Attach the currently loaded binary?\n\n%1").arg(cutterBin)) == QMessageBox::Yes) {
                    if (attachBinaryToPage(pageId, cutterBin)) {
                        onOpenPipe();
                    }
                }
            } else {
                if (QMessageBox::question(nullptr,
                                          QStringLiteral("Notebook"),
                                          QStringLiteral("This page has no binary attached yet. Select one now?")) == QMessageBox::Yes) {
                    if (attachBinaryToPage(pageId)) {
                        onOpenPipe();
                    }
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
    const QString pageId = selectedPageId().isEmpty() ? activePageId : selectedPageId();
    if (pageId.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select or create a page first."));
        return;
    }
    const QString content = editor->toPlainText().trimmed();
    if (content.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Enter some content first."));
        return;
    }

    const QString mode = editorMode->currentData().toString();

    if (mode == QLatin1String("exec-command")) {
        /* Verify a file is open in Cutter before executing locally */
        const QString currentFile = currentBinaryPath();
        if (currentFile.isEmpty()) {
            QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                                 QStringLiteral("No file is open in Cutter. Open a binary first to execute commands locally."));
            return;
        }

        /* Run command locally via Cutter core, then record to server */
        const QString output = Core()->cmdRaw(content).trimmed();

        QJsonObject req;
        req.insert(QStringLiteral("command"), content);
        req.insert(QStringLiteral("output"), output);

        int statusCode = 0;
        QString error;
        const QByteArray data = sendJsonRequest(QStringLiteral("POST"),
                                                QStringLiteral("/api/v1/json/pages/%1/record").arg(pageId),
                                                QJsonDocument(req).toJson(QJsonDocument::Compact),
                                                &statusCode,
                                                &error);
        if (data.isEmpty()) {
            QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                                 error.isEmpty() ? QStringLiteral("Failed to record command.") : error);
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (statusCode >= 400) {
            QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                                 obj.value(QStringLiteral("error")).toString(QStringLiteral("Failed to record command.")));
            return;
        }

        loadPage(pageId, true);
        refreshDockStatus();
        editor->setFocus();
        return;
    }

    QString path;
    QJsonObject req;
    if (mode == QLatin1String("markdown") || mode == QLatin1String("command") || mode == QLatin1String("script")) {
        path = QStringLiteral("/api/v1/json/pages/%1/cells").arg(pageId);
        req.insert(QStringLiteral("type"), mode);
        req.insert(QStringLiteral("content"), content);
    } else {
        /* exec-script */
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

    const bool keepEditorOpen = (mode == QLatin1String("exec-script"));
    if (!keepEditorOpen) {
        editor->clear();
    }
    loadPage(pageId, true);
    refreshDockStatus();
    editor->setFocus();
}

void CutterNotebookPlugin::onReloadSelectedPage()
{
    const QString pageId = selectedPageId().isEmpty() ? activePageId : selectedPageId();
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
    connected = false;
    updateConnectionUI();
}

/* ── Export / Import ──────────────────────────────────────────────────── */

void CutterNotebookPlugin::onExportPage()
{
    ensureDockVisible();
    const QString pageId = selectedPageId().isEmpty() ? activePageId : selectedPageId();
    if (pageId.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Select a page first."));
        return;
    }
    if (!ensureNotebookReady(true)) {
        return;
    }

    int statusCode = 0;
    QString contentDisposition;
    QString error;
    const QByteArray rawData = sendRawGetRequest(
        QStringLiteral("/api/v1/json/pages/%1/export").arg(pageId),
        &statusCode,
        &contentDisposition,
        &error);

    if (rawData.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                             error.isEmpty() ? QStringLiteral("Export returned empty data.") : error);
        return;
    }
    if (statusCode >= 400) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                             QStringLiteral("Server returned error %1 during export.").arg(statusCode));
        return;
    }

    /* Try to extract filename from Content-Disposition */
    QString suggestedName = QStringLiteral("notebook.rznb");
    if (!contentDisposition.isEmpty()) {
        int idx = contentDisposition.indexOf(QStringLiteral("filename="));
        if (idx >= 0) {
            QString fname = contentDisposition.mid(idx + 9).trimmed();
            if (fname.startsWith('"') && fname.endsWith('"')) {
                fname = fname.mid(1, fname.length() - 2);
            }
            if (!fname.isEmpty()) {
                suggestedName = fname;
            }
        }
    }

    const QString savePath = QFileDialog::getSaveFileName(nullptr,
                                                          QStringLiteral("Export Notebook Page"),
                                                          suggestedName,
                                                          QStringLiteral("Rizin Notebook (*.rznb);;All Files (*)"));
    if (savePath.isEmpty()) {
        return;
    }

    QFile outFile(savePath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                             QStringLiteral("Failed to write file: %1").arg(savePath));
        return;
    }
    outFile.write(rawData);
    outFile.close();
    QMessageBox::information(nullptr, QStringLiteral("Notebook"),
                             QStringLiteral("Page exported to %1").arg(savePath));
}

void CutterNotebookPlugin::onImportPage()
{
    ensureDockVisible();
    if (!ensureNotebookReady(true)) {
        return;
    }

    const QString filePath = QFileDialog::getOpenFileName(nullptr,
                                                          QStringLiteral("Import Notebook Page"),
                                                          QString(),
                                                          QStringLiteral("Rizin Notebook (*.rznb);;All Files (*)"));
    if (filePath.isEmpty()) {
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                             QStringLiteral("Failed to read file: %1").arg(filePath));
        return;
    }
    const QByteArray fileData = file.readAll();
    file.close();

    int statusCode = 0;
    QString error;
    const QByteArray responseData = sendMultipartPost(
        QStringLiteral("/api/v1/json/pages/import"),
        QStringLiteral("file"),
        QFileInfo(filePath).fileName(),
        fileData,
        &statusCode,
        &error);

    if (responseData.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                             error.isEmpty() ? QStringLiteral("Import failed.") : error);
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(responseData).object();
    if (statusCode >= 400) {
        QMessageBox::warning(nullptr, QStringLiteral("Notebook"),
                             obj.value(QStringLiteral("error")).toString(QStringLiteral("Import failed.")));
        return;
    }

    const QString newPageId = obj.value(QStringLiteral("page")).toObject().value(QStringLiteral("id")).toString();
    if (!newPageId.isEmpty()) {
        activePageId = newPageId;
    }
    populatePages(false);
    if (!newPageId.isEmpty()) {
        loadPage(newPageId, true);
    }
    QMessageBox::information(nullptr, QStringLiteral("Notebook"), QStringLiteral("Page imported successfully."));
    refreshDockStatus();
}

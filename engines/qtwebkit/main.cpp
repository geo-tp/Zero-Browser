// Deliberately separate from the framebuffer supervisor and GTK adapter.
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QFrame>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QShortcut>
#include <QByteArray>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QPaintEvent>
#include <QTimer>
#include <QWebElement>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QSocketNotifier>
#include <QWindow>
#include <QtWebKit/QWebSettings>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebView>
#include <QtWebKit/QWebHistory>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t stopping = 0;
void stopSignal(int) { stopping = 1; }
void event(const char *kind, const QUrl &url = {}, double seconds = 0, const QString &message = {}, int progress = -1) {
    auto quote = [](QByteArray value) {
        value.replace('\n', ' '); value.replace('\r', ' ');
        value.replace("\\", "\\\\"); value.replace("\"", "\\\"");
        return value;
    };
    const auto address = quote(url.toEncoded()), detail = quote(message.toUtf8());
    printf("ZERO_QT_EVENT %s %.3f \"%s\" \"%s\" %d\n", kind, seconds, address.constData(), detail.constData(), progress);
    fflush(stdout);
}
class Network final : public QNetworkAccessManager {
  public:
    int pending = 0;
    std::function<void()> changed;
    using QNetworkAccessManager::QNetworkAccessManager;
  protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *data) override {
        auto *reply = QNetworkAccessManager::createRequest(op, request, data);
        ++pending;
        if (changed) changed();
        QObject::connect(reply, &QNetworkReply::finished, this, [this] {
            --pending;
            if (changed) changed();
        });
        return reply;
    }
};
// Stop direct file links before WebKit replaces the page with a policy error.
// Keep HTML5 media and documents in the renderer; only unambiguous file
// archives/packages are routed directly to our download manager.
class Page final : public QWebPage {
  public:
    explicit Page(QObject *parent = nullptr) : QWebPage(parent) {}
    std::function<void(const QNetworkRequest &)> directDownload;
  protected:
    // Keep the native WebKit/Safari capability tokens, but omit the Qt-specific
    // brand that can trigger browser allow-lists. This does NOT upgrade WebKit.
    // ZERO_QT_NATIVE_UA=1 restores the upstream UA for A/B comparison.
    QString userAgentForUrl(const QUrl &url) const override {
        QString agent = QWebPage::userAgentForUrl(url);
        if (qEnvironmentVariableIsSet("ZERO_QT_NATIVE_UA")) return agent;
        agent.remove(QRegularExpression("\\s+(?:QtWebKit|Qt)/[^\\s]+"));
        return agent;
    }
    bool acceptNavigationRequest(QWebFrame *frame, const QNetworkRequest &request,
                                 NavigationType type) override {
        if (frame && type == QWebPage::NavigationTypeLinkClicked &&
            webDownloadUrl(request.url())) {
            if (directDownload) {
                const QNetworkRequest copy(request);
                QTimer::singleShot(0, this, [this, copy] {
                    if (directDownload) directDownload(copy);
                });
                return false;
            }
        }
        return QWebPage::acceptNavigationRequest(frame, request, type);
    }
  private:
    static bool webDownloadUrl(const QUrl &url) {
        if (url.scheme() != "http" && url.scheme() != "https") return false;
        const QString suffix = QFileInfo(url.path()).suffix().toLower();
        return QSet<QString>{"zip", "deb", "tar", "gz", "tgz", "xz", "bz2", "7z",
                             "rar", "apk", "iso", "dmg", "exe", "msi", "bin"}.contains(suffix);
    }
};
class View final : public QWebView {
  public:
    QElapsedTimer clock;
    qint64 lastInput = -1, maxInputPaint = 0;
    int paints = 0, inputPaints = 0;
    double maxPaintMs = 0;
    std::function<void()> painted;
    View() { clock.start(); }
  protected:
    // This browser has a single view: target=_blank, window.open() and
    // context-menu new-window actions must navigate in the existing view.
    // QtWebKit will load the popup request into the returned QWebView.
    QWebView *createWindow(QWebPage::WebWindowType) override {
        return this;
    }
    bool event(QEvent *e) override {
        if (e->type() == QEvent::KeyPress || e->type() == QEvent::MouseButtonPress ||
            e->type() == QEvent::Wheel) lastInput = clock.elapsed();
        return QWebView::event(e);
    }
    void paintEvent(QPaintEvent *e) override {
        QElapsedTimer timer;
        timer.start();
        QWebView::paintEvent(e);
        maxPaintMs = std::max(maxPaintMs, timer.nsecsElapsed() / 1000000.0);
        ++paints;
        if (painted) painted();
        if (lastInput >= 0) {
            maxInputPaint = std::max(maxInputPaint, clock.elapsed() - lastInput);
            ++inputPaints;
            lastInput = -1;
        }
    }
};
bool webUrl(const QUrl &url) {
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty();
}
} // namespace

int main(int argc, char **argv) {
    // Capture this before QApplication consumes its own platform arguments.
    const QUrl initial = QUrl::fromEncoded(argc > 1 ? argv[1] : "https://lite.duckduckgo.com/lite/");
    if (!webUrl(initial)) return 2;
    std::signal(SIGTERM, stopSignal);
    std::signal(SIGINT, stopSignal);
    QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
    QApplication app(argc, argv);
    app.setApplicationName("zero-qtwebkit");
    app.setDesktopFileName("org.cardputer.ZeroBrowser.QtWebKit");
    View view;
    auto *page = new Page(&view);
    view.setPage(page);
    view.setWindowTitle("Zero Browser - QtWebKit Experimental");
    QObject::connect(&view, &QWebView::titleChanged, &view, &QWidget::setWindowTitle);
    view.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    bool heightOk = false;
    const int height = qEnvironmentVariableIntValue("ZERO_QT_VIEW_HEIGHT", &heightOk);
    view.setFixedSize(320, heightOk ? std::clamp(height, 130, 170) : 170);
    view.settings()->setAttribute(QWebSettings::JavascriptEnabled, true);
    view.settings()->setAttribute(QWebSettings::AutoLoadImages, true);
    // WebKit scrollbars are part of the rendered page, not QtWidgets scrollbars.
    // Use WebKit's scrollbar pseudo-elements without modifying the website CSS.
    const QByteArray compactScrollbars =
        "::-webkit-scrollbar { width: 7px; height: 7px; }"
        "::-webkit-scrollbar-track { background: #f1f3f5; }"
        "::-webkit-scrollbar-thumb { background: #b0b8c1; border-radius: 3px; }";
    view.settings()->setUserStyleSheetUrl(QUrl("data:text/css;charset=utf-8;base64," +
                                            QString::fromLatin1(compactScrollbars.toBase64())));
    // A smaller initial zoom reduces horizontal clipping.
    // This does not reduce the DOM's memory requirements on heavy pages.
    view.setZoomFactor(0.55);
    auto availableMemoryMb = []() -> long {
        std::ifstream in("/proc/meminfo");
        std::string key, unit; long value;
        while (in >> key >> value >> unit)
            if (key == "MemAvailable:") return value / 1024;
        return -1;
    };
    QElapsedTimer zoomClock;
    zoomClock.start();
    qint64 lastZoomMs = -1000;
    double pendingZoom = 0;
    QSet<QString> downloadUrls;
    bool waitingForPaint = false;
    QTimer renderFallback;
    renderFallback.setSingleShot(true);
    auto renderingDone = [&] {
        if (!waitingForPaint) return;
        waitingForPaint = false;
        renderFallback.stop();
        event("rendered", view.url());
    };
    QObject::connect(&renderFallback, &QTimer::timeout, &view, renderingDone);
    view.painted = renderingDone;
    // QtWebKit QWebView paints through QtWidgets. Hardware video/compositing is not
    // assumed: GPU presence does not imply accelerated DOM or video decoding.
    // CSS/DOM rendering remains in QWebView.
    view.setContextMenuPolicy(Qt::DefaultContextMenu);
    bool loading = false, ready = false;
    bool downloadHandoff = false;
    QUrl lastSuccessfulUrl;
    // QtWebKit does not provide a ready-made downloads window. Keep one small
    // Qt-native manager, independent of the supervisor framebuffer overlay.
    QDialog downloads(nullptr);
    downloads.setWindowTitle("Downloads");
    downloads.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    downloads.setFixedSize(316, 156);
    downloads.setStyleSheet(
        "QDialog { background: #f3f4f6; color: #243447; }"
        "QLabel { color: #65717b; font-size: 10px; }"
        "QPushButton { background: #e8ebef; border: 1px solid #cdd3d8;"
        " border-radius: 4px; padding: 2px 8px; color: #243747; }"
        "QPushButton:pressed { background: #d6dee5; }");
    auto *downloadsLayout = new QVBoxLayout(&downloads);
    downloadsLayout->setContentsMargins(3, 4, 3, 4);
    downloadsLayout->setSpacing(4);
    auto *header = new QHBoxLayout();
    auto *heading = new QLabel("DOWNLOADS", &downloads);
    auto *closeButton = new QPushButton("Close", &downloads);
    header->addWidget(heading);
    header->addStretch();
    header->addWidget(closeButton);
    downloadsLayout->addLayout(header);
    // Scrollable, persistent per-download cards (instead of an unstructured log).
    auto *downloadScroll = new QScrollArea(&downloads);
    downloadScroll->setWidgetResizable(true);
    downloadScroll->setFrameShape(QFrame::NoFrame);
    downloadScroll->setStyleSheet("QScrollArea { background: #ffffff; border: 1px solid #d1dce6; border-radius: 5px; }");
    auto *downloadList = new QWidget(downloadScroll);
    downloadList->setStyleSheet("background: white;");
    auto *transfersLayout = new QVBoxLayout(downloadList);
    transfersLayout->setContentsMargins(4, 4, 4, 4);
    transfersLayout->setSpacing(4);
    auto *emptyLabel = new QLabel("No downloads yet.", downloadList);
    transfersLayout->addWidget(emptyLabel);
    transfersLayout->addStretch();
    downloadScroll->setWidget(downloadList);
    downloadsLayout->addWidget(downloadScroll, 1);
    auto addDownloadNotice = [&](const QString &message, const QString &color) {
        emptyLabel->hide();
        auto *item = new QLabel(message, downloadList);
        item->setWordWrap(true);
        item->setStyleSheet("color: " + color + "; font-size: 10px;");
        transfersLayout->insertWidget(transfersLayout->count()-1, item);
    };
    auto hideDownloads = [&] { downloads.hide(); event("notice", {}, 0, "downloads:closed"); view.raise(); view.activateWindow(); view.setFocus(); };
    QObject::connect(closeButton, &QPushButton::clicked, &downloads, hideDownloads);
    auto *closeDownloads = new QShortcut(QKeySequence(Qt::Key_Escape), &downloads);
    QObject::connect(closeDownloads, &QShortcut::activated, &downloads, hideDownloads);
    auto showDownloads = [&] {
        downloads.show();
        event("notice", {}, 0, "downloads:open");
        downloads.raise();
        downloads.activateWindow();
    };
    auto toggleDownloads = [&] {
        if (downloads.isVisible()) hideDownloads();
        else showDownloads();
    };
    auto *network = new Network(view.page());
    // Persist HTTP responses under our isolated XDG cache root. QNetworkDiskCache
    // manages eviction internally; never prune its files from outside Qt.
    auto *diskCache = new QNetworkDiskCache(network);
    const QString cacheRoot = QString::fromLocal8Bit(qgetenv("XDG_CACHE_HOME"));
    const QString cacheDirectory = cacheRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/http"
        : QDir(cacheRoot).filePath("http");
    QDir().mkpath(cacheDirectory);
    diskCache->setCacheDirectory(cacheDirectory);
    diskCache->setMaximumCacheSize(256LL * 1024 * 1024);
    network->setCache(diskCache);
    view.page()->setNetworkAccessManager(network);
    const QString downloadDirectory = [] {
        auto directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (directory.isEmpty()) directory = QDir::home().filePath("Downloads");
        QDir().mkpath(directory);
        return directory;
    }();
    auto *destinationLabel = new QLabel("Save to: " + QDir::toNativeSeparators(downloadDirectory), &downloads);
    destinationLabel->setWordWrap(true);
    destinationLabel->setStyleSheet("color: #445663; font-size: 9px;");
    downloadsLayout->insertWidget(1, destinationLabel);
    // Ask before committing a download. Keep the dialog small enough for
    // the Cardputer Zero LCD and show the exact destination directory.
    auto confirmDownload = [&](const QUrl &url, const QString &filename) -> bool {
        QDialog confirm(&view);
        confirm.setWindowTitle("Confirm download");
        confirm.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        confirm.setFixedSize(310, 150);
        confirm.setStyleSheet(
            "QDialog { background: #f5f5f5; color: #222; }"
            "QLabel { color: #222; font-size: 10px; }"
            "QPushButton { background: #e9e9e9; color: #222; border: 1px solid #aaa;"
            " border-radius: 3px; padding: 3px 9px; min-height: 20px; }"
        );
        auto *layout = new QVBoxLayout(&confirm);
        layout->setContentsMargins(7, 6, 7, 6);
        layout->setSpacing(4);
        auto *title = new QLabel("Download this file?", &confirm);
        title->setStyleSheet("font-size: 12px; font-weight: bold;");
        layout->addWidget(title);
        auto *name = new QLabel(filename, &confirm);
        name->setWordWrap(true);
        name->setMaximumHeight(28);
        layout->addWidget(name);
        auto *folder = new QLabel("Save to: " + QDir::toNativeSeparators(downloadDirectory), &confirm);
        folder->setWordWrap(true);
        folder->setMaximumHeight(30);
        folder->setToolTip(url.toString());
        layout->addWidget(folder);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save, &confirm);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &confirm, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &confirm, &QDialog::reject);
        layout->addWidget(buttons);
        return confirm.exec() == QDialog::Accepted;
    };
    auto suggestedName = [](const QUrl &url, const QByteArray &disposition) {
        QString filename = QFileInfo(url.path()).fileName();
        const int filenameAt = disposition.indexOf("filename=");
        if (filenameAt >= 0) {
            QString suggested = QString::fromLatin1(disposition.mid(filenameAt + 9)).split(';').first().trimmed();
            if (suggested.startsWith('"') && suggested.endsWith('"')) suggested = suggested.mid(1, suggested.size()-2);
            if (!suggested.isEmpty()) filename = QFileInfo(suggested).fileName();
        }
        if (filename.isEmpty() || filename == "." || filename == "..") filename = "download.bin";
        filename.replace('/', '_'); filename.replace('\\', '_');
        return filename;
    };
    // Qt 5 does not reliably follow redirects for manually issued requests by
    // default. Release assets frequently redirect to short-lived CDN URLs.
    auto requestDownload = [&](QNetworkRequest request) -> QNetworkReply * {
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        request.setMaximumRedirectsAllowed(10);
        return network->get(request);
    };
    auto saveReply = [&](QNetworkReply *reply, bool preconfirmed = false) {

        if (!reply) return;
        const QUrl url = reply->url();
        if (downloadUrls.contains(url.toString())) return;
        const QString filename = suggestedName(url, reply->rawHeader("Content-Disposition"));
        if (!preconfirmed && !confirmDownload(url, filename)) {
            reply->abort();
            reply->deleteLater();
            return;
        }
        downloadUrls.insert(url.toString());
        QString output = QDir(downloadDirectory).filePath(filename);
        for (int i = 1; QFile::exists(output) && i < 10000; ++i)
            output = QDir(downloadDirectory).filePath(QString::number(i) + "-" + filename);
        auto file = std::make_shared<QFile>(output);
        if (!file->open(QIODevice::WriteOnly)) {
            downloadUrls.remove(url.toString());
            reply->abort();
            reply->deleteLater();
            addDownloadNotice("✕ Cannot save: " + output, "#b42318");
            showDownloads();
            return;
        }
        emptyLabel->hide();
        // Compact status card with clear filename, result icon and real cancel.
        auto *transferRow = new QFrame(downloadList);
        transferRow->setStyleSheet("QFrame { background: #f6f8fa; border: 1px solid #dde3e8; border-radius: 4px; }");
        auto *transferLayout = new QVBoxLayout(transferRow);
        transferLayout->setContentsMargins(4, 3, 4, 3);
        transferLayout->setSpacing(2);
        auto *line = new QHBoxLayout();
        auto *statusIcon = new QLabel("↓", transferRow);
        statusIcon->setFixedWidth(13);
        statusIcon->setStyleSheet("color: #376a99; font-size: 13px; font-weight: bold; border: none;");
        auto *transferName = new QLabel(QFileInfo(output).fileName(), transferRow);
        transferName->setToolTip(output);
        transferName->setStyleSheet("color: #243447; font-size: 10px; border: none;");
        auto *cancel = new QPushButton("Cancel", transferRow);
        cancel->setFixedHeight(20);
        line->addWidget(statusIcon);
        line->addWidget(transferName, 1);
        line->addWidget(cancel);
        transferLayout->addLayout(line);
        auto *transferProgress = new QProgressBar(transferRow);
        transferProgress->setRange(0, 0);
        transferProgress->setTextVisible(false);
        transferProgress->setFixedHeight(6);
        transferProgress->setStyleSheet("QProgressBar { border: none; background: #dde5ec; border-radius: 3px; } QProgressBar::chunk { background: #5c98c5; border-radius: 3px; }");
        transferLayout->addWidget(transferProgress);
        transfersLayout->insertWidget(transfersLayout->count()-1, transferRow);
        QObject::connect(cancel, &QPushButton::clicked, reply, &QNetworkReply::abort);
        QObject::connect(reply, &QNetworkReply::downloadProgress, transferRow,
                         [transferProgress](qint64 received, qint64 total) {
            if (total > 0) {
                transferProgress->setRange(0, 100);
                transferProgress->setValue(static_cast<int>(std::clamp<qint64>(received * 100 / total, 0, 100)));
            }
        });
        showDownloads();
        const auto bytesWritten = std::make_shared<qint64>(0);
        const auto writeFailed = std::make_shared<bool>(false);
        const auto drain = [reply, file, bytesWritten, writeFailed] {
            const QByteArray chunk = reply->readAll();
            if (chunk.isEmpty() || *writeFailed) return;
            const qint64 written = file->write(chunk);
            if (written != chunk.size()) {
                *writeFailed = true;
                reply->abort();
                return;
            }
            *bytesWritten += written;
        };
        QObject::connect(reply, &QIODevice::readyRead, &view, drain);
        const auto finalized = std::make_shared<bool>(false);
        const auto finish = [reply, file, output, url, &downloadUrls, transferRow,
                             statusIcon, transferProgress, cancel, finalized,
                             bytesWritten, writeFailed, drain] {
            if (*finalized) return;
            *finalized = true;
            downloadUrls.remove(url.toString());
            drain();
            const bool flushed = file->flush();
            file->close();
            cancel->hide();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const qint64 expected = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
            const bool httpOk = httpStatus >= 200 && httpStatus < 300;
            const bool lengthOk = expected <= 0 || *bytesWritten == expected;
            const bool ok = reply->error() == QNetworkReply::NoError &&
                            httpOk && *bytesWritten > 0 && lengthOk &&
                            !*writeFailed && flushed;
            if (ok) {
                statusIcon->setText("✓");
                statusIcon->setStyleSheet("color: #198754; font-size: 13px; font-weight: bold; border: none;");
                transferProgress->setRange(0, 100);
                transferProgress->setValue(100);
            } else {
                QFile::remove(output);
                statusIcon->setText("✕");
                statusIcon->setStyleSheet("color: #bf3b32; font-size: 13px; font-weight: bold; border: none;");
                transferProgress->hide();
                QString reason = reply->error() != QNetworkReply::NoError
                    ? reply->errorString()
                    : !httpOk ? QString("HTTP %1").arg(httpStatus)
                    : *writeFailed || !flushed ? QString("Disk write failed")
                    : *bytesWritten == 0 ? QString("Empty response")
                    : QString("Incomplete transfer (%1/%2 bytes)").arg(*bytesWritten).arg(expected);
                transferRow->setToolTip(reason);
                event("notice", url, 0, "download failed: " + reason);
            }
            reply->deleteLater();
        };
        QObject::connect(reply, &QNetworkReply::finished, &view, finish);
        // Confirmation may have nested the event loop, leaving the reply
        // completed or with data buffered before these connections existed.
        drain();
        if (reply->isFinished()) finish();
    };
    view.page()->setForwardUnsupportedContent(true);
    QObject::connect(view.page(), &QWebPage::unsupportedContent, &view,
                     [&](QNetworkReply *reply) {
        downloadHandoff = true;
        saveReply(reply);
        // The download is not a page fetch: release the page progress overlay.
        if (loading) {
            loading = false;
            event("loaded", lastSuccessfulUrl.isEmpty() ? view.url() : lastSuccessfulUrl);
        }
    });
    QObject::connect(view.page(), &QWebPage::downloadRequested, &view,
                     [&](const QNetworkRequest &request) {
        // downloadRequested supplies only a request, unlike unsupportedContent.
        // Duplicate requests may fail for single-use URLs or POST endpoints.
        if (downloadUrls.contains(request.url().toString())) return;
        downloadHandoff = true;
        if (loading) {
            loading = false;
            event("loaded", lastSuccessfulUrl.isEmpty() ? view.url() : lastSuccessfulUrl);
        }
        if (!confirmDownload(request.url(), suggestedName(request.url(), {}))) return;
        saveReply(requestDownload(request), true);
    });
    // Direct file links never enter WebKit's frame loading state.
    page->directDownload = [&](const QNetworkRequest &request) {
        if (downloadUrls.contains(request.url().toString())) return;
        if (!confirmDownload(request.url(), suggestedName(request.url(), {}))) return;
        saveReply(requestDownload(request), true);
    };
    int progress = 0, reportedProgress = -1;
    QString reportedPhase;
    auto reportProgress = [&] {
        if (!loading) return;
        // These describe outstanding resource requests vs. engine work, not
        // guessed DNS/download/JavaScript stages or a percentage of wall time.
        const QString phase = network->pending ? "Fetching" : "Processing";
        if (progress != reportedProgress || phase != reportedPhase) {
            event("progress", {}, 0, phase, progress);
            reportedProgress = progress;
            reportedPhase = phase;
        }
    };
    network->changed = reportProgress;
    QElapsedTimer loadTime, sampleTime;
    sampleTime.start();
    QObject::connect(&view, &QWebView::loadStarted, [&] {
        downloadHandoff = false;
        loading = true;
        waitingForPaint = false;
        renderFallback.stop();
        progress = 0;
        reportedProgress = -1;
        reportedPhase.clear();
        loadTime.start();
        const auto requested = view.page()->mainFrame()->requestedUrl();
        event("start", webUrl(requested) ? requested : QUrl());
    });
    QObject::connect(&view, &QWebView::loadProgress, [&](int value) {
        progress = value;
        reportProgress();
    });
    // Link hover is advisory: never navigate on hover. Keep IPC bounded.
    QObject::connect(view.page(), &QWebPage::linkHovered,
                     [&](const QString &link, const QString &, const QString &) {
        QString address = link;
        address.replace('\n', ' ');
        address.replace('\r', ' ');
        event("hover", {}, 0, QStringLiteral("hover:") + address.left(240));
    });
    QObject::connect(&view, &QWebView::urlChanged, [&](const QUrl &url) {
        if (webUrl(url)) event("url", url);
    });
    QObject::connect(&view, &QWebView::loadFinished, [&](bool ok) {
        loading = false;
        const double seconds = loadTime.isValid() ? loadTime.elapsed() / 1000.0 : 0;
        // A WebKit download can cancel the originating frame navigation by policy.
        // That does not mean the download failed: the download manager tracks the reply.
        const bool isDownloadHandoff = !ok && downloadHandoff;
        downloadHandoff = false;
        if (ok) lastSuccessfulUrl = view.url();
        if (isDownloadHandoff) {
            // WebKit can report an interrupted frame as a failed navigation.
            // Do not leave the host UI on Fetching 100% after the handoff.
            event("loaded", lastSuccessfulUrl.isEmpty() ? view.url() : lastSuccessfulUrl, seconds);
        } else {
            event(ok ? "loaded" : "failed", view.url(), seconds,
                  ok ? QString() : QStringLiteral("QtWebKit page load failed; see qtwebkit.log"));
        }
        if (ok) {
            waitingForPaint = true;
            event("rendering", view.url(), seconds);
            renderFallback.start(5000); // Paint is a heuristic, not GPU completion.
        }
        printf("qtwebkit.load ok=%d seconds=%.3f\n", ok, seconds);
        fflush(stdout);
    });
    QObject::connect(view.page()->networkAccessManager(), &QNetworkAccessManager::finished,
                     [](QNetworkReply *reply) {
        if (reply->error() != QNetworkReply::NoError)
            fprintf(stderr, "qtwebkit.network %d %s\n", reply->error(), qPrintable(reply->errorString()));
    });
    bool fdOk = false;
    const int fd = qEnvironmentVariableIntValue("ZERO_QT_CONTROL_FD", &fdOk);
    std::unique_ptr<QSocketNotifier> control;
    if (fdOk && fd >= 0) {
        control = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
        QObject::connect(control.get(), &QSocketNotifier::activated, [&](int) {
            char data[16384];
            const auto size = recv(fd, data, sizeof(data), MSG_DONTWAIT);
            if (size == 0) { app.quit(); return; }
            if (size < 0) return;
            const QByteArray cmd(data, static_cast<int>(size));
            if (cmd == "quit") app.quit();
            else if (cmd == "back") { if (view.history()->canGoBack()) view.back(); }
            else if (cmd == "forward") { if (view.history()->canGoForward()) view.forward(); }
            else if (cmd == "stop") { view.stop(); event("notice", view.url(), 0, QStringLiteral("Loading stopped")); }
            else if (cmd == "reload") {
                if (pendingZoom > 0) { view.setZoomFactor(pendingZoom); pendingZoom = 0; }
                view.reload();
            }
            else if (cmd == "toggle-js") {
                const bool enabled = !view.settings()->testAttribute(QWebSettings::JavascriptEnabled);
                view.settings()->setAttribute(QWebSettings::JavascriptEnabled, enabled);
                view.stop();
                view.reload();
                event("notice", view.url(), 0, enabled ? QStringLiteral("Reloading with JavaScript")
                                                     : QStringLiteral("Reloading without JavaScript"));
            }
            else if (cmd == "downloads") toggleDownloads();
            else if (cmd == "close-downloads") hideDownloads();
            else if (cmd == "zoom-in" || cmd == "zoom-out") {
                // Each zoom reflows the page; ignore rapid key repeats to avoid
                // queuing expensive reflows on memory-constrained hardware.
                const qint64 nowMs = zoomClock.elapsed();
                const long available = availableMemoryMb();
                const auto host = view.url().host().toLower();
                const bool lightweightHome = host == "lite.duckduckgo.com";
                // DuckDuckGo Lite is cheap to reflow, even with low reported free RAM.
                // On other pages refuse expensive reflow only when memory is genuinely tight.
                if (nowMs - lastZoomMs >= 200) {
                    lastZoomMs = nowMs;
                    const double step = cmd == "zoom-in" ? 1.05 : 1.0 / 1.05;
                    const double target = std::clamp((pendingZoom > 0 ? pendingZoom : view.zoomFactor()) * step, 0.25, 1.8);
                    if (!lightweightHome && available >= 0 && available < 85) {
                        pendingZoom = target;
                        event("notice", view.url(), 0, QStringLiteral("LOW RAM: zoom queued. Reload page to apply."));
                    } else {
                        pendingZoom = 0;
                        view.setZoomFactor(target);
                        printf("qtwebkit.zoom factor=%.3f\n", view.zoomFactor());
                        fflush(stdout);
                    }
                }
            }
            else if (cmd.startsWith("open ")) {
                const auto url = QUrl::fromEncoded(cmd.mid(5));
                if (webUrl(url)) { view.stop(); view.load(url); }
            }
        });
    }
    // GUI-thread lag and paint cost are proxies, not LCD input-to-photon timing.
    qint64 previousBeat = view.clock.elapsed(), maxLag = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (stopping) { app.quit(); return; }
        const auto current = view.clock.elapsed();
        maxLag = std::max(maxLag, std::max<qint64>(0, current - previousBeat - 100));
        previousBeat = current;
        if (!ready && view.windowHandle() && view.windowHandle()->isExposed()) {
            ready = true;
            event("ready", {}, current / 1000.0);
            printf("qtwebkit.display platform=%s viewport=%dx%d wayland=%s\n",
                   qPrintable(app.platformName()), view.width(), view.height(), qgetenv("WAYLAND_DISPLAY").constData());
            fflush(stdout);
            view.load(initial);
        }
        if (sampleTime.elapsed() >= 2000) {
            printf("ZERO_QT_HEARTBEAT %lld\n", static_cast<long long>(current));
            fflush(stdout);
            printf("qtwebkit.render phase=%s interval_ms=%lld paints=%d paint_max_ms=%.3f "
                   "loop_lag_max_ms=%lld input_paint_samples=%d input_to_paint_max_ms=%lld\n",
                   loading ? "loading" : "idle", static_cast<long long>(sampleTime.elapsed()),
                   view.paints, view.maxPaintMs, static_cast<long long>(maxLag), view.inputPaints,
                   static_cast<long long>(view.maxInputPaint));
            fflush(stdout);
            view.paints = view.inputPaints = 0;
            view.maxPaintMs = 0;
            view.maxInputPaint = maxLag = 0;
            sampleTime.restart();
        }
    });
    heartbeat.start(100);
    view.show();
    view.setFocus();
    const int result = app.exec();
    network->changed = {}; // Callbacks capture locals that are destroyed before view.
    view.stop();
    if (fdOk && fd >= 0) close(fd);
    return result;
}

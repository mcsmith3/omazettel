#include "pdfdocument.h"

#include "pdfrender.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QQmlEngine>
#include <QTimer>

namespace {
using SizesFunction = bool (*)(const QString &, QList<QSizeF> *, QString *);
using RenderFunction = void (*)(const QString &, int, int, QImage *);

struct Reader {
    SizesFunction sizes = nullptr;
    RenderFunction render = nullptr;
};

// The PDF reader: built in (the tests), or the add-on library beside the
// program (a build), in lib/omazettel (a package or OmaStore), in the app
// bundle's PlugIns (macOS), or named by OMAZETTEL_PDF_LIBRARY. Missing when
// poppler-qt6 is not installed.
const Reader &reader() {
    static const Reader found = [] {
        Reader result;
#ifdef OMAZETTEL_PDF_BUILT_IN
        result.sizes = &omazettel_pdf_sizes;
        result.render = &omazettel_pdf_render;
#else
        const QString appDir = QCoreApplication::applicationDirPath();
        QStringList candidates;
        if (qEnvironmentVariableIsSet("OMAZETTEL_PDF_LIBRARY"))
            candidates.append(qEnvironmentVariable("OMAZETTEL_PDF_LIBRARY"));
#ifdef Q_OS_MACOS
        // In the app bundle's PlugIns folder (Contents/MacOS/../PlugIns).
        const QString file = QStringLiteral("libomazettel-pdf.dylib");
        candidates << QDir(appDir).filePath(QStringLiteral("../PlugIns/") + file);
#else
        const QString file = QStringLiteral("libomazettel-pdf.so");
#endif
        candidates << QDir(appDir).filePath(file)
                   << QDir(appDir).filePath(QStringLiteral("../lib/omazettel/") + file)
                   << QStringLiteral("/usr/lib/omazettel/") + file;
        for (const QString &candidate : std::as_const(candidates)) {
            if (!QFileInfo::exists(candidate))
                continue;
            QLibrary library(candidate);
            // Stays loaded for the rest of the run.
            if (!library.load())
                continue;
            result.sizes = reinterpret_cast<SizesFunction>(library.resolve("omazettel_pdf_sizes"));
            result.render = reinterpret_cast<RenderFunction>(library.resolve("omazettel_pdf_render"));
            if (result.sizes && result.render)
                break;
            result = Reader();
        }
#endif
        return result;
    }();
    return found;
}
}

PdfDocument::PdfDocument(QObject *parent) : QObject(parent) {
    // An export writes the file again; read it once the writing settles.
    auto *settle = new QTimer(this);
    settle->setSingleShot(true);
    settle->setInterval(300);
    connect(settle, &QTimer::timeout, this, &PdfDocument::load);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, settle, qOverload<>(&QTimer::start));
}

bool PdfDocument::available() {
    return reader().sizes != nullptr;
}

void PdfDocument::setPath(const QString &path) {
    if (path == m_path)
        return;
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    m_path = path;
    load();
}

void PdfDocument::load() {
    m_sizes.clear();
    m_error.clear();
    if (!m_path.isEmpty()) {
        // Saving a file anew replaces it, which ends the watch on the old one.
        if (QFileInfo::exists(m_path) && !m_watcher.files().contains(m_path))
            m_watcher.addPath(m_path);
        if (!available())
            m_error = QStringLiteral("Showing PDFs here needs poppler-qt6:\n%1\n\n"
                                     "Open shows it in your PDF viewer meanwhile.")
#ifdef Q_OS_MACOS
                          .arg(QStringLiteral("brew install poppler-qt6, then build OmaZettel again"));
#else
                          .arg(QStringLiteral("sudo pacman -S poppler-qt6"));
#endif
        else
            reader().sizes(m_path, &m_sizes, &m_error);
    }
    ++m_revision;
    emit loaded();
}

QVariantList PdfDocument::pageSizes() const {
    QVariantList sizes;
    for (const QSizeF &size : m_sizes)
        sizes.append(QVariantMap{{QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}});
    return sizes;
}

QString PdfDocument::pageSource(int page, int widthPixels) const {
    // The path goes last, base64 so slashes and spaces survive the address.
    return QStringLiteral("image://pdf/%1/%2/%3/%4")
        .arg(m_revision)
        .arg(page)
        .arg(qMax(1, widthPixels))
        .arg(QString::fromLatin1(m_path.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
}

void PdfDocument::registerType() {
    static const int registered = qmlRegisterType<PdfDocument>("OmaZettel", 1, 0, "PdfDocument");
    Q_UNUSED(registered);
}

void PdfDocument::install(QQmlEngine *engine) {
    registerType();
    if (!engine->imageProvider(QStringLiteral("pdf")))
        engine->addImageProvider(QStringLiteral("pdf"), new PdfImageProvider);
}

QImage PdfDocument::render(const QString &path, int page, int widthPixels) {
    QImage image;
    if (available())
        reader().render(path, page, widthPixels, &image);
    return image;
}

PdfImageProvider::PdfImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading) {}

QImage PdfImageProvider::requestImage(const QString &id, QSize *size, const QSize &) {
    // revision/page/width/base64(path)
    const QStringList parts = id.split(QLatin1Char('/'));
    QImage image;
    if (parts.size() == 4) {
        const QString path = QString::fromUtf8(QByteArray::fromBase64(parts.at(3).toLatin1(), QByteArray::Base64UrlEncoding));
        image = PdfDocument::render(path, parts.at(1).toInt(), qBound(1, parts.at(2).toInt(), 8000));
    }
    if (size)
        *size = image.size();
    return image;
}

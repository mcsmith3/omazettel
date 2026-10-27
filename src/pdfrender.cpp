#include "pdfrender.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>

#include <poppler-qt6.h>

#include <memory>

namespace {
// The last PDF opened, kept so each page doesn't read the file again.
// Poppler documents are not safe to share between threads, and pages are
// drawn on the image loader's threads, so one lock covers it.
struct Cache {
    QMutex mutex;
    QString path;
    QDateTime modified;
    qint64 size = -1;
    std::unique_ptr<Poppler::Document> document;
};

Cache &cache() {
    static Cache instance;
    return instance;
}

// The cached document for path, opened again when the file has changed.
// Call with the cache's lock held.
Poppler::Document *documentFor(Cache &c, const QString &path) {
    // Poppler reports damaged files on the terminal; the view says so itself.
    static const bool quiet = [] {
        Poppler::setDebugErrorFunction([](const QString &, const QVariant &) {}, QVariant());
        return true;
    }();
    Q_UNUSED(quiet);
    const QFileInfo info(path);
    if (c.path != path || c.modified != info.lastModified() || c.size != info.size() || !c.document) {
        c.document = Poppler::Document::load(path);
        c.path = path;
        c.modified = info.lastModified();
        c.size = info.size();
        if (c.document && !c.document->isLocked()) {
            c.document->setRenderHint(Poppler::Document::Antialiasing);
            c.document->setRenderHint(Poppler::Document::TextAntialiasing);
            c.document->setRenderHint(Poppler::Document::TextHinting);
        }
    }
    return c.document && !c.document->isLocked() ? c.document.get() : nullptr;
}
}

extern "C" Q_DECL_EXPORT bool omazettel_pdf_sizes(const QString &path, QList<QSizeF> *sizes, QString *error) {
    Cache &c = cache();
    QMutexLocker lock(&c.mutex);
    sizes->clear();
    const QString name = QFileInfo(path).fileName();
    if (!QFileInfo::exists(path)) {
        *error = QStringLiteral("%1 is no longer there.").arg(name);
        return false;
    }
    Poppler::Document *document = documentFor(c, path);
    if (!document) {
        *error = c.document && c.document->isLocked() ? QStringLiteral("%1 is protected by a password.").arg(name)
                                                      : QStringLiteral("%1 could not be read as a PDF.").arg(name);
        return false;
    }
    for (int i = 0; i < document->numPages(); ++i) {
        const std::unique_ptr<Poppler::Page> page = document->page(i);
        sizes->append(page ? page->pageSizeF() : QSizeF(612, 792));
    }
    return true;
}

extern "C" Q_DECL_EXPORT void omazettel_pdf_render(const QString &path, int page, int widthPixels, QImage *out) {
    *out = QImage();
    Cache &c = cache();
    QMutexLocker lock(&c.mutex);
    Poppler::Document *document = documentFor(c, path);
    if (!document || page < 0 || page >= document->numPages())
        return;
    const std::unique_ptr<Poppler::Page> pdfPage = document->page(page);
    if (!pdfPage)
        return;
    const double points = pdfPage->pageSizeF().width();
    const double dpi = points > 0 ? 72.0 * widthPixels / points : 72.0;
    const QImage image = pdfPage->renderToImage(dpi, dpi);
    if (image.isNull())
        return;
    // Pages with no background of their own are white on paper.
    QImage flat(image.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    QPainter painter(&flat);
    painter.drawImage(0, 0, image);
    painter.end();
    *out = flat;
}

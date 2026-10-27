#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QQuickImageProvider>
#include <QSizeF>
#include <QString>
#include <QVariantList>

class QQmlEngine;

// A PDF shown in the middle of the window (an exported note, or any PDF in
// the notes folder), read with Poppler. QML makes one as PdfDocument
// { path: ... } and shows its pages as images from image://pdf/, which
// renders each page at the size it is drawn.
class PdfDocument : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY loaded)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY loaded)
    // Each page's size in points, as {width, height}.
    Q_PROPERTY(QVariantList pageSizes READ pageSizes NOTIFY loaded)
    Q_PROPERTY(QString error READ error NOTIFY loaded)
    // Changes when the file is written again (a new export), so the pages'
    // image addresses change and they are drawn afresh.
    Q_PROPERTY(int revision READ revision NOTIFY loaded)

public:
    explicit PdfDocument(QObject *parent = nullptr);

    QString path() const { return m_path; }
    void setPath(const QString &path);
    int pageCount() const { return int(m_sizes.size()); }
    QVariantList pageSizes() const;
    QString error() const { return m_error; }
    int revision() const { return m_revision; }

    // Whether the PDF reader (libomazettel-pdf.so, which needs poppler-qt6)
    // could be loaded.
    static bool available();

    // The image address of a page drawn widthPixels wide.
    Q_INVOKABLE QString pageSource(int page, int widthPixels) const;

    // Registers PdfDocument for QML (once per program) and the image://pdf
    // provider (once per engine).
    static void registerType();
    static void install(QQmlEngine *engine);

    // The page, drawn widthPixels wide; a null image if it cannot be read.
    static QImage render(const QString &path, int page, int widthPixels);

signals:
    void loaded();

private:
    void load();

    QString m_path;
    QList<QSizeF> m_sizes;
    QString m_error;
    int m_revision = 0;
    QFileSystemWatcher m_watcher;
};

class PdfImageProvider : public QQuickImageProvider {
public:
    PdfImageProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

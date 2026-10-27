#pragma once

#include <QImage>
#include <QList>
#include <QSizeF>
#include <QString>

// Reading PDFs with Poppler. This is built into a small add-on library,
// libomazettel-pdf.so, which OmaZettel loads when it shows a PDF. So
// OmaZettel still starts without poppler-qt6, and says what to install.
extern "C" {
// Each page's size in points. False, with a reason, when it can't be read.
bool omazettel_pdf_sizes(const QString &path, QList<QSizeF> *sizes, QString *error);
// The page drawn widthPixels wide, on white; a null image when it can't be.
void omazettel_pdf_render(const QString &path, int page, int widthPixels, QImage *out);
}

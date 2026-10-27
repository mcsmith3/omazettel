# libomazettel-pdf.so: the PDF view's reader (Poppler), kept apart so
# OmaZettel starts without poppler-qt6. bin/build puts it beside the
# program; packages install it in lib/omazettel/.
TEMPLATE = lib
TARGET = omazettel-pdf
QT = core gui
CONFIG += plugin c++17 release link_pkgconfig
PKGCONFIG += poppler-qt6
DESTDIR = $$OUT_PWD/..
SOURCES += ../src/pdfrender.cpp
HEADERS += ../src/pdfrender.h

QT += core gui concurrent quick testlib
CONFIG += testcase c++17 exceptions link_pkgconfig
PKGCONFIG += poppler-qt6
# The tests read PDFs directly rather than through the add-on library.
DEFINES += OMAZETTEL_PDF_BUILT_IN
TEMPLATE = app
TARGET = tst_notes

INCLUDEPATH += ../../src
SOURCES += \
    ../../src/cli.cpp \
    ../../src/transcriber.cpp \
    ../../src/mcpserver.cpp \
    ../../src/notesapi.cpp \
    ../../src/assistant.cpp \
    tst_notes.cpp \
    ../../src/backend.cpp \
    ../../src/apptheme.cpp \
    ../../src/backend_notes.cpp \
    ../../src/markdownhighlighter.cpp \
    ../../src/markdownhighlighter_notes.cpp \
    ../../src/citations.cpp \
    ../../src/exporter.cpp \
    ../../src/noteindex.cpp \
    ../../src/layout.cpp \
    ../../src/moc.cpp \
    ../../src/notetemplates.cpp \
    ../../src/sync.cpp \
    ../../src/textdiff.cpp \
    ../../src/zettel.cpp \
    ../../src/notestree.cpp \
    ../../src/pdfdocument.cpp \
    ../../src/pdfrender.cpp \
    ../../src/singleinstance.cpp \
    ../../src/vault.cpp
HEADERS += \
    ../../src/cli.h \
    ../../src/transcriber.h \
    ../../src/mcpserver.h \
    ../../src/notesapi.h \
    ../../src/assistant.h \
    ../../src/apptheme.h \
    ../../src/backend.h \
    ../../src/markdownhighlighter.h \
    ../../src/citations.h \
    ../../src/exporter.h \
    ../../src/noteindex.h \
    ../../src/layout.h \
    ../../src/moc.h \
    ../../src/notetemplates.h \
    ../../src/sync.h \
    ../../src/textdiff.h \
    ../../src/zettel.h \
    ../../src/notestree.h \
    ../../src/pdfdocument.h \
    ../../src/singleinstance.h \
    ../../src/vault.h

QT += widgets printsupport quickcontrols2 quickdialogs2 dbus network

# The app's bundled files (the built-in theme template).
RESOURCES += ../../src/resources.qrc

QT += core gui concurrent widgets printsupport qml quick quickcontrols2 quickdialogs2 dbus network

CONFIG += c++17 exceptions release
TARGET = omazettel
TEMPLATE = app

HEADERS += \
    src/transcriber.h \
    src/mcpserver.h \
    src/notesapi.h \
    src/assistant.h \
    src/apptheme.h \
    src/backend.h \
    src/cli.h \
    src/markdownhighlighter.h \
    src/citations.h \
    src/exporter.h \
    src/noteindex.h \
    src/layout.h \
    src/moc.h \
    src/notetemplates.h \
    src/sync.h \
    src/textdiff.h \
    src/systemtheme.h \
    src/notestree.h \
    src/pdfdocument.h \
    src/singleinstance.h \
    src/vault.h \
    src/zettel.h

SOURCES += \
    src/transcriber.cpp \
    src/mcpserver.cpp \
    src/notesapi.cpp \
    src/assistant.cpp \
    src/main.cpp \
    src/backend.cpp \
    src/apptheme.cpp \
    src/backend_notes.cpp \
    src/cli.cpp \
    src/markdownhighlighter.cpp \
    src/markdownhighlighter_notes.cpp \
    src/citations.cpp \
    src/exporter.cpp \
    src/noteindex.cpp \
    src/layout.cpp \
    src/moc.cpp \
    src/notetemplates.cpp \
    src/sync.cpp \
    src/textdiff.cpp \
    src/systemtheme.cpp \
    src/notestree.cpp \
    src/pdfdocument.cpp \
    src/singleinstance.cpp \
    src/vault.cpp \
    src/zettel.cpp

RESOURCES += src/resources.qrc

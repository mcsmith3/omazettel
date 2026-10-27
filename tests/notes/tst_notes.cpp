// Tests for OmaZettel's notes features. Kept apart from tst_omazettel.cpp,
// which tracks upstream Omawrite's tests.

#include <QtTest>
#include <QPainter>
#include <QPdfWriter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtConcurrent>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonObject>

#include "apptheme.h"
#include "assistant.h"
#include "backend.h"
#include "citations.h"
#include "cli.h"
#include "mcpserver.h"
#include "layout.h"
#include "moc.h"
#include "notesapi.h"
#include "pdfdocument.h"
#include "transcriber.h"
#include "exporter.h"
#include "noteindex.h"
#include "notestree.h"
#include "singleinstance.h"
#include "notetemplates.h"
#include "sync.h"
#include "textdiff.h"
#include "vault.h"
#include "zettel.h"

namespace {
QString readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

void writeFile(const QString &path, const QByteArray &contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(contents);
}
}

class NotesTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
        // The sync log goes in the test's folder, not the real one.
        qputenv("XDG_STATE_HOME", m_settingsDirectory.path().toUtf8());
    }

    void init() {
        // Each test starts with no notes folder remembered.
        QSettings().clear();
    }

    // ── Vault ───────────────────────────────────────────────────────────────

    void cleansNames() {
        QCOMPARE(Vault::cleanName(QStringLiteral("  Meeting   notes ")), QStringLiteral("Meeting notes"));
        QCOMPARE(Vault::cleanName(QStringLiteral("a/b\\c")), QStringLiteral("a-b-c"));
        QCOMPARE(Vault::cleanName(QStringLiteral("..hidden")), QStringLiteral("hidden"));
        QCOMPARE(Vault::cleanName(QStringLiteral("   ")), QString());
    }

    void recognizesNoteFiles() {
        QVERIFY(Vault::isNoteFile(QStringLiteral("/a/b.md")));
        QVERIFY(Vault::isNoteFile(QStringLiteral("/a/b.MARKDOWN")));
        QVERIFY(!Vault::isNoteFile(QStringLiteral("/a/b.txt")));
        QVERIFY(!Vault::isNoteFile(QStringLiteral("/a/md")));
    }

    void showsHomeAsTilde() {
        Vault vault;
        QCOMPARE(vault.prettyPath(QDir::homePath() + QStringLiteral("/Notes")), QStringLiteral("~/Notes"));
        QCOMPARE(vault.prettyPath(QStringLiteral("/srv/notes")), QStringLiteral("/srv/notes"));
    }

    void opensAndRemembersFolders() {
        QTemporaryDir one, two;
        Vault vault;
        QVERIFY(!vault.isOpen());
        QVERIFY(!vault.openFolder(one.filePath(QStringLiteral("missing"))));
        QVERIFY(!vault.lastError().isEmpty());

        QVERIFY(vault.openFolder(one.path()));
        QVERIFY(vault.isOpen());
        QCOMPARE(vault.rootPath(), QFileInfo(one.path()).canonicalFilePath());
        QVERIFY(vault.lastError().isEmpty());
        QVERIFY(vault.openFolder(two.path()));
        QCOMPARE(vault.recentFolders().first(), QFileInfo(two.path()).canonicalFilePath());
        QCOMPARE(vault.recentFolders().size(), 2);

        // A new Vault, as on the next start, reopens the last folder.
        Vault reopened;
        QCOMPARE(reopened.rootPath(), QFileInfo(two.path()).canonicalFilePath());

        reopened.closeFolder();
        QVERIFY(!reopened.isOpen());
        Vault afterClose;
        QVERIFY(!afterClose.isOpen());
        QCOMPARE(afterClose.recentFolders().size(), 2);
    }

    void capsRecentFolders() {
        QList<QSharedPointer<QTemporaryDir>> folders;
        Vault vault;
        for (int i = 0; i < 10; ++i) {
            folders.append(QSharedPointer<QTemporaryDir>::create());
            QVERIFY(vault.openFolder(folders.last()->path()));
        }
        QCOMPARE(vault.recentFolders().size(), 8);
        QCOMPARE(vault.recentFolders().first(),
                 QFileInfo(folders.last()->path()).canonicalFilePath());
    }

    void createsNotesWithUniqueNames() {
        QTemporaryDir dir;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));

        const QString first = vault.createNote(vault.rootPath(), QStringLiteral("Idea"));
        QCOMPARE(QFileInfo(first).fileName(), QStringLiteral("Idea.md"));
        QCOMPARE(readFile(first), QStringLiteral("# Idea\n\n"));

        const QString second = vault.createNote(vault.rootPath(), QStringLiteral("Idea"));
        QCOMPARE(QFileInfo(second).fileName(), QStringLiteral("Idea 2.md"));

        const QString markdown = vault.createNote(vault.rootPath(), QStringLiteral("Long.markdown"));
        QCOMPARE(QFileInfo(markdown).fileName(), QStringLiteral("Long.markdown"));

        const QString untitled = vault.createNote(vault.rootPath(), QStringLiteral("  "));
        QCOMPARE(QFileInfo(untitled).fileName(), QStringLiteral("Untitled.md"));

        // Given a note, the new one goes in the folder holding it.
        const QString sub = vault.createFolder(vault.rootPath(), QStringLiteral("Projects"));
        const QString inSub = vault.createNote(vault.createNote(sub, QStringLiteral("A")),
                                               QStringLiteral("B"));
        QCOMPARE(QFileInfo(inSub).absolutePath(), sub);
    }

    void refusesToWorkOutsideTheFolder() {
        QTemporaryDir dir, outside;
        Vault vault;
        QCOMPARE(vault.createNote(QString(), QStringLiteral("x")), QString());
        QVERIFY(vault.openFolder(dir.path()));

        const QString escape = vault.rootPath() + QStringLiteral("/../escape.md");
        QVERIFY(!vault.contains(escape));
        writeFile(outside.filePath(QStringLiteral("other.md")), "x");
        QCOMPARE(vault.renamePath(outside.filePath(QStringLiteral("other.md")), QStringLiteral("y")),
                 QString());
        QVERIFY(!vault.trashPath(outside.filePath(QStringLiteral("other.md"))));
        QVERIFY(!vault.trashPath(vault.rootPath()));
        QVERIFY(QFile::exists(outside.filePath(QStringLiteral("other.md"))));
        // Asked for a folder outside, a new note goes to the vault root.
        QCOMPARE(QFileInfo(vault.createNote(outside.path(), QStringLiteral("n"))).absolutePath(),
                 vault.rootPath());
    }

    void renamesKeepingTheExtension() {
        QTemporaryDir dir;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        const QString note = vault.createNote(vault.rootPath(), QStringLiteral("Draft"));
        vault.createNote(vault.rootPath(), QStringLiteral("Taken"));

        QCOMPARE(QFileInfo(vault.renameTarget(note, QStringLiteral("Final"))).fileName(),
                 QStringLiteral("Final.md"));
        QCOMPARE(vault.renameTarget(note, QStringLiteral("Taken")), QString());
        QVERIFY(vault.lastError().contains(QStringLiteral("already exists")));
        QCOMPARE(vault.renameTarget(note, QStringLiteral("  ")), QString());

        const QString renamed = vault.renamePath(note, QStringLiteral("Final"));
        QVERIFY(QFile::exists(renamed));
        QVERIFY(!QFile::exists(note));
        QCOMPARE(readFile(renamed), QStringLiteral("# Draft\n\n"));

        const QString folder = vault.createFolder(vault.rootPath(), QStringLiteral("Old"));
        QCOMPARE(QFileInfo(vault.renamePath(folder, QStringLiteral("New"))).fileName(),
                 QStringLiteral("New"));
    }

    void movesNotesAndFoldersBetweenFolders() {
        QTemporaryDir dir, outside;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        const QString a = vault.createFolder(vault.rootPath(), QStringLiteral("A"));
        const QString b = vault.createFolder(vault.rootPath(), QStringLiteral("B"));
        const QString note = vault.createNote(a, QStringLiteral("Note"));

        const QString moved = vault.movePath(note, b);
        QCOMPARE(moved, b + QStringLiteral("/Note.md"));
        QVERIFY(QFile::exists(moved));
        QVERIFY(!QFile::exists(note));

        // Folders move with what is in them.
        const QString movedFolder = vault.movePath(b, a);
        QCOMPARE(movedFolder, a + QStringLiteral("/B"));
        QVERIFY(QFile::exists(movedFolder + QStringLiteral("/Note.md")));

        // Back to the top level.
        QCOMPARE(vault.movePath(movedFolder + QStringLiteral("/Note.md"), vault.rootPath()),
                 vault.rootPath() + QStringLiteral("/Note.md"));

        // Not into itself, not over an existing name, not outside.
        QCOMPARE(vault.movePath(a, movedFolder), QString());
        QVERIFY(vault.lastError().contains(QStringLiteral("into itself")));
        QCOMPARE(vault.movePath(a, a), QString());
        vault.createNote(a, QStringLiteral("Note"));
        QCOMPARE(vault.movePath(vault.rootPath() + QStringLiteral("/Note.md"), a), QString());
        QVERIFY(vault.lastError().contains(QStringLiteral("already has")));
        QCOMPARE(vault.movePath(vault.rootPath() + QStringLiteral("/Note.md"), outside.path()), QString());
        QVERIFY(QFile::exists(vault.rootPath() + QStringLiteral("/Note.md")));
    }

    void movesNotesToTheTrash() {
        QTemporaryDir dir;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        const QString note = vault.createNote(vault.rootPath(), QStringLiteral("Gone"));
        if (!vault.trashPath(note))
            QSKIP("No trash is available for the temporary folder on this system.");
        QVERIFY(!QFile::exists(note));
    }

    void remembersTheLastNoteInsideTheFolder() {
        QTemporaryDir dir, outside;
        {
            Vault vault;
            QVERIFY(vault.openFolder(dir.path()));
            const QString note = vault.createNote(vault.rootPath(), QStringLiteral("Last"));
            vault.rememberLastNote(QUrl::fromLocalFile(note));
            writeFile(outside.filePath(QStringLiteral("x.md")), "x");
            vault.rememberLastNote(QUrl::fromLocalFile(outside.filePath(QStringLiteral("x.md"))));
        }
        Vault reopened;
        QCOMPARE(QFileInfo(reopened.lastNotePath()).fileName(), QStringLiteral("Last.md"));
    }

    void showsOnlyMarkdownAndFolders() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("a.md")), "a");
        writeFile(dir.filePath(QStringLiteral("b.txt")), "b");
        writeFile(dir.filePath(QStringLiteral(".git/config")), "c");
        writeFile(dir.filePath(QStringLiteral("Sub/c.markdown")), "c");

        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QAbstractItemModel *model = vault.model();
        QTRY_COMPARE(model->rowCount(vault.rootIndex()), 2);
        QStringList names;
        for (int row = 0; row < model->rowCount(vault.rootIndex()); ++row)
            names << vault.displayName(model->index(row, 0, vault.rootIndex()));
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("Sub"), QStringLiteral("a")}));
    }

    void showsTheWholeTreeAtOnce() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("10 Ten.md")), "x");
        writeFile(dir.filePath(QStringLiteral("2 Two.md")), "x");
        writeFile(dir.filePath(QStringLiteral("b.md")), "x");
        writeFile(dir.filePath(QStringLiteral("Zed/Deep/inner.md")), "x");
        writeFile(dir.filePath(QStringLiteral("Alpha/note.markdown")), "x");
        writeFile(dir.filePath(QStringLiteral("Alpha/skip.docx")), "x");
        writeFile(dir.filePath(QStringLiteral("Alpha/report.pdf")), "x");
        writeFile(dir.filePath(QStringLiteral(".omazettel/templates/t.md")), "x");
        QDir().mkpath(dir.filePath(QStringLiteral("Empty")));

        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QAbstractItemModel *model = vault.model();
        // Every level is there the moment the folder opens: no waiting, no
        // folder that fills in later.
        const auto tree = [&]() {
            QStringList lines;
            std::function<void(const QModelIndex &, int)> walk = [&](const QModelIndex &parent, int depth) {
                for (int row = 0; row < model->rowCount(parent); ++row) {
                    const QModelIndex index = model->index(row, 0, parent);
                    lines << QString(depth * 2, QLatin1Char(' ')) + index.data(NotesTree::FileNameRole).toString();
                    walk(index, depth + 1);
                }
            };
            walk(QModelIndex(), 0);
            return lines;
        };
        const QStringList expected{QStringLiteral("Alpha"), QStringLiteral("  note.markdown"), QStringLiteral("  report.pdf"), QStringLiteral("Empty"),
                                   QStringLiteral("Zed"), QStringLiteral("  Deep"), QStringLiteral("    inner.md"),
                                   QStringLiteral("2 Two.md"), QStringLiteral("10 Ten.md"), QStringLiteral("b.md")};
        QCOMPARE(tree(), expected);
        // The same again when it opens again.
        Vault again;
        QVERIFY(again.openFolder(dir.path()));
        QCOMPARE(again.model()->rowCount(again.rootIndex()), 6);

        // A note the app makes is in the tree at once, ready to reveal.
        const QString made = vault.createNote(dir.filePath(QStringLiteral("Zed/Deep")), QStringLiteral("Fresh"));
        QVERIFY(vault.indexForPath(made).isValid());
        // A note made by something else (sync, the command line) shows up
        // shortly, and the rows around it stay the same objects.
        const QPersistentModelIndex alpha = vault.indexForPath(dir.filePath(QStringLiteral("Alpha")));
        writeFile(dir.filePath(QStringLiteral("Alpha/from-sync.md")), "x");
        QTRY_VERIFY(vault.indexForPath(dir.filePath(QStringLiteral("Alpha/from-sync.md"))).isValid());
        QVERIFY(alpha.isValid());
        QCOMPARE(alpha.data(NotesTree::FileNameRole).toString(), QStringLiteral("Alpha"));
        QVERIFY(QFile::remove(dir.filePath(QStringLiteral("b.md"))));
        QTRY_VERIFY(!vault.indexForPath(dir.filePath(QStringLiteral("b.md"))).isValid());
        QVERIFY(alpha.isValid());

        QVERIFY(NotesTree::naturalCompare(QStringLiteral("note 2"), QStringLiteral("Note 10")) < 0);
        QVERIFY(NotesTree::naturalCompare(QStringLiteral("a"), QStringLiteral("B")) < 0);
        QCOMPARE(NotesTree::naturalCompare(QStringLiteral("x07"), QStringLiteral("x7")), 0);
    }

    void handsALaterStartToTheRunningWindow() {
        // A socket name of this test's own, away from a real OmaZettel.
        qputenv("OMAZETTEL_INSTANCE_NAME", QStringLiteral("omazettel-test-%1").arg(QCoreApplication::applicationPid()).toUtf8());
        const auto restore = qScopeGuard([]() { qunsetenv("OMAZETTEL_INSTANCE_NAME"); });

        // Nothing running yet: the start goes ahead on its own.
        QVERIFY(!SingleInstance::forwardToRunning({QStringLiteral("/tmp/x.md")}, 400));

        SingleInstance running;
        QVERIFY(running.listen());
        QSignalSpy asked(&running, &SingleInstance::openRequested);
        // A second one cannot take over while the first answers.
        SingleInstance second;
        QVERIFY(!second.listen());

        // A later start hands over its file (from another thread here, as it
        // would come from another process) and is told to exit.
        QFuture<bool> forwarded = QtConcurrent::run([]() {
            return SingleInstance::forwardToRunning({QStringLiteral("/notes/Plan.md")});
        });
        QTRY_COMPARE(asked.count(), 1);
        QCOMPARE(asked.first().at(0).toStringList(), QStringList{QStringLiteral("/notes/Plan.md")});
        QTRY_VERIFY(forwarded.isFinished());
        QVERIFY(forwarded.result());

        // With no file it only asks to come to the front.
        forwarded = QtConcurrent::run([]() { return SingleInstance::forwardToRunning({}); });
        QTRY_COMPARE(asked.count(), 2);
        QVERIFY(asked.at(1).at(0).toStringList().isEmpty());
        QTRY_VERIFY(forwarded.isFinished());
    }

    void opensAFileFromALaterStart() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("One.md")), "# One\n");
        writeFile(dir.filePath(QStringLiteral("Two.md")), "# Two\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        backend.open(QUrl::fromLocalFile(dir.filePath(QStringLiteral("One.md"))));

        QMetaObject::invokeMethod(window.get(), "openFromLaunch",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Two.md")))));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Two.md"))));
        // Back returns to the note that was open.
        QMetaObject::invokeMethod(window.get(), "goBack");
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("One.md"))));

        // A file that does not exist yet starts a new note there.
        const QString fresh = dir.filePath(QStringLiteral("Fresh.md"));
        QMetaObject::invokeMethod(window.get(), "openFromLaunch", Q_ARG(QVariant, QUrl::fromLocalFile(fresh)));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(fresh));
        QVERIFY(!QFile::exists(fresh));
    }

    // ── Backend additions ───────────────────────────────────────────────────

    void flushesAutoSaveForSavedNotesOnly() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("note.md"));
        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);

        // An untitled draft is never saved behind the writer's back.
        editor->setProperty("text", QStringLiteral("draft"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
        QVERIFY(!backend.flushAutoSave());
        QVERIFY(backend.modified());

        backend.saveAs(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("edited"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
        QVERIFY(backend.flushAutoSave());
        QVERIFY(!backend.modified());
        QCOMPARE(readFile(path), QStringLiteral("edited"));
    }

    void movesTheOpenNote() {
        QTemporaryDir dir;
        const QString from = dir.filePath(QStringLiteral("old.md"));
        const QString to = dir.filePath(QStringLiteral("sub/new.md"));
        writeFile(from, "body");
        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        backend.open(QUrl::fromLocalFile(from));

        QSignalSpy external(&backend, &Backend::externalChangeDetected);
        QVERIFY(backend.moveCurrentFile(QUrl::fromLocalFile(to)));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(to));
        QVERIFY(!QFile::exists(from));
        QCOMPARE(readFile(to), QStringLiteral("body"));

        // Edits keep saving to the new path, and the move was not reported
        // as an outside change.
        editor->setProperty("text", QStringLiteral("after move"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.flushAutoSave());
        QCOMPARE(readFile(to), QStringLiteral("after move"));
        QTest::qWait(300);
        QCOMPARE(external.count(), 0);

        // Never onto an existing file.
        writeFile(dir.filePath(QStringLiteral("taken.md")), "taken");
        QVERIFY(!backend.moveCurrentFile(QUrl::fromLocalFile(dir.filePath(QStringLiteral("taken.md")))));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("taken.md"))), QStringLiteral("taken"));
    }

    void startsANewDocument() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("note.md"));
        writeFile(path, "body");
        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        backend.open(QUrl::fromLocalFile(path));
        backend.startNewDocument();
        QVERIFY(backend.fileUrl().isEmpty());
        QVERIFY(!backend.modified());
        QCOMPARE(editor->property("text").toString(), QString());
        QCOMPARE(readFile(path), QStringLiteral("body"));
    }


    // ── Zettel syntax ───────────────────────────────────────────────────────

    void findsWikiLinks() {
        const auto links = Zettel::wikiLinks(QStringLiteral("See [[Index]], [[Projects/Plan#Goals|the plan]] and `[[code]]`."));
        QCOMPARE(links.size(), 2);
        QCOMPARE(links.at(0).target, QStringLiteral("Index"));
        QCOMPARE(links.at(0).start, 4);
        QCOMPARE(links.at(0).length, 9);
        QCOMPARE(links.at(1).target, QStringLiteral("Projects/Plan"));
        QCOMPARE(links.at(1).heading, QStringLiteral("Goals"));
        QCOMPARE(links.at(1).label, QStringLiteral("the plan"));
        QVERIFY(Zettel::wikiLinks(QStringLiteral("[[ ]] [[]] [single]")).isEmpty());
        // A heading in the same note.
        const auto local = Zettel::wikiLinks(QStringLiteral("[[#Setup]]"));
        QCOMPARE(local.size(), 1);
        QCOMPARE(local.first().target, QString());
        QCOMPARE(local.first().heading, QStringLiteral("Setup"));
        QVERIFY(Zettel::parse(QStringLiteral("[[#Setup]]")).links.isEmpty());
        QCOMPARE(Zettel::plainWikiLinks(QStringLiteral("See [[#Setup]].")), QStringLiteral("See Setup."));
    }

    void findsTags() {
        const auto found = Zettel::tags(QStringLiteral("#idea about #project/client-a, not#this, #123, `#code`, https://x.com/#frag [[Note#Heading]] #ok."));
        QStringList names;
        for (const auto &tag : found)
            names << tag.tag;
        QCOMPARE(names, (QStringList{QStringLiteral("idea"), QStringLiteral("project/client-a"), QStringLiteral("ok")}));
        QCOMPARE(found.first().start, 0);
        QCOMPARE(found.first().length, 5);
        QVERIFY(Zettel::tags(QStringLiteral("# Heading")).isEmpty());
        QVERIFY(Zettel::tags(QStringLiteral("## Heading")).isEmpty());
        QCOMPARE(Zettel::tags(QStringLiteral("#Über")).first().tag, QStringLiteral("Über"));
    }

    void readsFrontMatterTags() {
        QCOMPARE(Zettel::frontMatterTags(QStringLiteral("tags: [a, \"b c\", #d]")),
                 (QStringList{QStringLiteral("a"), QStringLiteral("b c"), QStringLiteral("d")}));
        QCOMPARE(Zettel::frontMatterTags(QStringLiteral("tags: one two")),
                 (QStringList{QStringLiteral("one"), QStringLiteral("two")}));
        QCOMPARE(Zettel::frontMatterTags(QStringLiteral("title: x\ntags:\n  - one\n  - 'two'\nother: y")),
                 (QStringList{QStringLiteral("one"), QStringLiteral("two")}));
        QCOMPARE(Zettel::frontMatterTags(QStringLiteral("keywords: alpha, beta")),
                 (QStringList{QStringLiteral("alpha"), QStringLiteral("beta")}));
    }

    void parsesANote() {
        const Zettel::ParsedNote note = Zettel::parse(QStringLiteral(
            "---\ntitle: \"Front Title\"\ntags: [meta]\n---\n# Heading\n\nText #idea and [[Other]].\n"
            "```\n#notatag [[NotALink]]\n```\nMore #Idea and #new.\n"));
        QCOMPARE(note.title, QStringLiteral("Front Title"));
        QCOMPARE(note.tags, (QStringList{QStringLiteral("meta"), QStringLiteral("idea"), QStringLiteral("new")}));
        QCOMPARE(note.links.size(), 1);
        QCOMPARE(note.links.first().target, QStringLiteral("Other"));
        QCOMPARE(note.links.first().context, QStringLiteral("Text #idea and [[Other]]."));
        QCOMPARE(Zettel::parse(QStringLiteral("no heading")).title, QString());
        QCOMPARE(Zettel::parse(QStringLiteral("intro\n# Late Title #\n")).title, QStringLiteral("Late Title"));
    }

    // ── Note index ──────────────────────────────────────────────────────────

    void indexesLinksBacklinksAndTags() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Index.md")), "# Index\n\nStart at [[Ideas]] and [[Projects/Plan]]. #hub\n");
        writeFile(dir.filePath(QStringLiteral("Ideas.md")), "# Ideas\n\nBack to [[index]]. #idea #Hub\n");
        writeFile(dir.filePath(QStringLiteral("Projects/Plan.md")), "---\ntags: [project]\n---\n# The Plan\n\n[[Ideas]] [[Missing]]\n");
        writeFile(dir.filePath(QStringLiteral("Archive/Plan.md")), "# Old plan\n");
        writeFile(dir.filePath(QStringLiteral(".hidden/Secret.md")), "[[Index]]\n");

        NoteIndex index;
        index.setRoot(dir.path());
        index.rescanNow();
        QCOMPARE(index.noteCount(), 4);

        const QString indexPath = dir.filePath(QStringLiteral("Index.md"));
        const QString ideas = dir.filePath(QStringLiteral("Ideas.md"));
        const QString plan = dir.filePath(QStringLiteral("Projects/Plan.md"));
        QCOMPARE(index.resolve(QStringLiteral("ideas"), indexPath), ideas);
        QCOMPARE(index.resolve(QStringLiteral("Ideas.md"), indexPath), ideas);
        QCOMPARE(index.resolve(QStringLiteral("Projects/Plan"), indexPath), plan);
        QCOMPARE(index.resolve(QStringLiteral("The Plan"), indexPath), plan);
        QCOMPARE(index.resolve(QStringLiteral("Missing"), indexPath), QString());
        // Two notes named Plan: the one in the linking note's folder wins.
        QCOMPARE(index.resolve(QStringLiteral("Plan"), dir.filePath(QStringLiteral("Archive/x.md"))),
                 dir.filePath(QStringLiteral("Archive/Plan.md")));

        const QVariantList back = index.backlinks(ideas);
        QCOMPARE(back.size(), 2);
        QCOMPARE(back.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Index"));
        QVERIFY(back.at(0).toMap().value(QStringLiteral("context")).toString().contains(QStringLiteral("[[Ideas]]")));
        QCOMPARE(index.backlinks(indexPath).size(), 1);
        QCOMPARE(index.titleOf(plan), QStringLiteral("The Plan"));

        // Tags group regardless of case and keep the first spelling.
        const QVariantList tags = index.tags();
        QCOMPARE(tags.size(), 3);
        // Ideas.md comes before Index.md, so its spelling wins.
        QCOMPARE(tags.at(0).toMap().value(QStringLiteral("tag")).toString(), QStringLiteral("Hub"));
        QCOMPARE(tags.at(0).toMap().value(QStringLiteral("count")).toInt(), 2);
        QCOMPARE(index.notesWithTag(QStringLiteral("#HUB")).size(), 2);
        QCOMPARE(index.tagsOf(plan), QStringList{QStringLiteral("project")});

        // Saving a note updates it alone.
        QSignalSpy changed(&index, &NoteIndex::changed);
        writeFile(ideas, "# Ideas\n\nNo links now. #fresh\n");
        index.updateFile(ideas);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(index.backlinks(indexPath).size(), 0);
        QCOMPARE(index.notesWithTag(QStringLiteral("fresh")).size(), 1);
        QFile::remove(ideas);
        index.updateFile(ideas);
        QCOMPARE(index.noteCount(), 3);
    }

    void rescansInTheBackground() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("A.md")), "# A\n");
        NoteIndex index;
        QSignalSpy changed(&index, &NoteIndex::changed);
        index.setRoot(dir.path());
        QTRY_COMPARE(index.noteCount(), 1);
        writeFile(dir.filePath(QStringLiteral("B.md")), "[[A]]\n");
        index.scheduleRescan();
        QTRY_COMPARE(index.noteCount(), 2);
        QCOMPARE(index.backlinks(dir.filePath(QStringLiteral("A.md"))).size(), 1);
        index.setRoot(QString());
        QCOMPARE(index.noteCount(), 0);
    }

    // ── Links in the window ─────────────────────────────────────────────────

    void followsAndCreatesWikiLinks() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Start.md")), "# Start\n\nGo to [[Target]] or [[New idea]].\n");
        writeFile(dir.filePath(QStringLiteral("Sub/Target.md")), "# Target\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);
        const QUrl start = QUrl::fromLocalFile(dir.filePath(QStringLiteral("Start.md")));
        QMetaObject::invokeMethod(window.get(), "openNote", Q_ARG(QVariant, start));

        // The position of a character inside [[Target]].
        const int inTarget = QStringLiteral("# Start\n\nGo to [[Tar").size();
        QCOMPARE(backend.wikiLinkAt(inTarget), QStringLiteral("Target"));
        QVariant handled;
        QMetaObject::invokeMethod(window.get(), "followLinkAt", Q_RETURN_ARG(QVariant, handled), Q_ARG(QVariant, inTarget));
        QVERIFY(handled.toBool());
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Sub/Target.md"))));

        // Back returns to the start note.
        QMetaObject::invokeMethod(window.get(), "goBack");
        QCOMPARE(backend.fileUrl(), start);
        QMetaObject::invokeMethod(window.get(), "goForward");
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Sub/Target.md"))));
        QMetaObject::invokeMethod(window.get(), "goBack");

        // A link to a note that does not exist yet makes it beside this one.
        const int inNew = QStringLiteral("# Start\n\nGo to [[Target]] or [[New").size();
        QMetaObject::invokeMethod(window.get(), "followLinkAt", Q_RETURN_ARG(QVariant, handled), Q_ARG(QVariant, inNew));
        const QString created = dir.filePath(QStringLiteral("New-idea.md"));
        QVERIFY(QFile::exists(created));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(created));
        // From the Note template, with its pandoc header.
        QVERIFY(readFile(created).startsWith(QStringLiteral("---\ntitle: \"New idea\"\n")));
    }

    void completesWikiLinks() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Draft.md")), "# Draft\n\n");
        writeFile(dir.filePath(QStringLiteral("Zettelkasten method.md")), "# Z\n");
        writeFile(dir.filePath(QStringLiteral("Other.md")), "# O\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Draft.md")))));

        auto *editor = window->findChild<QQuickItem *>(QStringLiteral("sourceEditor"));
        auto *completer = window->findChild<QObject *>(QStringLiteral("linkCompleter"));
        QVERIFY(editor && completer);
        editor->setProperty("text", QStringLiteral("# Draft\n\nSee [[zett"));
        editor->setProperty("cursorPosition", editor->property("length"));
        QTRY_VERIFY(completer->property("opened").toBool());
        QCOMPARE(completer->property("query").toString(), QStringLiteral("zett"));
        const QVariantList results = completer->property("results").toList();
        QVERIFY(!results.isEmpty());
        QCOMPARE(results.first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Zettelkasten method"));

        QMetaObject::invokeMethod(completer, "accept");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("# Draft\n\nSee [[Zettelkasten method]]"));
        QTRY_VERIFY(!completer->property("opened").toBool());

        // Not inside code, and closed once the link is finished.
        editor->setProperty("text", QStringLiteral("`[[zett"));
        editor->setProperty("cursorPosition", editor->property("length"));
        QTest::qWait(50);
        QVERIFY(!completer->property("opened").toBool());
    }

    void quickSwitcherFindsAndCreatesNotes() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Alpha.md")), "# Alpha\n");
        writeFile(dir.filePath(QStringLiteral("Projects/Beta plan.md")), "# Beta\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        auto *switcher = window->findChild<QObject *>(QStringLiteral("quickSwitcher"));
        QVERIFY(switcher);

        switcher->setProperty("query", QStringLiteral("bpl"));
        QCOMPARE(switcher->property("results").toList().first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Beta plan"));
        QMetaObject::invokeMethod(switcher, "accept");
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Projects/Beta plan.md"))));

        switcher->setProperty("query", QStringLiteral("Brand new"));
        QCOMPARE(switcher->property("results").toList().size(), 0);
        QMetaObject::invokeMethod(switcher, "accept");
        // Made in the selected folder, which follows the open note.
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Projects/Brand-new.md"))));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Projects/Brand-new.md"))));
    }

    void panelShowsBacklinksAndTags() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Hub.md")), "# Hub\n\n#central #topic\n");
        writeFile(dir.filePath(QStringLiteral("Spoke.md")), "# Spoke\n\nUp to [[Hub]].\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        vault.setPanelVisible(true);
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        auto *panel = window->findChild<QQuickItem *>(QStringLiteral("notePanel"));
        QVERIFY(panel && panel->isVisible());
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Hub.md")))));
        QCOMPARE(panel->property("backlinks").toList().size(), 1);
        QCOMPARE(panel->property("noteTags").toStringList(), (QStringList{QStringLiteral("central"), QStringLiteral("topic")}));
        QCOMPARE(window->property("contentWidth").toInt(),
                 window->property("width").toInt() - window->property("sidebarWidth").toInt()
                     - window->property("panelWidth").toInt());

        vault.setPanelVisible(false);
        QVERIFY(!panel->isVisible());
        QCOMPARE(window->property("panelWidth").toInt(), 0);
    }


    // ── Note templates ──────────────────────────────────────────────────────

    void rendersTemplates() {
        const QDateTime now(QDate(2026, 10, 20), QTime(9, 5, 7));
        const auto rendered = NoteTemplates::render(QStringLiteral(
            "<!-- omazettel: name=\"{{date}} {{title}}\" folder=\"Meetings\" existing=\"open\" -->\n"
            "---\ntitle: \"{{title}}\"\nauthor: \"{{author}}\"\ndate: \"{{date}}\"\nid: {{id}}\n---\n\n"
            "# {{title}}\n{{date:yyyy/MM}} {{time}} in {{folder}} {{unknown}}\n- {{cursor}}\n"),
            QStringLiteral("Plan \"Q4\""), QStringLiteral("Matt"), QStringLiteral("Work"), now);
        QCOMPARE(rendered.fileName, QStringLiteral("2026-10-20-Plan-\"Q4\""));
        QCOMPARE(rendered.folder, QStringLiteral("Meetings"));
        QVERIFY(rendered.openExisting);
        // Quotes are escaped in the YAML header only.
        QCOMPARE(rendered.content, QStringLiteral(
            "---\ntitle: \"Plan \\\"Q4\\\"\"\nauthor: \"Matt\"\ndate: \"2026-10-20\"\nid: 20261020090507\n---\n\n"
            "# Plan \"Q4\"\n2026/10 09:05 in Work {{unknown}}\n- \n"));
        QCOMPARE(rendered.cursor, rendered.content.size() - 1);

        const auto plain = NoteTemplates::render(QStringLiteral("Just {{title}}"), QStringLiteral("a/b"),
                                                 QString(), QString(), now);
        QCOMPARE(plain.fileName, QStringLiteral("a-b"));
        QCOMPARE(plain.cursor, -1);
        QVERIFY(!plain.openExisting);
    }

    void startersCarryAPandocHeader() {
        for (const auto &[name, text] : NoteTemplates::starterTemplates()) {
            const auto rendered = NoteTemplates::render(text, QStringLiteral("T"), QStringLiteral("A"),
                                                        QString(), QDateTime::currentDateTime());
            QVERIFY2(rendered.content.startsWith(QStringLiteral("---\ntitle: ")), qPrintable(name));
            QVERIFY2(rendered.content.contains(QStringLiteral("\nauthor: \"A\"\n")), qPrintable(name));
            QVERIFY2(rendered.content.contains(QStringLiteral("\ndate: \"")), qPrintable(name));
            QVERIFY2(rendered.cursor > 0, qPrintable(name));
            QVERIFY2(!Zettel::parse(rendered.content).title.isEmpty(), qPrintable(name));
        }
    }

    void createsNotesFromTemplates() {
        QTemporaryDir dir;
        NoteTemplates templates;
        templates.setRoot(dir.path());
        templates.setAuthor(QStringLiteral("Test Author"));
        templates.ensureDefaults();
        // An empty folder starts with the numbered layout.
        QVERIFY(Layout::present(dir.path()));
        QCOMPARE(templates.list().size(), 4);
        QVERIFY(templates.needsTitle(QStringLiteral("Meeting")));
        QVERIFY(!templates.needsTitle(QStringLiteral("Daily")));

        // Defaults are written once; a deleted starter stays deleted.
        QFile::remove(templates.pathOf(QStringLiteral("Literature note")));
        templates.ensureDefaults();
        QCOMPARE(templates.list().size(), 3);

        const QString today = QDate::currentDate().toString(Qt::ISODate);
        const QVariantMap meeting = templates.create(QStringLiteral("Meeting"), QStringLiteral("Kickoff"), dir.path());
        QCOMPARE(meeting.value(QStringLiteral("path")).toString(),
                 dir.filePath(QStringLiteral("03-projects/meetings/%1-Kickoff.md").arg(today)));
        const QString text = readFile(meeting.value(QStringLiteral("path")).toString());
        QVERIFY(text.contains(QStringLiteral("title: \"Kickoff\"")));
        QVERIFY(text.contains(QStringLiteral("author: \"Test Author\"")));
        QVERIFY(!text.contains(QStringLiteral("omazettel:")));
        QVERIFY(meeting.value(QStringLiteral("cursor")).toInt() > 0);

        // Daily: one per day, opened again rather than duplicated.
        const QVariantMap daily = templates.create(QStringLiteral("Daily"), QString(), dir.path());
        QCOMPARE(daily.value(QStringLiteral("path")).toString(), dir.filePath(QStringLiteral("03-projects/daily/%1.md").arg(today)));
        QVERIFY(!daily.value(QStringLiteral("existed")).toBool());
        const QVariantMap again = templates.create(QStringLiteral("Daily"), QString(), dir.path());
        QCOMPARE(again.value(QStringLiteral("path")), daily.value(QStringLiteral("path")));
        QVERIFY(again.value(QStringLiteral("existed")).toBool());

        // An exact name (from a [[link]]) wins over the template's own.
        const QVariantMap exact = templates.create(QStringLiteral("Meeting"), QStringLiteral("Linked"),
                                                   dir.filePath(QStringLiteral("Here")), QStringLiteral("Linked"));
        QCOMPARE(exact.value(QStringLiteral("path")).toString(), dir.filePath(QStringLiteral("Here/Linked.md")));

        QVERIFY(templates.create(QStringLiteral("Nope"), QStringLiteral("x"), dir.path()).isEmpty());
        QVERIFY(!templates.lastError().isEmpty());
    }

    void newNoteDialogUsesTemplates() {
        QTemporaryDir dir;
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);
        QMetaObject::invokeMethod(window.get(), "newNote", Q_ARG(QVariant, vault.rootPath()));
        auto *dialog = window->findChild<QObject *>(QStringLiteral("nameDialog"));
        QVERIFY(dialog && dialog->property("opened").toBool() || dialog->property("visible").toBool());
        const QVariantList choices = dialog->property("templates").toList();
        QCOMPARE(choices.size(), 5); // Four starters and Blank note.
        QCOMPARE(dialog->property("chosenTemplate").toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Note"));

        auto *field = window->findChild<QObject *>(QStringLiteral("nameField"));
        field->setProperty("text", QStringLiteral("Fresh idea"));
        QMetaObject::invokeMethod(dialog, "finish");
        const QString path = dir.filePath(QStringLiteral("Fresh-idea.md"));
        QVERIFY(QFile::exists(path));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(path));
        QVERIFY(readFile(path).startsWith(QStringLiteral("---\ntitle: \"Fresh idea\"")));
        // The caret starts where the template said.
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QCOMPARE(editor->property("cursorPosition").toInt(), readFile(path).indexOf(QStringLiteral("---\n\n")) + 5);

        // Daily note twice: the same note.
        QMetaObject::invokeMethod(window.get(), "openDailyNote");
        const QUrl daily = backend.fileUrl();
        QVERIFY(daily.toLocalFile().endsWith(QDate::currentDate().toString(Qt::ISODate) + QStringLiteral(".md")));
        QMetaObject::invokeMethod(window.get(), "openNote", Q_ARG(QVariant, QUrl::fromLocalFile(path)));
        QMetaObject::invokeMethod(window.get(), "openDailyNote");
        QCOMPARE(backend.fileUrl(), daily);
    }

    // ── Linked notes are made, and links follow renames ─────────────────────

    void createsMissingLinkedNotesOnSave() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Hub.md")), "# Hub\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Hub.md")))));
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));

        // The caret is still in [[Beta]], which may be half typed.
        const QString text = QStringLiteral("# Hub\n\n[[Alpha]] and [[Sub/Gamma]] and [[../Escape]] and [[Beta]]");
        editor->setProperty("text", text);
        editor->setProperty("cursorPosition", text.size() - 3);
        backend.save();
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Alpha.md"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Sub/Gamma.md"))));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("Beta.md"))));
        QVERIFY(!QFileInfo::exists(QFileInfo(dir.path()).absolutePath() + QStringLiteral("/Escape.md")));
        QVERIFY(readFile(dir.filePath(QStringLiteral("Alpha.md"))).contains(QStringLiteral("title: \"Alpha\"")));

        // Once the caret leaves it, the next save makes it.
        editor->setProperty("cursorPosition", 0);
        editor->setProperty("text", text + QStringLiteral("."));
        backend.save();
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Beta.md"))));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Hub.md"))));
    }

    void renamingANoteUpdatesLinksToIt() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Sub/Target.md")), "# Target\n\nSelf: [[Target]]\n");
        writeFile(dir.filePath(QStringLiteral("A.md")), "See [[Target]], [[target#Part|label]] and [[Other]].\n```\n[[Target]]\n```\n");
        writeFile(dir.filePath(QStringLiteral("B.md")), "Path: [[Sub/Target]]\n");
        writeFile(dir.filePath(QStringLiteral("Other.md")), "x\n");
        writeFile(dir.filePath(QStringLiteral("Open.md")), "Open links to [[Target]].\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Open.md")))));

        renameThroughDialog(window.get(), dir.filePath(QStringLiteral("Sub/Target.md")), QStringLiteral("Renamed"));

        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Sub/Renamed.md"))));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("A.md"))),
                 QStringLiteral("See [[Renamed]], [[Renamed#Part|label]] and [[Other]].\n```\n[[Target]]\n```\n"));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("B.md"))), QStringLiteral("Path: [[Sub/Renamed]]\n"));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("Sub/Renamed.md"))), QStringLiteral("# Target\n\nSelf: [[Renamed]]\n"));
        // The open note changes in the editor, then saves without a prompt.
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Open links to [[Renamed]].\n"));
        QCOMPARE(backend.status(), QStringLiteral("Updated 5 links in 4 notes"));
        QVERIFY(backend.flushAutoSave());
        QCOMPARE(readFile(dir.filePath(QStringLiteral("Open.md"))), QStringLiteral("Open links to [[Renamed]].\n"));
    }

    void renamingTheOpenNoteUpdatesLinksToIt() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Me.md")), "# Me\n");
        writeFile(dir.filePath(QStringLiteral("Fan.md")), "[[Me]]\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Me.md")))));
        renameThroughDialog(window.get(), dir.filePath(QStringLiteral("Me.md")), QStringLiteral("Myself"));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Myself.md"))));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("Fan.md"))), QStringLiteral("[[Myself]]\n"));
    }

    void movingNotesAndFoldersUpdatesPathLinks() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Old/Deep/Note.md")), "# Note\n");
        writeFile(dir.filePath(QStringLiteral("Links.md")), "[[Note]] and [[Old/Deep/Note]] and [[Deep/Note]]\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        const QString archive = vault.createFolder(vault.rootPath(), QStringLiteral("Archive"));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));

        // Moving a folder: name links stay, path links follow.
        QMetaObject::invokeMethod(window.get(), "moveItem",
                                  Q_ARG(QVariant, dir.filePath(QStringLiteral("Old/Deep"))), Q_ARG(QVariant, archive));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Archive/Deep/Note.md"))));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("Links.md"))),
                 QStringLiteral("[[Note]] and [[Archive/Deep/Note]] and [[Archive/Deep/Note]]\n"));

        // Renaming a folder too.
        m_windowIndex->rescanNow();
        renameThroughDialog(window.get(), dir.filePath(QStringLiteral("Archive")), QStringLiteral("Kept"));
        QCOMPARE(readFile(dir.filePath(QStringLiteral("Links.md"))),
                 QStringLiteral("[[Note]] and [[Kept/Deep/Note]] and [[Kept/Deep/Note]]\n"));
    }


    // ── Export ──────────────────────────────────────────────────────────────

    void preparesTextForPandoc() {
        QCOMPARE(Exporter::prepareText(QStringLiteral(
                     "<!-- omazettel: name=\"x\" -->\nSee [[Index]], [[Plan#Goals]], [[a/B|the b]].\n```\n[[Code]]\n```\n`[[inline]]`")),
                 QStringLiteral("See Index, Plan › Goals, the b.\n```\n[[Code]]\n```\n`[[inline]]`"));
        QCOMPARE(Exporter::formatOf(QStringLiteral("# c\nto: docx\n")), QStringLiteral("docx"));
        QCOMPARE(Exporter::formatOf(QStringLiteral("writer: \"html5+smart\"\n")), QStringLiteral("html5"));
        QCOMPARE(Exporter::formatOf(QStringLiteral("# to: pdf\n")), QStringLiteral("html"));
        QCOMPARE(Exporter::extensionFor(QStringLiteral("latex")), QStringLiteral("tex"));
        QCOMPARE(Exporter::extensionFor(QStringLiteral("html5")), QStringLiteral("html"));
        QCOMPARE(Exporter::extensionFor(QStringLiteral("pdf")), QStringLiteral("pdf"));
        QCOMPARE(Exporter::withTemplate(QStringLiteral("to: pdf\n# template: ${.}/old.tex\n"),
                                        QStringLiteral("template"), QStringLiteral("${.}/../templates/A.tex")),
                 QStringLiteral("to: pdf\ntemplate: ${.}/../templates/A.tex\n"));
        QCOMPARE(Exporter::withTemplate(QStringLiteral("to: docx"), QStringLiteral("reference-doc"), QStringLiteral("r.docx")),
                 QStringLiteral("to: docx\nreference-doc: r.docx\n"));
    }

    void writesStarterProfiles() {
        QTemporaryDir dir;
        Exporter exporter;
        exporter.setRoot(dir.path());
        exporter.ensureDefaults();
        const QVariantList profiles = exporter.profiles();
        QStringList names;
        for (const QVariant &profile : profiles)
            names << profile.toMap().value(QStringLiteral("name")).toString();
        QCOMPARE(names, (QStringList{QStringLiteral("HTML"), QStringLiteral("LaTeX source"),
                                     QStringLiteral("OpenDocument"), QStringLiteral("PDF"), QStringLiteral("Word")}));
        QCOMPARE(profiles.at(3).toMap().value(QStringLiteral("extension")).toString(), QStringLiteral("pdf"));
        QVERIFY(QFileInfo(exporter.templatesFolder()).isDir());
        // Only once: a removed profile stays removed.
        QFile::remove(profiles.at(0).toMap().value(QStringLiteral("path")).toString());
        exporter.ensureDefaults();
        QCOMPARE(exporter.profiles().size(), 4);
        QCOMPARE(exporter.defaultOutputPath(dir.filePath(QStringLiteral("Sub/My note.md")),
                                            profiles.at(4).toMap().value(QStringLiteral("path")).toString()),
                 dir.filePath(QStringLiteral("Sub/My note.docx")));

        const QString none = Exporter::starterProfiles(QString()).first().second;
        QVERIFY(none.contains(QStringLiteral("texlive-xetex")));
        QVERIFY(Exporter::starterProfiles(QStringLiteral("pdflatex")).first().second.contains(QStringLiteral("pdf-engine: pdflatex")));
    }

    void makesTemplatesAndProfiles() {
        QTemporaryDir dir;
        Exporter exporter;
        if (!exporter.pandocAvailable())
            QSKIP("pandoc is not installed.");
        exporter.setRoot(dir.path());
        exporter.ensureDefaults();

        const QString tex = exporter.newTemplate(QStringLiteral("latex"), QStringLiteral("Letter"));
        QCOMPARE(QFileInfo(tex).fileName(), QStringLiteral("Letter.tex"));
        QVERIFY(readFile(tex).contains(QStringLiteral("$body$")));
        const QString docx = exporter.newTemplate(QStringLiteral("docx"), QString());
        QCOMPARE(QFileInfo(docx).fileName(), QStringLiteral("Reference.docx"));
        QVERIFY(QFileInfo(docx).size() > 1000);
        QCOMPARE(exporter.newTemplate(QStringLiteral("nope"), QStringLiteral("x")), QString());
        QVERIFY(!exporter.error().isEmpty());

        const QVariantList templates = exporter.templates();
        QCOMPARE(templates.size(), 2);
        QCOMPARE(templates.at(0).toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("template"));
        QVERIFY(templates.at(0).toMap().value(QStringLiteral("text")).toBool());
        QCOMPARE(templates.at(1).toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("reference"));

        const QString pdfProfile = exporter.profilesFolder() + QStringLiteral("/PDF.yaml");
        const QString letter = exporter.newProfile(QStringLiteral("Letter"), pdfProfile, tex);
        QVERIFY(readFile(letter).contains(QStringLiteral("\ntemplate: ${.}/../templates/Letter.tex\n")));
        // A reference document switches the profile to its own format.
        const QString word = exporter.newProfile(QStringLiteral("Styled Word"), pdfProfile, docx);
        QCOMPARE(Exporter::formatOf(readFile(word)), QStringLiteral("docx"));
        QVERIFY(readFile(word).contains(QStringLiteral("reference-doc: ${.}/../templates/Reference.docx")));
    }

    void exportsNotes() {
        QTemporaryDir dir;
        Exporter exporter;
        if (!exporter.pandocAvailable())
            QSKIP("pandoc is not installed.");
        exporter.setRoot(dir.path());
        exporter.ensureDefaults();
        const QString note = dir.filePath(QStringLiteral("Notes/Plan.md"));
        writeFile(note, "---\ntitle: \"The \\\"Q4\\\" plan\"\nauthor: \"Matt\"\n---\n\nSee [[Index|the index]] and #tag.\n");
        const QString html = dir.filePath(QStringLiteral("out/Plan.html"));

        QSignalSpy finished(&exporter, &Exporter::finished);
        QVERIFY(exporter.exportNote(note, exporter.profilesFolder() + QStringLiteral("/HTML.yaml"), html));
        QVERIFY(exporter.busy());
        QVERIFY(finished.wait(30000));
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(2).toString()));
        const QString page = readFile(html);
        QVERIFY(page.contains(QStringLiteral("The “Q4” plan")) || page.contains(QStringLiteral("The &quot;Q4&quot; plan")));
        QVERIFY(page.contains(QStringLiteral("See the index and #tag.")));
        QVERIFY(!page.contains(QStringLiteral("[[")));

        // The editor's text (unsaved changes) can be exported instead.
        finished.clear();
        QVERIFY(exporter.exportText(note, exporter.profilesFolder() + QStringLiteral("/HTML.yaml"), html,
                                    QStringLiteral("# Unsaved\n\nFresh text.\n")));
        QVERIFY(finished.wait(30000));
        QVERIFY(readFile(html).contains(QStringLiteral("Fresh text.")));

        finished.clear();
        const QString docx = dir.filePath(QStringLiteral("Plan.docx"));
        QVERIFY(exporter.exportNote(note, exporter.profilesFolder() + QStringLiteral("/Word.yaml"), docx));
        QVERIFY(finished.wait(30000));
        QVERIFY(finished.first().at(0).toBool());
        QVERIFY(QFileInfo(docx).size() > 1000);

        // A broken profile reports pandoc's error.
        finished.clear();
        writeFile(exporter.profilesFolder() + QStringLiteral("/Broken.yaml"), "to: nosuchformat\n");
        QVERIFY(exporter.exportNote(note, exporter.profilesFolder() + QStringLiteral("/Broken.yaml"),
                                    dir.filePath(QStringLiteral("x.out"))));
        QVERIFY(finished.wait(30000));
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(!finished.first().at(2).toString().isEmpty());
    }

    void exportsPdfWhenAnEngineWorks() {
        const QString engine = Exporter::detectPdfEngine();
        if (engine.isEmpty())
            QSKIP("No PDF engine is installed.");
        QTemporaryDir dir;
        Exporter exporter;
        exporter.setRoot(dir.path());
        exporter.ensureDefaults();
        const QString note = dir.filePath(QStringLiteral("Report.md"));
        writeFile(note, "---\ntitle: \"Report\"\nauthor: \"Matt\"\ndate: \"2026-10-20\"\n---\n\n# Intro\n\nText and [[Link]].\n");
        const QString pdf = dir.filePath(QStringLiteral("Report.pdf"));
        QSignalSpy finished(&exporter, &Exporter::finished);
        QVERIFY(exporter.exportNote(note, exporter.profilesFolder() + QStringLiteral("/PDF.yaml"), pdf));
        QVERIFY(finished.wait(120000));
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(2).toString()));
        QFile file(pdf);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.read(5) == "%PDF-");
    }

    void exportDialogExportsTheOpenNote() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Open.md")), "# Saved\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        if (!m_windowExporter->pandocAvailable())
            QSKIP("pandoc is not installed.");
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Open.md")))));
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        editor->setProperty("text", QStringLiteral("# Not saved yet\n"));

        QMetaObject::invokeMethod(window.get(), "exportNote", Q_ARG(QVariant, dir.filePath(QStringLiteral("Open.md"))));
        auto *dialog = window->findChild<QObject *>(QStringLiteral("exportDialog"));
        QVERIFY(dialog);
        QVERIFY(dialog->property("profileList").toList().size() >= 5);
        // Pick the HTML profile.
        const QVariantList profiles = dialog->property("profileList").toList();
        for (int i = 0; i < profiles.size(); ++i) {
            if (profiles.at(i).toMap().value(QStringLiteral("name")) == QStringLiteral("HTML")) {
                dialog->setProperty("profileIndex", i);
                QMetaObject::invokeMethod(dialog, "updateOutput");
            }
        }
        QCOMPARE(dialog->property("outputPath").toString(), dir.filePath(QStringLiteral("Open.html")));
        QSignalSpy finished(m_windowExporter.get(), &Exporter::finished);
        QMetaObject::invokeMethod(dialog, "runExport");
        QVERIFY(finished.wait(30000));
        QCOMPARE(dialog->property("resultState").toString(), QStringLiteral("ok"));
        QVERIFY(readFile(dir.filePath(QStringLiteral("Open.html"))).contains(QStringLiteral("Not saved yet")));
        QCOMPARE(m_windowExporter->lastProfile(), profiles.at(0).toMap().value(QStringLiteral("path")).toString());
    }


    // ── Search and outline ──────────────────────────────────────────────────

    void parsesSearchQueries() {
        const auto query = NoteIndex::parseQuery(QStringLiteral("alpha \"exact  phrase\" #Work tag:home/kids beta \"unclosed"));
        QCOMPARE(query.terms, (QStringList{QStringLiteral("alpha"), QStringLiteral("exact phrase"),
                                           QStringLiteral("beta"), QStringLiteral("unclosed")}));
        QCOMPARE(query.tags, (QStringList{QStringLiteral("work"), QStringLiteral("home/kids")}));
        QVERIFY(NoteIndex::parseQuery(QStringLiteral("  ")).terms.isEmpty());
    }

    void searchesAllNotes() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Coffee.md")), "# Coffee\n\nNotes on brewing. #drink\nPour over and espresso.\n");
        writeFile(dir.filePath(QStringLiteral("Tea.md")), "# Tea\n\nGreen tea, brewing at 80°C. Café culture <script>. #drink/hot\n");
        writeFile(dir.filePath(QStringLiteral("Work/Plan.md")), "# Plan\n\nBrewing a plan for coffee sales. #work\n");
        NoteIndex index;
        index.setRoot(dir.path());
        index.rescanNow();

        const auto titles = [](const QVariantList &results) {
            QStringList list;
            for (const QVariant &result : results)
                list << result.toMap().value(QStringLiteral("title")).toString();
            return list;
        };
        // All words must appear; a title match ranks first.
        QCOMPARE(titles(index.search(QStringLiteral("coffee brewing"))),
                 (QStringList{QStringLiteral("Coffee"), QStringLiteral("Plan")}));
        QCOMPARE(titles(index.search(QStringLiteral("\"pour over\""))), QStringList{QStringLiteral("Coffee")});
        QCOMPARE(titles(index.search(QStringLiteral("\"over pour\""))), QStringList());
        // Accents and case are ignored.
        QCOMPARE(titles(index.search(QStringLiteral("CAFE"))), QStringList{QStringLiteral("Tea")});
        // Tags filter, including tags under them.
        QCOMPARE(titles(index.search(QStringLiteral("#drink"))), (QStringList{QStringLiteral("Coffee"), QStringLiteral("Tea")}));
        QCOMPARE(titles(index.search(QStringLiteral("brewing tag:work"))), QStringList{QStringLiteral("Plan")});
        QCOMPARE(index.search(QStringLiteral("")).size(), 0);

        // Snippets: the match in bold, the rest escaped, with its position.
        const QVariantMap tea = index.search(QStringLiteral("café"), QStringLiteral("#ff0000")).first().toMap();
        const QVariantMap snippet = tea.value(QStringLiteral("snippets")).toList().first().toMap();
        const QString html = snippet.value(QStringLiteral("html")).toString();
        QVERIFY2(html.contains(QStringLiteral("<b><font color=\"#ff0000\">Café</font></b>")), qPrintable(html));
        QVERIFY(html.contains(QStringLiteral("&lt;script&gt;")));
        const QString teaText = readFile(dir.filePath(QStringLiteral("Tea.md")));
        QCOMPARE(teaText.mid(snippet.value(QStringLiteral("position")).toInt(),
                             snippet.value(QStringLiteral("length")).toInt()), QStringLiteral("Café"));
    }

    void outlinesNotes() {
        const auto headings = Zettel::outline(QStringLiteral(
            "---\ntitle: x\n# not a heading\n---\n# One\ntext\n## Two ##\n```\n# code\n```\n#nospace\n   ### Three\n####### Seven\n"));
        QCOMPARE(headings.size(), 3);
        QCOMPARE(headings.at(0).text, QStringLiteral("One"));
        QCOMPARE(headings.at(0).level, 1);
        QCOMPARE(headings.at(0).offset, 33);
        QCOMPARE(headings.at(1).text, QStringLiteral("Two"));
        QCOMPARE(headings.at(1).level, 2);
        QCOMPARE(headings.at(2).text, QStringLiteral("Three"));
        QCOMPARE(headings.at(2).level, 3);
    }

    void searchTabOpensResults() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Start.md")), "# Start\n");
        writeFile(dir.filePath(QStringLiteral("Deep/Found.md")), "# Found\n\nSome text\nthe needle is here\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        vault.setSidebarVisible(true);
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Start.md")))));

        QMetaObject::invokeMethod(window.get(), "showNotesSearch");
        auto *sidebar = window->findChild<QObject *>(QStringLiteral("sidebar"));
        QCOMPARE(sidebar->property("mode").toString(), QStringLiteral("search"));
        auto *field = window->findChild<QObject *>(QStringLiteral("notesSearchField"));
        auto *view = window->findChild<QObject *>(QStringLiteral("searchView"));
        field->setProperty("text", QStringLiteral("needle"));
        QTRY_COMPARE(view->property("results").toList().size(), 1);

        const QVariantMap result = view->property("results").toList().first().toMap();
        const QVariantMap snippet = result.value(QStringLiteral("snippets")).toList().first().toMap();
        QMetaObject::invokeMethod(window.get(), "openSearchResult",
                                  Q_ARG(QVariant, result.value(QStringLiteral("path"))),
                                  Q_ARG(QVariant, snippet.value(QStringLiteral("position"))),
                                  Q_ARG(QVariant, snippet.value(QStringLiteral("length"))));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Deep/Found.md"))));
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("needle"));
    }

    void outlineAndHeadingLinks() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Guide.md")), "# Guide\n\nIntro\n\n## Setup\n\nSteps\n\n## Usage\n\nMore\n");
        writeFile(dir.filePath(QStringLiteral("Links.md")), "See [[Guide#usage]] and [[#Local]].\n\n## Local\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        vault.setPanelVisible(true);
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Guide.md")))));
        const QVariantList headings = window->property("outlineHeadings").toList();
        QCOMPARE(headings.size(), 3);
        QCOMPARE(headings.at(1).toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Setup"));
        QCOMPARE(backend.headingPosition(QStringLiteral("USAGE")), 33);

        // The panel marks the section the caret is in.
        auto *panel = window->findChild<QObject *>(QStringLiteral("notePanel"));
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        editor->setProperty("cursorPosition", 25);
        QCOMPARE(panel->property("currentHeading").toInt(), 1);

        // Edits update the outline shortly after.
        editor->setProperty("text", QStringLiteral("# Guide\n\n## Only\n"));
        QTRY_COMPARE(window->property("outlineHeadings").toList().size(), 2);
        QVERIFY(backend.flushAutoSave());
        writeFile(dir.filePath(QStringLiteral("Guide.md")), "# Guide\n\nIntro\n\n## Setup\n\nSteps\n\n## Usage\n\nMore\n");

        // [[Note#Heading]] opens the note at the heading; [[#Heading]] jumps
        // within the note.
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Links.md")))));
        const int inLocal = QStringLiteral("See [[Guide#usage]] and [[#Lo").size();
        QVariant handled;
        QMetaObject::invokeMethod(window.get(), "followLinkAt", Q_RETURN_ARG(QVariant, handled), Q_ARG(QVariant, inLocal));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Links.md"))));
        QCOMPARE(editor->property("cursorPosition").toInt(), QStringLiteral("See [[Guide#usage]] and [[#Local]].\n\n").size());

        const int inGuide = QStringLiteral("See [[Gui").size();
        QMetaObject::invokeMethod(window.get(), "followLinkAt", Q_RETURN_ARG(QVariant, handled), Q_ARG(QVariant, inGuide));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("Guide.md"))));
        QCOMPARE(editor->property("cursorPosition").toInt(), 33);
    }


    // ── Folder export ───────────────────────────────────────────────────────

    void ordersFolderNotes() {
        QTemporaryDir dir;
        for (const char *name : {"10 Ten.md", "2 Two.md", "1 One.md", "Part B/2 Bee.md", "Part A/1 Ay.md",
                                 "Part A/Sub/x.md", ".hidden/no.md", "notes.txt"})
            writeFile(dir.filePath(QString::fromUtf8(name)), "x\n");
        Exporter exporter;
        const auto names = [&exporter, &dir]() {
            QStringList list;
            for (const QVariant &entry : exporter.folderNotes(dir.path()))
                list << entry.toMap().value(QStringLiteral("name")).toString()
                        + (entry.toMap().value(QStringLiteral("included")).toBool() ? QString() : QStringLiteral(" (out)"));
            return list;
        };
        QCOMPARE(names(), (QStringList{QStringLiteral("1 One"), QStringLiteral("2 Two"), QStringLiteral("10 Ten"),
                                       QStringLiteral("Part A/1 Ay"), QStringLiteral("Part A/Sub/x"),
                                       QStringLiteral("Part B/2 Bee")}));

        // A saved order and choices come back; new notes follow, included.
        QVariantList entries = exporter.folderNotes(dir.path());
        entries.move(2, 0);
        QVariantMap out = entries[1].toMap();
        out.insert(QStringLiteral("included"), false);
        entries[1] = out;
        QVERIFY(exporter.saveFolderSettings(dir.path(), entries, QStringLiteral("My \"Book\""),
                                            QStringLiteral("Matt"), QString()));
        writeFile(dir.filePath(QStringLiteral("0 New.md")), "x\n");
        QCOMPARE(names(), (QStringList{QStringLiteral("10 Ten"), QStringLiteral("1 One (out)"), QStringLiteral("2 Two"),
                                       QStringLiteral("Part A/1 Ay"), QStringLiteral("Part A/Sub/x"),
                                       QStringLiteral("Part B/2 Bee"), QStringLiteral("0 New")}));
        const QVariantMap settings = exporter.folderSettings(dir.path());
        QCOMPARE(settings.value(QStringLiteral("title")).toString(), QStringLiteral("My \"Book\""));
        QCOMPARE(settings.value(QStringLiteral("author")).toString(), QStringLiteral("Matt"));
        QTemporaryDir fresh;
        QCOMPARE(exporter.folderSettings(fresh.path()).value(QStringLiteral("title")).toString(),
                 QFileInfo(fresh.path()).fileName());
    }

    void combinesNotes() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("a.md")), "<!-- omazettel: name=\"x\" -->\n---\ntitle: \"Intro\"\ntags: [a]\n---\n\nHello [[b]].\n");
        writeFile(dir.filePath(QStringLiteral("b.md")), "---\ntitle: \"Ignored\"\n---\n# Own heading\n\nBody.\n");
        writeFile(dir.filePath(QStringLiteral("c.md")), "Plain note.\n");
        const QString combined = Exporter::combineNotes(
            {dir.filePath(QStringLiteral("a.md")), dir.filePath(QStringLiteral("b.md")), dir.filePath(QStringLiteral("c.md"))},
            QStringLiteral("Book"), QStringLiteral("Matt"), QDate(2026, 10, 21));
        QCOMPARE(combined, QStringLiteral(
            "---\ntitle: \"Book\"\nauthor: \"Matt\"\ndate: \"2026-10-21\"\n---\n\n"
            "# Intro\n\nHello [[b]].\n\n# Own heading\n\nBody.\n\nPlain note.\n\n"));
    }

    void exportsAFolder() {
        QTemporaryDir dir;
        Exporter exporter;
        if (!exporter.pandocAvailable())
            QSKIP("pandoc is not installed.");
        exporter.setRoot(dir.path());
        exporter.ensureDefaults();
        writeFile(dir.filePath(QStringLiteral("Book/1 Start.md")), "---\ntitle: \"Start\"\n---\n\nFirst [[2 Middle|middle]].\n");
        writeFile(dir.filePath(QStringLiteral("Book/2 Middle.md")), "# Middle\n\nSecond.\n");
        const QString folder = dir.filePath(QStringLiteral("Book"));
        const QString html = exporter.defaultFolderOutputPath(folder, exporter.profilesFolder() + QStringLiteral("/HTML.yaml"));
        QCOMPARE(html, dir.filePath(QStringLiteral("Book/Book.html")));
        QSignalSpy finished(&exporter, &Exporter::finished);
        QVERIFY(exporter.exportFolder(folder, {dir.filePath(QStringLiteral("Book/1 Start.md")),
                                               dir.filePath(QStringLiteral("Book/2 Middle.md"))},
                                      QStringLiteral("The Book"), QStringLiteral("Matt"),
                                      exporter.profilesFolder() + QStringLiteral("/HTML.yaml"), html));
        QVERIFY(finished.wait(30000));
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(2).toString()));
        const QString page = readFile(html);
        QVERIFY(page.contains(QStringLiteral("The Book")));
        QVERIFY(page.indexOf(QStringLiteral("First middle.")) < page.indexOf(QStringLiteral("Second.")));
        QVERIFY(page.contains(QStringLiteral(">Start</h1>")));
        QVERIFY(!exporter.exportFolder(folder, {}, QString(), QString(), exporter.profilesFolder() + QStringLiteral("/HTML.yaml"), html));
    }

    // ── Citations ───────────────────────────────────────────────────────────

    void readsBibTeX() {
        const auto entries = Citations::parseBibTeX(QStringLiteral(
            "@comment{ignore me}\n@string{jx = \"Journal X\"}\n"
            "@article{luhmann1981,\n  author = {Luhmann, Niklas},\n  title = {Kommunikation mit {Zettelk\\\"a}sten (Part 1},\n"
            "  year = 1981, journal = jx }\n"
            "@book( ahrens2017 , author = \"S{\\\"o}nke Ahrens and Jane Doe\", title = \"How to Take Smart Notes\", date = {2017-02-24})\n"
            "@misc{many, author = {A, B and C, D and E, F}, title = {Group}}\n"));
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries.at(0).key, QStringLiteral("luhmann1981"));
        QCOMPARE(entries.at(0).author, QStringLiteral("Luhmann"));
        QCOMPARE(entries.at(0).year, QStringLiteral("1981"));
        QCOMPARE(entries.at(0).title, QStringLiteral("Kommunikation mit Zettelkästen (Part 1"));
        QCOMPARE(entries.at(1).key, QStringLiteral("ahrens2017"));
        QCOMPARE(entries.at(1).author, QStringLiteral("Ahrens & Doe"));
        QCOMPARE(entries.at(1).year, QStringLiteral("2017"));
        QCOMPARE(entries.at(2).author, QStringLiteral("A et al."));
    }

    void readsCslJson() {
        const auto entries = Citations::parseCslJson(
            "[{\"id\":\"doe2020\",\"title\":\"A  Title\",\"author\":[{\"family\":\"Doe\",\"given\":\"J\"}],"
            "\"issued\":{\"date-parts\":[[2020,5]]}},{\"id\":\"org\",\"author\":[{\"literal\":\"WHO\"}],"
            "\"issued\":{\"raw\":\"2019\"}},{\"title\":\"no id\"}]");
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).key, QStringLiteral("doe2020"));
        QCOMPARE(entries.at(0).title, QStringLiteral("A Title"));
        QCOMPARE(entries.at(0).year, QStringLiteral("2020"));
        QCOMPARE(entries.at(1).author, QStringLiteral("WHO"));
        QCOMPARE(entries.at(1).year, QStringLiteral("2019"));
    }

    void keepsCitationSettingsInTheFolder() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("refs/library.bib")),
                  "@book{one, author={One, A}, title={First}, year={2001}}\n");
        Citations citations;
        citations.setRoot(QFileInfo(dir.path()).canonicalFilePath());
        QCOMPARE(citations.count(), 0);
        QVERIFY(citations.setBibliography(dir.filePath(QStringLiteral("refs/library.bib"))));
        QCOMPARE(citations.count(), 1);
        // Stored relative to the notes folder, so it moves with it.
        QVERIFY(readFile(dir.filePath(QStringLiteral(".omazettel/citations.yaml")))
                    .contains(QStringLiteral("bibliography: \"refs/library.bib\"")));
        QCOMPARE(Citations::settingsFor(QFileInfo(dir.path()).canonicalFilePath()).bibliography,
                 QFileInfo(dir.filePath(QStringLiteral("refs/library.bib"))).canonicalFilePath());
        QCOMPARE(citations.entries().first().toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("One 2001 · First"));
        QCOMPARE(citations.describe(QStringLiteral("one")), QStringLiteral("One (2001) First"));

        // Zotero rewrites the file: it reloads.
        writeFile(dir.filePath(QStringLiteral("refs/library.bib")),
                  "@book{one, title={First}}\n@book{two, title={Second}}\n");
        QTRY_COMPARE(citations.count(), 2);

        writeFile(dir.filePath(QStringLiteral("refs/empty.bib")), "nothing here\n");
        citations.setBibliography(dir.filePath(QStringLiteral("refs/empty.bib")));
        QVERIFY(!citations.error().isEmpty());
        citations.setBibliography(QString());
        QCOMPARE(citations.count(), 0);
        QVERIFY(citations.error().isEmpty());
    }

    void exportsCitations() {
        QTemporaryDir dir;
        Exporter exporter;
        if (!exporter.pandocAvailable())
            QSKIP("pandoc is not installed.");
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        exporter.setRoot(root);
        exporter.ensureDefaults();
        writeFile(dir.filePath(QStringLiteral("refs.bib")),
                  "@book{ahrens2017, author={Ahrens, S\\\"onke}, title={How to Take Smart Notes}, year={2017}, publisher={CreateSpace}}\n");
        Citations citations;
        citations.setRoot(root);
        citations.setBibliography(dir.filePath(QStringLiteral("refs.bib")));
        QVERIFY(exporter.citationArguments(QStringLiteral("No citations here.")).isEmpty());
        QVERIFY(exporter.citationArguments(QStringLiteral("As shown [@ahrens2017].")).contains(QStringLiteral("--citeproc")));
        QVERIFY(exporter.citationArguments(QStringLiteral("mail me at a@b.com")).isEmpty());

        const QString note = dir.filePath(QStringLiteral("Cited.md"));
        writeFile(note, "---\ntitle: \"Cited\"\n---\n\nSlip boxes work [@ahrens2017, p. 12].\n");
        const QString html = dir.filePath(QStringLiteral("Cited.html"));
        QSignalSpy finished(&exporter, &Exporter::finished);
        QVERIFY(exporter.exportNote(note, exporter.profilesFolder() + QStringLiteral("/HTML.yaml"), html));
        QVERIFY(finished.wait(30000));
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(2).toString()));
        const QString page = readFile(html);
        QVERIFY2(page.contains(QStringLiteral("Ahrens 2017, 12")), qPrintable(page.right(1500)));
        QVERIFY(page.contains(QStringLiteral("How to Take Smart Notes")));
        QVERIFY(page.contains(QStringLiteral("refs")));
    }

    void completesCitations() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Draft.md")), "# Draft\n");
        writeFile(dir.filePath(QStringLiteral("refs.bib")),
                  "@book{ahrens2017, author={Ahrens, S}, title={Smart Notes}, year={2017}}\n@book{luhmann1981, author={Luhmann, N}, title={Kommunikation}, year={1981}}\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        m_windowCitations->setBibliography(dir.filePath(QStringLiteral("refs.bib")));
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Draft.md")))));
        auto *editor = window->findChild<QQuickItem *>(QStringLiteral("sourceEditor"));
        auto *completer = window->findChild<QObject *>(QStringLiteral("citeCompleter"));
        QVERIFY(completer);
        editor->setProperty("text", QStringLiteral("# Draft\n\nAs argued [see @luh"));
        editor->setProperty("cursorPosition", editor->property("length"));
        QTRY_VERIFY(completer->property("opened").toBool());
        const QVariantList results = completer->property("results").toList();
        QCOMPARE(results.first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("luhmann1981"));
        QMetaObject::invokeMethod(completer, "accept");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("# Draft\n\nAs argued [see @luhmann1981"));

        // Search by author too, and not outside brackets.
        QCOMPARE(backend.citationQueryAt(editor->property("length").toInt()).value(QStringLiteral("query")).toString(),
                 QStringLiteral("luhmann1981"));
        // Right after the bracket, and after a semicolon.
        editor->setProperty("text", QStringLiteral("x [@"));
        QCOMPARE(backend.citationQueryAt(4).value(QStringLiteral("query")).toString(), QString());
        QVERIFY(!backend.citationQueryAt(4).isEmpty());
        editor->setProperty("text", QStringLiteral("[@one; @ah"));
        QCOMPARE(backend.citationQueryAt(10).value(QStringLiteral("query")).toString(), QStringLiteral("ah"));
        editor->setProperty("text", QStringLiteral("email a@b and @luh"));
        QVERIFY(backend.citationQueryAt(editor->property("length").toInt()).isEmpty());
    }


    // ── Sync ────────────────────────────────────────────────────────────────

    void diffsLines() {
        const QVariantList diff = TextDiff::lines(QStringLiteral("a\nb\nc\nd"), QStringLiteral("a\nB\nc\nd\ne"));
        QStringList shown;
        for (const QVariant &line : diff)
            shown << line.toMap().value(QStringLiteral("kind")).toString().left(1) + line.toMap().value(QStringLiteral("text")).toString();
        QCOMPARE(shown, (QStringList{QStringLiteral("sa"), QStringLiteral("rb"), QStringLiteral("aB"),
                                     QStringLiteral("sc"), QStringLiteral("sd"), QStringLiteral("ae")}));
        QCOMPARE(TextDiff::counts(diff), (QPair<int, int>(2, 1)));
        QCOMPARE(TextDiff::lines(QStringLiteral("same"), QStringLiteral("same")).size(), 1);
    }

    void recognisesConflictCopies() {
        QString tool;
        QCOMPARE(Zettel::conflictOriginal(QStringLiteral("Plan.sync-conflict-20261021-101010-ABCDEFG.md"), &tool), QStringLiteral("Plan.md"));
        QCOMPARE(tool, QStringLiteral("Syncthing"));
        QCOMPARE(Zettel::conflictOriginal(QStringLiteral("My plan (conflicted copy 2026-10-21 101010).md"), &tool), QStringLiteral("My plan.md"));
        QCOMPARE(tool, QStringLiteral("Nextcloud"));
        QCOMPARE(Zettel::conflictOriginal(QStringLiteral("Plan (Matt's conflicted copy 2026-10-21).md"), &tool), QStringLiteral("Plan.md"));
        QCOMPARE(tool, QStringLiteral("Dropbox"));
        QCOMPARE(Zettel::conflictOriginal(QStringLiteral("Plan_conflict-20261021-101010.md"), &tool), QStringLiteral("Plan.md"));
        QCOMPARE(Zettel::conflictOriginal(QStringLiteral("Plan (draft).md")), QString());
        QCOMPARE(Zettel::conflictOriginal(QStringLiteral("Plan.md")), QString());
    }

    void resolvesConflictCopies() {
        QTemporaryDir dir;
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        writeFile(root + QStringLiteral("/Plan.md"), "# Plan\n\nmine\n");
        writeFile(root + QStringLiteral("/Plan.sync-conflict-20261021-101010-ABCDEFG.md"), "# Plan\n\ntheirs\n");
        writeFile(root + QStringLiteral("/Idea (conflicted copy 2026-10-21 101010).md"), "other idea\n");
        writeFile(root + QStringLiteral("/Idea.md"), "idea\n");
        writeFile(root + QStringLiteral("/Note_conflict-20261021-101010.md"), "copy\n");
        writeFile(root + QStringLiteral("/Note.md"), "note\n");

        // Copies are not notes of their own.
        NoteIndex index;
        index.setRoot(root);
        index.rescanNow();
        QCOMPARE(index.noteCount(), 3);

        Sync sync;
        sync.setRoot(root);
        QCOMPARE(sync.conflictCopies().size(), 3);
        QCOMPARE(sync.state(), QStringLiteral("off"));
        const QString planCopy = root + QStringLiteral("/Plan.sync-conflict-20261021-101010-ABCDEFG.md");
        const QVariantMap item = sync.conflict(planCopy);
        QCOMPARE(item.value(QStringLiteral("kind")).toString(), QStringLiteral("copy"));
        QCOMPARE(item.value(QStringLiteral("mine")).toString(), QStringLiteral("# Plan\n\nmine\n"));
        QCOMPARE(item.value(QStringLiteral("theirs")).toString(), QStringLiteral("# Plan\n\ntheirs\n"));

        if (!sync.resolve(planCopy, QStringLiteral("theirs")))
            QSKIP("No trash is available for the temporary folder on this system.");
        QCOMPARE(readFile(root + QStringLiteral("/Plan.md")), QStringLiteral("# Plan\n\ntheirs\n"));
        QVERIFY(!QFile::exists(planCopy));

        QVERIFY(sync.resolve(root + QStringLiteral("/Idea (conflicted copy 2026-10-21 101010).md"), QStringLiteral("both")));
        QCOMPARE(readFile(root + QStringLiteral("/Idea (other version).md")), QStringLiteral("other idea\n"));
        QCOMPARE(readFile(root + QStringLiteral("/Idea.md")), QStringLiteral("idea\n"));

        QVERIFY(sync.resolve(root + QStringLiteral("/Note_conflict-20261021-101010.md"), QStringLiteral("mine")));
        QCOMPARE(readFile(root + QStringLiteral("/Note.md")), QStringLiteral("note\n"));
        QCOMPARE(sync.conflictCopies().size(), 0);
    }

    void syncsTwoDevicesThroughARemote() {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("git is not installed.");
        QTemporaryDir remoteDir, aDir, bDir;
        const QString remote = QFileInfo(remoteDir.path()).canonicalFilePath();
        const QString a = QFileInfo(aDir.path()).canonicalFilePath();
        const QString b = QFileInfo(bDir.path()).canonicalFilePath() + QStringLiteral("/notes");
        QProcess::execute(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("--bare"), QStringLiteral("-b"),
                                                  QStringLiteral("main"), remote});

        // Device A: turn on history, connect the remote, sync.
        writeFile(a + QStringLiteral("/Plan.md"), "# Plan\n\nline one\nline two\n");
        Sync syncA;
        syncA.setRoot(a);
        QVERIFY(syncA.enable());
        QCOMPARE(syncA.state(), QStringLiteral("local"));
        QVERIFY(syncA.setRemote(remote));
        QSignalSpy syncedA(&syncA, &Sync::synced);
        QVERIFY(syncA.syncNow());
        QVERIFY(syncedA.wait(60000));
        QVERIFY2(syncedA.last().at(0).toBool(), qPrintable(syncA.message()));
        QCOMPARE(syncA.state(), QStringLiteral("clean"));

        // Device B: a clone.
        QProcess::execute(QStringLiteral("git"), {QStringLiteral("clone"), QStringLiteral("-q"), remote, b});
        Sync syncB;
        syncB.setRoot(b);
        QVERIFY(syncB.isRepo());
        QCOMPARE(syncB.remoteUrl(), remote);

        // A changes a note; B receives it and is told which file changed.
        writeFile(a + QStringLiteral("/Plan.md"), "# Plan\n\nline one\nline two\nline three\n");
        syncA.refresh();
        QCOMPARE(syncA.state(), QStringLiteral("changes"));
        QVERIFY(syncA.syncNow());
        QVERIFY(syncedA.wait(60000));
        QSignalSpy syncedB(&syncB, &Sync::synced);
        QVERIFY(syncB.syncNow());
        QVERIFY(syncedB.wait(60000));
        QVERIFY2(syncedB.last().at(0).toBool(), qPrintable(syncB.message()));
        QCOMPARE(syncedB.last().at(1).toStringList(), QStringList{b + QStringLiteral("/Plan.md")});
        QCOMPARE(readFile(b + QStringLiteral("/Plan.md")), QStringLiteral("# Plan\n\nline one\nline two\nline three\n"));

        // Both change the same line: B's sync stops with a conflict.
        writeFile(a + QStringLiteral("/Plan.md"), "# Plan\n\nline one from A\nline two\nline three\n");
        QVERIFY(syncA.syncNow());
        QVERIFY(syncedA.wait(60000));
        writeFile(b + QStringLiteral("/Plan.md"), "# Plan\n\nline one from B\nline two\nline three\n");
        QVERIFY(syncB.syncNow());
        QVERIFY(syncedB.wait(60000));
        QVERIFY(!syncedB.last().at(0).toBool());
        QCOMPARE(syncB.state(), QStringLiteral("conflict"));
        QCOMPARE(syncB.gitConflicts(), QStringList{b + QStringLiteral("/Plan.md")});
        QVERIFY(!syncB.syncNow());

        const QVariantMap item = syncB.conflict(b + QStringLiteral("/Plan.md"));
        QVERIFY(item.value(QStringLiteral("mine")).toString().contains(QStringLiteral("from B")));
        QVERIFY(item.value(QStringLiteral("theirs")).toString().contains(QStringLiteral("from A")));

        // Editing by hand: refused while the markers are there.
        QVERIFY(readFile(b + QStringLiteral("/Plan.md")).contains(QStringLiteral("<<<<<<<")));
        QVERIFY(!syncB.resolve(b + QStringLiteral("/Plan.md"), QStringLiteral("edited")));

        // Keep both: B's text stays, A's goes to "Plan (other version).md".
        QVERIFY(syncB.resolve(b + QStringLiteral("/Plan.md"), QStringLiteral("both")));
        QCOMPARE(syncB.state(), QStringLiteral("clean"));
        QVERIFY(readFile(b + QStringLiteral("/Plan.md")).contains(QStringLiteral("from B")));
        QVERIFY(readFile(b + QStringLiteral("/Plan (other version).md")).contains(QStringLiteral("from A")));
        QVERIFY(syncB.syncNow());
        QVERIFY(syncedB.wait(60000));
        QVERIFY2(syncedB.last().at(0).toBool(), qPrintable(syncB.message()));

        // A gets the outcome.
        QVERIFY(syncA.syncNow());
        QVERIFY(syncedA.wait(60000));
        QVERIFY(readFile(a + QStringLiteral("/Plan.md")).contains(QStringLiteral("from B")));
        QVERIFY(QFile::exists(a + QStringLiteral("/Plan (other version).md")));

        // History lists the versions, newest first, and can show each.
        const QVariantList history = syncA.history(a + QStringLiteral("/Plan.md"));
        QVERIFY(history.size() >= 4);
        const QVariantMap oldest = history.last().toMap();
        QCOMPARE(syncA.versionText(oldest.value(QStringLiteral("hash")).toString(),
                                   oldest.value(QStringLiteral("path")).toString()),
                 QStringLiteral("# Plan\n\nline one\nline two\n"));
    }

    void keepsMineOrTheirsInAGitConflict() {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("git is not installed.");
        QTemporaryDir remoteDir, aDir, bDir;
        const QString remote = QFileInfo(remoteDir.path()).canonicalFilePath();
        const QString a = QFileInfo(aDir.path()).canonicalFilePath();
        const QString b = QFileInfo(bDir.path()).canonicalFilePath() + QStringLiteral("/n");
        QProcess::execute(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("--bare"), QStringLiteral("-b"), QStringLiteral("main"), remote});
        writeFile(a + QStringLiteral("/X.md"), "base\n");
        Sync syncA;
        syncA.setRoot(a);
        syncA.enable();
        syncA.setRemote(remote);
        QSignalSpy syncedA(&syncA, &Sync::synced);
        syncA.syncNow();
        QVERIFY(syncedA.wait(60000));
        QProcess::execute(QStringLiteral("git"), {QStringLiteral("clone"), QStringLiteral("-q"), remote, b});
        Sync syncB;
        syncB.setRoot(b);
        QSignalSpy syncedB(&syncB, &Sync::synced);

        for (const QString &choice : {QStringLiteral("theirs"), QStringLiteral("mine")}) {
            writeFile(a + QStringLiteral("/X.md"), ("A " + choice + "\n").toUtf8());
            syncA.syncNow();
            QVERIFY(syncedA.wait(60000));
            writeFile(b + QStringLiteral("/X.md"), ("B " + choice + "\n").toUtf8());
            syncB.syncNow();
            QVERIFY(syncedB.wait(60000));
            QCOMPARE(syncB.state(), QStringLiteral("conflict"));
            QVERIFY(syncB.resolve(b + QStringLiteral("/X.md"), choice));
            QCOMPARE(readFile(b + QStringLiteral("/X.md")),
                     (choice == QLatin1String("mine") ? "B " : "A ") + choice + QStringLiteral("\n"));
            syncB.syncNow();
            QVERIFY(syncedB.wait(60000));
            QVERIFY(syncedB.last().at(0).toBool());
            syncA.syncNow();
            QVERIFY(syncedA.wait(60000));
        }

        // Undo a merge instead.
        writeFile(a + QStringLiteral("/X.md"), "A again\n");
        syncA.syncNow();
        QVERIFY(syncedA.wait(60000));
        writeFile(b + QStringLiteral("/X.md"), "B again\n");
        syncB.syncNow();
        QVERIFY(syncedB.wait(60000));
        QVERIFY(syncB.abortMerge());
        QCOMPARE(syncB.state(), QStringLiteral("clean"));
        QCOMPARE(readFile(b + QStringLiteral("/X.md")), QStringLiteral("B again\n"));
    }

    void explainsUnreachableRemotes() {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("git is not installed.");
        QTemporaryDir dir;
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        writeFile(root + QStringLiteral("/A.md"), "a\n");
        Sync sync;
        sync.setRoot(root);
        sync.enable();
        sync.setRemote(root + QStringLiteral("/no-such-remote"));
        QSignalSpy synced(&sync, &Sync::synced);
        QVERIFY(sync.syncNow());
        QVERIFY(synced.wait(60000));
        QVERIFY(!synced.last().at(0).toBool());
        QCOMPARE(sync.state(), QStringLiteral("error"));
        QVERIFY(!sync.message().isEmpty());
        // The change was still saved as a version locally.
        QCOMPARE(sync.changeCount(), 0);
    }

    void reloadsTheOpenNoteAfterASync() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("Open.md"));
        writeFile(path, "before\n");
        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy external(&backend, &Backend::externalChangeDetected);

        // Unchanged in the editor: reloads quietly.
        backend.pauseFileWatching();
        writeFile(path, "after sync\n");
        QVERIFY(backend.resumeFileWatching());
        QCOMPARE(editor->property("text").toString(), QStringLiteral("after sync\n"));
        QTest::qWait(300);
        QCOMPARE(external.count(), 0);

        // Nothing changed on disk: nothing happens.
        backend.pauseFileWatching();
        QVERIFY(!backend.resumeFileWatching());

        // Unsaved edits: asks, as for any outside change.
        editor->setProperty("text", QStringLiteral("typed during sync\n"));
        QVERIFY(backend.editorTextChanged());
        backend.pauseFileWatching();
        writeFile(path, "from elsewhere\n");
        QVERIFY(!backend.resumeFileWatching());
        QCOMPARE(external.count(), 1);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("typed during sync\n"));
    }

    void historyDialogRestoresAVersion() {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("git is not installed.");
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Note.md")), "version one\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(m_windowSync->enable());
        writeFile(dir.filePath(QStringLiteral("Note.md")), "version two\n");
        QSignalSpy synced(m_windowSync.get(), &Sync::synced);
        QMetaObject::invokeMethod(window.get(), "syncNotes", Q_ARG(QVariant, QVariant(true)));
        QVERIFY(synced.wait(60000));
        QCOMPARE(m_windowSync->state(), QStringLiteral("local"));

        const QString note = vault.rootPath() + QStringLiteral("/Note.md");
        QMetaObject::invokeMethod(window.get(), "openHistory", Q_ARG(QVariant, note));
        auto *history = window->findChild<QObject *>(QStringLiteral("historyDialog"));
        QCOMPARE(history->property("versions").toList().size(), 2);
        history->setProperty("versionIndex", 1);
        QCOMPARE(history->property("versionText").toString(), QStringLiteral("version one\n"));
        QVERIFY(!history->property("lines").toList().isEmpty());
        QMetaObject::invokeMethod(history, "restoreRequested", Q_ARG(QString, QStringLiteral("version one\n")));
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("version one\n"));
    }


    // ── Command palette and settings ────────────────────────────────────────

    void commandPaletteRunsCommands() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("A.md")), "# A\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        vault.setSidebarVisible(true);
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVariant list;
        QMetaObject::invokeMethod(window.get(), "commandList", Q_RETURN_ARG(QVariant, list));
        QStringList ids;
        for (const QVariant &command : list.toList())
            ids << command.toMap().value(QStringLiteral("id")).toString();
        QVERIFY(ids.contains(QStringLiteral("newNote")));
        QVERIFY(ids.contains(QStringLiteral("settings")));
        // Nothing open: no note commands, and no history without git.
        QVERIFY(!ids.contains(QStringLiteral("export")));
        QVERIFY(!ids.contains(QStringLiteral("history")));
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("A.md")))));
        QMetaObject::invokeMethod(window.get(), "commandList", Q_RETURN_ARG(QVariant, list));
        bool hasExport = false;
        for (const QVariant &command : list.toList())
            hasExport = hasExport || command.toMap().value(QStringLiteral("id")) == QStringLiteral("export");
        QVERIFY(hasExport);

        // The palette filters them by name.
        auto *palette = window->findChild<QObject *>(QStringLiteral("commandPalette"));
        palette->setProperty("sourceItems", list);
        palette->setProperty("query", QStringLiteral("sidebar"));
        QCOMPARE(palette->property("results").toList().first().toMap().value(QStringLiteral("id")).toString(),
                 QStringLiteral("sidebar"));

        QMetaObject::invokeMethod(window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sidebar")));
        QVERIFY(!vault.sidebarVisible());
        QMetaObject::invokeMethod(window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("palette")));
        QVERIFY(backend.omaPalette());
        QMetaObject::invokeMethod(window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("settings")));
        QVERIFY(window->findChild<QObject *>(QStringLiteral("settingsDialog"))->property("opened").toBool()
                || window->findChild<QObject *>(QStringLiteral("settingsDialog"))->property("visible").toBool());
    }

    void exportsGoToTheExportsFolderWhenChosen() {
        QTemporaryDir dir;
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        Exporter exporter;
        exporter.setRoot(root);
        exporter.ensureDefaults();
        const QString pdf = exporter.profilesFolder() + QStringLiteral("/PDF.yaml");
        exporter.setExportLocation(QStringLiteral("beside"));
        QCOMPARE(exporter.defaultOutputPath(root + QStringLiteral("/Sub/N.md"), pdf), root + QStringLiteral("/Sub/N.pdf"));
        exporter.setExportLocation(QStringLiteral("folder"));
        QCOMPARE(exporter.exportLocation(), QStringLiteral("folder"));
        QCOMPARE(exporter.defaultOutputPath(root + QStringLiteral("/Sub/N.md"), pdf), root + QStringLiteral("/Exports/N.pdf"));
        QCOMPARE(exporter.defaultFolderOutputPath(root + QStringLiteral("/Sub"), pdf), root + QStringLiteral("/Exports/Sub.pdf"));
        // Kept out of git sync, once.
        exporter.setExportLocation(QStringLiteral("beside"));
        exporter.setExportLocation(QStringLiteral("folder"));
        QCOMPARE(readFile(root + QStringLiteral("/.gitignore")).count(QStringLiteral("/Exports/")), 1);
        exporter.setExportLocation(QStringLiteral("beside"));
    }

    void autoCreatingLinkedNotesCanBeTurnedOff() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Hub.md")), "# Hub\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        vault.setAutoCreateLinks(false);
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Hub.md")))));
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        editor->setProperty("text", QStringLiteral("[[Elsewhere]] text"));
        editor->setProperty("cursorPosition", 0);
        backend.save();
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("Elsewhere.md"))));
        vault.setAutoCreateLinks(true);
        Vault again;
        QVERIFY(again.autoCreateLinks());
    }


    // ── Theme colours ───────────────────────────────────────────────────────

    void rendersThemeTemplates() {
        const QHash<QString, QString> colors{{QStringLiteral("background"), QStringLiteral("#000000")},
                                             {QStringLiteral("foreground"), QStringLiteral("#ffffff")},
                                             {QStringLiteral("selection_background"), QStringLiteral("#123456")}};
        QCOMPARE(AppTheme::render(QStringLiteral("a = \"{{ background }}\"\nb = \"{{ foreground_strip }}\"\n"
                                                 "c = \"{{ mix background foreground 50% }}\"\nd = \"{{ selection_background }}\"\n"
                                                 "e = \"{{ nosuch }}\"\nf = \"{{ background_rgb }}\""),
                                  colors),
                 QStringLiteral("a = \"#000000\"\nb = \"ffffff\"\nc = \"#808080\"\nd = \"#123456\"\n"
                                "e = \"{{ nosuch }}\"\nf = \"0,0,0\""));
        const auto parsed = AppTheme::parse(QStringLiteral(
            "# c\n[editor]\nbackground = \"#101010\"\nlink = '#ABCDEF'\nbad = \"{{ nosuch }}\"\n[ui]\npanel = #202020 # trailing\n"));
        QCOMPARE(parsed.value(QStringLiteral("editor.background")), QStringLiteral("#101010"));
        QCOMPARE(parsed.value(QStringLiteral("editor.link")), QStringLiteral("#abcdef"));
        QCOMPARE(parsed.value(QStringLiteral("ui.panel")), QStringLiteral("#202020"));
        QVERIFY(!parsed.contains(QStringLiteral("editor.bad")));

        QHash<QString, QString> partial{{QStringLiteral("editor.background"), QStringLiteral("#ffffff")},
                                        {QStringLiteral("editor.foreground"), QStringLiteral("#000000")}};
        AppTheme::complete(partial, false);
        for (const QString &name : AppTheme::names())
            QVERIFY2(QColor(partial.value(name)).isValid(), qPrintable(name));
        QCOMPARE(partial.value(QStringLiteral("editor.heading")), QStringLiteral("#000000"));

        const auto oma = AppTheme::omaPalette(QStringLiteral("#ff0000"));
        QCOMPARE(oma.colors.value(QStringLiteral("ui.panel")), QStringLiteral("#111722"));
        QCOMPARE(oma.colors.value(QStringLiteral("ui.accent")), QStringLiteral("#ff0000"));
    }

    void builtInTemplateCoversEveryName() {
        QFile file(QFINDTESTDATA("../../themed/omazettel.toml.tpl"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString text = QString::fromUtf8(file.readAll());
        QHash<QString, QString> colors;
        for (const char *name : {"background", "foreground", "cursor", "selection_background", "selection_foreground",
                                 "bright_foreground", "blue", "accent", "magenta", "yellow", "cyan", "lighter_background",
                                 "light_foreground", "dark_foreground", "orange", "dark_background"})
            colors.insert(QString::fromLatin1(name), QStringLiteral("#336699"));
        const auto parsed = AppTheme::parse(AppTheme::render(text, colors));
        for (const QString &name : AppTheme::names())
            QVERIFY2(parsed.contains(name), qPrintable(name));
    }

    void followsTheOmarchyTheme() {
        // A home of its own with a current Omarchy theme.
        QTemporaryDir home;
        const QByteArray realHome = qgetenv("HOME");
        qputenv("HOME", home.path().toUtf8());
        const auto restore = qScopeGuard([&realHome]() { qputenv("HOME", realHome); });
        const QString themeDir = home.filePath(QStringLiteral(".local/state/omarchy/current/theme"));
        writeFile(themeDir + QStringLiteral("/colors.toml"),
                  "mode = \"light\"\naccent = \"#ff8800\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                  "blue = \"#0044cc\"\nmagenta = \"#aa00aa\"\nyellow = \"#aa8800\"\ncyan = \"#007788\"\n");

        Backend backend;
        backend.setOmaPalette(false);
        backend.setDarkMode(false);
        QCOMPARE(backend.themeSource(), QStringLiteral("built-in template"));
        QCOMPARE(backend.themeBackground().toLower(), QStringLiteral("#fafafa"));
        QVERIFY(!backend.darkMode());
        QVariantMap tokens = backend.themeTokens();
        QCOMPARE(tokens.value(QStringLiteral("ui.accent")).toString(), QStringLiteral("#ff8800"));
        QCOMPARE(tokens.value(QStringLiteral("editor.tag")).toString(), QStringLiteral("#aa00aa"));
        for (const QString &name : AppTheme::names())
            QVERIFY2(tokens.contains(name), qPrintable(name));

        // The user's template wins over the built-in one...
        const QString user = backend.customizeThemeTemplate();
        QCOMPARE(user, home.filePath(QStringLiteral(".config/omarchy/themed/omazettel.toml.tpl")));
        QString text = readFile(user);
        text.replace(QStringLiteral("tag = \"{{ magenta }}\""), QStringLiteral("tag = \"{{ blue }}\""));
        writeFile(user, text.toUtf8());
        backend.setDarkMode(false);
        QCOMPARE(backend.themeSource(), QStringLiteral("your template"));
        QCOMPARE(backend.themeTokens().value(QStringLiteral("editor.tag")).toString(), QStringLiteral("#0044cc"));

        // ...and a theme's own file wins over both.
        writeFile(themeDir + QStringLiteral("/omazettel.toml"), "[editor]\ntag = \"#00ff00\"\n[ui]\npanel = \"#eeeeee\"\n");
        backend.setDarkMode(false);
        QCOMPARE(backend.themeSource(), QStringLiteral("theme file"));
        tokens = backend.themeTokens();
        QCOMPARE(tokens.value(QStringLiteral("editor.tag")).toString(), QStringLiteral("#00ff00"));
        QCOMPARE(tokens.value(QStringLiteral("ui.panel")).toString(), QStringLiteral("#eeeeee"));
        // Names the file leaves out still have colours.
        QCOMPARE(tokens.value(QStringLiteral("editor.background")).toString(), QStringLiteral("#fafafa"));

        // The Oma palette overrides it all but the accent.
        backend.setOmaPalette(true);
        QCOMPARE(backend.themeSource(), QStringLiteral("Oma palette"));
        QCOMPARE(backend.themeTokens().value(QStringLiteral("ui.panel")).toString(), QStringLiteral("#111722"));
        QCOMPARE(backend.themeAccent().toLower(), QStringLiteral("#ff8800"));
        backend.setOmaPalette(false);
    }

    // ── Oma palette ─────────────────────────────────────────────────────────

    void omaPaletteSetsTheSurround() {
        Backend backend;
        QSignalSpy colors(&backend, &Backend::themeColorsChanged);
        backend.setOmaPalette(true);
        QVERIFY(colors.count() >= 1);
        QVERIFY(backend.darkMode());
        QCOMPARE(backend.themeBackground().toUpper(), QStringLiteral("#0B0E13"));
        QCOMPARE(backend.themeForeground().toUpper(), QStringLiteral("#E7EDF7"));
        // The selection mixes the accent into the page, so it stays opaque.
        QVERIFY(QColor(backend.themeSelection()).isValid());
        QVERIFY(backend.themeSelection().toUpper() != backend.themeAccent().toUpper());
        QVERIFY(QSettings().value(QStringLiteral("appearance/omaPalette")).toBool());

        backend.setOmaPalette(false);
        QVERIFY(backend.themeBackground().toUpper() != QStringLiteral("#0B0E13"));
        QVERIFY(!QSettings().value(QStringLiteral("appearance/omaPalette")).toBool());
    }

    void windowUsesPaletteColours() {
        Backend backend;
        backend.setOmaPalette(true);
        Vault vault;
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QCOMPARE(window->property("panelColor").value<QColor>(), QColor(QStringLiteral("#111722")));
        auto *sidebar = window->findChild<QObject *>(QStringLiteral("sidebar"));
        QCOMPARE(sidebar->property("panelColor").value<QColor>(), QColor(QStringLiteral("#111722")));
        backend.setOmaPalette(false);
        QVERIFY(window->property("panelColor").value<QColor>() != QColor(QStringLiteral("#111722")));
    }

    // ── The window ──────────────────────────────────────────────────────────

    void sidebarOpensNotesInPlace() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("One.md")), "# One\n");
        writeFile(dir.filePath(QStringLiteral("Two.md")), "# Two\n");

        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        vault.setSidebarVisible(true);
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);

        auto *sidebar = window->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(sidebar);
        QVERIFY(sidebar->isVisible());
        QCOMPARE(qRound(sidebar->width()), vault.sidebarWidth());

        const QUrl one = QUrl::fromLocalFile(dir.filePath(QStringLiteral("One.md")));
        const QUrl two = QUrl::fromLocalFile(dir.filePath(QStringLiteral("Two.md")));
        QMetaObject::invokeMethod(window.get(), "openNote", Q_ARG(QVariant, one));
        QCOMPARE(backend.fileUrl(), one);

        // A quick edit is saved, not asked about, on switching notes.
        auto *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("# One\n\nmore"));
        QVERIFY(backend.modified());
        QMetaObject::invokeMethod(window.get(), "openNote", Q_ARG(QVariant, two));
        QCOMPARE(backend.fileUrl(), two);
        QCOMPARE(readFile(dir.filePath(QStringLiteral("One.md"))), QStringLiteral("# One\n\nmore"));
        auto *unsaved = window->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(unsaved);
        QVERIFY(!unsaved->property("opened").toBool());

        // The last note is remembered for the next start.
        QCOMPARE(vault.lastNotePath(), dir.filePath(QStringLiteral("Two.md")));
    }

    void newNotesGoInTheSelectedFolder() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Top.md")), "# Top\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        vault.setSidebarVisible(true);
        const QString first = vault.createFolder(vault.rootPath(), QStringLiteral("First"));
        const QString second = vault.createFolder(vault.rootPath(), QStringLiteral("Second"));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);
        auto *sidebar = window->findChild<QQuickItem *>(QStringLiteral("sidebar"));

        // With a note open and nothing else picked, its folder.
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Top.md")))));
        QCOMPARE(sidebar->property("targetFolder").toString(), vault.rootPath());

        // Clicking a folder picks it, even with a note open elsewhere.
        sidebar->setProperty("selectedPath", first);
        QCOMPARE(sidebar->property("targetFolder").toString(), first);
        sidebar->setProperty("selectedPath", second + QStringLiteral("/x.md"));
        QCOMPARE(sidebar->property("targetFolder").toString(), second);
    }

    void movingTheOpenNoteKeepsItOpen() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("Open.md")), "# Open\n");
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        const QString folder = vault.createFolder(vault.rootPath(), QStringLiteral("Archive"));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);
        QMetaObject::invokeMethod(window.get(), "openNote",
                                  Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Open.md")))));

        QMetaObject::invokeMethod(window.get(), "moveItem",
                                  Q_ARG(QVariant, vault.rootPath() + QStringLiteral("/Open.md")),
                                  Q_ARG(QVariant, folder));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(folder + QStringLiteral("/Open.md")));

        // And moving the folder holding it.
        const QString outer = vault.createFolder(vault.rootPath(), QStringLiteral("Outer"));
        QMetaObject::invokeMethod(window.get(), "moveItem", Q_ARG(QVariant, folder), Q_ARG(QVariant, outer));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(outer + QStringLiteral("/Archive/Open.md")));
        QVERIFY(QFile::exists(outer + QStringLiteral("/Archive/Open.md")));
    }

    void dragsANoteOntoAFolder() {
        QTemporaryDir dir;
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        vault.setSidebarVisible(true);
        const QString folder = vault.createFolder(vault.rootPath(), QStringLiteral("Folder"));
        const QString note = vault.createNote(vault.rootPath(), QStringLiteral("Loose"));
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        quickWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(quickWindow));

        QQuickItem *noteRow = nullptr;
        QQuickItem *folderRow = nullptr;
        // The folder model loads and sorts in the background; wait until it
        // has put the folder first, so rows do not move under the drag.
        QTRY_VERIFY((noteRow = rowLabelled(quickWindow, QStringLiteral("Loose")))
                    && (folderRow = rowLabelled(quickWindow, QStringLiteral("Folder")))
                    && folderRow->mapToScene(QPointF()).y() < noteRow->mapToScene(QPointF()).y());
        QTest::qWait(100);

        const QPoint from = noteRow->mapToScene(QPointF(noteRow->width() / 2, noteRow->height() / 2)).toPoint();
        const QPoint to = folderRow->mapToScene(QPointF(folderRow->width() / 2, folderRow->height() / 2)).toPoint();
        QTest::mousePress(quickWindow, Qt::LeftButton, {}, from);
        for (int step = 1; step <= 10; ++step)
            QTest::mouseMove(quickWindow, from + (to - from) * step / 10, 10);
        QTest::mouseRelease(quickWindow, Qt::LeftButton, {}, to, 10);

        QTRY_VERIFY(QFile::exists(folder + QStringLiteral("/Loose.md")));
        QVERIFY(!QFile::exists(note));
    }

    void sidebarHidesAndShowsTheEmptyState() {
        Backend backend;
        Vault vault;
        vault.setSidebarVisible(true);
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);
        auto *empty = window->findChild<QQuickItem *>(QStringLiteral("sidebarEmptyState"));
        QVERIFY(empty);
        QVERIFY(empty->isVisible());

        auto *sidebar = window->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        vault.setSidebarVisible(false);
        QVERIFY(!sidebar->isVisible());
        QCOMPARE(window->property("contentWidth").toInt(), window->property("width").toInt());
    }

    // ── Command line and MCP ────────────────────────────────────────────────

    void editsSectionsAndHeaders() {
        const QString note = QStringLiteral(
            "---\ntitle: \"Kickoff\"\ntags: [meeting]\nattendees:\n  - Ann\n---\n\n"
            "## Agenda\n\n- \n\n## Notes\n\nFirst.\n\n### Detail\n\nDeep.\n\n## Action items\n\n- [ ] \n");

        // Empty template bullets make way; list items join the list.
        QString text = NotesApi::appendText(note, QStringLiteral("- [ ] Ann: send deck"), QStringLiteral("Action items"));
        QVERIFY(text.endsWith(QStringLiteral("## Action items\n\n- [ ] Ann: send deck\n")));
        text = NotesApi::appendText(text, QStringLiteral("- [ ] Bo: book room"), QStringLiteral("## action ITEMS"));
        QVERIFY(text.contains(QStringLiteral("- [ ] Ann: send deck\n- [ ] Bo: book room\n")));

        // A section runs to the next heading of its level, subsections included.
        text = NotesApi::appendText(text, QStringLiteral("Second."), QStringLiteral("Notes"));
        QVERIFY(text.contains(QStringLiteral("Deep.\n\nSecond.\n\n## Action items")));

        // Missing sections are added at the end.
        text = NotesApi::appendText(text, QStringLiteral("Late data."), QStringLiteral("Risks"));
        QVERIFY(text.endsWith(QStringLiteral("- [ ] Bo: book room\n\n## Risks\n\nLate data.\n")));

        text = NotesApi::replaceSection(text, QStringLiteral("Agenda"), QStringLiteral("- Budget\n- Dates"));
        QVERIFY(text.contains(QStringLiteral("## Agenda\n\n- Budget\n- Dates\n\n## Notes")));
        text = NotesApi::replaceSection(text, QStringLiteral("Agenda"), QString());
        QVERIFY(text.contains(QStringLiteral("## Agenda\n\n## Notes")));

        // The header: replace a block list, add, remove.
        text = NotesApi::setMeta(text, {{QStringLiteral("attendees"), QJsonArray{QStringLiteral("Ann"), QStringLiteral("Bo")}},
                                        {QStringLiteral("status"), QStringLiteral("draft \"v1\"")},
                                        {QStringLiteral("tags"), QJsonValue()}});
        QVERIFY(text.startsWith(QStringLiteral("---\ntitle: \"Kickoff\"\nattendees: [\"Ann\", \"Bo\"]\nstatus: \"draft \\\"v1\\\"\"\n---\n")));
        const QJsonObject meta = NotesApi::readMeta(text);
        QCOMPARE(meta.value(QStringLiteral("title")).toString(), QStringLiteral("Kickoff"));
        QCOMPARE(meta.value(QStringLiteral("attendees")).toArray().size(), 2);
        QCOMPARE(meta.value(QStringLiteral("status")).toString(), QStringLiteral("draft \"v1\""));
        QVERIFY(!meta.contains(QStringLiteral("tags")));
        QCOMPARE(NotesApi::readMeta(note).value(QStringLiteral("attendees")).toArray().first().toString(), QStringLiteral("Ann"));

        // A note with no header gets one.
        QCOMPARE(NotesApi::setMeta(QStringLiteral("# Hi\n"), {{QStringLiteral("n"), 2}}),
                 QStringLiteral("---\nn: 2\n---\n\n# Hi\n"));

        // Content goes where the template's cursor was, in place of an empty bullet.
        const QString drafted = QStringLiteral("## Agenda\n\n- \n\n## Notes\n");
        QCOMPARE(NotesApi::insertAt(drafted, 13, QStringLiteral("- One\n- Two")),
                 QStringLiteral("## Agenda\n\n- One\n- Two\n\n## Notes\n"));

        // Tasks, not inside code or the header.
        const QList<NotesApi::Task> tasks = NotesApi::tasks(QStringLiteral(
            "---\nx: \"- [ ] no\"\n---\n- [ ] one\n```\n- [ ] code\n```\n  * [x] two\n- [ ] \n"));
        QCOMPARE(tasks.size(), 2);
        QCOMPARE(tasks.at(0).text, QStringLiteral("one"));
        QCOMPARE(tasks.at(0).line, 4);
        QVERIFY(tasks.at(1).done);
    }

    void readsCaptionFiles() {
        const QString vtt = QStringLiteral(
            "WEBVTT\n\nNOTE made by Teams\n\n1\n00:00:01.000 --> 00:00:03.000\n<v Ann Lee>Hello &amp; welcome.</v>\n\n"
            "2\n00:00:03.500 --> 00:00:05.000\n<v Ann Lee>Let's start.</v>\n\n"
            "3\n00:01:05.000 --> 00:01:07.000\n<v Bo>Thanks.</v>\n");
        QStringList speakers;
        QCOMPARE(Transcriber::fromCaptions(vtt, false, &speakers),
                 QStringLiteral("Ann Lee: Hello & welcome. Let's start.\n\nBo: Thanks."));
        QCOMPARE(speakers, (QStringList{QStringLiteral("Ann Lee"), QStringLiteral("Bo")}));
        QVERIFY(Transcriber::fromCaptions(vtt, true).startsWith(QStringLiteral("[00:00:01] Ann Lee: Hello")));

        const QString srt = QStringLiteral(
            "1\r\n00:00:01,000 --> 00:00:02,000\r\nAnn: One\r\n\r\n2\r\n00:00:02,000 --> 00:00:03,000\r\nAnn: One two\r\n");
        QCOMPARE(Transcriber::fromCaptions(srt, false), QStringLiteral("Ann: One two"));

        // Untimed lines with no speaker stay paragraphs of their own.
        QCOMPARE(Transcriber::fromCaptions(QStringLiteral("First.\n\nSecond.\n"), false),
                 QStringLiteral("First.\n\nSecond."));
    }

    void runsNoteToolsOnAFolder() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        writeFile(root.filePath(QStringLiteral("Projects/Atlas.md")), "# Atlas\n\nSee [[Kickoff]].\n");
        writeFile(root.filePath(QStringLiteral("A/Same.md")), "# One\n");
        writeFile(root.filePath(QStringLiteral("B/Same.md")), "# Two\n");

        NotesApi api;
        QString error;
        QVERIFY(api.open(root.path(), &error));
        const auto call = [&api](const QString &tool, const QJsonObject &args) {
            return api.call(*NotesApi::toolNamed(tool), args);
        };
        const auto fails = [&call](const QString &tool, const QJsonObject &args) {
            try {
                call(tool, args);
            } catch (const NotesApi::Error &error) {
                return error.message;
            }
            return QString();
        };

        // A meeting note from the template, filled in.
        QJsonObject made = call(QStringLiteral("create_note"), {
            {QStringLiteral("title"), QStringLiteral("Kickoff")},
            {QStringLiteral("template"), QStringLiteral("meeting")},
            {QStringLiteral("sections"), QJsonObject{{QStringLiteral("Notes"), QStringLiteral("Talked about [[Atlas]] and [[Budget]].")},
                                                     {QStringLiteral("Action items"), QJsonArray{QStringLiteral("[ ] Ann: deck")}}}},
            {QStringLiteral("meta"), QJsonObject{{QStringLiteral("attendees"), QJsonArray{QStringLiteral("Ann")}}}},
            {QStringLiteral("tags"), QJsonArray{QStringLiteral("atlas")}},
            {QStringLiteral("linkNotes"), false}});
        const QString kickoff = made.value(QStringLiteral("path")).toString();
        QVERIFY(kickoff.startsWith(QStringLiteral("Meetings/")));
        QVERIFY(kickoff.endsWith(QStringLiteral("-Kickoff.md")));
        QString text = readFile(root.filePath(kickoff));
        QVERIFY(text.contains(QStringLiteral("tags: [\"meeting\", \"atlas\"]")));
        QVERIFY(text.contains(QStringLiteral("attendees: [\"Ann\"]")));
        QVERIFY(text.contains(QStringLiteral("## Action items\n\n- [ ] Ann: deck\n")));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("Meetings/Budget.md"))));

        // Links to notes that do not exist make them, when asked.
        made = call(QStringLiteral("append_to_note"), {{QStringLiteral("note"), QStringLiteral("kickoff")},
                                                       {QStringLiteral("text"), QStringLiteral("- [ ] Bo: [[Budget]]")},
                                                       {QStringLiteral("section"), QStringLiteral("Action items")},
                                                       {QStringLiteral("linkNotes"), true}});
        QCOMPARE(made.value(QStringLiteral("linkedNotesCreated")).toArray().first().toString(), QStringLiteral("Meetings/Budget.md"));
        QVERIFY(readFile(root.filePath(kickoff)).contains(QStringLiteral("- [ ] Ann: deck\n- [ ] Bo: [[Budget]]\n")));

        // Found by name, title or path; several of one name need a path.
        QCOMPARE(call(QStringLiteral("read_note"), {{QStringLiteral("note"), QStringLiteral("Atlas")}})
                     .value(QStringLiteral("backlinks")).toArray().size(), 1);
        QVERIFY(fails(QStringLiteral("read_note"), {{QStringLiteral("note"), QStringLiteral("Same")}}).contains(QStringLiteral("several")));
        QCOMPARE(call(QStringLiteral("read_note"), {{QStringLiteral("note"), QStringLiteral("B/Same")}})
                     .value(QStringLiteral("title")).toString(), QStringLiteral("Two"));
        QVERIFY(fails(QStringLiteral("read_note"), {{QStringLiteral("note"), QStringLiteral("Nope")}}).contains(QStringLiteral("no note")));
        QVERIFY(fails(QStringLiteral("create_note"), {{QStringLiteral("title"), QStringLiteral("X")},
                                                      {QStringLiteral("folder"), QStringLiteral("../out")}}).contains(QStringLiteral("outside")));

        // Renaming rewrites the links to it.
        made = call(QStringLiteral("rename_note"), {{QStringLiteral("note"), QStringLiteral("Atlas")},
                                                    {QStringLiteral("name"), QStringLiteral("Atlas Program")}});
        QCOMPARE(made.value(QStringLiteral("linksUpdated")).toInt(), 1);
        QVERIFY(readFile(root.filePath(kickoff)).contains(QStringLiteral("[[Atlas-Program]]")));
        made = call(QStringLiteral("move_note"), {{QStringLiteral("note"), QStringLiteral("Atlas Program")},
                                                  {QStringLiteral("folder"), QStringLiteral("Archive/2026")}});
        QCOMPARE(made.value(QStringLiteral("path")).toString(), QStringLiteral("Archive/2026/Atlas-Program.md"));

        // Daily notes: a given day, added to under Log.
        made = call(QStringLiteral("daily_note"), {{QStringLiteral("date"), QStringLiteral("2026-01-05")},
                                                   {QStringLiteral("append"), QStringLiteral("- shipped")}});
        QCOMPARE(made.value(QStringLiteral("path")).toString(), QStringLiteral("Daily/2026-01-05.md"));
        QVERIFY(made.value(QStringLiteral("text")).toString().contains(QStringLiteral("## Log\n\n- shipped\n")));
        QVERIFY(made.value(QStringLiteral("text")).toString().contains(QStringLiteral("Monday 5 January 2026")));
        made = call(QStringLiteral("daily_note"), {{QStringLiteral("date"), QStringLiteral("2026-01-05")}});
        QVERIFY(!made.value(QStringLiteral("created")).toBool());

        call(QStringLiteral("capture"), {{QStringLiteral("text"), QStringLiteral("Call the bank")}, {QStringLiteral("task"), true}});
        const QJsonArray open = call(QStringLiteral("list_tasks"), {}).value(QStringLiteral("tasks")).toArray();
        QStringList openTexts;
        for (const QJsonValue &task : open)
            openTexts.append(task.toObject().value(QStringLiteral("text")).toString());
        QCOMPARE(openTexts, (QStringList{QStringLiteral("Call the bank"), QStringLiteral("Ann: deck"), QStringLiteral("Bo: [[Budget]]")}));

        // Content with its own header is written as given.
        made = call(QStringLiteral("create_note"), {{QStringLiteral("title"), QStringLiteral("Raw")},
                                                    {QStringLiteral("content"), QStringLiteral("---\ntitle: Raw\n---\nBody")}});
        QCOMPARE(readFile(root.filePath(QStringLiteral("Raw.md"))), QStringLiteral("---\ntitle: Raw\n---\nBody\n"));

        QCOMPARE(call(QStringLiteral("search_notes"), {{QStringLiteral("query"), QStringLiteral("shipped")}})
                     .value(QStringLiteral("results")).toArray().first().toObject().value(QStringLiteral("path")).toString(),
                 QStringLiteral("Daily/2026-01-05.md"));
        QCOMPARE(call(QStringLiteral("list_notes"), {{QStringLiteral("tag"), QStringLiteral("atlas")}})
                     .value(QStringLiteral("count")).toInt(), 1);
    }

    void answersCommandsAndMcp() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString version = QStringLiteral("9.9");
        const auto answer = [&](const QStringList &arguments) {
            return Cli::handleArguments(QStringList{QStringLiteral("omazettel")} + arguments, version);
        };
        QCOMPARE(answer({QStringLiteral("new"), QStringLiteral("Hello"), QStringLiteral("there"), QStringLiteral("--vault"),
                         root.path(), QStringLiteral("-t"), QStringLiteral("none"), QStringLiteral("--json")}),
                 std::optional<int>(0));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("Hello-there.md"))));
        QCOMPARE(answer({QStringLiteral("append"), QStringLiteral("Hello there"), QStringLiteral("- [ ] task"),
                         QStringLiteral("--vault=") + root.path(), QStringLiteral("--json")}),
                 std::optional<int>(0));
        QVERIFY(readFile(root.filePath(QStringLiteral("Hello-there.md"))).endsWith(QStringLiteral("- [ ] task\n")));
        QCOMPARE(answer({QStringLiteral("show"), QStringLiteral("--bogus"), QStringLiteral("--json")}), std::optional<int>(1));
        QCOMPARE(answer({QStringLiteral("nope")}), std::optional<int>(1));
        QCOMPARE(answer({QStringLiteral("help"), QStringLiteral("append")}), std::optional<int>(0));
        QVERIFY(Cli::verbUsage(QStringLiteral("new")).contains(QStringLiteral("--sections")));

        NotesApi api;
        QString error;
        QVERIFY(api.open(root.path(), &error));
        const QJsonObject init = McpServer::handle(api, {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), 1},
                                                         {QStringLiteral("method"), QStringLiteral("initialize")},
                                                         {QStringLiteral("params"), QJsonObject{{QStringLiteral("protocolVersion"), QStringLiteral("2025-03-26")}}}},
                                                   version);
        QCOMPARE(init.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("2025-03-26"));
        QVERIFY(McpServer::handle(api, {{QStringLiteral("method"), QStringLiteral("notifications/initialized")}}, version).isEmpty());
        const QJsonObject tools = McpServer::handle(api, {{QStringLiteral("id"), 2}, {QStringLiteral("method"), QStringLiteral("tools/list")}}, version);
        QCOMPARE(tools.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray().size(), int(NotesApi::tools().size()));
        const QJsonObject call = McpServer::handle(api, {{QStringLiteral("id"), 3}, {QStringLiteral("method"), QStringLiteral("tools/call")},
                                                         {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("read_note")},
                                                                                                {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("note"), QStringLiteral("hello there")}}}}}},
                                                   version);
        QVERIFY(!call.value(QStringLiteral("result")).toObject().value(QStringLiteral("isError")).toBool());
        QCOMPARE(call.value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("path")).toString(), QStringLiteral("Hello-there.md"));
        const QJsonObject missing = McpServer::handle(api, {{QStringLiteral("id"), 4}, {QStringLiteral("method"), QStringLiteral("tools/call")},
                                                            {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("read_note")},
                                                                                                   {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("note"), QStringLiteral("gone")}}}}}},
                                                      version);
        QVERIFY(missing.value(QStringLiteral("result")).toObject().value(QStringLiteral("isError")).toBool());
        QCOMPARE(McpServer::handle(api, {{QStringLiteral("id"), 5}, {QStringLiteral("method"), QStringLiteral("nope")}}, version)
                     .value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32601);
    }

    void choosesAndReadsTheAi() {
        QSettings().remove(QStringLiteral("ai/provider"));
        qputenv("OMAZETTEL_AI", "");
        Assistant::Choice choice = Assistant::choose(QStringLiteral("claude-code:sonnet"));
        QCOMPARE(choice.provider, QStringLiteral("claude"));
        QCOMPARE(choice.model, QStringLiteral("sonnet"));
        QCOMPARE(choice.origin, QStringLiteral("--ai"));
        choice = Assistant::choose(QStringLiteral("ollama:llama3.2:8b"));
        QCOMPARE(choice.provider, QStringLiteral("ollama"));
        QCOMPARE(choice.model, QStringLiteral("llama3.2:8b"));

        Assistant::save(QStringLiteral("codex"));
        QCOMPARE(Assistant::choose(QString()).provider, QStringLiteral("codex"));
        qputenv("OMAZETTEL_AI", "gemini");
        QCOMPARE(Assistant::choose(QString()).provider, QStringLiteral("gemini"));
        qputenv("OMAZETTEL_AI", "");
        Assistant::save(QString());

        QVERIFY(Assistant::problem({QStringLiteral("grok"), QString(), QStringLiteral("--ai")}).contains(QStringLiteral("cannot write notes")));
        QVERIFY(Assistant::problem({QStringLiteral("openai"), QString(), QStringLiteral("--ai")}).contains(QStringLiteral("openai:<model>")));

        QString error;
        QCOMPARE(Assistant::extractJson(QStringLiteral("Sure!\n```json\n{\"a\": [1]}\n```"), &error)
                     .value(QStringLiteral("a")).toArray().size(), 1);
        QVERIFY(error.isEmpty());
        QVERIFY(Assistant::extractJson(QStringLiteral("no idea"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void namesFilesWithTheSeparator() {
        QCOMPARE(Vault::fileNameSeparator(), QStringLiteral("-"));
        QCOMPARE(Vault::noteFileName(QStringLiteral("Plan  - draft one")), QStringLiteral("Plan-draft-one"));
        QCOMPARE(Vault::matchKey(QStringLiteral("2026-10-22_Cutover-Plan")), QStringLiteral("2026 10 22 cutover plan"));
        Vault vault;
        vault.setFileNameSeparator(QStringLiteral("_"));
        QCOMPARE(Vault::noteFileName(QStringLiteral("Cutover plan")), QStringLiteral("Cutover_plan"));
        vault.setFileNameSeparator(QStringLiteral(" "));
        QCOMPARE(Vault::noteFileName(QStringLiteral("Cutover plan")), QStringLiteral("Cutover plan"));
        vault.setFileNameSeparator(QStringLiteral("-"));

        // Links and note names find a note whatever separates its words.
        QTemporaryDir root;
        writeFile(root.filePath(QStringLiteral("Cutover-plan.md")), "# Cutover plan\n");
        writeFile(root.filePath(QStringLiteral("Old note.md")), "See [[Cutover plan]] and [[old_note]].\n");
        NotesApi api;
        QString error;
        QVERIFY(api.open(root.path(), &error));
        const QJsonObject read = api.call(*NotesApi::toolNamed(QStringLiteral("read_note")),
                                          {{QStringLiteral("note"), QStringLiteral("cutover plan")}});
        QVERIFY(read.value(QStringLiteral("path")).toString().endsWith(QStringLiteral("Cutover-plan.md")));
        const QJsonObject report = api.call(*NotesApi::toolNamed(QStringLiteral("link_report")), {});
        QVERIFY(report.value(QStringLiteral("broken")).toArray().isEmpty());
        QCOMPARE(api.call(*NotesApi::toolNamed(QStringLiteral("read_note")),
                          {{QStringLiteral("note"), QStringLiteral("old-note")}}).value(QStringLiteral("path")).toString(),
                 QStringLiteral("Old note.md"));

        // tidy_names: a preview first, then renames with links rewritten.
        writeFile(root.filePath(QStringLiteral("Meetings/2026-10-22 Vendor kickoff.md")), "# Vendor kickoff\nWith [[Old note]].\n");
        writeFile(root.filePath(QStringLiteral("Taken one.md")), "# A\n");
        writeFile(root.filePath(QStringLiteral("Taken-one.md")), "# B\n");
        QVERIFY(api.open(root.path(), &error));
        const auto tidy = *NotesApi::toolNamed(QStringLiteral("tidy_names"));
        QJsonObject plan = api.call(tidy, {});
        QCOMPARE(plan.value(QStringLiteral("renamed")).toArray().size(), 2);
        QCOMPARE(plan.value(QStringLiteral("skipped")).toArray().size(), 1);
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("Old note.md"))));
        plan = api.call(tidy, {{QStringLiteral("folder"), QStringLiteral("Meetings")}});
        QCOMPARE(plan.value(QStringLiteral("renamed")).toArray().size(), 1);
        const QJsonObject done = api.call(tidy, {{QStringLiteral("apply"), true}});
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("Old-note.md"))));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("Meetings/2026-10-22-Vendor-kickoff.md"))));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("Taken one.md"))));
        QVERIFY(readFile(root.filePath(QStringLiteral("Meetings/2026-10-22-Vendor-kickoff.md"))).contains(QStringLiteral("[[Old-note]]")));
        QVERIFY(done.value(QStringLiteral("linksUpdated")).toInt() >= 1);
        QCOMPARE(api.call(tidy, {}).value(QStringLiteral("renamed")).toArray().size(), 0);
    }

    void startsANewFolderWithTheLayout() {
        // An empty notes folder gets the numbered layout, and the built-in
        // templates file their notes inside it.
        QTemporaryDir root;
        NotesApi api;
        QString error;
        QVERIFY(api.open(root.path(), &error));
        QVERIFY(Layout::present(root.path()));
        for (const QString &folder : Layout::folders())
            QVERIFY2(QFileInfo(root.filePath(folder)).isDir(), qPrintable(folder));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("01-system/SYSTEM-RULES.md"))));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("02-context/.keep"))));
        const QJsonObject daily = api.call(*NotesApi::toolNamed(QStringLiteral("daily_note")), {});
        QVERIFY(daily.value(QStringLiteral("path")).toString().startsWith(QStringLiteral("03-projects/daily/")));
        const QJsonObject meeting = api.call(*NotesApi::toolNamed(QStringLiteral("create_note")),
                                             {{QStringLiteral("title"), QStringLiteral("Kickoff")},
                                              {QStringLiteral("template"), QStringLiteral("Meeting")}});
        QVERIFY(meeting.value(QStringLiteral("path")).toString().startsWith(QStringLiteral("03-projects/meetings/")));

        // Agents get the rules with info; the rules files link nothing.
        const QJsonObject layout = api.call(*NotesApi::toolNamed(QStringLiteral("vault_info")), {})
                                       .value(QStringLiteral("layout")).toObject();
        QVERIFY(layout.value(QStringLiteral("numbered")).toBool());
        QVERIFY(layout.value(QStringLiteral("rules")).toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("## Naming")));
        QVERIFY(layout.value(QStringLiteral("howIWork")).toObject().contains(QStringLiteral("text")));
        QVERIFY(api.call(*NotesApi::toolNamed(QStringLiteral("link_report")), {})
                    .value(QStringLiteral("broken")).toArray().isEmpty());

        // A folder that already has notes keeps its own folders.
        QTemporaryDir used;
        writeFile(used.filePath(QStringLiteral("Idea.md")), "# Idea\n");
        NotesApi other;
        QVERIFY(other.open(used.path(), &error));
        QVERIFY(!Layout::present(used.path()));
        QVERIFY(other.call(*NotesApi::toolNamed(QStringLiteral("daily_note")), {})
                    .value(QStringLiteral("path")).toString().startsWith(QStringLiteral("Daily/")));
    }

    void asksHowANewFolderStarts() {
        // Traditional: an empty folder gets only the templates' own folders.
        Layout::setChoice(QStringLiteral("traditional"));
        QTemporaryDir plain;
        NotesApi api;
        QString error;
        QVERIFY(api.open(plain.path(), &error));
        QVERIFY(!Layout::present(plain.path()));
        QVERIFY(api.call(*NotesApi::toolNamed(QStringLiteral("daily_note")), {})
                    .value(QStringLiteral("path")).toString().startsWith(QStringLiteral("Daily/")));
        QVERIFY(!api.call(*NotesApi::toolNamed(QStringLiteral("vault_info")), {})
                     .value(QStringLiteral("layout")).toObject().value(QStringLiteral("numbered")).toBool());

        // The first empty folder opened in the app asks, and the answer sticks.
        QSettings().remove(QStringLiteral("notes/layout"));
        QTemporaryDir dir;
        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        QVERIFY(vault.isNewFolder());
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);
        auto *dialog = window->findChild<QObject *>(QStringLiteral("layoutChoiceDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QMetaObject::invokeMethod(dialog, "pick", Q_ARG(QVariant, QStringLiteral("numbered")));
        QCOMPARE(vault.folderLayout(), QStringLiteral("numbered"));
        QVERIFY(Layout::present(dir.path()));
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral(".omazettel/note-templates/Daily.md"))));
        QVERIFY(!vault.isNewFolder());

        // Once chosen, another empty folder follows the choice without asking.
        QTemporaryDir second;
        QVERIFY(vault.openFolder(second.path()));
        QTest::qWait(50);
        QVERIFY(!dialog->property("opened").toBool());

        // A folder with notes never asks.
        QSettings().remove(QStringLiteral("notes/layout"));
        QTemporaryDir used;
        writeFile(used.filePath(QStringLiteral("Idea.md")), "# Idea\n");
        QVERIFY(vault.openFolder(used.path()));
        QVERIFY(!vault.isNewFolder());
        QTest::qWait(50);
        QVERIFY(!dialog->property("opened").toBool());
    }

    void keepsMapsOfContent() {
        QTemporaryDir root;
        Layout::create(root.path());
        writeFile(root.filePath(QStringLiteral("03-projects/alpha/plan.md")), "# Plan\n");
        writeFile(root.filePath(QStringLiteral("03-projects/alpha/research/source-a.md")), "# A\n");
        writeFile(root.filePath(QStringLiteral("03-projects/alpha/2026-10-27-kickoff.md")), "# Kickoff\n");
        writeFile(root.filePath(QStringLiteral("03-projects/alpha/transcripts/2026-10-27-kickoff-transcript.md")), "# T\n");
        writeFile(root.filePath(QStringLiteral("03-projects/alpha/old-call-transcript.md")), "# T\n");
        writeFile(root.filePath(QStringLiteral("03-projects/beta/plan.md")), "# Beta plan\n");
        writeFile(root.filePath(QStringLiteral("03-projects/daily/2026-10-27.md")), "# Day\n");
        writeFile(root.filePath(QStringLiteral("03-projects/empty/.keep")), "");

        QVERIFY(Moc::isTranscript(QStringLiteral("a/transcripts/x.md")));
        QVERIFY(Moc::isTranscript(QStringLiteral("a/x_Transcript.md")));
        QVERIFY(!Moc::isTranscript(QStringLiteral("a/transcription-tools.md")));

        const QStringList written = Moc::update(root.path());
        QCOMPARE(written, (QStringList{QStringLiteral("03-projects/alpha/alpha-moc.md"),
                                       QStringLiteral("03-projects/beta/beta-moc.md"),
                                       QStringLiteral("03-projects/projects-moc.md")}));
        const QString alpha = readFile(root.filePath(QStringLiteral("03-projects/alpha/alpha-moc.md")));
        QVERIFY(alpha.startsWith(QStringLiteral("---\ntitle: \"alpha: map of content\"\ntags: [moc]\n---\n")));
        QVERIFY(alpha.contains(QStringLiteral("Up: [[projects-moc]]")));
        // "plan" is in two projects, so it is linked by its path.
        QVERIFY(alpha.contains(QStringLiteral("- [[2026-10-27-kickoff]]\n- [[03-projects/alpha/plan]]\n\n### research\n\n- [[source-a]]\n")));
        QVERIFY(!alpha.contains(QStringLiteral("transcript")));
        const QString top = readFile(root.filePath(QStringLiteral("03-projects/projects-moc.md")));
        QVERIFY(top.contains(QStringLiteral("- [[alpha-moc]]\n- [[beta-moc]]\n<!-- /omazettel-map -->")));
        QVERIFY(!top.contains(QStringLiteral("daily")) && !top.contains(QStringLiteral("empty")));

        // Again: nothing to write. Words around the list stay.
        QVERIFY(Moc::update(root.path()).isEmpty());
        const QString mapPath = root.filePath(QStringLiteral("03-projects/alpha/alpha-moc.md"));
        writeFile(mapPath, ("Start here.\n\n" + alpha + "\nMy notes.\n").toUtf8());
        writeFile(root.filePath(QStringLiteral("03-projects/alpha/zeta.md")), "# Zeta\n");
        QCOMPARE(Moc::update(root.path()), QStringList{QStringLiteral("03-projects/alpha/alpha-moc.md")});
        const QString edited = readFile(mapPath);
        QVERIFY(edited.startsWith(QStringLiteral("Start here.\n")) && edited.endsWith(QStringLiteral("\nMy notes.\n")));
        QVERIFY(edited.contains(QStringLiteral("- [[zeta]]\n")));
        // A note of that name without the markers is someone's own: left alone.
        writeFile(root.filePath(QStringLiteral("03-projects/beta/beta-moc.md")), "# Mine\n");
        writeFile(root.filePath(QStringLiteral("03-projects/beta/more.md")), "# More\n");
        Moc::update(root.path());
        QCOMPARE(readFile(root.filePath(QStringLiteral("03-projects/beta/beta-moc.md"))), QStringLiteral("# Mine\n"));

        // The tools keep them current; a preview writes nothing.
        NotesApi api;
        QString error;
        QVERIFY(api.open(root.path(), &error));
        const QJsonObject made = api.call(*NotesApi::toolNamed(QStringLiteral("create_note")),
                                          {{QStringLiteral("title"), QStringLiteral("Risks")},
                                           {QStringLiteral("folder"), QStringLiteral("03-projects/alpha")}});
        QVERIFY(made.value(QStringLiteral("mapsUpdated")).toArray().contains(QStringLiteral("03-projects/alpha/alpha-moc.md")));
        QVERIFY(readFile(mapPath).contains(QStringLiteral("- [[Risks]]\n")));
        QFile::remove(root.filePath(QStringLiteral("03-projects/alpha/zeta.md")));
        api.call(*NotesApi::toolNamed(QStringLiteral("setup_layout")), {});
        QVERIFY(readFile(mapPath).contains(QStringLiteral("[[zeta]]")));
        const QJsonObject plan = api.call(*NotesApi::toolNamed(QStringLiteral("update_maps")), {});
        QCOMPARE(plan.value(QStringLiteral("maps")).toArray().size(), 1);
        QVERIFY(readFile(mapPath).contains(QStringLiteral("[[zeta]]")));
        api.call(*NotesApi::toolNamed(QStringLiteral("update_maps")), {{QStringLiteral("apply"), true}});
        QVERIFY(!readFile(mapPath).contains(QStringLiteral("[[zeta]]")));
        // setup_layout files a loose transcript in its transcripts folder.
        api.call(*NotesApi::toolNamed(QStringLiteral("setup_layout")), {{QStringLiteral("apply"), true}});
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("03-projects/alpha/transcripts/old-call-transcript.md"))));
        QVERIFY(api.call(*NotesApi::toolNamed(QStringLiteral("link_report")), {})
                    .value(QStringLiteral("broken")).toArray().isEmpty());

        // The app's notes folder follows changes on its own, unless turned off.
        Vault vault;
        QVERIFY(vault.openFolder(root.path()));
        writeFile(root.filePath(QStringLiteral("03-projects/gamma/idea.md")), "# Idea\n");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath(QStringLiteral("03-projects/gamma/gamma-moc.md"))), 5000);
        QTRY_VERIFY(readFile(root.filePath(QStringLiteral("03-projects/projects-moc.md"))).contains(QStringLiteral("[[gamma-moc]]")));
        vault.setMapsOfContent(false);
        writeFile(root.filePath(QStringLiteral("03-projects/delta/idea2.md")), "# Idea\n");
        QTest::qWait(1500);
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("03-projects/delta/delta-moc.md"))));
    }

    void setsUpTheLayoutInAnOlderFolder() {
        QTemporaryDir root;
        writeFile(root.filePath(QStringLiteral("Idea.md")), "# Idea\nFrom [[2026-01-05]] and [[Meetings/Kickoff]].\n");
        writeFile(root.filePath(QStringLiteral("Daily/2026-01-05.md")), "# Monday\n");
        writeFile(root.filePath(QStringLiteral("Meetings/Kickoff.md")), "# Kickoff\nSee [[Idea]].\n");
        NotesApi api;
        QString error;
        QVERIFY(api.open(root.path(), &error));
        const auto setup = *NotesApi::toolNamed(QStringLiteral("setup_layout"));

        const QJsonObject plan = api.call(setup, {});
        QCOMPARE(plan.value(QStringLiteral("moved")).toArray().size(), 2);
        QVERIFY(plan.value(QStringLiteral("templates")).toArray().contains(QStringLiteral("Meeting")));
        QVERIFY(plan.value(QStringLiteral("leftInPlace")).toArray().contains(QStringLiteral("Idea.md")));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("Daily/2026-01-05.md"))));
        QVERIFY(!Layout::present(root.path()));

        const QJsonObject done = api.call(setup, {{QStringLiteral("apply"), true}});
        QVERIFY(done.value(QStringLiteral("applied")).toBool());
        QVERIFY(Layout::present(root.path()));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("03-projects/daily/2026-01-05.md"))));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("03-projects/meetings/Kickoff.md"))));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("Daily"))));
        QVERIFY(readFile(root.filePath(QStringLiteral("Idea.md"))).contains(QStringLiteral("[[03-projects/meetings/Kickoff]]")));
        QVERIFY(readFile(root.filePath(QStringLiteral(".omazettel/note-templates/Meeting.md")))
                    .contains(QStringLiteral("folder=\"03-projects/meetings\"")));
        QVERIFY(api.call(*NotesApi::toolNamed(QStringLiteral("link_report")), {})
                    .value(QStringLiteral("broken")).toArray().isEmpty());
        QVERIFY(api.call(*NotesApi::toolNamed(QStringLiteral("daily_note")), {{QStringLiteral("date"), QStringLiteral("2026-01-06")}})
                    .value(QStringLiteral("path")).toString().startsWith(QStringLiteral("03-projects/daily/")));

        // Run again: nothing left to do. An edited rules file is kept.
        writeFile(root.filePath(QStringLiteral("01-system/SYSTEM-RULES.md")), "# Mine\n");
        const QJsonObject again = api.call(setup, {{QStringLiteral("apply"), true}});
        QVERIFY(again.value(QStringLiteral("moved")).toArray().isEmpty());
        QVERIFY(again.value(QStringLiteral("made")).toArray().isEmpty());
        QCOMPARE(readFile(root.filePath(QStringLiteral("01-system/SYSTEM-RULES.md"))), QStringLiteral("# Mine\n"));
    }

    void showsPdfsInTheMiddle() {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("plan.md")), "# Plan\n");
        // A two-page PDF, as an export would make.
        const QString pdf = dir.filePath(QStringLiteral("plan.pdf"));
        {
            QPdfWriter writer(pdf);
            writer.setPageSize(QPageSize(QPageSize::A4));
            QPainter painter(&writer);
            painter.setFont(QFont(QStringLiteral("Sans"), 28));
            painter.drawText(QPointF(400, 1200), QStringLiteral("Page one of the plan"));
            writer.newPage();
            painter.drawText(QPointF(400, 1200), QStringLiteral("Page two"));
        }

        // Pages are drawn at the width asked for.
        const QImage page = PdfDocument::render(pdf, 0, 600);
        QCOMPARE(page.width(), 600);
        QVERIFY(qAbs(page.height() - 849) <= 2); // A4 is 1:1.414.
        QVERIFY(PdfDocument::render(pdf, 5, 600).isNull());
        QVERIFY(PdfDocument::render(dir.filePath(QStringLiteral("plan.md")), 0, 600).isNull());

        Backend backend;
        Vault vault;
        QVERIFY(vault.openFolder(dir.path()));
        // The sidebar lists it beside the note; renaming keeps it a PDF.
        QVERIFY(vault.indexForPath(pdf).isValid());
        QQmlEngine engine;
        QScopedPointer<QObject> window(loadMain(engine, backend, vault));
        QVERIFY(window);
        QMetaObject::invokeMethod(window.get(), "openNote", Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("plan.md")))));
        QTRY_COMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("plan.md"))));
        QMetaObject::invokeMethod(window.get(), "openNote", Q_ARG(QVariant, QUrl::fromLocalFile(pdf)));
        auto *view = window->findChild<QObject *>(QStringLiteral("pdfView"));
        QVERIFY(view);
        QTRY_VERIFY(view->property("visible").toBool());
        auto *document = window->findChild<QObject *>(QStringLiteral("pdfDocument"));
        QTRY_COMPARE(document->property("pageCount").toInt(), 2);
        QVERIFY(document->property("error").toString().isEmpty());
        // The note stays open behind it.
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("plan.md"))));
        if (qEnvironmentVariableIsSet("OMAZETTEL_PDF_SHOT")) {
            auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
            quickWindow->resize(1100, 800);
            quickWindow->show();
            QTest::qWait(1500);
            quickWindow->grabWindow().save(qEnvironmentVariable("OMAZETTEL_PDF_SHOT"));
        }
        // Clicking the open note again goes back to it.
        QMetaObject::invokeMethod(window.get(), "openNote", Q_ARG(QVariant, QUrl::fromLocalFile(dir.filePath(QStringLiteral("plan.md")))));
        QTRY_VERIFY(!view->property("visible").toBool());

        // A new export replaces the file; the view reads it again.
        QMetaObject::invokeMethod(window.get(), "openNote", Q_ARG(QVariant, QUrl::fromLocalFile(pdf)));
        QTRY_VERIFY(view->property("visible").toBool());
        const int revision = document->property("revision").toInt();
        {
            QPdfWriter writer(pdf + QStringLiteral(".new"));
            QPainter painter(&writer);
            painter.drawText(QPointF(400, 1200), QStringLiteral("Only one page now"));
        }
        QFile::remove(pdf);
        QFile::rename(pdf + QStringLiteral(".new"), pdf);
        QTRY_VERIFY_WITH_TIMEOUT(document->property("revision").toInt() > revision
                                     && document->property("pageCount").toInt() == 1, 5000);

        QCOMPARE(QFileInfo(vault.renamePath(pdf, QStringLiteral("final plan"))).fileName(), QStringLiteral("final-plan.pdf"));
    }

    void writesMeetingNotesWithAnAgent() {
        // A stand-in "claude" that answers like the real one.
        QTemporaryDir bin;
        QVERIFY(bin.isValid());
        const QString fake = bin.filePath(QStringLiteral("claude"));
        writeFile(fake,
                  "#!/bin/sh\ncat > \"$PWD/prompt.txt\"\ncp \"$PWD/prompt.txt\" \"" + bin.filePath(QStringLiteral("seen.txt")).toUtf8() + "\"\n"
                  "printf '%s' '{\"title\":\"Cutover plan\",\"summary\":\"Friday cutover agreed.\",\"attendees\":[\"Ann\",\"Bo\"],"
                  "\"tags\":[\"atlas\"],\"sections\":{\"Agenda\":[\"Cutover\"],\"Notes\":\"Bo finishes [[Atlas]] models.\","
                  "\"Decisions\":\"\",\"Action items\":\"- [ ] Bo: finish models (Thu)\"}}'\n");
        QFile(fake).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", bin.path().toUtf8() + ':' + oldPath);

        QTemporaryDir root;
        writeFile(root.filePath(QStringLiteral("Atlas.md")), "# Atlas\n");
        writeFile(root.filePath(QStringLiteral("call.vtt")),
                  "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\n<v Ann>Cutover Friday?</v>\n\n"
                  "00:00:02.000 --> 00:00:03.000\n<v Bo>Yes, models by Thursday.</v>\n");
        NotesApi api;
        QString error;
        QVERIFY(api.open(root.path(), &error));
        const QJsonObject made = api.call(*NotesApi::toolNamed(QStringLiteral("transcribe")),
                                          {{QStringLiteral("file"), root.filePath(QStringLiteral("call.vtt"))},
                                           {QStringLiteral("note"), QString()},
                                           {QStringLiteral("ai"), QStringLiteral("claude")}});
        qputenv("PATH", oldPath);

        QCOMPARE(made.value(QStringLiteral("writtenBy")).toString(), QStringLiteral("Claude Code"));
        QCOMPARE(made.value(QStringLiteral("actionItems")).toArray().first().toString(), QStringLiteral("Bo: finish models (Thu)"));
        const QString path = made.value(QStringLiteral("path")).toString();
        QVERIFY(path.startsWith(QStringLiteral("Meetings/")) && path.endsWith(QStringLiteral("-Cutover-plan.md")));
        const QString text = readFile(root.filePath(path));
        QVERIFY(text.contains(QStringLiteral("attendees: [\"Ann\", \"Bo\"]")));
        QVERIFY(text.contains(QStringLiteral("abstract: \"Friday cutover agreed.\"")));
        QVERIFY(text.contains(QStringLiteral("**Attendees:** Ann, Bo\n")));
        QVERIFY(text.contains(QStringLiteral("## Agenda\n\n- Cutover\n")));
        QVERIFY(text.contains(QStringLiteral("## Decisions\n\n## Action items\n\n- [ ] Bo: finish models (Thu)\n")));
        // The transcript is a note of its own beside it, each linking the other.
        const QString base = QFileInfo(path).completeBaseName();
        QVERIFY(!text.contains(QStringLiteral("Ann: Cutover Friday?")));
        QVERIFY(text.contains(QStringLiteral("## Transcript\n\n[[%1-transcript]]\n").arg(base)));
        const QString transcriptPath = made.value(QStringLiteral("transcriptNote")).toString();
        QCOMPARE(transcriptPath, QStringLiteral("Meetings/transcripts/%1-transcript.md").arg(base));
        const QString transcript = readFile(root.filePath(transcriptPath));
        QVERIFY(transcript.contains(QStringLiteral("Transcript of [[%1]], from call.vtt.").arg(base)));
        QVERIFY(transcript.contains(QStringLiteral("Ann: Cutover Friday?\n\nBo: Yes, models by Thursday.\n")));
        QVERIFY(transcript.contains(QStringLiteral("transcript")));
        // The AI was told the headings and the notes it may link.
        const QString prompt = readFile(bin.filePath(QStringLiteral("seen.txt")));
        QVERIFY(prompt.contains(QStringLiteral("\"Action items\":")));
        QVERIFY(prompt.contains(QStringLiteral("only these): Atlas.")));
        QVERIFY(prompt.contains(QStringLiteral("Ann: Cutover Friday?")));

        // No AI: the transcript alone, named after the file.
        const QJsonObject plain = api.call(*NotesApi::toolNamed(QStringLiteral("transcribe")),
                                           {{QStringLiteral("file"), root.filePath(QStringLiteral("call.vtt"))},
                                            {QStringLiteral("note"), QString()},
                                            {QStringLiteral("ai"), QStringLiteral("none")}});
        QVERIFY(plain.value(QStringLiteral("path")).toString().endsWith(QStringLiteral("-call.md")));

        // A missing AI is reported before anything is transcribed.
        bool refused = false;
        try {
            api.call(*NotesApi::toolNamed(QStringLiteral("transcribe")),
                     {{QStringLiteral("file"), QStringLiteral("/nonexistent.mp3")}, {QStringLiteral("note"), QString()},
                      {QStringLiteral("ai"), QStringLiteral("openai")}});
        } catch (const NotesApi::Error &failure) {
            refused = failure.message.contains(QStringLiteral("openai:<model>"));
        }
        QVERIFY(refused);
    }

    void explainsSyncFailures() {
        QCOMPARE(Sync::hostOf(QStringLiteral("git@codeberg.org:me/notes.git")), QStringLiteral("codeberg.org"));
        QCOMPARE(Sync::hostOf(QStringLiteral("https://me:secret@git.example.com:3000/me/notes.git")), QStringLiteral("git.example.com"));
        QCOMPARE(Sync::hostOf(QStringLiteral("ssh://git@gitlab.example.org:2222/me/notes.git")), QStringLiteral("gitlab.example.org"));
        QCOMPARE(Sync::hostKindOf(QStringLiteral("https://github.com/me/notes.git")), QStringLiteral("github"));
        QCOMPARE(Sync::hostKindOf(QStringLiteral("git@codeberg.org:me/notes.git")), QStringLiteral("forgejo"));
        QCOMPARE(Sync::hostKindOf(QStringLiteral("https://gitlab.com/me/notes.git")), QStringLiteral("gitlab"));
        QCOMPARE(Sync::hostKindOf(QStringLiteral("https://git.example.com/me/notes.git")), QStringLiteral("other"));

        const QString github = QStringLiteral("https://github.com/me/notes.git");
        const auto kind = [](const QString &err, const QString &url) { return Sync::explain(err, url, QStringLiteral("push")).kind; };
        QCOMPARE(kind(QStringLiteral("remote: error: GH007: Your push would publish a private email address.\n"
                                     "error: failed to push some refs to 'https://github.com/me/notes.git'"), github),
                 QStringLiteral("private-email"));
        QCOMPARE(kind(QStringLiteral("git@gitlab.com: Permission denied (publickey)."), QStringLiteral("git@gitlab.com:me/n.git")),
                 QStringLiteral("ssh-key"));
        QVERIFY(Sync::explain(QStringLiteral("git@gitlab.com: Permission denied (publickey)."), QStringLiteral("git@gitlab.com:me/n.git"),
                              QStringLiteral("fetch")).message.contains(QStringLiteral("https://gitlab.com/-/user_settings/ssh_keys")));
        QCOMPARE(kind(QStringLiteral("fatal: could not read Username for 'https://git.example.com': terminal prompts disabled"),
                      QStringLiteral("https://git.example.com/me/n.git")), QStringLiteral("auth"));
        QCOMPARE(kind(QStringLiteral("remote: Permission to me/notes.git denied to other.\nfatal: unable to access: The requested URL returned error: 403"), github),
                 QStringLiteral("forbidden"));
        QCOMPARE(kind(QStringLiteral(" ! [rejected]        main -> main (fetch first)\nerror: failed to push some refs"), github), QStringLiteral("behind"));
        QCOMPARE(kind(QStringLiteral("fatal: unable to access: SSL certificate problem: self-signed certificate"),
                      QStringLiteral("https://git.lan/me/n.git")), QStringLiteral("certificate"));
        QCOMPARE(kind(QStringLiteral("fatal: unable to access: Could not resolve host: github.com"), github), QStringLiteral("network"));
        // Something unknown: the server's own words, not "failed to push some refs".
        const Sync::Explained odd = Sync::explain(QStringLiteral("remote: Quota exceeded for this namespace\nerror: failed to push some refs to 'x'"),
                                                  github, QStringLiteral("push"));
        QCOMPARE(odd.message, QStringLiteral("git push failed: Quota exceeded for this namespace"));
        QVERIFY(Sync::explain(QString(), github, QStringLiteral("push"), 128).message.contains(QStringLiteral("exit 128")));
    }

    void reportsWhyAPushWasRefused() {
        // A local "server" that refuses pushes the way GitHub does.
        QTemporaryDir server;
        QTemporaryDir notes;
        const auto git = [](const QString &dir, const QStringList &args) {
            QProcess process;
            process.setWorkingDirectory(dir);
            process.start(QStringLiteral("git"), args);
            process.waitForFinished(30000);
            return process.exitCode() == 0;
        };
        QVERIFY(git(server.path(), {QStringLiteral("init"), QStringLiteral("--bare"), QStringLiteral("-b"), QStringLiteral("main")}));
        writeFile(server.filePath(QStringLiteral("hooks/pre-receive")),
                  "#!/bin/sh\necho 'error: GH007: Your push would publish a private email address.' >&2\nexit 1\n");
        QFile(server.filePath(QStringLiteral("hooks/pre-receive"))).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QVERIFY(git(notes.path(), {QStringLiteral("init"), QStringLiteral("-b"), QStringLiteral("main")}));
        git(notes.path(), {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("T")});
        git(notes.path(), {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@example.com")});
        QVERIFY(git(notes.path(), {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), server.path()}));
        writeFile(notes.filePath(QStringLiteral("Note.md")), "# Note\n");

        const Sync::Result result = Sync::runSync(notes.path(), QStringLiteral("main"), true, QStringLiteral("T"));
        QVERIFY(!result.ok);
        QCOMPARE(result.kind, QStringLiteral("private-email"));
        QVERIFY(result.details.contains(QStringLiteral("GH007")));
        QVERIFY(result.message.contains(QStringLiteral("no-reply")));

        // Without the hook, the connection test says reading and writing work.
        QFile::remove(server.filePath(QStringLiteral("hooks/pre-receive")));
        const Sync::Result test = Sync::testRemoteBlocking(notes.path(), QStringLiteral("main"));
        QVERIFY2(test.ok, qPrintable(test.message));
        QVERIFY(Sync::runSync(notes.path(), QStringLiteral("main"), true, QStringLiteral("T")).ok);
    }

private:
    // Starts a rename from the sidebar and answers the name dialog.
    void renameThroughDialog(QObject *window, const QString &path, const QString &newName) {
        QMetaObject::invokeMethod(window, "renameItem", Q_ARG(QVariant, path));
        auto *field = window->findChild<QObject *>(QStringLiteral("nameField"));
        auto *dialog = window->findChild<QObject *>(QStringLiteral("nameDialog"));
        QVERIFY(field && dialog);
        field->setProperty("text", newName);
        QMetaObject::invokeMethod(dialog, "finish");
    }

    // The tree row whose label reads text.
    static QQuickItem *rowLabelled(QQuickWindow *window, const QString &text) {
        auto *tree = window->findChild<QQuickItem *>(QStringLiteral("notesTree"));
        return tree ? findText(tree, text) : nullptr;
    }

    // Views do not make their delegates QObject children, so walk the
    // visual item tree.
    static QQuickItem *findText(QQuickItem *item, const QString &text) {
        for (QQuickItem *child : item->childItems()) {
            if (child->inherits("QQuickText") && child->isVisible()
                    && child->property("text").toString() == text)
                return child->parentItem();
            if (QQuickItem *found = findText(child, text))
                return found;
        }
        return nullptr;
    }

    QObject *makeEditor(Backend &backend) {
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nTextEdit { property var doc: textDocument }", QUrl());
        QObject *editor = component.create();
        if (editor)
            backend.attachDocument(editor->property("doc").value<QObject *>());
        return editor;
    }

    QObject *loadMain(QQmlEngine &engine, Backend &backend, Vault &vault) {
        const QString mainQmlPath = QFINDTESTDATA("../../src/Main.qml");
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("vault"), &vault);
        m_windowIndex = std::make_unique<NoteIndex>();
        m_windowIndex->setRoot(vault.rootPath());
        m_windowIndex->rescanNow();
        engine.rootContext()->setContextProperty(QStringLiteral("noteIndex"), m_windowIndex.get());
        m_windowTemplates = std::make_unique<NoteTemplates>();
        m_windowTemplates->setRoot(vault.rootPath());
        engine.rootContext()->setContextProperty(QStringLiteral("noteTemplates"), m_windowTemplates.get());
        m_windowExporter = std::make_unique<Exporter>();
        m_windowExporter->setRoot(vault.rootPath());
        engine.rootContext()->setContextProperty(QStringLiteral("exporter"), m_windowExporter.get());
        m_windowCitations = std::make_unique<Citations>();
        m_windowCitations->setRoot(vault.rootPath());
        engine.rootContext()->setContextProperty(QStringLiteral("citations"), m_windowCitations.get());
        m_windowSync = std::make_unique<Sync>();
        m_windowSync->setRoot(vault.rootPath());
        engine.rootContext()->setContextProperty(QStringLiteral("sync"), m_windowSync.get());
        PdfDocument::install(&engine);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        if (!component.isReady()) {
            qWarning() << component.errorString();
            return nullptr;
        }
        QObject *window = component.create();
        if (!window)
            qWarning() << component.errorString();
        return window;
    }

    QTemporaryDir m_settingsDirectory;
    QQmlEngine m_engine;
    std::unique_ptr<NoteIndex> m_windowIndex;
    std::unique_ptr<NoteTemplates> m_windowTemplates;
    std::unique_ptr<Exporter> m_windowExporter;
    std::unique_ptr<Citations> m_windowCitations;
    std::unique_ptr<Sync> m_windowSync;
};

QTEST_MAIN(NotesTest)
#include "tst_notes.moc"

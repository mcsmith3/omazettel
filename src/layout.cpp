#include "layout.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>

namespace Layout {

QStringList folders() {
    return {QString::fromLatin1(systemFolder), QString::fromLatin1(contextFolder),
            QString::fromLatin1(projectsFolder), QString::fromLatin1(outputsFolder),
            QString::fromLatin1(archiveFolder)};
}

namespace {
const QString choiceSetting = QStringLiteral("notes/layout");
}

QString choice() {
    const QString value = QSettings().value(choiceSetting).toString();
    return value == QLatin1String("numbered") || value == QLatin1String("traditional") ? value : QString();
}

void setChoice(const QString &value) {
    if (value == QLatin1String("numbered") || value == QLatin1String("traditional"))
        QSettings().setValue(choiceSetting, value);
}

bool wanted() {
    return choice() != QLatin1String("traditional");
}

bool present(const QString &root) {
    if (root.isEmpty())
        return false;
    const QDir dir(root);
    return QFileInfo(dir.filePath(QString::fromLatin1(systemFolder))).isDir()
           && QFileInfo(dir.filePath(QString::fromLatin1(projectsFolder))).isDir();
}

bool isEmpty(const QString &root) {
    if (root.isEmpty() || !QFileInfo(root).isDir())
        return false;
    // Hidden entries (.git, .omazettel) are not notes.
    return QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
}

QStringList create(const QString &root) {
    QStringList made;
    if (root.isEmpty())
        return made;
    const QDir dir(root);
    for (const QString &folder : folders()) {
        const QString path = dir.filePath(folder);
        if (!QFileInfo(path).isDir()) {
            if (!QDir().mkpath(path))
                continue;
            made.append(folder + QLatin1Char('/'));
        }
    }
    const QList<QPair<QString, QString>> files{{QString::fromLatin1(rulesFile), starterRules()},
                                               {QString::fromLatin1(howIWorkFile), starterHowIWork()}};
    for (const auto &[name, text] : files) {
        const QString path = dir.filePath(name);
        if (QFileInfo::exists(path))
            continue;
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(text.toUtf8());
            if (file.commit())
                made.append(name);
        }
    }
    // Git keeps no empty folders; a hidden .keep keeps them on every synced
    // computer without showing in the sidebar.
    for (const QString &folder : folders()) {
        const QDir inside(dir.filePath(folder));
        if (inside.exists() && inside.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) {
            QFile keep(inside.filePath(QStringLiteral(".keep")));
            if (keep.open(QIODevice::WriteOnly))
                keep.close();
        }
    }
    return made;
}

QString dailyFolder(const QString &root) {
    return present(root) ? QStringLiteral("03-projects/daily") : QStringLiteral("Daily");
}

QString meetingsFolder(const QString &root) {
    return present(root) ? QStringLiteral("03-projects/meetings") : QStringLiteral("Meetings");
}

QString literatureFolder(const QString &root) {
    return present(root) ? QStringLiteral("02-context/literature") : QStringLiteral("Literature");
}

QList<Move> moves() {
    return {{QStringLiteral("Daily"), QStringLiteral("03-projects/daily")},
            {QStringLiteral("Meetings"), QStringLiteral("03-projects/meetings")},
            {QStringLiteral("Literature"), QStringLiteral("02-context/literature")},
            {QStringLiteral("Archive"), QStringLiteral("99-archive")}};
}

QString guide() {
    return QStringLiteral(
        "Folders: 01-system holds the rules to follow (01-system/SYSTEM-RULES.md) and how the person works "
        "(01-system/HOW-I-WORK.md), read them first; 02-context is stable background and reference "
        "(literature notes in 02-context/literature); 03-projects is active work, one folder per project "
        "(03-projects/<project>/, starting with a 00-brief note), with daily notes in 03-projects/daily and "
        "meetings in 03-projects/meetings; 04-outputs is finished work; 99-archive is old material, still "
        "searched, moved there rather than deleted. File names say when, what and how far along: "
        "date first for anything tied to a day (2026-10-25-kickoff, and 2026-10-25-q4-plan-final in "
        "04-outputs), plain topic names for lasting ideas so [[links]] read well.");
}

QString starterRules() {
    return QStringLiteral(
        "---\n"
        "title: \"System rules\"\n"
        "tags: [system]\n"
        "---\n"
        "\n"
        "Rules for AI agents working in these notes. Read this first, then HOW-I-WORK.\n"
        "Edit it to suit you; OmaZettel never changes it.\n"
        "\n"
        "## Where things go\n"
        "\n"
        "- `01-system/`: these rules and how I work. Change them only when I ask.\n"
        "- `02-context/`: stable background and reference: people, clients, glossaries, literature notes.\n"
        "  Link to it rather than copying it into projects.\n"
        "- `03-projects/`: active work, one folder per project (`03-projects/<project>/`). A project starts\n"
        "  with a `00-brief` note: goal, people, deadline. Daily notes are in `03-projects/daily`, meetings in\n"
        "  `03-projects/meetings`.\n"
        "- `04-outputs/`: finished work, ready to send or publish.\n"
        "- `99-archive/`: finished or dropped projects. Move them here instead of deleting them.\n"
        "\n"
        "## Naming\n"
        "\n"
        "- File names use hyphens, no spaces.\n"
        "- Anything tied to a day starts with the date: `2026-10-25-kickoff`.\n"
        "- Drafts and outputs end with their status: `2026-10-25-q4-plan-draft-v1`, `2026-10-25-q4-plan-final`.\n"
        "- Lasting ideas keep a plain topic name (`zettelkasten-method`) so links read well.\n"
        "- Avoid vague names such as `notes`, `final` or `untitled`.\n"
        "\n"
        "## Working rules\n"
        "\n"
        "- Search before writing; add to an existing note rather than making a near-duplicate.\n"
        "- Change one section at a time; don't rewrite whole notes.\n"
        "- Link related notes with wiki links (the note's name in double square brackets); link only\n"
        "  notes that exist or should.\n"
        "- Action items are tasks: `- [ ] Owner: task (due date)`.\n");
}

QString starterHowIWork() {
    return QStringLiteral(
        "---\n"
        "title: \"How I work\"\n"
        "tags: [system]\n"
        "---\n"
        "\n"
        "Background for AI agents: who I am and how I like things done. Fill this in.\n"
        "\n"
        "## About me\n"
        "\n"
        "- Role:\n"
        "- What I'm working on:\n"
        "\n"
        "## How I like answers\n"
        "\n"
        "- Tone:\n"
        "- Length:\n"
        "- Spelling (British or American):\n"
        "\n"
        "## People and projects to know\n"
        "\n"
        "- \n");
}

}

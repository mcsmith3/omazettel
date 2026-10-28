#include "moc.h"

#include "layout.h"
#include "notestree.h"
#include "vault.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>

#include <algorithm>

namespace Moc {

namespace {
const QString enabledSetting = QStringLiteral("notes/mapsOfContent");
const QString startMark = QStringLiteral("<!-- omazettel-map: OmaZettel keeps this list up to date; write above or below it -->");
const QString endMark = QStringLiteral("<!-- /omazettel-map -->");

QString readText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

bool writeText(const QString &path, const QString &text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    file.write(text.toUtf8());
    return file.commit();
}

bool isNote(const QFileInfo &info) {
    const QString suffix = info.suffix().toLower();
    return info.isFile() && (suffix == QLatin1String("md") || suffix == QLatin1String("markdown"));
}

// Every note in the folder and below, relative to root; hidden ones skipped.
QStringList notesUnder(const QString &root, const QString &folder) {
    QStringList found;
    QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QDir base(root);
    while (it.hasNext()) {
        it.next();
        const QString relative = base.relativeFilePath(it.filePath());
        bool hidden = false;
        for (const QString &part : relative.split(QLatin1Char('/')))
            hidden = hidden || part.startsWith(QLatin1Char('.'));
        if (!hidden && isNote(it.fileInfo()))
            found.append(relative);
    }
    return found;
}

QString withoutSuffix(const QString &relative) {
    const QFileInfo info(relative);
    return info.path() == QLatin1String(".") ? info.completeBaseName()
                                            : info.path() + QLatin1Char('/') + info.completeBaseName();
}

// The list's text, between the marks.
QString block(const QStringList &lines) {
    return startMark + QLatin1Char('\n') + lines.join(QLatin1Char('\n')) + QLatin1Char('\n') + endMark;
}

// The map's whole text: the list replaced in an existing map, or a new one.
// An empty result means a file of that name is there without the marks
// (someone's own note), and is left alone.
QString mapText(const QString &existing, const QString &title, const QString &intro, const QStringList &lines) {
    const QString list = block(lines);
    if (existing.isNull())
        return QStringLiteral("---\ntitle: \"%1\"\ntags: [moc]\n---\n\n%2%3\n")
            .arg(title, intro.isEmpty() ? QString() : intro + QStringLiteral("\n\n"), list);
    const qsizetype start = existing.indexOf(startMark.left(18));
    const qsizetype end = existing.indexOf(endMark);
    if (start < 0 || end < start)
        return QString();
    return existing.left(start) + list + existing.mid(end + endMark.size());
}
}

bool enabled() {
    return QSettings().value(enabledSetting, true).toBool();
}

void setEnabled(bool on) {
    QSettings().setValue(enabledSetting, on);
}

QString topName() {
    return QStringLiteral("projects-moc");
}

QString nameFor(const QString &projectFolder) {
    const QString name = Vault::noteFileName(projectFolder.toLower());
    return name + (name.endsWith(Vault::fileNameSeparator()) ? QString() : Vault::fileNameSeparator())
           + QStringLiteral("moc");
}

bool isMap(const QString &relativePath) {
    const QStringList parts = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.first() != QLatin1String(Layout::projectsFolder))
        return false;
    const QString base = QFileInfo(parts.last()).completeBaseName();
    if (parts.size() == 2)
        return base.compare(topName(), Qt::CaseInsensitive) == 0;
    return parts.size() == 3 && base.compare(nameFor(parts.at(1)), Qt::CaseInsensitive) == 0;
}

bool isTranscript(const QString &relativePath) {
    const QStringList parts = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i + 1 < parts.size(); ++i) {
        if (parts.at(i).compare(QLatin1String("transcripts"), Qt::CaseInsensitive) == 0)
            return true;
    }
    const QString key = Vault::matchKey(QFileInfo(relativePath).completeBaseName());
    return key == QLatin1String("transcript") || key.endsWith(QLatin1String(" transcript"));
}

QStringList update(const QString &root, bool dryRun) {
    QStringList written;
    if (!Layout::present(root))
        return written;
    const QDir base(root);
    const QString projects = base.filePath(QString::fromLatin1(Layout::projectsFolder));

    // A name that more than one note has is linked by its path instead.
    QHash<QString, int> nameCount;
    for (const QString &note : notesUnder(root, root))
        ++nameCount[Vault::matchKey(QFileInfo(note).completeBaseName())];
    const auto linkTo = [&](const QString &relative) {
        const QString name = QFileInfo(relative).completeBaseName();
        return QStringLiteral("[[%1]]").arg(nameCount.value(Vault::matchKey(name)) > 1 ? withoutSuffix(relative) : name);
    };
    const auto byName = [](const QString &a, const QString &b) { return NotesTree::naturalCompare(a, b) < 0; };

    QStringList folders = QDir(projects).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(folders.begin(), folders.end(), byName);
    QStringList projectMaps;
    for (const QString &folder : std::as_const(folders)) {
        // Daily notes are a calendar, not a project.
        if (folder.compare(QLatin1String("daily"), Qt::CaseInsensitive) == 0)
            continue;
        const QString mapName = nameFor(folder);
        const QString folderRelative = base.relativeFilePath(QDir(projects).filePath(folder));
        // Notes grouped by the subfolder they are in ("" for the project's own).
        QMap<QString, QStringList> groups;
        for (const QString &note : notesUnder(root, QDir(projects).filePath(folder))) {
            if (isTranscript(note))
                continue;
            const QString inside = note.mid(folderRelative.size() + 1);
            if (inside.compare(mapName + QStringLiteral(".md"), Qt::CaseInsensitive) == 0)
                continue;
            const QString group = QFileInfo(inside).path() == QLatin1String(".") ? QString() : QFileInfo(inside).path();
            groups[group].append(note);
        }
        if (groups.isEmpty())
            continue;
        QStringList groupNames = groups.keys();
        std::sort(groupNames.begin(), groupNames.end(), byName);
        QStringList lines;
        for (const QString &group : std::as_const(groupNames)) {
            QStringList notes = groups.value(group);
            std::sort(notes.begin(), notes.end(), byName);
            if (!group.isEmpty()) {
                if (!lines.isEmpty())
                    lines.append(QString());
                lines.append(QStringLiteral("### %1").arg(group));
                lines.append(QString());
            }
            for (const QString &note : std::as_const(notes))
                lines.append(QStringLiteral("- ") + linkTo(note));
        }
        const QString mapPath = QDir(projects).filePath(folder + QLatin1Char('/') + mapName + QStringLiteral(".md"));
        const QString existing = QFileInfo::exists(mapPath) ? readText(mapPath) : QString();
        const QString text = mapText(existing, QStringLiteral("%1: map of content").arg(folder),
                                     QStringLiteral("Up: [[%1]]").arg(topName()), lines);
        if (text.isEmpty())
            continue;
        projectMaps.append(QStringLiteral("- [[%1]]").arg(mapName));
        if (text != existing) {
            if (dryRun || writeText(mapPath, text))
                written.append(base.relativeFilePath(mapPath));
        }
    }

    if (projectMaps.isEmpty())
        return written;
    const QString topPath = QDir(projects).filePath(topName() + QStringLiteral(".md"));
    const QString existing = QFileInfo::exists(topPath) ? readText(topPath) : QString();
    const QString text = mapText(existing, QStringLiteral("Projects: map of content"), QString(), projectMaps);
    if (!text.isEmpty() && text != existing) {
        if (dryRun || writeText(topPath, text))
            written.append(base.relativeFilePath(topPath));
    }
    return written;
}

}

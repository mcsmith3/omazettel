#include "dashboard.h"

#include "layout.h"
#include "moc.h"
#include "sync.h"
#include "taskdates.h"
#include "noteindex.h"
#include "notesapi.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent>

#include <algorithm>

namespace {

QString readText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

bool isNotePath(const QString &relative) {
    const QString suffix = QFileInfo(relative).suffix().toLower();
    if (suffix != QLatin1String("md") && suffix != QLatin1String("markdown"))
        return false;
    for (const QString &part : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part.startsWith(QLatin1Char('.')))
            return false;
    }
    return true;
}

bool under(const QString &relative, const QString &folder) {
    return relative.startsWith(folder + QLatin1Char('/'), Qt::CaseInsensitive);
}

// Runs git in dir; returns its output, or a null string when it failed.
QString git(const QString &dir, const QStringList &args) {
    const QString program = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (program.isEmpty())
        return {};
    QProcess process;
    process.setWorkingDirectory(dir);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    // Reading must not take the lock a sync running at the same time needs.
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    process.setProcessEnvironment(env);
    process.start(program, args);
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(2000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    const QString out = QString::fromUtf8(process.readAllStandardOutput());
    return out.isNull() ? QStringLiteral("") : out;
}

// Body text without the YAML header, headings, empty list items and
// whitespace: what is left when a note has nothing written in it.
bool isEmptyNote(const QString &text) {
    const NotesApi::FrontMatter header = NotesApi::frontMatter(text);
    static const QRegularExpression blank(
        QStringLiteral("^\\s*(#{1,6}(\\s.*)?|(?:[-*+]|\\d+[.)])(\\s+\\[[ xX]\\])?\\s*|<!--.*-->\\s*)?$"));
    const QStringList lines = text.mid(header.present ? header.bodyStart : 0).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (!blank.match(line).hasMatch())
            return false;
    }
    return true;
}

const QRegularExpression &taskLine() {
    static const QRegularExpression re(
        QStringLiteral("^\\s*(?:[-*+]|\\d+[.)])\\s+\\[([ xX])\\]\\s+(.*\\S)\\s*$"));
    return re;
}

} // namespace

Dashboard::Dashboard(NoteIndex *index, QObject *parent) : QObject(parent), m_index(index) {
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(800);
    connect(&m_refreshTimer, &QTimer::timeout, this, &Dashboard::start);
    if (m_index) {
        connect(m_index, &NoteIndex::changed, this, [this]() {
            if (m_active)
                m_refreshTimer.start();
        });
    }
    connect(&m_watcher, &QFutureWatcher<QJsonObject>::finished, this, [this]() {
        m_data = m_watcher.result().toVariantMap();
        emit dataChanged();
        emit loadingChanged();
        if (m_again) {
            m_again = false;
            start();
        }
    });
}

Dashboard::~Dashboard() {
    m_watcher.waitForFinished();
}

void Dashboard::setActive(bool active) {
    if (m_active == active)
        return;
    m_active = active;
    emit activeChanged();
    if (m_active)
        start();
}

void Dashboard::setRoot(const QString &root) {
    if (m_root == root)
        return;
    m_root = root;
    m_data.clear();
    emit dataChanged();
    if (m_active)
        start();
}

void Dashboard::refresh() {
    start();
}

void Dashboard::start() {
    if (m_root.isEmpty() || !m_index)
        return;
    if (m_watcher.isRunning()) {
        m_again = true;
        return;
    }
    const QString root = m_root;
    const Snapshot notes = snapshot(*m_index, root);
    m_watcher.setFuture(QtConcurrent::run([root, notes]() {
        return build(root, notes, 365, QDate::currentDate());
    }));
    emit loadingChanged();
}

QString Dashboard::completeTask(const QString &path, int line, const QString &text) {
    const QString file = QDir(m_root).filePath(path);
    if (m_root.isEmpty() || !QFileInfo(file).canonicalFilePath().startsWith(QFileInfo(m_root).canonicalFilePath()))
        return QStringLiteral("That note is not in the notes folder.");
    const QString changed = tickTask(readText(file), line, text);
    if (changed.isEmpty()) {
        refresh();
        return QStringLiteral("The task has changed since the dashboard was drawn; it shows the note as it is now.");
    }
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(changed.toUtf8()) < 0 || !out.commit()) {
        refresh();
        return QStringLiteral("Could not save %1.").arg(QFileInfo(file).fileName());
    }
    if (m_index)
        m_index->updateFile(file);
    refresh();
    return {};
}

QString Dashboard::tickTask(const QString &noteText, int line, const QString &taskText, const QDate &today) {
    QStringList lines = noteText.split(QLatin1Char('\n'));
    auto isTheTask = [&](int index) {
        const QRegularExpressionMatch match = taskLine().match(lines.at(index));
        return match.hasMatch() && match.captured(1) == QLatin1String(" ") && match.captured(2) == taskText;
    };
    // The line the dashboard saw, or else the one open task with that text,
    // when lines above it have changed since.
    int found = line >= 1 && line <= lines.size() && isTheTask(line - 1) ? line - 1 : -1;
    if (found < 0) {
        for (int i = 0; i < lines.size(); ++i) {
            if (!isTheTask(i))
                continue;
            if (found >= 0)
                return {};
            found = i;
        }
    }
    if (found < 0)
        return {};
    const QString before = lines.at(found);
    QString after = before;
    after[taskLine().match(before).capturedStart(1)] = QLatin1Char('x');
    lines[found] = TaskDates::applied(after, TaskDates::forBox(after, today));
    return lines.join(QLatin1Char('\n'));
}

Dashboard::Snapshot Dashboard::snapshot(const NoteIndex &index, const QString &root) {
    Snapshot result;
    const QDir base(root);
    for (const QVariant &entry : index.notes()) {
        const QString path = entry.toMap().value(QStringLiteral("path")).toString();
        Snapshot::Note note;
        note.path = base.relativeFilePath(path);
        note.title = entry.toMap().value(QStringLiteral("title")).toString();
        for (const QVariant &link : index.backlinks(path))
            note.linkedFrom.append(base.relativeFilePath(link.toMap().value(QStringLiteral("path")).toString()));
        note.missing = index.missingTargets(path);
        note.tags = index.tagsOf(path);
        result.notes.append(note);
    }
    return result;
}

QJsonObject Dashboard::build(const QString &root, const Snapshot &snapshot, int days, const QDate &today) {
    const QDir base(root);
    const QDate first = today.addDays(-(qMax(1, days) - 1));
    const bool layout = Layout::present(root);

    struct Info {
        QString title;
        QDateTime modified;
        QDateTime born;
        QString text;
    };
    QHash<QString, Info> info;
    QStringList paths; // Notes the dashboard counts: not the maps it keeps itself.
    for (const Snapshot::Note &note : snapshot.notes) {
        const QFileInfo file(base.filePath(note.path));
        info.insert(note.path, {note.title, file.lastModified(), file.birthTime(), readText(file.filePath())});
        if (!Moc::isMap(note.path))
            paths.append(note.path);
    }
    auto titleOf = [&info](const QString &path) {
        return info.contains(path) ? info.value(path).title : QFileInfo(path).completeBaseName();
    };

    // ── Activity: which notes were made or changed on each day ──────────────
    // Day -> note -> whether it was made that day (else changed).
    QMap<QDate, QHash<QString, bool>> touched;
    auto mark = [&](const QDate &day, const QString &path, bool created) {
        if (!day.isValid() || day < first || day > today || !isNotePath(path) || Moc::isMap(path))
            return;
        QHash<QString, bool> &notes = touched[day];
        if (created)
            notes.insert(path, true);
        else if (!notes.contains(path))
            notes.insert(path, false);
    };

    QString history = QStringLiteral("files");
    // The notes' own repository, as version history uses: not one of the
    // whole home folder.
    const QString top = Sync::repositoryOf(root);
    // The whole history, not just the days shown: a note's last version
    // says when it last changed, where the file's date only says when git
    // last wrote it (a fresh clone or a pull makes every note look new).
    const QString log = top.isEmpty() ? QString() : git(root, {QStringLiteral("-c"), QStringLiteral("core.quotePath=false"), QStringLiteral("log"),
                                   QStringLiteral("--format=%x01%aI"), QStringLiteral("--name-status"),
                                   QStringLiteral("-z"), QStringLiteral("--relative"), QStringLiteral("--"),
                                   QStringLiteral(".")});
    // A repository with no versions yet has no log, but git still knows
    // which notes are new.
    if (!top.isEmpty()) {
        history = QStringLiteral("git");
        // Newest first, so a rename is seen before the older changes made
        // under the old name, which then count for the note as it is named now.
        QHash<QString, QString> renamed;
        auto current = [&renamed](const QString &path) { return renamed.value(path, path); };
        QHash<QString, QDateTime> saved; // Each note's newest version.
        const QStringList tokens = log.split(QLatin1Char('\0'));
        QDateTime when;
        QDate day;
        for (int i = 0; i < tokens.size(); ++i) {
            const QString token = tokens.at(i).trimmed();
            if (token.isEmpty())
                continue;
            if (token.startsWith(QChar(1))) {
                when = QDateTime::fromString(token.mid(1), Qt::ISODate);
                day = when.toLocalTime().date();
                continue;
            }
            const QChar status = token.at(0);
            if (status == QLatin1Char('R') || status == QLatin1Char('C')) {
                const QString from = tokens.value(i + 1);
                const QString to = current(tokens.value(i + 2));
                i += 2;
                if (status == QLatin1Char('R'))
                    renamed.insert(from, to);
                else
                    mark(day, to, true);
                if (!saved.contains(to))
                    saved.insert(to, when);
                continue;
            }
            const QString path = current(tokens.value(++i));
            if (status != QLatin1Char('D') && !saved.contains(path))
                saved.insert(path, when);
            if (status == QLatin1Char('A'))
                mark(day, path, true);
            else if (status == QLatin1Char('M'))
                mark(day, path, false);
        }
        // Changes not saved as a version yet.
        const QString changes = git(root, {QStringLiteral("-c"), QStringLiteral("core.quotePath=false"),
                                           QStringLiteral("status"), QStringLiteral("--porcelain"), QStringLiteral("-z"),
                                           QStringLiteral("--untracked-files=all"), QStringLiteral(".")});
        const QDir notes(QFileInfo(root).canonicalFilePath());
        const QStringList entries = changes.split(QLatin1Char('\0'), Qt::SkipEmptyParts);
        for (int i = 0; i < entries.size(); ++i) {
            const QString entry = entries.at(i);
            if (entry.size() < 4)
                continue;
            const QString code = entry.left(2);
            if (code.contains(QLatin1Char('R')) || code.contains(QLatin1Char('C')))
                ++i; // The old name follows.
            if (code.contains(QLatin1Char('D')))
                continue;
            // status names paths from the top of the repository.
            const QString path = notes.relativeFilePath(QDir(top).filePath(entry.mid(3)));
            const Info note = info.value(path);
            if (!info.contains(path))
                continue;
            saved.remove(path); // Changed since its last version: the file's date is right.
            if (code == QLatin1String("??") || code.contains(QLatin1Char('A')))
                mark((note.born.isValid() ? note.born : note.modified).toLocalTime().date(), path, true);
            else
                mark(note.modified.toLocalTime().date(), path, false);
        }
        for (auto note = info.begin(); note != info.end(); ++note) {
            const QDateTime last = saved.value(note.key());
            if (last.isValid())
                note->modified = last;
        }
    } else {
        // No history: when each note was made (where the file system knows)
        // and last changed.
        for (const QString &path : std::as_const(paths)) {
            const Info &note = info[path];
            if (note.born.isValid())
                mark(note.born.toLocalTime().date(), path, true);
            mark(note.modified.toLocalTime().date(), path, false);
        }
    }

    std::sort(paths.begin(), paths.end(), [&info](const QString &a, const QString &b) {
        return info.value(a).modified > info.value(b).modified;
    });

    QJsonArray activity;
    for (auto day = touched.cbegin(); day != touched.cend(); ++day) {
        int created = 0;
        QJsonArray notes;
        QStringList names = day.value().keys();
        std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
            if (day.value().value(a) != day.value().value(b))
                return day.value().value(a);
            return titleOf(a).compare(titleOf(b), Qt::CaseInsensitive) < 0;
        });
        for (const QString &path : std::as_const(names)) {
            const bool made = day.value().value(path);
            created += made ? 1 : 0;
            notes.append(QJsonObject{{QStringLiteral("path"), path},
                                     {QStringLiteral("title"), titleOf(path)},
                                     {QStringLiteral("created"), made},
                                     {QStringLiteral("exists"), info.contains(path)}});
        }
        activity.append(QJsonObject{{QStringLiteral("date"), day.key().toString(Qt::ISODate)},
                                    {QStringLiteral("created"), created},
                                    {QStringLiteral("edited"), int(names.size()) - created},
                                    {QStringLiteral("notes"), notes}});
    }

    // Days in a row with something written, up to today (or yesterday, so
    // the streak is not lost before today's first note).
    int streak = 0;
    for (QDate day = touched.contains(today) ? today : today.addDays(-1); touched.contains(day); day = day.addDays(-1))
        ++streak;
    int createdThisWeek = 0;
    int editedThisWeek = 0;
    QSet<QString> seenThisWeek;
    for (QDate day = today.addDays(-6); day <= today; day = day.addDays(1)) {
        const QHash<QString, bool> notes = touched.value(day);
        for (auto note = notes.cbegin(); note != notes.cend(); ++note) {
            if (note.value())
                ++createdThisWeek;
            else if (!seenThisWeek.contains(note.key()))
                ++editedThisWeek;
            seenThisWeek.insert(note.key());
        }
    }

    // ── Open tasks, newest notes first ──────────────────────────────────────
    auto projectOf = [layout](const QString &path) {
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (layout) {
            if (parts.size() > 2 && parts.first() == QLatin1String(Layout::projectsFolder))
                return parts.at(1);
            return parts.size() > 1 ? parts.first() : QString();
        }
        return parts.size() > 1 ? parts.first() : QString();
    };
    QJsonArray tasks;
    QHash<QString, int> openByNote;
    for (const QString &path : std::as_const(paths)) {
        if (Moc::isTranscript(path))
            continue;
        const QString text = info.value(path).text;
        const QStringList lines = text.split(QLatin1Char('\n'));
        QList<int> starts;
        int offset = 0;
        for (const QString &line : lines) {
            starts.append(offset);
            offset += int(line.size()) + 1;
        }
        for (const NotesApi::Task &task : NotesApi::tasks(text)) {
            if (task.done)
                continue;
            ++openByNote[path];
            const QString line = lines.value(task.line - 1);
            tasks.append(QJsonObject{{QStringLiteral("path"), path},
                                     {QStringLiteral("title"), titleOf(path)},
                                     {QStringLiteral("project"), projectOf(path)},
                                     {QStringLiteral("line"), task.line},
                                     {QStringLiteral("position"), starts.value(task.line - 1) + int(line.lastIndexOf(task.text))},
                                     {QStringLiteral("length"), int(task.text.size())},
                                     {QStringLiteral("text"), task.text}});
        }
    }

    // ── Projects (or, without the layout, top-level folders) ────────────────
    QJsonArray projects;
    const QString projectsRoot = layout ? QString::fromLatin1(Layout::projectsFolder) : QString();
    const QString daily = Layout::dailyFolder(root);
    const QFileInfoList folders = QDir(base.filePath(projectsRoot.isEmpty() ? QStringLiteral(".") : projectsRoot))
                                      .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &folder : folders) {
        const QString relative = base.relativeFilePath(folder.filePath());
        if (folder.fileName().startsWith(QLatin1Char('.')) || relative.compare(daily, Qt::CaseInsensitive) == 0)
            continue;
        int notes = 0;
        int open = 0;
        QDateTime last;
        for (const QString &path : std::as_const(paths)) {
            if (!under(path, relative) || Moc::isTranscript(path))
                continue;
            ++notes;
            open += openByNote.value(path);
            last = qMax(last, info.value(path).modified);
        }
        if (notes == 0)
            continue;
        const QString map = layout ? relative + QLatin1Char('/') + Moc::nameFor(folder.fileName()) + QStringLiteral(".md")
                                   : QString();
        projects.append(QJsonObject{{QStringLiteral("name"), folder.fileName()},
                                    {QStringLiteral("folder"), relative},
                                    {QStringLiteral("map"), info.contains(map) ? map : QString()},
                                    {QStringLiteral("notes"), notes},
                                    {QStringLiteral("openTasks"), open},
                                    {QStringLiteral("lastActivity"), last.toLocalTime().date().toString(Qt::ISODate)}});
    }

    // ── Notes that need a look ──────────────────────────────────────────────
    // Notes nothing links to, leaving out the kinds nobody links by habit:
    // daily notes, the rules for agents, the archive and transcripts. A
    // link from a map of content does not count, since every project note
    // has one.
    QJsonArray broken;
    QJsonArray unlinked;
    QJsonArray empty;
    for (const Snapshot::Note &note : snapshot.notes) {
        if (Moc::isMap(note.path))
            continue;
        for (const QString &target : note.missing)
            broken.append(QJsonObject{{QStringLiteral("path"), note.path}, {QStringLiteral("title"), note.title},
                                      {QStringLiteral("target"), target}});
        const bool skip = Moc::isTranscript(note.path) || under(note.path, daily)
                          || under(note.path, QString::fromLatin1(Layout::systemFolder))
                          || under(note.path, QString::fromLatin1(Layout::archiveFolder))
                          || under(note.path, QStringLiteral("Archive"));
        const bool linked = std::any_of(note.linkedFrom.cbegin(), note.linkedFrom.cend(),
                                        [](const QString &from) { return !Moc::isMap(from); });
        if (!skip && !linked)
            unlinked.append(QJsonObject{{QStringLiteral("path"), note.path}, {QStringLiteral("title"), note.title}});
        if (!under(note.path, QString::fromLatin1(Layout::systemFolder)) && isEmptyNote(info.value(note.path).text))
            empty.append(QJsonObject{{QStringLiteral("path"), note.path}, {QStringLiteral("title"), note.title}});
    }
    // ── Tags and recent notes ───────────────────────────────────────────────
    // Counted from the notes themselves, not the maps (which carry #moc).
    QHash<QString, QPair<QString, int>> tagCounts;
    for (const Snapshot::Note &note : snapshot.notes) {
        if (Moc::isMap(note.path))
            continue;
        for (const QString &tag : note.tags) {
            QPair<QString, int> &count = tagCounts[tag.toLower()];
            if (count.first.isEmpty())
                count.first = tag;
            ++count.second;
        }
    }
    QList<QPair<QString, int>> tagList = tagCounts.values();
    std::sort(tagList.begin(), tagList.end(), [](const auto &a, const auto &b) {
        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });
    std::stable_sort(tagList.begin(), tagList.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    QJsonArray tags;
    for (int i = 0; i < tagList.size() && i < 15; ++i)
        tags.append(QJsonObject{{QStringLiteral("tag"), tagList.at(i).first}, {QStringLiteral("count"), tagList.at(i).second}});
    QJsonArray recent;
    for (int i = 0; i < paths.size() && i < 10; ++i)
        recent.append(QJsonObject{{QStringLiteral("path"), paths.at(i)},
                                  {QStringLiteral("title"), titleOf(paths.at(i))},
                                  {QStringLiteral("modified"), info.value(paths.at(i)).modified.toLocalTime().toString(Qt::ISODate)}});

    return {
        {QStringLiteral("today"), today.toString(Qt::ISODate)},
        {QStringLiteral("from"), first.toString(Qt::ISODate)},
        {QStringLiteral("history"), history},
        {QStringLiteral("summary"), QJsonObject{{QStringLiteral("notes"), int(paths.size())},
                                                {QStringLiteral("openTasks"), int(tasks.size())},
                                                {QStringLiteral("activeDays"), int(touched.size())},
                                                {QStringLiteral("streak"), streak},
                                                {QStringLiteral("createdThisWeek"), createdThisWeek},
                                                {QStringLiteral("editedThisWeek"), editedThisWeek}}},
        {QStringLiteral("activity"), activity},
        {QStringLiteral("tasks"), tasks},
        {QStringLiteral("projectsAre"), layout ? QStringLiteral("projects") : QStringLiteral("folders")},
        {QStringLiteral("projects"), projects},
        {QStringLiteral("attention"), QJsonObject{{QStringLiteral("broken"), broken},
                                                  {QStringLiteral("unlinked"), unlinked},
                                                  {QStringLiteral("empty"), empty}}},
        {QStringLiteral("tags"), tags},
        {QStringLiteral("recent"), recent},
    };
}

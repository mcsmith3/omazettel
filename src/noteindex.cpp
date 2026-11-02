#include "noteindex.h"

#include "markdownhighlighter.h"
#include "vault.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtConcurrent>

#include <algorithm>

namespace {
bool isNote(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
}

QString stripNoteSuffix(QString name) {
    if (name.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        name.chop(3);
    else if (name.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive))
        name.chop(9);
    return name;
}
}

NoteIndex::NoteIndex(QObject *parent) : QObject(parent) {
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(250);
    connect(&m_rescanTimer, &QTimer::timeout, this, [this]() {
        if (m_root.isEmpty())
            return;
        if (m_watcher.isRunning()) {
            m_rescanAgain = true;
            return;
        }
        const QString root = m_root;
        m_watcher.setFuture(QtConcurrent::run([root]() { return NoteIndex::scan(root); }));
        emit indexingChanged();
    });
    connect(&m_watcher, &QFutureWatcher<QHash<QString, Note>>::finished, this, [this]() {
        applyScan();
        emit indexingChanged();
        if (m_rescanAgain) {
            m_rescanAgain = false;
            m_rescanTimer.start();
        }
    });
}

NoteIndex::~NoteIndex() {
    m_watcher.waitForFinished();
}

void NoteIndex::setRoot(const QString &root) {
    if (m_root == root)
        return;
    m_root = root;
    m_notes.clear();
    rebuild();
    if (!root.isEmpty())
        scheduleRescan();
}

void NoteIndex::scheduleRescan() {
    m_rescanTimer.start();
}

void NoteIndex::rescanNow() {
    m_rescanTimer.stop();
    m_watcher.waitForFinished();
    m_notes = m_root.isEmpty() ? QHash<QString, Note>() : scan(m_root);
    rebuild();
}

void NoteIndex::applyScan() {
    const QHash<QString, Note> result = m_watcher.result();
    // A result for a folder that is no longer open is dropped.
    for (auto it = result.cbegin(); it != result.cend(); ++it) {
        if (!it.key().startsWith(m_root + QLatin1Char('/')))
            return;
    }
    m_notes = result;
    rebuild();
}

NoteIndex::Note NoteIndex::readNote(const QString &path) {
    Note note;
    note.path = path;
    note.name = stripNoteSuffix(QFileInfo(path).fileName());
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        note.text = QString::fromUtf8(file.readAll());
        const Zettel::ParsedNote parsed = Zettel::parse(note.text);
        note.title = parsed.title;
        note.tags = parsed.tags;
        note.links = parsed.links;
    }
    if (note.title.isEmpty())
        note.title = note.name;
    return note;
}

QHash<QString, NoteIndex::Note> NoteIndex::scan(const QString &root) {
    QHash<QString, Note> notes;
    // Hidden folders (.git, .omazettel) are skipped, as the sidebar does.
    QDirIterator it(root, {QStringLiteral("*.md"), QStringLiteral("*.markdown")},
                    QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.mid(root.size()).contains(QLatin1String("/.")))
            continue;
        // A sync tool's conflict copy is not a note of its own; Sync lists it.
        if (!Zettel::conflictOriginal(QFileInfo(path).fileName()).isEmpty())
            continue;
        notes.insert(path, readNote(path));
    }
    return notes;
}

bool NoteIndex::covers(const QString &path) const {
    return !m_root.isEmpty() && path.startsWith(m_root + QLatin1Char('/')) && isNote(path)
        && !path.mid(m_root.size()).contains(QLatin1String("/."))
        && Zettel::conflictOriginal(QFileInfo(path).fileName()).isEmpty();
}

void NoteIndex::updateFile(const QString &path) {
    if (!covers(path))
        return;
    if (QFileInfo(path).isFile())
        m_notes.insert(path, readNote(path));
    else
        m_notes.remove(path);
    rebuild();
}

void NoteIndex::rebuild() {
    m_byName.clear();
    m_backlinks.clear();
    m_byTag.clear();
    m_tagSpelling.clear();

    for (const Note &note : std::as_const(m_notes))
        m_byName[Vault::matchKey(note.name)].append(note.path);

    // In path order, so which spelling of a tag shows, and the order of
    // backlinks, never depend on hash order.
    QStringList paths = m_notes.keys();
    std::sort(paths.begin(), paths.end());
    for (const QString &path : std::as_const(paths)) {
        const Note &note = m_notes[path];
        QSet<QString> linked;
        for (const Zettel::LinkUse &link : note.links) {
            const QString target = resolve(link.target, note.path);
            if (target.isEmpty() || target == note.path || linked.contains(target))
                continue;
            linked.insert(target);
            m_backlinks[target].append({note.path, link.context});
        }
        for (const QString &tag : note.tags) {
            const QString key = tag.toLower();
            m_byTag[key].append(note.path);
            if (!m_tagSpelling.contains(key))
                m_tagSpelling.insert(key, tag);
        }
    }
    ++m_revision;
    emit changed();
}

QString NoteIndex::resolve(const QString &rawTarget, const QString &fromPath) const {
    QString target = stripNoteSuffix(rawTarget.trimmed());
    if (target.isEmpty() || m_root.isEmpty())
        return QString();

    QStringList candidates;
    if (target.contains(QLatin1Char('/'))) {
        // A path: relative to the notes folder, matched from the end.
        const QString wanted = Vault::matchKey(QLatin1Char('/') + target);
        for (const Note &note : m_notes) {
            const QString relative = Vault::matchKey(stripNoteSuffix(note.path.mid(m_root.size())));
            if (relative.endsWith(wanted))
                candidates.append(note.path);
        }
    } else {
        candidates = m_byName.value(Vault::matchKey(target));
    }

    if (candidates.isEmpty()) {
        // Fall back to a note whose title matches.
        for (const Note &note : m_notes) {
            if (Vault::matchKey(note.title) == Vault::matchKey(target))
                candidates.append(note.path);
        }
    }
    if (candidates.isEmpty())
        return QString();
    if (candidates.size() == 1)
        return candidates.first();

    // Several notes share the name: prefer the linking note's folder, then
    // the one nearest the top.
    const QString fromFolder = QFileInfo(fromPath).absolutePath();
    std::sort(candidates.begin(), candidates.end(), [&fromFolder](const QString &a, const QString &b) {
        const bool aHere = QFileInfo(a).absolutePath() == fromFolder;
        const bool bHere = QFileInfo(b).absolutePath() == fromFolder;
        if (aHere != bHere)
            return aHere;
        const int aDepth = int(a.count(QLatin1Char('/')));
        const int bDepth = int(b.count(QLatin1Char('/')));
        return aDepth != bDepth ? aDepth < bDepth : a < b;
    });
    return candidates.first();
}

QVariantMap NoteIndex::noteEntry(const Note &note) const {
    const QString folder = QFileInfo(note.path).absolutePath().mid(m_root.size() + 1);
    return {{QStringLiteral("path"), note.path},
            {QStringLiteral("name"), note.name},
            {QStringLiteral("title"), note.title},
            {QStringLiteral("folder"), folder}};
}

QVariantList NoteIndex::backlinks(const QString &path) const {
    QVariantList result;
    QList<QPair<QString, QString>> sources = m_backlinks.value(path);
    std::sort(sources.begin(), sources.end(), [this](const auto &a, const auto &b) {
        return titleOf(a.first).compare(titleOf(b.first), Qt::CaseInsensitive) < 0;
    });
    for (const auto &source : sources) {
        QVariantMap entry = noteEntry(m_notes.value(source.first));
        entry.insert(QStringLiteral("context"), source.second);
        result.append(entry);
    }
    return result;
}

QVariantList NoteIndex::notes() const {
    QList<Note> sorted = m_notes.values();
    std::sort(sorted.begin(), sorted.end(), [](const Note &a, const Note &b) {
        const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
        return byName != 0 ? byName < 0 : a.path < b.path;
    });
    QVariantList result;
    for (const Note &note : sorted)
        result.append(noteEntry(note));
    return result;
}

QVariantList NoteIndex::tags() const {
    QStringList keys = m_byTag.keys();
    std::sort(keys.begin(), keys.end());
    QVariantList result;
    for (const QString &key : keys) {
        result.append(QVariantMap{{QStringLiteral("tag"), m_tagSpelling.value(key)},
                                  {QStringLiteral("count"), m_byTag.value(key).size()}});
    }
    return result;
}

QVariantList NoteIndex::notesWithTag(const QString &tag) const {
    QString key = tag.toLower();
    if (key.startsWith(QLatin1Char('#')))
        key.remove(0, 1);
    QList<Note> tagged;
    for (const QString &path : m_byTag.value(key))
        tagged.append(m_notes.value(path));
    std::sort(tagged.begin(), tagged.end(), [](const Note &a, const Note &b) {
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    });
    QVariantList result;
    for (const Note &note : tagged)
        result.append(noteEntry(note));
    return result;
}

QStringList NoteIndex::tagsOf(const QString &path) const {
    return m_notes.value(path).tags;
}

QString NoteIndex::titleOf(const QString &path) const {
    const auto it = m_notes.constFind(path);
    return it == m_notes.cend() ? stripNoteSuffix(QFileInfo(path).fileName()) : it->title;
}

QStringList NoteIndex::missingTargets(const QString &path) const {
    QStringList missing;
    const auto it = m_notes.constFind(path);
    if (it == m_notes.cend())
        return missing;
    for (const Zettel::LinkUse &link : it->links) {
        if (resolve(link.target, path).isEmpty()
                && !missing.contains(link.target, Qt::CaseInsensitive))
            missing.append(link.target);
    }
    return missing;
}

QVariantMap NoteIndex::movesForFolder(const QString &oldFolder, const QString &newFolder) const {
    QVariantMap moves;
    const QString prefix = oldFolder + QLatin1Char('/');
    for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it) {
        if (it.key().startsWith(prefix))
            moves.insert(it.key(), newFolder + it.key().mid(oldFolder.size()));
    }
    return moves;
}

QString NoteIndex::rewriteText(const QString &text, const QString &notePath,
                               const QVariantMap &moves) const {
    m_lastRewriteCount = 0;
    if (moves.isEmpty() || !text.contains(QLatin1String("[[")))
        return text;

    QStringList lines = text.split(QLatin1Char('\n'));
    QChar fenceMarker;
    int fence = 0;
    static const QRegularExpression fenceRe(QStringLiteral("^ {0,3}(`{3,}|~{3,})"));
    for (QString &line : lines) {
        const QRegularExpressionMatch fenceMatch = fenceRe.match(line);
        if (fence > 0) {
            if (fenceMatch.hasMatch() && fenceMatch.capturedLength(1) >= fence
                    && fenceMatch.captured(1).at(0) == fenceMarker)
                fence = 0;
            continue;
        }
        if (fenceMatch.hasMatch()) {
            fence = int(fenceMatch.capturedLength(1));
            fenceMarker = fenceMatch.captured(1).at(0);
            continue;
        }
        const QList<Zettel::WikiLink> links = Zettel::wikiLinks(line);
        // Right to left, so earlier offsets stay valid.
        for (auto link = links.crbegin(); link != links.crend(); ++link) {
            if (link->target.isEmpty())
                continue;
            const QString oldTarget = resolve(link->target, notePath);
            if (!moves.contains(oldTarget))
                continue;
            const QString newPath = moves.value(oldTarget).toString();
            QString newTarget;
            if (link->target.contains(QLatin1Char('/'))) {
                newTarget = stripNoteSuffix(newPath.mid(m_root.size() + 1));
            } else {
                newTarget = stripNoteSuffix(QFileInfo(newPath).fileName());
            }
            if (newTarget == link->target)
                continue;
            line.replace(link->targetStart, link->target.size(), newTarget);
            ++m_lastRewriteCount;
        }
    }
    return m_lastRewriteCount > 0 ? lines.join(QLatin1Char('\n')) : text;
}

int NoteIndex::rewriteLinks(const QVariantMap &moves, const QString &skipPath) {
    int total = 0;
    m_lastRewriteFiles = 0;
    QStringList paths = m_notes.keys();
    std::sort(paths.begin(), paths.end());
    for (const QString &path : std::as_const(paths)) {
        if (path == skipPath)
            continue;
        // Only notes that link somewhere being moved.
        bool relevant = false;
        for (const Zettel::LinkUse &link : m_notes.value(path).links) {
            if (moves.contains(resolve(link.target, path))) {
                relevant = true;
                break;
            }
        }
        if (!relevant)
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(file.readAll());
        file.close();
        const QString rewritten = rewriteText(text, path, moves);
        const int changed = m_lastRewriteCount;
        if (changed == 0)
            continue;
        QSaveFile out(path);
        out.setDirectWriteFallback(true);
        if (!out.open(QIODevice::WriteOnly))
            continue;
        out.write(rewritten.toUtf8());
        if (out.commit()) {
            total += changed;
            ++m_lastRewriteFiles;
        }
    }
    return total;
}

NoteIndex::Query NoteIndex::parseQuery(const QString &query) {
    Query parsed;
    static const QRegularExpression tokenRe(QStringLiteral("\"([^\"]*)\"?|(\\S+)"));
    auto it = tokenRe.globalMatch(query);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedStart(1) >= 0) {
            const QString phrase = match.captured(1).simplified();
            if (!phrase.isEmpty())
                parsed.terms.append(phrase);
            continue;
        }
        const QString word = match.captured(2);
        if (word.startsWith(QLatin1Char('#')) && word.size() > 1)
            parsed.tags.append(word.mid(1).toLower());
        else if (word.startsWith(QLatin1String("tag:"), Qt::CaseInsensitive) && word.size() > 4)
            parsed.tags.append(word.mid(4).toLower());
        else
            parsed.terms.append(word);
    }
    return parsed;
}

QVariantList NoteIndex::search(const QString &query, const QString &highlightColor, int limit) const {
    const Query parsed = parseQuery(query);
    if (parsed.terms.isEmpty() && parsed.tags.isEmpty())
        return {};

    struct Hit {
        const Note *note;
        int score;
        QList<QPair<int, int>> matches; // start, length
    };
    QList<Hit> hits;
    for (const Note &note : m_notes) {
        if (!parsed.tags.isEmpty()) {
            bool tagged = true;
            for (const QString &wanted : parsed.tags) {
                bool found = false;
                for (const QString &tag : note.tags) {
                    const QString lower = tag.toLower();
                    if (lower == wanted || lower.startsWith(wanted + QLatin1Char('/'))) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    tagged = false;
                    break;
                }
            }
            if (!tagged)
                continue;
        }

        Hit hit{&note, 0, {}};
        bool all = true;
        for (const QString &term : parsed.terms) {
            const QList<int> inText = MarkdownHighlighter::findMatches(note.text, term);
            const bool inTitle = !MarkdownHighlighter::findMatches(note.title, term).isEmpty()
                || !MarkdownHighlighter::findMatches(note.name, term).isEmpty();
            if (inText.isEmpty() && !inTitle) {
                all = false;
                break;
            }
            if (inTitle)
                hit.score += 100;
            hit.score += int(qMin<qsizetype>(inText.size(), 20));
            for (int start : inText)
                hit.matches.append({start, int(term.size())});
        }
        if (!all)
            continue;
        std::sort(hit.matches.begin(), hit.matches.end());
        hits.append(hit);
    }

    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.note->title.compare(b.note->title, Qt::CaseInsensitive) < 0;
    });

    const QString open = highlightColor.isEmpty()
        ? QStringLiteral("<b>") : QStringLiteral("<b><font color=\"%1\">").arg(highlightColor);
    const QString close = highlightColor.isEmpty() ? QStringLiteral("</b>") : QStringLiteral("</font></b>");

    QVariantList results;
    for (const Hit &hit : std::as_const(hits)) {
        if (results.size() >= limit)
            break;
        const QString &text = hit.note->text;
        QVariantList snippets;
        int lastLineStart = -1;
        for (const auto &match : hit.matches) {
            if (snippets.size() >= 3)
                break;
            const int lineStart = int(text.lastIndexOf(QLatin1Char('\n'), match.first - 1)) + 1;
            if (lineStart == lastLineStart)
                continue;
            lastLineStart = lineStart;
            int lineEnd = int(text.indexOf(QLatin1Char('\n'), match.first));
            if (lineEnd < 0)
                lineEnd = int(text.size());
            // A window of the line around the match.
            const int from = qMax(lineStart, match.first - 50);
            const int to = qMin(lineEnd, match.first + match.second + 90);
            QString html;
            if (from > lineStart)
                html += QStringLiteral("…");
            int at = from;
            for (const auto &other : hit.matches) {
                if (other.first < at || other.first + other.second > to)
                    continue;
                html += text.mid(at, other.first - at).toHtmlEscaped();
                html += open + text.mid(other.first, other.second).toHtmlEscaped() + close;
                at = other.first + other.second;
            }
            html += text.mid(at, to - at).toHtmlEscaped();
            if (to < lineEnd)
                html += QStringLiteral("…");
            snippets.append(QVariantMap{{QStringLiteral("html"), html.trimmed()},
                                        {QStringLiteral("position"), match.first},
                                        {QStringLiteral("length"), match.second}});
        }
        QVariantMap entry = noteEntry(*hit.note);
        entry.insert(QStringLiteral("count"), int(hit.matches.size()));
        entry.insert(QStringLiteral("snippets"), snippets);
        results.append(entry);
    }
    return results;
}

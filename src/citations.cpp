#include "citations.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

namespace {
const QString settingsSubpath = QStringLiteral(".omazettel/citations.yaml");

QString resolvePath(const QString &root, QString value) {
    value = value.trimmed();
    if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))))
        value = value.mid(1, value.size() - 2);
    if (value.isEmpty())
        return QString();
    if (value.startsWith(QLatin1String("~/")))
        value = QDir::homePath() + value.mid(1);
    if (QDir::isRelativePath(value))
        value = QDir(root).filePath(value);
    return QDir::cleanPath(value);
}

// The path to store: relative when inside the notes folder, so the folder
// can move (or sync to another machine) with its settings.
QString storedPath(const QString &root, const QString &path) {
    if (path.isEmpty())
        return QString();
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!root.isEmpty() && clean.startsWith(root + QLatin1Char('/')))
        return clean.mid(root.size() + 1);
    return clean;
}

// LaTeX-ish field text made readable: braces and simple accents removed.
QString cleanField(QString value) {
    static const QHash<QString, QString> accents{
        {QStringLiteral("\\\"a"), QStringLiteral("ä")}, {QStringLiteral("\\\"o"), QStringLiteral("ö")},
        {QStringLiteral("\\\"u"), QStringLiteral("ü")}, {QStringLiteral("\\'e"), QStringLiteral("é")},
        {QStringLiteral("\\`e"), QStringLiteral("è")}, {QStringLiteral("\\'a"), QStringLiteral("á")},
        {QStringLiteral("\\~n"), QStringLiteral("ñ")}, {QStringLiteral("\\c c"), QStringLiteral("ç")},
        {QStringLiteral("\\ss"), QStringLiteral("ß")}, {QStringLiteral("\\&"), QStringLiteral("&")}};
    value.remove(QLatin1Char('{'));
    value.remove(QLatin1Char('}'));
    for (auto it = accents.cbegin(); it != accents.cend(); ++it)
        value.replace(it.key(), it.value());
    return value.simplified();
}

QString firstAuthor(const QString &authors) {
    const QStringList people = authors.split(QRegularExpression(QStringLiteral("\\s+and\\s+")),
                                             Qt::SkipEmptyParts);
    if (people.isEmpty())
        return QString();
    const QString first = people.first().trimmed();
    QString family = first.contains(QLatin1Char(','))
        ? first.section(QLatin1Char(','), 0, 0).trimmed()
        : first.section(QLatin1Char(' '), -1).trimmed();
    if (people.size() > 2)
        family += QStringLiteral(" et al.");
    else if (people.size() == 2) {
        const QString second = people.at(1).trimmed();
        family += QStringLiteral(" & ") + (second.contains(QLatin1Char(','))
                                               ? second.section(QLatin1Char(','), 0, 0).trimmed()
                                               : second.section(QLatin1Char(' '), -1).trimmed());
    }
    return family;
}
}

Citations::Citations(QObject *parent) : QObject(parent) {
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(300);
    connect(&m_reloadTimer, &QTimer::timeout, this, &Citations::reload);
    // Zotero rewrites the file on every change; reload shortly after.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
}

Citations::Settings Citations::settingsFor(const QString &notesRoot) {
    Settings settings;
    if (notesRoot.isEmpty())
        return settings;
    QFile file(QDir(notesRoot).filePath(settingsSubpath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return settings;
    static const QRegularExpression keyRe(QStringLiteral("^(bibliography|csl)\\s*:\\s*(.*)$"),
                                          QRegularExpression::MultilineOption);
    auto it = keyRe.globalMatch(QString::fromUtf8(file.readAll()));
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString path = resolvePath(notesRoot, match.captured(2));
        if (match.captured(1) == QLatin1String("bibliography"))
            settings.bibliography = path;
        else
            settings.style = path;
    }
    return settings;
}

void Citations::setRoot(const QString &notesRoot) {
    if (m_root == notesRoot)
        return;
    m_root = notesRoot;
    reload();
}

void Citations::save() {
    if (m_root.isEmpty())
        return;
    const QString path = QDir(m_root).filePath(settingsSubpath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QString text = QStringLiteral(
        "# OmaZettel citations: a bibliography (BibTeX .bib or CSL JSON) and an optional\n"
        "# citation style (.csl). Paths are relative to the notes folder, or absolute.\n");
    if (!m_settings.bibliography.isEmpty())
        text += QStringLiteral("bibliography: \"%1\"\n").arg(storedPath(m_root, m_settings.bibliography));
    if (!m_settings.style.isEmpty())
        text += QStringLiteral("csl: \"%1\"\n").arg(storedPath(m_root, m_settings.style));
    file.write(text.toUtf8());
    file.commit();
}

bool Citations::setBibliography(const QString &path) {
    if (m_root.isEmpty())
        return false;
    m_settings.bibliography = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    save();
    reload();
    return m_error.isEmpty();
}

bool Citations::setStyle(const QString &path) {
    if (m_root.isEmpty())
        return false;
    m_settings.style = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    save();
    reload();
    return true;
}

void Citations::reload() {
    m_settings = settingsFor(m_root);
    m_entries.clear();
    m_error.clear();
    const QString path = m_settings.bibliography;
    if (!path.isEmpty()) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            m_error = QStringLiteral("Could not read %1.").arg(QFileInfo(path).fileName());
        } else {
            const QByteArray data = file.readAll();
            const QString suffix = QFileInfo(path).suffix().toLower();
            m_entries = suffix == QLatin1String("json") ? parseCslJson(data)
                                                         : parseBibTeX(QString::fromUtf8(data));
            if (m_entries.isEmpty())
                m_error = QStringLiteral("No references found in %1.").arg(QFileInfo(path).fileName());
        }
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) {
        return a.key.compare(b.key, Qt::CaseInsensitive) < 0;
    });
    watch();
    ++m_revision;
    emit changed();
}

void Citations::watch() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    const QString path = m_settings.bibliography;
    if (path.isEmpty())
        return;
    // Watch the folder too: an export that replaces the file ends a file watch.
    if (QFileInfo(path).isFile())
        m_watcher.addPath(path);
    if (QFileInfo(QFileInfo(path).absolutePath()).isDir())
        m_watcher.addPath(QFileInfo(path).absolutePath());
}

QList<Citations::Entry> Citations::parseBibTeX(const QString &text) {
    QList<Entry> entries;
    int i = 0;
    const int n = int(text.size());
    while ((i = int(text.indexOf(QLatin1Char('@'), i))) >= 0) {
        ++i;
        int typeEnd = i;
        while (typeEnd < n && text.at(typeEnd).isLetter())
            ++typeEnd;
        const QString type = text.mid(i, typeEnd - i).toLower();
        int j = typeEnd;
        while (j < n && text.at(j).isSpace())
            ++j;
        if (j >= n || (text.at(j) != QLatin1Char('{') && text.at(j) != QLatin1Char('(')))
            continue;
        const QChar open = text.at(j);
        const QChar close = open == QLatin1Char('{') ? QLatin1Char('}') : QLatin1Char(')');
        // The whole entry, to its balanced closing bracket. Only the bracket
        // that opened it counts, so a "(" in a braced title does not.
        int depth = 0;
        int end = j;
        for (; end < n; ++end) {
            const QChar c = text.at(end);
            if (c == open)
                ++depth;
            else if (c == close && --depth == 0)
                break;
        }
        const QString body = text.mid(j + 1, end - j - 1);
        i = end;
        if (type == QLatin1String("comment") || type == QLatin1String("string")
                || type == QLatin1String("preamble") || type.isEmpty())
            continue;

        const int comma = int(body.indexOf(QLatin1Char(',')));
        if (comma <= 0)
            continue;
        Entry entry;
        entry.key = body.left(comma).trimmed();
        if (entry.key.isEmpty())
            continue;

        // Fields: name = {value} | "value" | bare.
        QHash<QString, QString> fields;
        int k = comma + 1;
        const int m = int(body.size());
        while (k < m) {
            while (k < m && (body.at(k).isSpace() || body.at(k) == QLatin1Char(',')))
                ++k;
            int nameEnd = k;
            while (nameEnd < m && (body.at(nameEnd).isLetterOrNumber() || body.at(nameEnd) == QLatin1Char('-')
                                   || body.at(nameEnd) == QLatin1Char('_')))
                ++nameEnd;
            const QString name = body.mid(k, nameEnd - k).toLower();
            k = nameEnd;
            while (k < m && body.at(k).isSpace())
                ++k;
            if (name.isEmpty() || k >= m || body.at(k) != QLatin1Char('=')) {
                // Not a field; skip to the next comma.
                const int next = int(body.indexOf(QLatin1Char(','), k));
                k = next < 0 ? m : next + 1;
                continue;
            }
            ++k;
            while (k < m && body.at(k).isSpace())
                ++k;
            QString value;
            if (k < m && body.at(k) == QLatin1Char('{')) {
                int level = 0;
                const int start = k;
                for (; k < m; ++k) {
                    if (body.at(k) == QLatin1Char('{'))
                        ++level;
                    else if (body.at(k) == QLatin1Char('}') && --level == 0)
                        break;
                }
                value = body.mid(start + 1, k - start - 1);
                ++k;
            } else if (k < m && body.at(k) == QLatin1Char('"')) {
                const int start = ++k;
                int level = 0;
                for (; k < m; ++k) {
                    if (body.at(k) == QLatin1Char('{'))
                        ++level;
                    else if (body.at(k) == QLatin1Char('}'))
                        --level;
                    else if (body.at(k) == QLatin1Char('"') && level == 0)
                        break;
                }
                value = body.mid(start, k - start);
                ++k;
            } else {
                const int start = k;
                while (k < m && body.at(k) != QLatin1Char(','))
                    ++k;
                value = body.mid(start, k - start).trimmed();
            }
            fields.insert(name, value);
        }
        entry.title = cleanField(fields.value(QStringLiteral("title")));
        entry.author = firstAuthor(cleanField(fields.value(QStringLiteral("author"),
                                                           fields.value(QStringLiteral("editor")))));
        static const QRegularExpression yearRe(QStringLiteral("\\d{4}"));
        const QString year = fields.value(QStringLiteral("year"), fields.value(QStringLiteral("date")));
        entry.year = yearRe.match(year).captured(0);
        entries.append(entry);
    }
    return entries;
}

QList<Citations::Entry> Citations::parseCslJson(const QByteArray &json) {
    QList<Entry> entries;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    const QJsonArray items = document.isArray() ? document.array()
                                                : document.object().value(QStringLiteral("items")).toArray();
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        Entry entry;
        entry.key = item.value(QStringLiteral("id")).toVariant().toString();
        if (entry.key.isEmpty())
            continue;
        entry.title = item.value(QStringLiteral("title")).toString().simplified();
        QStringList names;
        const QJsonArray authors = item.value(QStringLiteral("author")).toArray();
        for (const QJsonValue &author : authors) {
            const QJsonObject person = author.toObject();
            const QString family = person.value(QStringLiteral("family")).toString();
            names.append(family.isEmpty() ? person.value(QStringLiteral("literal")).toString() : family);
        }
        if (!names.isEmpty()) {
            entry.author = names.first();
            if (names.size() == 2)
                entry.author += QStringLiteral(" & ") + names.at(1);
            else if (names.size() > 2)
                entry.author += QStringLiteral(" et al.");
        }
        const QJsonObject issued = item.value(QStringLiteral("issued")).toObject();
        const QJsonArray parts = issued.value(QStringLiteral("date-parts")).toArray();
        if (!parts.isEmpty() && parts.first().toArray().size() > 0)
            entry.year = QString::number(parts.first().toArray().first().toVariant().toInt());
        else {
            static const QRegularExpression yearRe(QStringLiteral("\\d{4}"));
            entry.year = yearRe.match(issued.value(QStringLiteral("literal")).toString()
                                      + issued.value(QStringLiteral("raw")).toString()).captured(0);
        }
        entries.append(entry);
    }
    return entries;
}

QVariantList Citations::entries() const {
    QVariantList result;
    for (const Entry &entry : m_entries) {
        QString label = entry.author;
        if (!entry.year.isEmpty())
            label += (label.isEmpty() ? QString() : QStringLiteral(" ")) + entry.year;
        if (!entry.title.isEmpty())
            label += (label.isEmpty() ? QString() : QStringLiteral(" · ")) + entry.title;
        result.append(QVariantMap{{QStringLiteral("name"), entry.key},
                                  {QStringLiteral("title"), label},
                                  {QStringLiteral("folder"), QString()},
                                  {QStringLiteral("path"), QString()}});
    }
    return result;
}

QString Citations::describe(const QString &key) const {
    for (const Entry &entry : m_entries) {
        if (entry.key == key) {
            QString text = entry.author;
            if (!entry.year.isEmpty())
                text += QStringLiteral(" (%1)").arg(entry.year);
            if (!entry.title.isEmpty())
                text += QStringLiteral(" ") + entry.title;
            return text.trimmed();
        }
    }
    return QString();
}

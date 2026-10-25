#include "notetemplates.h"

#include "layout.h"
#include "vault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>

#include <pwd.h>
#include <unistd.h>

namespace {
const QString defaultTemplateSetting = QStringLiteral("notes/defaultTemplate");
const QString authorSetting = QStringLiteral("notes/author");
const QString templatesSubfolder = QStringLiteral(".omazettel/note-templates");

QString readText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}
}

NoteTemplates::NoteTemplates(QObject *parent) : QObject(parent) {}

QString NoteTemplates::folder() const {
    return m_root.isEmpty() ? QString() : QDir(m_root).filePath(templatesSubfolder);
}

QString NoteTemplates::defaultTemplate() const {
    return QSettings().value(defaultTemplateSetting, QStringLiteral("Note")).toString();
}

void NoteTemplates::setDefaultTemplate(const QString &name) {
    if (name == defaultTemplate())
        return;
    QSettings().setValue(defaultTemplateSetting, name);
    reload();
}

QString NoteTemplates::author() const {
    const QString saved = QSettings().value(authorSetting).toString();
    return saved.isEmpty() ? systemAuthor() : saved;
}

void NoteTemplates::setAuthor(const QString &author) {
    QSettings().setValue(authorSetting, author.trimmed());
    reload();
}

// The full name from the account, else the login name.
QString NoteTemplates::systemAuthor() {
    if (const passwd *entry = getpwuid(getuid())) {
        QString gecos = QString::fromLocal8Bit(entry->pw_gecos).section(QLatin1Char(','), 0, 0).trimmed();
        if (!gecos.isEmpty())
            return gecos;
        return QString::fromLocal8Bit(entry->pw_name);
    }
    return qEnvironmentVariable("USER");
}

void NoteTemplates::setRoot(const QString &notesRoot) {
    if (m_root == notesRoot)
        return;
    m_root = notesRoot;
    reload();
}

QList<QPair<QString, QString>> NoteTemplates::starterTemplates(const QString &notesRoot) {
    const QString meetings = Layout::meetingsFolder(notesRoot);
    const QString daily = Layout::dailyFolder(notesRoot);
    const QString literature = Layout::literatureFolder(notesRoot);
    return {
        {QStringLiteral("Note"),
         QStringLiteral(
             "---\n"
             "title: \"{{title}}\"\n"
             "author: \"{{author}}\"\n"
             "date: \"{{date}}\"\n"
             "id: \"{{id}}\"\n"
             "tags: []\n"
             "---\n"
             "\n"
             "{{cursor}}\n")},
        {QStringLiteral("Meeting"),
         QStringLiteral(
             "<!-- omazettel: name=\"{{date}} {{title}}\" folder=\"%1\" -->\n"
             "---\n"
             "title: \"{{title}}\"\n"
             "subtitle: \"Meeting notes\"\n"
             "author: \"{{author}}\"\n"
             "date: \"{{date}}\"\n"
             "tags: [meeting]\n"
             "attendees: []\n"
             "---\n"
             "\n"
             "**When:** {{date:dddd d MMMM yyyy}}, {{time}}  \n"
             "**Attendees:** \n"
             "\n"
             "## Agenda\n"
             "\n"
             "- {{cursor}}\n"
             "\n"
             "## Notes\n"
             "\n"
             "## Decisions\n"
             "\n"
             "## Action items\n"
             "\n"
             "- [ ] \n").arg(meetings)},
        {QStringLiteral("Daily"),
         QStringLiteral(
             "<!-- omazettel: name=\"{{date}}\" folder=\"%1\" existing=\"open\" -->\n"
             "---\n"
             "title: \"{{date:dddd d MMMM yyyy}}\"\n"
             "author: \"{{author}}\"\n"
             "date: \"{{date}}\"\n"
             "tags: [daily]\n"
             "---\n"
             "\n"
             "## Focus\n"
             "\n"
             "- {{cursor}}\n"
             "\n"
             "## Log\n"
             "\n"
             "## Notes\n").arg(daily)},
        {QStringLiteral("Literature note"),
         QStringLiteral(
             "<!-- omazettel: folder=\"%1\" -->\n"
             "---\n"
             "title: \"{{title}}\"\n"
             "author: \"{{author}}\"\n"
             "date: \"{{date}}\"\n"
             "source: \"\"\n"
             "url: \"\"\n"
             "tags: [literature]\n"
             "---\n"
             "\n"
             "## Summary\n"
             "\n"
             "{{cursor}}\n"
             "\n"
             "## Key ideas\n"
             "\n"
             "## Quotes\n"
             "\n"
             "## Related\n").arg(literature)},
    };
}

void NoteTemplates::ensureDefaults() {
    const QString dir = folder();
    if (dir.isEmpty() || QFileInfo(dir).exists())
        return;
    // A notes folder OmaZettel has never seen. If it is empty, it starts
    // with the numbered layout; one that has notes keeps its own folders.
    if (Layout::isEmpty(m_root))
        Layout::create(m_root);
    QDir().mkpath(dir);
    for (const auto &[name, text] : starterTemplates(m_root)) {
        QSaveFile file(QDir(dir).filePath(name + QStringLiteral(".md")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(text.toUtf8());
            file.commit();
        }
    }
    reload();
}

QVariantList NoteTemplates::list() const {
    QVariantList result;
    const QString dir = folder();
    if (dir.isEmpty())
        return result;
    QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.md"), QStringLiteral("*.markdown")},
                                                  QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : files) {
        const QString name = info.completeBaseName();
        result.append(QVariantMap{{QStringLiteral("name"), name},
                                  {QStringLiteral("path"), info.absoluteFilePath()},
                                  {QStringLiteral("needsTitle"), needsTitle(name)}});
    }
    return result;
}

QString NoteTemplates::pathOf(const QString &name) const {
    const QDir dir(folder());
    for (const QString &suffix : {QStringLiteral(".md"), QStringLiteral(".markdown")}) {
        const QString path = dir.filePath(name + suffix);
        if (QFileInfo(path).isFile())
            return path;
    }
    return QString();
}

bool NoteTemplates::needsTitle(const QString &name) const {
    const QString path = pathOf(name);
    if (path.isEmpty())
        return true;
    const Rendered probe = render(readText(path), QStringLiteral("\x01"), QString(), QString(),
                                  QDateTime::currentDateTime());
    return probe.fileName.contains(QChar(1)) || probe.content.contains(QChar(1));
}

QString NoteTemplates::yamlEscaped(const QString &value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return escaped;
}

NoteTemplates::Rendered NoteTemplates::render(const QString &templateText, const QString &title,
                                              const QString &author, const QString &folderName,
                                              const QDateTime &now) {
    Rendered rendered;
    QString text = templateText;

    // The settings line, if the template has one.
    static const QRegularExpression settingsRe(
        QStringLiteral("^\\s*<!--\\s*omazettel:(.*?)-->[ \\t]*\\n?"));
    QString namePattern = QStringLiteral("{{title}}");
    const QRegularExpressionMatch settings = settingsRe.match(text);
    if (settings.hasMatch()) {
        static const QRegularExpression pairRe(QStringLiteral("(\\w+)\\s*=\\s*\"([^\"]*)\""));
        auto it = pairRe.globalMatch(settings.captured(1));
        while (it.hasNext()) {
            const QRegularExpressionMatch pair = it.next();
            const QString key = pair.captured(1).toLower();
            if (key == QLatin1String("name"))
                namePattern = pair.captured(2);
            else if (key == QLatin1String("folder"))
                rendered.folder = pair.captured(2).trimmed();
            else if (key == QLatin1String("existing"))
                rendered.openExisting = pair.captured(2).trimmed().toLower() == QLatin1String("open");
        }
        text.remove(0, settings.capturedLength());
    }

    const QLocale locale;
    const auto fill = [&](const QString &source, bool yaml) {
        static const QRegularExpression placeholderRe(QStringLiteral("\\{\\{\\s*(\\w+)(?::([^}]*))?\\s*\\}\\}"));
        QString out;
        int last = 0;
        auto it = placeholderRe.globalMatch(source);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            out += source.mid(last, match.capturedStart() - last);
            last = match.capturedEnd();
            const QString key = match.captured(1).toLower();
            const QString format = match.captured(2).trimmed();
            QString value;
            bool known = true;
            if (key == QLatin1String("title")) value = title;
            else if (key == QLatin1String("author")) value = author;
            else if (key == QLatin1String("date"))
                value = format.isEmpty() ? now.date().toString(Qt::ISODate) : locale.toString(now, format);
            else if (key == QLatin1String("time"))
                value = format.isEmpty() ? now.time().toString(QStringLiteral("HH:mm")) : locale.toString(now, format);
            else if (key == QLatin1String("datetime")) value = now.toString(Qt::ISODate);
            else if (key == QLatin1String("weekday")) value = locale.dayName(now.date().dayOfWeek());
            else if (key == QLatin1String("id")) value = now.toString(QStringLiteral("yyyyMMddHHmmss"));
            else if (key == QLatin1String("folder")) value = folderName;
            else if (key == QLatin1String("cursor")) value = QStringLiteral("\x02");
            else known = false;
            if (!known) {
                out += match.captured(0);
                continue;
            }
            out += yaml && key != QLatin1String("cursor") ? yamlEscaped(value) : value;
        }
        out += source.mid(last);
        return out;
    };

    // Values in the YAML header are escaped so a title with quotes stays
    // valid for pandoc; the rest of the note gets them as typed.
    int bodyStart = 0;
    if (text.startsWith(QLatin1String("---\n"))) {
        static const QRegularExpression closeRe(QStringLiteral("\\n(---|\\.\\.\\.)[ \\t]*(\\n|$)"));
        const QRegularExpressionMatch close = closeRe.match(text, 3);
        if (close.hasMatch())
            bodyStart = close.capturedEnd();
    }
    QString content = fill(text.left(bodyStart), true) + fill(text.mid(bodyStart), false);

    rendered.cursor = int(content.indexOf(QChar(2)));
    content.remove(QChar(2));
    rendered.content = content;
    rendered.fileName = Vault::noteFileName(fill(namePattern, false).remove(QChar(2)));
    return rendered;
}

QVariantMap NoteTemplates::create(const QString &name, const QString &title,
                                  const QString &targetFolder, const QString &exactName) {
    return createAt(name, title, targetFolder, exactName, QDateTime::currentDateTime());
}

QVariantMap NoteTemplates::createAt(const QString &name, const QString &title,
                                    const QString &targetFolder, const QString &exactName,
                                    const QDateTime &when) {
    m_lastError.clear();
    if (m_root.isEmpty()) {
        m_lastError = QStringLiteral("Open a notes folder first.");
        return {};
    }
    const QString templatePath = pathOf(name);
    if (templatePath.isEmpty()) {
        m_lastError = QStringLiteral("There is no template named %1.").arg(name);
        return {};
    }

    const QDateTime now = when.isValid() ? when : QDateTime::currentDateTime();
    QString folderPath = targetFolder.isEmpty() ? m_root : targetFolder;
    Rendered rendered = render(readText(templatePath), title.trimmed(), author(),
                               QFileInfo(folderPath).fileName(), now);
    if (!rendered.folder.isEmpty() && exactName.isEmpty()) {
        const QString relative = QDir::cleanPath(rendered.folder);
        if (!relative.startsWith(QLatin1String("..")) && !QDir::isAbsolutePath(relative))
            folderPath = QDir(m_root).filePath(relative);
    }
    QString baseName = exactName.isEmpty() ? rendered.fileName : Vault::noteFileName(exactName);
    if (baseName.isEmpty())
        baseName = QStringLiteral("Untitled");

    QDir().mkpath(folderPath);
    const QString wanted = QDir(folderPath).filePath(baseName + QStringLiteral(".md"));
    if (rendered.openExisting && QFileInfo(wanted).isFile())
        return {{QStringLiteral("path"), wanted}, {QStringLiteral("cursor"), -1},
                {QStringLiteral("existed"), true}};

    const QString path = Vault::uniquePath(folderPath, baseName, QStringLiteral("md"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Could not create %1.").arg(QFileInfo(path).fileName());
        return {};
    }
    file.write(rendered.content.toUtf8());
    if (!file.commit()) {
        m_lastError = QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName());
        return {};
    }
    return {{QStringLiteral("path"), path}, {QStringLiteral("cursor"), rendered.cursor},
            {QStringLiteral("existed"), false}};
}

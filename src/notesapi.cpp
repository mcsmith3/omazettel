#include "notesapi.h"

#include "assistant.h"
#include "dashboard.h"
#include "exporter.h"
#include "layout.h"
#include "moc.h"
#include "sync.h"
#include "transcriber.h"
#include "vault.h"
#include "zettel.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <QSet>

#include <algorithm>

namespace {

std::function<void(const QString &)> &progressSink() {
    static std::function<void(const QString &)> sink;
    return sink;
}

void progress(const QString &message) {
    if (progressSink())
        progressSink()(message);
}

[[noreturn]] void fail(const QString &message) {
    throw NotesApi::Error{message};
}

QString str(const QJsonObject &args, const QString &key, const QString &fallback = QString()) {
    const QJsonValue value = args.value(key);
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble());
    return fallback;
}

bool flag(const QJsonObject &args, const QString &key, bool fallback) {
    const QJsonValue value = args.value(key);
    if (value.isBool())
        return value.toBool();
    if (value.isString())
        return value.toString() == QLatin1String("true");
    return fallback;
}

int integer(const QJsonObject &args, const QString &key, int fallback) {
    const QJsonValue value = args.value(key);
    if (value.isDouble())
        return value.toInt();
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

QStringList stringList(const QJsonValue &value) {
    QStringList result;
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            if (!item.toString().trimmed().isEmpty())
                result.append(item.toString().trimmed());
        }
    } else if (value.isString()) {
        for (const QString &item : value.toString().split(QLatin1Char(','), Qt::SkipEmptyParts))
            result.append(item.trimmed());
    }
    return result;
}

QString stripSuffix(QString name) {
    if (name.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        name.chop(3);
    else if (name.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive))
        name.chop(9);
    return name;
}

// Leading and trailing empty lines go; indentation stays.
QString trimBlankLines(const QString &text) {
    QStringList lines = QString(text).replace(QLatin1String("\r\n"), QLatin1String("\n")).split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
        lines.removeFirst();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

// "- ", "*", "- [ ]": a bullet a template leaves to be filled in.
bool isPlaceholder(const QString &line) {
    static const QRegularExpression re(QStringLiteral("^\\s*[-*+](\\s+\\[\\s?\\])?\\s*$"));
    return re.match(line).hasMatch();
}

bool isListLine(const QString &line) {
    static const QRegularExpression re(QStringLiteral("^\\s*([-*+]|\\d+[.)])\\s"));
    return re.match(line).hasMatch();
}

// What a section or note has, with the block added after it.
QString joinBlock(const QString &existing, const QString &block) {
    QStringList lines = trimBlankLines(existing).split(QLatin1Char('\n'));
    while (!lines.isEmpty() && (lines.last().trimmed().isEmpty() || isPlaceholder(lines.last())))
        lines.removeLast();
    const QString before = lines.join(QLatin1Char('\n'));
    const QString added = trimBlankLines(block);
    if (before.isEmpty())
        return added;
    if (added.isEmpty())
        return before;
    const QString firstAdded = added.left(added.indexOf(QLatin1Char('\n')));
    const bool list = isListLine(lines.last()) && isListLine(firstAdded);
    return before + (list ? QStringLiteral("\n") : QStringLiteral("\n\n")) + added;
}

QString yamlScalar(const QJsonValue &value) {
    switch (value.type()) {
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: {
        const double number = value.toDouble();
        return number == double(qint64(number)) ? QString::number(qint64(number)) : QString::number(number);
    }
    case QJsonValue::String:
        return QLatin1Char('"') + NoteTemplates::yamlEscaped(value.toString()) + QLatin1Char('"');
    case QJsonValue::Array: {
        QStringList items;
        for (const QJsonValue &item : value.toArray())
            items.append(yamlScalar(item));
        return QLatin1Char('[') + items.join(QStringLiteral(", ")) + QLatin1Char(']');
    }
    case QJsonValue::Object:
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    default:
        return QString();
    }
}

QJsonValue yamlValue(QString raw) {
    raw = raw.trimmed();
    if (raw.isEmpty())
        return QJsonValue(QString());
    if (raw.size() >= 2 && ((raw.startsWith(QLatin1Char('"')) && raw.endsWith(QLatin1Char('"')))
                            || (raw.startsWith(QLatin1Char('\'')) && raw.endsWith(QLatin1Char('\''))))) {
        QString inner = raw.mid(1, raw.size() - 2);
        if (raw.startsWith(QLatin1Char('"'))) {
            inner.replace(QLatin1String("\\\""), QLatin1String("\""));
            inner.replace(QLatin1String("\\\\"), QLatin1String("\\"));
        } else {
            inner.replace(QLatin1String("''"), QLatin1String("'"));
        }
        return inner;
    }
    if (raw.startsWith(QLatin1Char('[')) && raw.endsWith(QLatin1Char(']'))) {
        QJsonArray items;
        const QString inner = raw.mid(1, raw.size() - 2);
        // Splits on commas outside quotes.
        QString current;
        QChar quote;
        for (const QChar c : inner) {
            if (!quote.isNull()) {
                if (c == quote)
                    quote = QChar();
                current += c;
            } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                quote = c;
                current += c;
            } else if (c == QLatin1Char(',')) {
                if (!current.trimmed().isEmpty())
                    items.append(yamlValue(current));
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.trimmed().isEmpty())
            items.append(yamlValue(current));
        return items;
    }
    if (raw == QLatin1String("true") || raw == QLatin1String("false"))
        return raw == QLatin1String("true");
    bool ok = false;
    const double number = raw.toDouble(&ok);
    if (ok && !raw.contains(QLatin1Char('-')))
        return number;
    return raw;
}

QString isoDate(const QDateTime &when) {
    return when.toString(Qt::ISODate);
}

// "2026-10-22", "today", "yesterday", "tomorrow", "-3" (days ago).
QDate parseDay(const QString &value) {
    const QString day = value.trimmed().toLower();
    const QDate today = QDate::currentDate();
    if (day.isEmpty() || day == QLatin1String("today"))
        return today;
    if (day == QLatin1String("yesterday"))
        return today.addDays(-1);
    if (day == QLatin1String("tomorrow"))
        return today.addDays(1);
    bool ok = false;
    const int offset = day.toInt(&ok);
    if (ok && (day.startsWith(QLatin1Char('-')) || day.startsWith(QLatin1Char('+'))))
        return today.addDays(offset);
    const QDate parsed = QDate::fromString(day, Qt::ISODate);
    if (!parsed.isValid())
        fail(QStringLiteral("“%1” is not a date. Use YYYY-MM-DD, today, yesterday or -N.").arg(value));
    return parsed;
}

// "2026-10-01", "7d" (days), "12h" (hours).
QDateTime parseSince(const QString &value) {
    static const QRegularExpression agoRe(QStringLiteral("^(\\d+)\\s*([dhw])$"));
    const QRegularExpressionMatch ago = agoRe.match(value.trimmed().toLower());
    const QDateTime now = QDateTime::currentDateTime();
    if (ago.hasMatch()) {
        const int amount = ago.captured(1).toInt();
        const QString unit = ago.captured(2);
        if (unit == QLatin1String("h"))
            return now.addSecs(-3600LL * amount);
        return now.addDays(-(unit == QLatin1String("w") ? 7 : 1) * amount);
    }
    return QDateTime(parseDay(value), QTime(0, 0));
}

QString plainSnippet(QString html) {
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    html.remove(tagRe);
    html.replace(QLatin1String("&lt;"), QLatin1String("<"));
    html.replace(QLatin1String("&gt;"), QLatin1String(">"));
    html.replace(QLatin1String("&quot;"), QLatin1String("\""));
    html.replace(QLatin1String("&#39;"), QLatin1String("'"));
    html.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return html.simplified();
}

QJsonArray toJsonArray(const QStringList &list) {
    QJsonArray array;
    for (const QString &item : list)
        array.append(item);
    return array;
}

// Shared parameter descriptions.
NotesApi::Param noteParam(const QString &description = QStringLiteral(
                              "The note: its name, title, or path in the notes folder (\"Meetings/Kickoff\")")) {
    return {QStringLiteral("note"), QStringLiteral("string"), description, true, true};
}
NotesApi::Param linkNotesParam() {
    return {QStringLiteral("linkNotes"), QStringLiteral("boolean"),
            QStringLiteral("Make empty notes for [[links]] that name no note yet, as the app does on save "
                           "(default: the app's setting, on unless turned off)")};
}

}

// ── The tools ───────────────────────────────────────────────────────────────

const QList<NotesApi::Tool> &NotesApi::tools() {
    static const QList<Tool> list = {
        {QStringLiteral("vault_info"), QStringLiteral("info"),
         QStringLiteral("The notes folder in use: where it is, how many notes, the note templates and export "
                        "profiles there are, sync status, whether recordings can be transcribed, and the folder layout "
                        "with the person's rules for agents (01-system/SYSTEM-RULES.md and HOW-I-WORK.md): "
                        "call it first and follow those rules."),
         {}, true, false, &NotesApi::info},
        {QStringLiteral("list_notes"), QStringLiteral("list"),
         QStringLiteral("Lists notes, optionally only those in a folder, with a tag, or changed recently."),
         {{QStringLiteral("folder"), QStringLiteral("string"), QStringLiteral("Only notes in this folder (and its subfolders), relative to the notes folder")},
          {QStringLiteral("tag"), QStringLiteral("string"), QStringLiteral("Only notes with this tag (without #)")},
          {QStringLiteral("since"), QStringLiteral("string"), QStringLiteral("Only notes changed since: a date (2026-10-01) or an age (7d, 12h, 2w)")},
          {QStringLiteral("sort"), QStringLiteral("string"), QStringLiteral("path (default) or modified (newest first)")},
          {QStringLiteral("limit"), QStringLiteral("integer"), QStringLiteral("At most this many (default 500)")}},
         true, false, &NotesApi::listNotes},
        {QStringLiteral("search_notes"), QStringLiteral("search"),
         QStringLiteral("Full-text search across all notes, as in the app: every word must appear, \"quoted "
                        "phrases\" as written, #tag or tag:name narrows to a tag. Returns matching notes with "
                        "snippets, title matches first."),
         {{QStringLiteral("query"), QStringLiteral("string"), QStringLiteral("What to look for"), true, true},
          {QStringLiteral("limit"), QStringLiteral("integer"), QStringLiteral("At most this many notes (default 20)")}},
         true, false, &NotesApi::search},
        {QStringLiteral("read_note"), QStringLiteral("show"),
         QStringLiteral("Reads a note: its text, title, tags, YAML header, headings, the [[links]] in it and "
                        "the notes that link to it (backlinks)."),
         {noteParam(),
          {QStringLiteral("section"), QStringLiteral("string"), QStringLiteral("Only this heading's section of the text")}},
         true, false, &NotesApi::readNote},
        {QStringLiteral("create_note"), QStringLiteral("new"),
         QStringLiteral("Makes a note from a note template (Note, Meeting, Daily, Literature note, or the "
                        "notes folder's own), so it gets the template's pandoc header and headings. Fill it "
                        "with content (put where the template's cursor is), sections (text under named "
                        "headings, replacing the template's empty ones) and meta (YAML header keys). Content "
                        "that starts with its own --- header is written as given, with no template."),
         {{QStringLiteral("title"), QStringLiteral("string"), QStringLiteral("The note's title, which also names the file (templates may add the date)"), false, true},
          {QStringLiteral("template"), QStringLiteral("string"), QStringLiteral("Note template name (default Note; \"none\" for a bare note)")},
          {QStringLiteral("folder"), QStringLiteral("string"), QStringLiteral("Folder for the note, relative to the notes folder, made if missing (default: the template's folder, else the top)")},
          {QStringLiteral("content"), QStringLiteral("string"), QStringLiteral("Markdown to put in the note")},
          {QStringLiteral("sections"), QStringLiteral("object"), QStringLiteral("Heading → Markdown, e.g. {\"Notes\": \"...\", \"Action items\": \"- [ ] ...\"}; headings the template lacks are added")},
          {QStringLiteral("meta"), QStringLiteral("object"), QStringLiteral("YAML header keys to set, e.g. {\"attendees\": [\"Ann\", \"Bo\"], \"project\": \"Atlas\"}")},
          {QStringLiteral("tags"), QStringLiteral("array"), QStringLiteral("Tags to add to the header")},
          linkNotesParam(),
          {QStringLiteral("open"), QStringLiteral("boolean"), QStringLiteral("Open the note in the OmaZettel window afterwards")}},
         false, false, &NotesApi::createNote},
        {QStringLiteral("append_to_note"), QStringLiteral("append"),
         QStringLiteral("Adds Markdown to a note: at the end, or at the end of a section (made if the note "
                        "has no such heading). List items join the list already there."),
         {noteParam(),
          {QStringLiteral("text"), QStringLiteral("string"), QStringLiteral("The Markdown to add"), true, true},
          {QStringLiteral("section"), QStringLiteral("string"), QStringLiteral("Heading to add under, e.g. \"Action items\"")},
          {QStringLiteral("create"), QStringLiteral("boolean"), QStringLiteral("Make the note (from the Note template) if there is none by that name")},
          linkNotesParam()},
         false, false, &NotesApi::appendToNote},
        {QStringLiteral("replace_section"), QStringLiteral("set-section"),
         QStringLiteral("Replaces what is under one heading of a note (up to the next heading of the same "
                        "level), adding the heading if the note lacks it."),
         {noteParam(),
          {QStringLiteral("section"), QStringLiteral("string"), QStringLiteral("The heading"), true},
          {QStringLiteral("text"), QStringLiteral("string"), QStringLiteral("The section's new Markdown (empty clears it)"), true, true},
          linkNotesParam()},
         false, true, &NotesApi::replaceSectionTool},
        {QStringLiteral("set_metadata"), QStringLiteral("set-meta"),
         QStringLiteral("Sets keys in a note's YAML (pandoc) header: title, tags, date, attendees or any "
                        "other. A null value removes the key."),
         {noteParam(),
          {QStringLiteral("meta"), QStringLiteral("object"), QStringLiteral("Keys and values; on the command line key=value pairs (values may be JSON)"), true, true}},
         false, false, &NotesApi::setMetadata},
        {QStringLiteral("write_note"), QStringLiteral("write"),
         QStringLiteral("Replaces a note's whole text. Prefer append_to_note, replace_section or set_metadata "
                        "for changes to part of a note; read the note first."),
         {noteParam(),
          {QStringLiteral("text"), QStringLiteral("string"), QStringLiteral("The note's new text"), true, true},
          {QStringLiteral("create"), QStringLiteral("boolean"), QStringLiteral("Make the note if there is none by that name")},
          linkNotesParam()},
         false, true, &NotesApi::writeNote},
        {QStringLiteral("daily_note"), QStringLiteral("daily"),
         QStringLiteral("Opens (making it if needed, from the Daily template) the daily note for a day, and "
                        "optionally adds to it. Returns its text."),
         {{QStringLiteral("date"), QStringLiteral("string"), QStringLiteral("YYYY-MM-DD, today (default), yesterday, tomorrow or -N days"), false, true},
          {QStringLiteral("append"), QStringLiteral("string"), QStringLiteral("Markdown to add")},
          {QStringLiteral("section"), QStringLiteral("string"), QStringLiteral("Heading to add under (default Log)")}},
         false, false, &NotesApi::dailyNote},
        {QStringLiteral("capture"), QStringLiteral("capture"),
         QStringLiteral("Quick capture: adds a timestamped bullet to today's daily note, under Log."),
         {{QStringLiteral("text"), QStringLiteral("string"), QStringLiteral("The thought, task or link"), true, true},
          {QStringLiteral("section"), QStringLiteral("string"), QStringLiteral("Heading to add under (default Log)")},
          {QStringLiteral("task"), QStringLiteral("boolean"), QStringLiteral("Add it as a task (- [ ]) without a time")}},
         false, false, &NotesApi::capture},
        {QStringLiteral("list_tags"), QStringLiteral("tags"),
         QStringLiteral("Every tag in the notes, with how many notes use it."),
         {}, true, false, &NotesApi::listTags},
        {QStringLiteral("list_tasks"), QStringLiteral("tasks"),
         QStringLiteral("Markdown tasks (- [ ] and - [x]) across the notes, e.g. open action items from meetings."),
         {{QStringLiteral("status"), QStringLiteral("string"), QStringLiteral("open (default), done or all")},
          {QStringLiteral("note"), QStringLiteral("string"), QStringLiteral("Only this note's tasks")},
          {QStringLiteral("folder"), QStringLiteral("string"), QStringLiteral("Only notes in this folder")},
          {QStringLiteral("tag"), QStringLiteral("string"), QStringLiteral("Only notes with this tag")}},
         true, false, &NotesApi::listTasks},
        {QStringLiteral("link_report"), QStringLiteral("links"),
         QStringLiteral("Checks the links between notes: [[links]] that name no note (broken) and notes no "
                        "other note links to (orphans)."),
         {}, true, false, &NotesApi::linkReport},
        {QStringLiteral("dashboard"), QStringLiteral("dashboard"),
         QStringLiteral("An overview of the notes, as the app's dashboard shows it: notes made and changed "
                        "each day (from git history when the notes folder has it, else file dates), the "
                        "streak of days written, open tasks with their notes, projects with their note and "
                        "open-task counts, notes needing a look (broken links, notes nothing links to, empty "
                        "notes), top tags and recently changed notes. Good for \"what did I work on this "
                        "week?\" or a project status."),
         {{QStringLiteral("days"), QStringLiteral("integer"), QStringLiteral("How many days of activity, up to today (default 365)"), false, true}},
         true, false, &NotesApi::dashboard},
        {QStringLiteral("rename_note"), QStringLiteral("rename"),
         QStringLiteral("Renames a note or folder and rewrites every [[link]] to it, as the app does."),
         {{QStringLiteral("note"), QStringLiteral("string"), QStringLiteral("The note (or folder) to rename"), true, true},
          {QStringLiteral("name"), QStringLiteral("string"), QStringLiteral("The new name"), true, true}},
         false, false, &NotesApi::renameNote},
        {QStringLiteral("tidy_names"), QStringLiteral("tidy-names"),
         QStringLiteral("Renames existing notes whose file names have spaces to use the file name separator "
                        "(Settings; hyphen by default), rewriting every [[link]] to them. Shows the plan "
                        "unless apply is set. Folders and titles inside notes are left as they are."),
         {{QStringLiteral("folder"), QStringLiteral("string"), QStringLiteral("Only notes in this folder (default: all)"), false, true},
          {QStringLiteral("apply"), QStringLiteral("boolean"), QStringLiteral("Rename them (default: only show what would change)")}},
         false, false, &NotesApi::tidyNames},
        {QStringLiteral("setup_layout"), QStringLiteral("setup-layout"),
         QStringLiteral("Gives the notes folder the numbered layout: 01-system (SYSTEM-RULES.md, HOW-I-WORK.md), "
                        "02-context, 03-projects, 04-outputs, 99-archive. Moves the older Daily, Meetings, "
                        "Literature and Archive folders into it, puts transcript notes in a transcripts folder beside "
                        "their meeting notes, points the note templates there and keeps [[links]] working. Never overwrites a file. Shows the plan unless apply is set."),
         {{QStringLiteral("apply"), QStringLiteral("boolean"), QStringLiteral("Make the changes (default: only show what would change)")}},
         false, false, &NotesApi::setupLayout},
        {QStringLiteral("update_maps"), QStringLiteral("maps"),
         QStringLiteral("Brings the maps of content up to date: 03-projects/projects-moc links each project's "
                        "map, and each project's map (03-projects/<project>/<project>-moc) links every note in "
                        "that project except transcripts. OmaZettel keeps them current on its own after every "
                        "change; this shows which would change, and writes them when apply is set."),
         {{QStringLiteral("apply"), QStringLiteral("boolean"), QStringLiteral("Write the maps (default: only show which would change)")}},
         false, false, &NotesApi::updateMaps},
        {QStringLiteral("move_note"), QStringLiteral("move"),
         QStringLiteral("Moves a note or folder into another folder (made if missing) and rewrites [[links]] "
                        "that name it by path."),
         {{QStringLiteral("note"), QStringLiteral("string"), QStringLiteral("The note (or folder) to move"), true, true},
          {QStringLiteral("folder"), QStringLiteral("string"), QStringLiteral("Where to, relative to the notes folder (\"\" or / for the top)"), true, true}},
         false, false, &NotesApi::moveNote},
        {QStringLiteral("trash_note"), QStringLiteral("trash"),
         QStringLiteral("Moves a note to the desktop trash, where it can be restored from."),
         {noteParam()}, false, true, &NotesApi::trashNote},
        {QStringLiteral("list_templates"), QStringLiteral("templates"),
         QStringLiteral("The note templates, with their headings and whether they need a title."),
         {}, true, false, &NotesApi::listTemplates},
        {QStringLiteral("export_note"), QStringLiteral("export"),
         QStringLiteral("Exports a note, or a whole folder as one document, with pandoc using an export "
                        "profile (PDF, Word, HTML, ...). Returns the file written."),
         {{QStringLiteral("note"), QStringLiteral("string"), QStringLiteral("The note or folder"), true, true},
          {QStringLiteral("profile"), QStringLiteral("string"), QStringLiteral("Export profile name, e.g. PDF or Word (default: the one used last)")},
          {QStringLiteral("output"), QStringLiteral("string"), QStringLiteral("Where to write (default: as set in the app)")}},
         false, false, &NotesApi::exportNote},
        {QStringLiteral("sync"), QStringLiteral("sync"),
         QStringLiteral("Git sync, as the app's Sync now: saves a version, then pulls and pushes when there is "
                        "a remote (GitHub, GitLab, Codeberg, Forgejo, Gitea, Gogs or any git server). status only "
                        "reports; check tests signing in and writing without changing anything; remote sets the "
                        "address; create makes a repository on a server; token keeps an access token for https; "
                        "fixEmail switches to GitHub's no-reply address when GitHub blocks a private email."),
         {{QStringLiteral("status"), QStringLiteral("boolean"), QStringLiteral("Only report, do not sync")},
          {QStringLiteral("check"), QStringLiteral("boolean"), QStringLiteral("Test the connection: can this computer read and write the remote?")},
          {QStringLiteral("remote"), QStringLiteral("string"), QStringLiteral("Set the remote's address (https://host/you/notes.git or git@host:you/notes.git); \"\" removes it")},
          {QStringLiteral("create"), QStringLiteral("string"), QStringLiteral("Make a repository and sync to it: github, gitlab, codeberg, forgejo, gitea or gogs")},
          {QStringLiteral("server"), QStringLiteral("string"), QStringLiteral("The server for create, for self-hosted ones (default gitlab.com, codeberg.org)")},
          {QStringLiteral("name"), QStringLiteral("string"), QStringLiteral("The repository name for create (default: the notes folder's name)")},
          {QStringLiteral("public"), QStringLiteral("boolean"), QStringLiteral("Make the created repository public (default private)")},
          {QStringLiteral("user"), QStringLiteral("string"), QStringLiteral("Username for token (default: from the server, or oauth2)")},
          {QStringLiteral("token"), QStringLiteral("string"), QStringLiteral("An access token, kept in git's credential store (the desktop keyring); - reads it from standard input, or set OMAZETTEL_GIT_TOKEN")},
          {QStringLiteral("fixEmail"), QStringLiteral("boolean"), QStringLiteral("Use GitHub's no-reply email for these notes, re-sign unsent versions, and sync")}},
         false, false, &NotesApi::sync},
        {QStringLiteral("transcribe"), QStringLiteral("transcribe"),
         QStringLiteral("Turns a meeting into text: caption files (.vtt, .srt from Teams, Zoom, Meet) become "
                        "\"Speaker: words\" turns; .docx and .txt transcripts are read; recordings (audio or "
                        "video) are transcribed on this computer with Voxtype or whisper.cpp; voxtype:latest "
                        "imports Voxtype's last recorded meeting. With note, an AI (Omarchy's default agent, or "
                        "the one named by ai) then writes a meeting note from it: agenda, notes, decisions and "
                        "action items, with the transcript in a linked note of its own in a transcripts folder beside it."),
         {{QStringLiteral("file"), QStringLiteral("string"), QStringLiteral("The recording or transcript, or voxtype:latest"), false, true},
          {QStringLiteral("note"), QStringLiteral("string"), QStringLiteral("Write a meeting note, with this title (empty: the AI picks one)"), false, false, true},
          {QStringLiteral("ai"), QStringLiteral("string"), QStringLiteral("The AI that writes the note: claude, codex, gemini, opencode, ollama:<model>, anthropic[:<model>], openai:<model>, or none for the transcript alone (default: see omazettel ai)")},
          {QStringLiteral("template"), QStringLiteral("string"), QStringLiteral("Note template for the note (default Meeting)")},
          {QStringLiteral("folder"), QStringLiteral("string"), QStringLiteral("Folder for the note (default: the template's)")},
          {QStringLiteral("transcript"), QStringLiteral("boolean"), QStringLiteral("Keep the transcript in a separate note, linked from the meeting note (default on)")},
          {QStringLiteral("model"), QStringLiteral("string"), QStringLiteral("whisper.cpp model name or file for recordings (default: Voxtype's model, else the best downloaded)")},
          {QStringLiteral("language"), QStringLiteral("string"), QStringLiteral("Spoken language code for whisper, e.g. en (default: detect)")},
          {QStringLiteral("timestamps"), QStringLiteral("boolean"), QStringLiteral("Start each turn with its time")},
          {QStringLiteral("downloadModel"), QStringLiteral("string"), QStringLiteral("Download this whisper.cpp model (tiny, base.en, small.en, large-v3-turbo, ...) instead")}},
         false, false, &NotesApi::transcribe},
        {QStringLiteral("ai_status"), QStringLiteral("ai"),
         QStringLiteral("Which AI writes meeting notes and whether it is ready, the agents installed, and "
                        "whether recordings can be transcribed, with what to install if not. set saves a choice."),
         {{QStringLiteral("set"), QStringLiteral("string"), QStringLiteral("Save the AI to use: claude, codex, ollama:llama3.2, ...; \"default\" goes back to Omarchy's default agent"), false, true},
          {QStringLiteral("test"), QStringLiteral("boolean"), QStringLiteral("Send the AI a tiny request to check it answers")}},
         true, false, &NotesApi::aiStatus},
    };
    return list;
}

const NotesApi::Tool *NotesApi::toolForVerb(const QString &verb) {
    for (const Tool &tool : tools()) {
        if (tool.verb == verb)
            return &tool;
    }
    return nullptr;
}

const NotesApi::Tool *NotesApi::toolNamed(const QString &name) {
    for (const Tool &tool : tools()) {
        if (tool.name == name)
            return &tool;
    }
    return nullptr;
}

QJsonObject NotesApi::inputSchema(const Tool &tool) {
    QJsonObject properties;
    QJsonArray required;
    for (const Param &param : tool.params) {
        QJsonObject property{{QStringLiteral("type"), param.type},
                             {QStringLiteral("description"), param.description}};
        if (param.type == QLatin1String("array"))
            property.insert(QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
        properties.insert(param.name, property);
        if (param.required)
            required.append(param.name);
    }
    QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties}};
    if (!required.isEmpty())
        schema.insert(QStringLiteral("required"), required);
    return schema;
}

// ── Opening ─────────────────────────────────────────────────────────────────

void NotesApi::setProgress(std::function<void(const QString &)> sink) {
    progressSink() = std::move(sink);
}

NotesApi::NotesApi() = default;
NotesApi::~NotesApi() = default;

bool NotesApi::open(const QString &folder, QString *error) {
    QString wanted = folder;
    if (wanted.isEmpty())
        wanted = qEnvironmentVariable("OMAZETTEL_VAULT");
    if (wanted.isEmpty())
        wanted = QSettings().value(QStringLiteral("notes/folder")).toString();
    if (wanted.isEmpty()) {
        *error = QStringLiteral("No notes folder: open one in OmaZettel, or pass --vault <folder> "
                                "(or set OMAZETTEL_VAULT).");
        return false;
    }
    if (wanted.startsWith(QLatin1String("~/")))
        wanted = QDir::homePath() + wanted.mid(1);
    const QFileInfo info(wanted);
    if (!info.isDir()) {
        *error = QStringLiteral("%1 is not a folder.").arg(wanted);
        return false;
    }
    m_root = info.canonicalFilePath();
    m_index.setRoot(m_root);
    m_templates.setRoot(m_root);
    m_templates.ensureDefaults();
    refreshIndex(true);
    return true;
}

void NotesApi::refreshIndex(bool force) {
    // A long-running server (MCP) picks up edits made in the app or
    // elsewhere; a scan of a few thousand notes takes a moment.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - m_lastScan < 1500)
        return;
    m_index.rescanNow();
    m_lastScan = now;
}

QJsonObject NotesApi::call(const Tool &tool, const QJsonObject &arguments) {
    if (m_root.isEmpty())
        fail(QStringLiteral("No notes folder is open."));
    refreshIndex();
    QJsonObject result = tool.run(*this, arguments);
    if (!tool.readOnly) {
        // Notes made, moved or removed: the maps of content follow.
        // A preview (a tool with apply, called without it) writes nothing.
        bool preview = false;
        for (const Param &param : tool.params)
            preview = preview || (param.name == QLatin1String("apply") && !arguments.value(QStringLiteral("apply")).toBool());
        if (Moc::enabled() && !preview && tool.name != QLatin1String("update_maps")) {
            QJsonArray maps;
            for (const QString &map : Moc::update(m_root)) {
                m_index.updateFile(QDir(m_root).filePath(map));
                maps.append(map);
            }
            if (!maps.isEmpty())
                result.insert(QStringLiteral("mapsUpdated"), maps);
        }
        m_lastScan = 0;
    }
    return result;
}

// ── Paths and files ─────────────────────────────────────────────────────────

bool NotesApi::inside(const QString &path) const {
    return path == m_root || path.startsWith(m_root + QLatin1Char('/'));
}

QString NotesApi::relative(const QString &path) const {
    return inside(path) && path != m_root ? path.mid(m_root.size() + 1) : path;
}

QString NotesApi::notePath(const QString &reference) const {
    QString ref = reference.trimmed();
    if (ref.startsWith(QLatin1String("[[")) && ref.endsWith(QLatin1String("]]")))
        ref = ref.mid(2, ref.size() - 4);
    if (ref.contains(QLatin1Char('|')))
        ref = ref.left(ref.indexOf(QLatin1Char('|')));
    if (ref.isEmpty())
        fail(QStringLiteral("Name a note."));
    if (ref.startsWith(QLatin1String("~/")))
        ref = QDir::homePath() + ref.mid(1);

    // A file: absolute, in the notes folder, or from where the command runs.
    QStringList files;
    if (QDir::isAbsolutePath(ref)) {
        files << ref << ref + QStringLiteral(".md");
    } else {
        const QString inRoot = QDir(m_root).filePath(ref);
        files << inRoot << inRoot + QStringLiteral(".md") << QDir::current().filePath(ref);
    }
    for (const QString &file : std::as_const(files)) {
        const QFileInfo info(file);
        if (info.isFile() && Vault::isNoteFile(file) && inside(info.canonicalFilePath()))
            return info.canonicalFilePath();
    }

    // A name, a path without .md, or a title.
    const QString key = Vault::matchKey(stripSuffix(ref));
    QStringList byName;
    QStringList byTitle;
    for (const QVariant &entry : m_index.notes()) {
        const QVariantMap note = entry.toMap();
        const QString path = note.value(QStringLiteral("path")).toString();
        if (Vault::matchKey(note.value(QStringLiteral("name")).toString()) == key
                || Vault::matchKey(stripSuffix(relative(path))) == key)
            byName.append(path);
        else if (Vault::matchKey(note.value(QStringLiteral("title")).toString()) == key)
            byTitle.append(path);
    }
    const QStringList &found = byName.isEmpty() ? byTitle : byName;
    if (found.size() == 1)
        return found.first();
    if (found.size() > 1) {
        QStringList names;
        for (const QString &path : found)
            names.append(relative(path));
        fail(QStringLiteral("“%1” matches several notes: %2. Name one by its path.")
                 .arg(reference, names.join(QStringLiteral(", "))));
    }
    fail(QStringLiteral("There is no note “%1”. Search for it first.").arg(reference));
}

QString NotesApi::folderPath(const QString &reference, bool create) const {
    QString ref = reference.trimmed();
    if (ref.isEmpty() || ref == QLatin1String("/") || ref == QLatin1String("."))
        return m_root;
    if (ref.startsWith(QLatin1String("~/")))
        ref = QDir::homePath() + ref.mid(1);
    const QString path = QDir::cleanPath(QDir::isAbsolutePath(ref) ? ref : QDir(m_root).filePath(ref));
    if (!inside(path))
        fail(QStringLiteral("%1 is outside the notes folder.").arg(reference));
    if (path.mid(m_root.size()).contains(QLatin1String("/.")))
        fail(QStringLiteral("%1 is a hidden folder.").arg(reference));
    if (!QFileInfo(path).isDir()) {
        if (!create)
            fail(QStringLiteral("There is no folder %1.").arg(reference));
        if (!QDir().mkpath(path))
            fail(QStringLiteral("Could not make the folder %1.").arg(reference));
    }
    return path;
}

QString NotesApi::read(const QString &path) const {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        fail(QStringLiteral("Could not read %1.").arg(relative(path)));
    return QString::fromUtf8(file.readAll());
}

void NotesApi::write(const QString &path, const QString &text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    file.setDirectWriteFallback(true);
    QString out = text;
    if (!out.endsWith(QLatin1Char('\n')))
        out += QLatin1Char('\n');
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(out.toUtf8()) < 0 || !file.commit())
        fail(QStringLiteral("Could not write %1.").arg(relative(path)));
}

QJsonObject NotesApi::noteSummary(const QString &path) const {
    const QFileInfo info(path);
    QJsonObject note{{QStringLiteral("path"), relative(path)},
                     {QStringLiteral("title"), m_index.titleOf(path)},
                     {QStringLiteral("tags"), toJsonArray(m_index.tagsOf(path))},
                     {QStringLiteral("modified"), isoDate(info.lastModified())}};
    return note;
}

QJsonArray NotesApi::afterWrite(const QString &path, const QJsonObject &args) {
    m_index.updateFile(path);
    QJsonArray made;
    const bool setting = QSettings().value(QStringLiteral("notes/autoCreateLinks"), true).toBool();
    if (!flag(args, QStringLiteral("linkNotes"), setting))
        return made;
    for (const QString &target : m_index.missingTargets(path)) {
        if (target.contains(QLatin1String("..")))
            continue;
        const int slash = int(target.lastIndexOf(QLatin1Char('/')));
        const QString folder = slash < 0 ? QFileInfo(path).absolutePath()
                                         : QDir(m_root).filePath(target.left(slash).remove(QRegularExpression(QStringLiteral("^/+"))));
        const QString name = slash < 0 ? target : target.mid(slash + 1);
        if (Vault::cleanName(name).isEmpty())
            continue;
        QString created;
        QDir().mkpath(folder);
        if (!m_templates.pathOf(QStringLiteral("Note")).isEmpty()) {
            const QVariantMap result = m_templates.create(QStringLiteral("Note"), name, folder, name);
            created = result.value(QStringLiteral("path")).toString();
        } else {
            created = Vault::uniquePath(folder, Vault::noteFileName(name), QStringLiteral("md"));
            write(created, QStringLiteral("# %1\n").arg(name));
        }
        if (!created.isEmpty()) {
            m_index.updateFile(created);
            made.append(relative(created));
        }
    }
    return made;
}

QString NotesApi::makeNote(const QString &title, const QString &templateName, const QString &folder,
                           const QDateTime &when, bool *existed, int *cursor) {
    *existed = false;
    if (cursor)
        *cursor = -1;
    if (templateName.isEmpty() || templateName.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) {
        const QString name = Vault::noteFileName(stripSuffix(title));
        const QString path = Vault::uniquePath(folder.isEmpty() ? m_root : folder,
                                               name.isEmpty() ? QStringLiteral("Untitled") : name,
                                               QStringLiteral("md"));
        write(path, QStringLiteral("# %1\n").arg(title.trimmed().isEmpty() ? QStringLiteral("Untitled") : title.trimmed()));
        return path;
    }
    QString name = templateName;
    // Template names match without regard to case.
    for (const QVariant &entry : m_templates.list()) {
        const QString candidate = entry.toMap().value(QStringLiteral("name")).toString();
        if (candidate.compare(templateName, Qt::CaseInsensitive) == 0)
            name = candidate;
    }
    if (m_templates.pathOf(name).isEmpty()) {
        QStringList names;
        for (const QVariant &entry : m_templates.list())
            names.append(entry.toMap().value(QStringLiteral("name")).toString());
        fail(QStringLiteral("There is no note template “%1”. Templates: %2.")
                 .arg(templateName, names.join(QStringLiteral(", "))));
    }
    if (title.trimmed().isEmpty() && m_templates.needsTitle(name))
        fail(QStringLiteral("The %1 template needs a title.").arg(name));
    const QVariantMap result = m_templates.createAt(name, title, folder, QString(), when);
    if (result.isEmpty())
        fail(m_templates.lastError());
    *existed = result.value(QStringLiteral("existed")).toBool();
    if (cursor && !*existed)
        *cursor = result.value(QStringLiteral("cursor")).toInt();
    return result.value(QStringLiteral("path")).toString();
}

// ── Text helpers ────────────────────────────────────────────────────────────

NotesApi::FrontMatter NotesApi::frontMatter(const QString &text) {
    FrontMatter fm;
    if (!text.startsWith(QLatin1String("---\n")) && !text.startsWith(QLatin1String("---\r\n")))
        return fm;
    static const QRegularExpression closeRe(QStringLiteral("\\n(---|\\.\\.\\.)[ \\t]*\\r?(\\n|$)"));
    const QRegularExpressionMatch close = closeRe.match(text, 3);
    if (!close.hasMatch())
        return fm;
    fm.present = true;
    fm.innerStart = text.indexOf(QLatin1Char('\n')) + 1;
    fm.innerEnd = int(close.capturedStart()) + 1;
    if (fm.innerEnd < fm.innerStart)
        fm.innerEnd = fm.innerStart;
    fm.inner = text.mid(fm.innerStart, fm.innerEnd - fm.innerStart);
    fm.bodyStart = int(close.capturedEnd());
    return fm;
}

QString NotesApi::setMeta(const QString &text, const QJsonObject &meta) {
    if (meta.isEmpty())
        return text;
    FrontMatter fm = frontMatter(text);
    QString body = text;
    QStringList lines;
    if (fm.present) {
        lines = fm.inner.split(QLatin1Char('\n'));
        if (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        body = text.mid(fm.bodyStart);
    } else {
        body = text;
    }
    for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) {
        const QString key = it.key().trimmed();
        if (key.isEmpty())
            continue;
        const QRegularExpression keyRe(QStringLiteral("^%1\\s*:").arg(QRegularExpression::escape(key)));
        int at = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (keyRe.match(lines.at(i)).hasMatch()) {
                at = i;
                break;
            }
        }
        // The key's value may go on over indented or "- " lines.
        int count = 0;
        if (at >= 0) {
            count = 1;
            while (at + count < lines.size()
                   && (lines.at(at + count).startsWith(QLatin1Char(' ')) || lines.at(at + count).startsWith(QLatin1Char('\t'))
                       || lines.at(at + count).startsWith(QLatin1String("- "))))
                ++count;
        }
        const bool remove = it.value().isNull() || it.value().isUndefined();
        const QString line = key + QStringLiteral(": ") + yamlScalar(it.value());
        if (at >= 0) {
            for (int i = 0; i < count; ++i)
                lines.removeAt(at);
            if (!remove)
                lines.insert(at, line);
        } else if (!remove) {
            lines.append(line);
        }
    }
    QString header = QStringLiteral("---\n") + lines.join(QLatin1Char('\n'))
                     + (lines.isEmpty() ? QString() : QStringLiteral("\n")) + QStringLiteral("---\n");
    if (!fm.present && !body.startsWith(QLatin1Char('\n')))
        header += QLatin1Char('\n');
    return header + body;
}

QJsonObject NotesApi::readMeta(const QString &text) {
    QJsonObject meta;
    const FrontMatter fm = frontMatter(text);
    if (!fm.present)
        return meta;
    static const QRegularExpression keyRe(QStringLiteral("^([A-Za-z0-9_-]+)\\s*:\\s*(.*)$"));
    const QStringList lines = fm.inner.split(QLatin1Char('\n'));
    QString listKey;
    QJsonArray list;
    const auto endList = [&]() {
        if (!listKey.isEmpty())
            meta.insert(listKey, list);
        listKey.clear();
        list = QJsonArray();
    };
    for (const QString &line : lines) {
        if (!listKey.isEmpty() && line.trimmed().startsWith(QLatin1String("- "))) {
            list.append(yamlValue(line.trimmed().mid(2)));
            continue;
        }
        if (line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t')))
            continue;
        endList();
        const QRegularExpressionMatch match = keyRe.match(line);
        if (!match.hasMatch())
            continue;
        if (match.captured(2).trimmed().isEmpty()) {
            listKey = match.captured(1);
            continue;
        }
        meta.insert(match.captured(1), yamlValue(match.captured(2)));
    }
    endList();
    return meta;
}

NotesApi::Section NotesApi::findSection(const QString &text, const QString &heading) {
    Section section;
    QString wanted = heading.trimmed();
    while (wanted.startsWith(QLatin1Char('#')))
        wanted.remove(0, 1);
    wanted = wanted.trimmed();
    if (wanted.isEmpty())
        return section;
    const QList<Zettel::Heading> headings = Zettel::outline(text);
    for (int i = 0; i < headings.size(); ++i) {
        if (headings.at(i).text.trimmed().compare(wanted, Qt::CaseInsensitive) != 0)
            continue;
        section.found = true;
        section.headingStart = headings.at(i).offset;
        section.level = headings.at(i).level;
        const int lineEnd = int(text.indexOf(QLatin1Char('\n'), section.headingStart));
        section.bodyStart = lineEnd < 0 ? int(text.size()) : lineEnd + 1;
        section.end = int(text.size());
        for (int j = i + 1; j < headings.size(); ++j) {
            if (headings.at(j).level <= section.level) {
                section.end = headings.at(j).offset;
                break;
            }
        }
        return section;
    }
    return section;
}

QString NotesApi::appendText(const QString &text, const QString &block, const QString &heading) {
    if (heading.trimmed().isEmpty())
        return joinBlock(text, block) + QLatin1Char('\n');
    const Section section = findSection(text, heading);
    if (!section.found) {
        QString name = heading.trimmed();
        while (name.startsWith(QLatin1Char('#')))
            name.remove(0, 1);
        const QString base = trimBlankLines(text);
        return base + (base.isEmpty() ? QString() : QStringLiteral("\n\n")) + QStringLiteral("## ")
               + name.trimmed() + QStringLiteral("\n\n") + trimBlankLines(block) + QLatin1Char('\n');
    }
    const QString body = joinBlock(text.mid(section.bodyStart, section.end - section.bodyStart), block);
    const QString tail = text.mid(section.end);
    QString head = text.left(section.bodyStart);
    if (!head.endsWith(QLatin1Char('\n')))
        head += QLatin1Char('\n');
    return head + QLatin1Char('\n') + body + QLatin1Char('\n') + (tail.isEmpty() ? QString() : QLatin1Char('\n') + tail);
}

QString NotesApi::replaceSection(const QString &text, const QString &heading, const QString &body) {
    const Section section = findSection(text, heading);
    if (!section.found)
        return trimBlankLines(body).isEmpty() ? text : appendText(text, body, heading);
    const QString tail = text.mid(section.end);
    QString head = text.left(section.bodyStart);
    if (!head.endsWith(QLatin1Char('\n')))
        head += QLatin1Char('\n');
    const QString content = trimBlankLines(body);
    if (content.isEmpty())
        return head + (tail.isEmpty() ? QString() : QLatin1Char('\n') + tail);
    return head + QLatin1Char('\n') + content + QLatin1Char('\n') + (tail.isEmpty() ? QString() : QLatin1Char('\n') + tail);
}

QString NotesApi::insertAt(const QString &text, int position, const QString &block) {
    position = qBound(0, position, int(text.size()));
    const int lineStart = int(text.lastIndexOf(QLatin1Char('\n'), position - 1)) + 1;
    int lineEnd = int(text.indexOf(QLatin1Char('\n'), position));
    if (lineEnd < 0)
        lineEnd = int(text.size());
    const QString before = text.mid(lineStart, position - lineStart);
    const QString after = text.mid(position, lineEnd - position);
    const QString content = trimBlankLines(block);
    if (after.trimmed().isEmpty() && (before.trimmed().isEmpty() || isPlaceholder(before)))
        return text.left(lineStart) + content + text.mid(lineEnd);
    return text.left(position) + content + text.mid(position);
}

QList<NotesApi::Task> NotesApi::tasks(const QString &text) {
    QList<Task> found;
    static const QRegularExpression taskRe(QStringLiteral("^\\s*(?:[-*+]|\\d+[.)])\\s+\\[([ xX])\\]\\s+(.*\\S)\\s*$"));
    static const QRegularExpression fenceRe(QStringLiteral("^\\s{0,3}(`{3,}|~{3,})"));
    const QStringList lines = text.split(QLatin1Char('\n'));
    bool inFence = false;
    const FrontMatter fm = frontMatter(text);
    const int headerLines = fm.present ? int(text.left(fm.bodyStart).count(QLatin1Char('\n'))) : 0;
    for (int i = headerLines; i < lines.size(); ++i) {
        if (fenceRe.match(lines.at(i)).hasMatch()) {
            inFence = !inFence;
            continue;
        }
        if (inFence)
            continue;
        const QRegularExpressionMatch match = taskRe.match(lines.at(i));
        if (match.hasMatch())
            found.append({i + 1, match.captured(1) != QLatin1String(" "), match.captured(2)});
    }
    return found;
}

// ── Reading ─────────────────────────────────────────────────────────────────

QJsonObject NotesApi::info(const QJsonObject &) {
    QJsonArray templates;
    for (const QVariant &entry : m_templates.list())
        templates.append(entry.toMap().value(QStringLiteral("name")).toString());
    Exporter exporter;
    exporter.setRoot(m_root);
    QJsonArray profiles;
    for (const QVariant &entry : exporter.profiles())
        profiles.append(entry.toMap().value(QStringLiteral("name")).toString());
    Sync sync;
    sync.setRoot(m_root);
    QJsonObject syncInfo{{QStringLiteral("enabled"), sync.isRepo()}};
    if (sync.isRepo()) {
        syncInfo.insert(QStringLiteral("remote"), sync.remoteUrl());
        syncInfo.insert(QStringLiteral("branch"), sync.branch());
        syncInfo.insert(QStringLiteral("pendingChanges"), sync.changeCount());
        syncInfo.insert(QStringLiteral("conflicts"), int(sync.gitConflicts().size() + sync.conflictCopies().size()));
    }
    const QString engine = Transcriber::engine();
    const QString model = engine == QLatin1String("whisper.cpp") ? Transcriber::findModel(QString()) : QString();
    // The person's rules for agents travel with the answer, so an agent
    // that asks for info has them before it changes anything.
    QJsonObject layoutInfo{{QStringLiteral("numbered"), Layout::present(m_root)}};
    if (Layout::present(m_root)) {
        layoutInfo.insert(QStringLiteral("guide"), Layout::guide());
        for (const auto &[key, file] : {std::pair{"rules", Layout::rulesFile}, std::pair{"howIWork", Layout::howIWorkFile}}) {
            const QString path = QDir(m_root).filePath(QString::fromLatin1(file));
            if (QFileInfo(path).isFile())
                layoutInfo.insert(QLatin1String(key), QJsonObject{{QStringLiteral("path"), QString::fromLatin1(file)},
                                                                  {QStringLiteral("text"), read(path)}});
        }
    } else {
        layoutInfo.insert(QStringLiteral("hint"), QStringLiteral("This notes folder has its own folders. "
                                                                 "omazettel setup-layout would give it the numbered layout."));
    }
    return {{QStringLiteral("vault"), m_root},
            {QStringLiteral("notes"), m_index.noteCount()},
            {QStringLiteral("tags"), int(m_index.tags().size())},
            {QStringLiteral("templates"), templates},
            {QStringLiteral("exportProfiles"), profiles},
            {QStringLiteral("pandoc"), exporter.pandocAvailable()},
            {QStringLiteral("sync"), syncInfo},
            {QStringLiteral("autoCreateLinks"), QSettings().value(QStringLiteral("notes/autoCreateLinks"), true).toBool()},
            {QStringLiteral("fileNameSeparator"), Vault::fileNameSeparator()},
            {QStringLiteral("layout"), layoutInfo},
            {QStringLiteral("transcription"), QJsonObject{
                 {QStringLiteral("engine"), engine.isEmpty() ? QJsonValue() : QJsonValue(engine)},
                 {QStringLiteral("model"), model.isEmpty() ? QJsonValue() : QJsonValue(QFileInfo(model).fileName())},
                 {QStringLiteral("recordings"), engine == QLatin1String("whisper") || !model.isEmpty()}}}};
}

QJsonObject NotesApi::listNotes(const QJsonObject &args) {
    const QString folder = str(args, QStringLiteral("folder"));
    const QString folderAbs = folder.isEmpty() ? QString() : folderPath(folder, false);
    const QString tag = str(args, QStringLiteral("tag")).remove(QRegularExpression(QStringLiteral("^#")));
    const QString sinceText = str(args, QStringLiteral("since"));
    const QDateTime since = sinceText.isEmpty() ? QDateTime() : parseSince(sinceText);
    const bool byModified = str(args, QStringLiteral("sort")) == QLatin1String("modified");
    const int limit = integer(args, QStringLiteral("limit"), 500);

    QList<QPair<QString, QDateTime>> picked;
    QStringList all;
    for (const QVariant &entry : m_index.notes())
        all.append(entry.toMap().value(QStringLiteral("path")).toString());
    std::sort(all.begin(), all.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    for (const QString &path : std::as_const(all)) {
        if (!folderAbs.isEmpty() && folderAbs != m_root && !path.startsWith(folderAbs + QLatin1Char('/')))
            continue;
        if (!tag.isEmpty()) {
            bool has = false;
            for (const QString &noteTag : m_index.tagsOf(path)) {
                if (noteTag.compare(tag, Qt::CaseInsensitive) == 0
                        || noteTag.startsWith(tag + QLatin1Char('/'), Qt::CaseInsensitive))
                    has = true;
            }
            if (!has)
                continue;
        }
        const QDateTime modified = QFileInfo(path).lastModified();
        if (since.isValid() && modified < since)
            continue;
        picked.append({path, modified});
    }
    if (byModified) {
        std::stable_sort(picked.begin(), picked.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });
    }
    QJsonArray notes;
    for (const auto &[path, modified] : std::as_const(picked)) {
        if (notes.size() >= limit)
            break;
        notes.append(noteSummary(path));
    }
    return {{QStringLiteral("count"), int(picked.size())}, {QStringLiteral("notes"), notes}};
}

QJsonObject NotesApi::search(const QJsonObject &args) {
    const QString query = str(args, QStringLiteral("query")).trimmed();
    if (query.isEmpty())
        fail(QStringLiteral("Say what to search for."));
    const QVariantList hits = m_index.search(query, QString(), integer(args, QStringLiteral("limit"), 20));
    QJsonArray results;
    for (const QVariant &hitValue : hits) {
        const QVariantMap hit = hitValue.toMap();
        QJsonArray snippets;
        for (const QVariant &snippet : hit.value(QStringLiteral("snippets")).toList()) {
            if (snippets.size() >= 3)
                break;
            snippets.append(plainSnippet(snippet.toMap().value(QStringLiteral("html")).toString()));
        }
        const QString path = hit.value(QStringLiteral("path")).toString();
        results.append(QJsonObject{{QStringLiteral("path"), relative(path)},
                                   {QStringLiteral("title"), hit.value(QStringLiteral("title")).toString()},
                                   {QStringLiteral("matches"), hit.value(QStringLiteral("count")).toInt()},
                                   {QStringLiteral("snippets"), snippets}});
    }
    return {{QStringLiteral("query"), query}, {QStringLiteral("results"), results}};
}

QJsonObject NotesApi::readNote(const QJsonObject &args) {
    const QString path = notePath(str(args, QStringLiteral("note")));
    const QString text = read(path);
    QJsonObject note = noteSummary(path);

    const QString sectionName = str(args, QStringLiteral("section"));
    if (!sectionName.isEmpty()) {
        const Section section = findSection(text, sectionName);
        if (!section.found)
            fail(QStringLiteral("%1 has no heading “%2”.").arg(relative(path), sectionName));
        note.insert(QStringLiteral("section"), sectionName);
        note.insert(QStringLiteral("text"), trimBlankLines(text.mid(section.bodyStart, section.end - section.bodyStart)));
        return note;
    }

    note.insert(QStringLiteral("text"), text);
    note.insert(QStringLiteral("meta"), readMeta(text));
    QJsonArray outline;
    for (const Zettel::Heading &heading : Zettel::outline(text))
        outline.append(QJsonObject{{QStringLiteral("level"), heading.level}, {QStringLiteral("text"), heading.text}});
    note.insert(QStringLiteral("outline"), outline);

    QJsonArray links;
    QStringList seen;
    for (const Zettel::LinkUse &link : Zettel::parse(text).links) {
        if (seen.contains(link.target, Qt::CaseInsensitive))
            continue;
        seen.append(link.target);
        const QString target = m_index.resolve(link.target, path);
        links.append(QJsonObject{{QStringLiteral("target"), link.target},
                                 {QStringLiteral("path"), target.isEmpty() ? QJsonValue() : QJsonValue(relative(target))}});
    }
    note.insert(QStringLiteral("links"), links);

    QJsonArray backlinks;
    for (const QVariant &entry : m_index.backlinks(path)) {
        const QVariantMap source = entry.toMap();
        backlinks.append(QJsonObject{{QStringLiteral("path"), relative(source.value(QStringLiteral("path")).toString())},
                                     {QStringLiteral("title"), source.value(QStringLiteral("title")).toString()},
                                     {QStringLiteral("context"), source.value(QStringLiteral("context")).toString()}});
    }
    note.insert(QStringLiteral("backlinks"), backlinks);
    return note;
}

QJsonObject NotesApi::listTags(const QJsonObject &) {
    QJsonArray tags;
    for (const QVariant &entry : m_index.tags()) {
        const QVariantMap tag = entry.toMap();
        tags.append(QJsonObject{{QStringLiteral("tag"), tag.value(QStringLiteral("tag")).toString()},
                                {QStringLiteral("count"), tag.value(QStringLiteral("count")).toInt()}});
    }
    return {{QStringLiteral("tags"), tags}};
}

QJsonObject NotesApi::listTasks(const QJsonObject &args) {
    const QString status = str(args, QStringLiteral("status"), QStringLiteral("open")).toLower();
    if (status != QLatin1String("open") && status != QLatin1String("done") && status != QLatin1String("all"))
        fail(QStringLiteral("status is open, done or all."));
    QStringList paths;
    if (!str(args, QStringLiteral("note")).isEmpty()) {
        paths.append(notePath(str(args, QStringLiteral("note"))));
    } else {
        QJsonObject filter;
        filter.insert(QStringLiteral("folder"), str(args, QStringLiteral("folder")));
        filter.insert(QStringLiteral("tag"), str(args, QStringLiteral("tag")));
        filter.insert(QStringLiteral("limit"), 1000000);
        for (const QJsonValue &note : listNotes(filter).value(QStringLiteral("notes")).toArray())
            paths.append(QDir(m_root).filePath(note.toObject().value(QStringLiteral("path")).toString()));
    }
    QJsonArray found;
    for (const QString &path : std::as_const(paths)) {
        for (const Task &task : tasks(read(path))) {
            if ((status == QLatin1String("open") && task.done) || (status == QLatin1String("done") && !task.done))
                continue;
            found.append(QJsonObject{{QStringLiteral("path"), relative(path)},
                                     {QStringLiteral("title"), m_index.titleOf(path)},
                                     {QStringLiteral("line"), task.line},
                                     {QStringLiteral("done"), task.done},
                                     {QStringLiteral("text"), task.text}});
        }
    }
    return {{QStringLiteral("tasks"), found}};
}

QJsonObject NotesApi::linkReport(const QJsonObject &) {
    QJsonArray broken;
    QJsonArray orphans;
    for (const QVariant &entry : m_index.notes()) {
        const QString path = entry.toMap().value(QStringLiteral("path")).toString();
        for (const QString &target : m_index.missingTargets(path))
            broken.append(QJsonObject{{QStringLiteral("path"), relative(path)}, {QStringLiteral("target"), target}});
        if (m_index.backlinks(path).isEmpty())
            orphans.append(relative(path));
    }
    return {{QStringLiteral("broken"), broken}, {QStringLiteral("orphans"), orphans}};
}

QJsonObject NotesApi::dashboard(const QJsonObject &args) {
    const int days = integer(args, QStringLiteral("days"), 365);
    if (days < 1)
        fail(QStringLiteral("days is 1 or more."));
    return Dashboard::build(m_root, Dashboard::snapshot(m_index, m_root), qMin(days, 3660), QDate::currentDate());
}

QJsonObject NotesApi::listTemplates(const QJsonObject &) {
    QJsonArray templates;
    for (const QVariant &entry : m_templates.list()) {
        const QVariantMap item = entry.toMap();
        const QString path = item.value(QStringLiteral("path")).toString();
        const NoteTemplates::Rendered sample = NoteTemplates::render(read(path), QStringLiteral("Title"), QString(),
                                                                     QString(), QDateTime::currentDateTime());
        QJsonArray headings;
        for (const Zettel::Heading &heading : Zettel::outline(sample.content))
            headings.append(heading.text);
        templates.append(QJsonObject{{QStringLiteral("name"), item.value(QStringLiteral("name")).toString()},
                                     {QStringLiteral("needsTitle"), item.value(QStringLiteral("needsTitle")).toBool()},
                                     {QStringLiteral("folder"), sample.folder},
                                     {QStringLiteral("headings"), headings},
                                     {QStringLiteral("meta"), readMeta(sample.content)}});
    }
    return {{QStringLiteral("folder"), relative(m_templates.folder())}, {QStringLiteral("templates"), templates}};
}

// ── Writing ─────────────────────────────────────────────────────────────────

QJsonObject NotesApi::createNote(const QJsonObject &args) {
    const QString title = str(args, QStringLiteral("title")).trimmed();
    const QString content = str(args, QStringLiteral("content"));
    const QString folderArg = str(args, QStringLiteral("folder"));
    const QString folder = folderArg.isEmpty() ? QString() : folderPath(folderArg, true);
    QString templateName = str(args, QStringLiteral("template"));
    const bool ownHeader = content.trimmed().startsWith(QLatin1String("---"))
                           && frontMatter(content.trimmed() + QLatin1Char('\n')).present;
    if (ownHeader)
        templateName = QStringLiteral("none");
    else if (templateName.isEmpty())
        templateName = m_templates.pathOf(QStringLiteral("Note")).isEmpty() ? QStringLiteral("none") : QStringLiteral("Note");
    if (title.isEmpty() && (templateName == QLatin1String("none") || m_templates.needsTitle(templateName)))
        fail(QStringLiteral("A new note needs a title."));

    bool existed = false;
    int cursor = -1;
    QString path;
    if (ownHeader) {
        const QString name = Vault::noteFileName(title);
        path = Vault::uniquePath(folder.isEmpty() ? m_root : folder, name, QStringLiteral("md"));
        write(path, content.trimmed());
    } else {
        path = makeNote(title, templateName, folder, QDateTime(), &existed, &cursor);
        // An explicit folder wins over the template's own.
        if (!existed && !folder.isEmpty() && QFileInfo(path).absolutePath() != folder) {
            const QString moved = Vault::uniquePath(folder, QFileInfo(path).completeBaseName(), QStringLiteral("md"));
            if (QFile::rename(path, moved))
                path = moved;
        }
    }

    QString text = read(path);
    if (!content.trimmed().isEmpty() && !ownHeader)
        text = cursor >= 0 ? insertAt(text, cursor, content) : appendText(text, content);

    const QJsonObject sections = args.value(QStringLiteral("sections")).toObject();
    for (auto it = sections.constBegin(); it != sections.constEnd(); ++it) {
        // A list of strings becomes a bullet list.
        QString value = it.value().toString();
        if (it.value().isArray()) {
            QStringList items;
            for (const QJsonValue &item : it.value().toArray())
                items.append(QStringLiteral("- ") + item.toString());
            value = items.join(QLatin1Char('\n'));
        }
        text = existed ? appendText(text, value, it.key()) : replaceSection(text, it.key(), value);
    }

    QJsonObject meta = args.value(QStringLiteral("meta")).toObject();
    const QStringList newTags = stringList(args.value(QStringLiteral("tags")));
    if (!newTags.isEmpty()) {
        QStringList tags = stringList(readMeta(text).value(QStringLiteral("tags")));
        for (const QString &tag : newTags) {
            const QString clean = QString(tag).remove(QRegularExpression(QStringLiteral("^#")));
            if (!tags.contains(clean, Qt::CaseInsensitive))
                tags.append(clean);
        }
        meta.insert(QStringLiteral("tags"), toJsonArray(tags));
    }
    if (!meta.isEmpty())
        text = setMeta(text, meta);
    write(path, text);

    const QJsonArray made = afterWrite(path, args);
    if (flag(args, QStringLiteral("open"), false))
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {path});

    QJsonObject result = noteSummary(path);
    result.insert(QStringLiteral("created"), !existed);
    result.insert(QStringLiteral("linkedNotesCreated"), made);
    result.insert(QStringLiteral("message"), QStringLiteral("%1 %2").arg(existed ? QStringLiteral("Added to") : QStringLiteral("Created"), relative(path)));
    return result;
}

QJsonObject NotesApi::appendToNote(const QJsonObject &args) {
    const QString reference = str(args, QStringLiteral("note"));
    const QString block = str(args, QStringLiteral("text"));
    if (trimBlankLines(block).isEmpty())
        fail(QStringLiteral("There is nothing to add."));
    QString path;
    bool created = false;
    try {
        path = notePath(reference);
    } catch (const Error &) {
        if (!flag(args, QStringLiteral("create"), false))
            throw;
        bool existed = false;
        const int slash = int(reference.lastIndexOf(QLatin1Char('/')));
        const QString folder = slash < 0 ? QString() : folderPath(reference.left(slash), true);
        path = makeNote(stripSuffix(reference.mid(slash + 1)), QStringLiteral("Note"), folder, QDateTime(), &existed);
        created = true;
    }
    const QString text = read(path);
    const QString section = str(args, QStringLiteral("section"));
    write(path, appendText(text, block, section));
    const QJsonArray made = afterWrite(path, args);
    QJsonObject result = noteSummary(path);
    result.insert(QStringLiteral("created"), created);
    result.insert(QStringLiteral("linkedNotesCreated"), made);
    result.insert(QStringLiteral("message"), section.isEmpty()
                      ? QStringLiteral("Added to %1").arg(relative(path))
                      : QStringLiteral("Added to %1, under %2").arg(relative(path), section));
    return result;
}

QJsonObject NotesApi::replaceSectionTool(const QJsonObject &args) {
    const QString path = notePath(str(args, QStringLiteral("note")));
    const QString section = str(args, QStringLiteral("section"));
    if (section.trimmed().isEmpty())
        fail(QStringLiteral("Name the heading of the section to replace."));
    write(path, replaceSection(read(path), section, str(args, QStringLiteral("text"))));
    const QJsonArray made = afterWrite(path, args);
    QJsonObject result = noteSummary(path);
    result.insert(QStringLiteral("linkedNotesCreated"), made);
    result.insert(QStringLiteral("message"), QStringLiteral("Replaced %2 in %1").arg(relative(path), section));
    return result;
}

QJsonObject NotesApi::setMetadata(const QJsonObject &args) {
    const QString path = notePath(str(args, QStringLiteral("note")));
    const QJsonObject meta = args.value(QStringLiteral("meta")).toObject();
    if (meta.isEmpty())
        fail(QStringLiteral("Give keys to set, e.g. status=done."));
    write(path, setMeta(read(path), meta));
    m_index.updateFile(path);
    QJsonObject result = noteSummary(path);
    result.insert(QStringLiteral("meta"), readMeta(read(path)));
    result.insert(QStringLiteral("message"), QStringLiteral("Updated the header of %1").arg(relative(path)));
    return result;
}

QJsonObject NotesApi::writeNote(const QJsonObject &args) {
    const QString reference = str(args, QStringLiteral("note"));
    const QString text = str(args, QStringLiteral("text"));
    QString path;
    bool created = false;
    try {
        path = notePath(reference);
    } catch (const Error &) {
        if (!flag(args, QStringLiteral("create"), false))
            throw;
        const int slash = int(reference.lastIndexOf(QLatin1Char('/')));
        const QString folder = slash < 0 ? m_root : folderPath(reference.left(slash), true);
        const QString name = Vault::noteFileName(stripSuffix(reference.mid(slash + 1)));
        if (name.isEmpty())
            fail(QStringLiteral("Name the note."));
        path = Vault::uniquePath(folder, name, QStringLiteral("md"));
        created = true;
    }
    write(path, text);
    const QJsonArray made = afterWrite(path, args);
    QJsonObject result = noteSummary(path);
    result.insert(QStringLiteral("created"), created);
    result.insert(QStringLiteral("linkedNotesCreated"), made);
    result.insert(QStringLiteral("message"), QStringLiteral("Wrote %1").arg(relative(path)));
    return result;
}

QJsonObject NotesApi::dailyNote(const QJsonObject &args) {
    const QDate day = parseDay(str(args, QStringLiteral("date")));
    bool existed = false;
    QString path;
    if (m_templates.pathOf(QStringLiteral("Daily")).isEmpty()) {
        const QString folder = folderPath(Layout::dailyFolder(m_root), true);
        path = QDir(folder).filePath(day.toString(Qt::ISODate) + QStringLiteral(".md"));
        existed = QFileInfo(path).isFile();
        if (!existed)
            write(path, QStringLiteral("# %1\n").arg(day.toString(Qt::ISODate)));
    } else {
        // Today keeps the current time for {{time}}; another day starts at 9.
        const QDateTime when = day == QDate::currentDate() ? QDateTime::currentDateTime()
                                                           : QDateTime(day, QTime(9, 0));
        path = makeNote(QString(), QStringLiteral("Daily"), QString(), when, &existed);
    }
    QString text = read(path);
    const QString block = str(args, QStringLiteral("append"));
    const QString section = str(args, QStringLiteral("section"), QStringLiteral("Log"));
    if (!trimBlankLines(block).isEmpty())
        text = appendText(text, block, section);
    if (!existed || !trimBlankLines(block).isEmpty())
        write(path, text);
    m_index.updateFile(path);
    QJsonObject result = noteSummary(path);
    result.insert(QStringLiteral("date"), day.toString(Qt::ISODate));
    result.insert(QStringLiteral("created"), !existed);
    result.insert(QStringLiteral("text"), read(path));
    return result;
}

QJsonObject NotesApi::capture(const QJsonObject &args) {
    const QString text = trimBlankLines(str(args, QStringLiteral("text")));
    if (text.isEmpty())
        fail(QStringLiteral("There is nothing to capture."));
    const bool task = flag(args, QStringLiteral("task"), false);
    QStringList lines = text.split(QLatin1Char('\n'));
    QString first = lines.takeFirst();
    first = task ? QStringLiteral("- [ ] ") + first
                 : QStringLiteral("- %1 %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm")), first);
    for (QString &line : lines)
        line.prepend(QStringLiteral("  "));
    lines.prepend(first);
    const QJsonObject daily = dailyNote({{QStringLiteral("append"), lines.join(QLatin1Char('\n'))},
                                         {QStringLiteral("section"), str(args, QStringLiteral("section"), QStringLiteral("Log"))}});
    return {{QStringLiteral("path"), daily.value(QStringLiteral("path"))},
            {QStringLiteral("message"), QStringLiteral("Captured to %1").arg(daily.value(QStringLiteral("path")).toString())}};
}

// ── Organising ──────────────────────────────────────────────────────────────

QJsonObject NotesApi::relocate(const QString &from, const QString &to) {
    if (QFileInfo::exists(to))
        fail(QStringLiteral("%1 already exists.").arg(relative(to)));
    QVariantMap moves;
    if (QFileInfo(from).isDir())
        moves = m_index.movesForFolder(from, to);
    else
        moves.insert(from, to);
    // Links are rewritten while the index still knows the old paths.
    const int links = m_index.rewriteLinks(moves);
    const int notes = m_index.lastRewriteFiles();
    QDir().mkpath(QFileInfo(to).absolutePath());
    if (!QFile::rename(from, to) && !QDir().rename(from, to))
        fail(QStringLiteral("Could not move %1.").arg(relative(from)));
    m_index.rescanNow();
    return {{QStringLiteral("from"), relative(from)},
            {QStringLiteral("path"), relative(to)},
            {QStringLiteral("linksUpdated"), links},
            {QStringLiteral("notesUpdated"), notes},
            {QStringLiteral("message"), QStringLiteral("Moved %1 to %2; updated %3 link(s) in %4 note(s)")
                                            .arg(relative(from), relative(to)).arg(links).arg(notes)}};
}

QJsonObject NotesApi::renameNote(const QJsonObject &args) {
    const QString reference = str(args, QStringLiteral("note"));
    QString from;
    const QString asFolder = QDir::cleanPath(QDir(m_root).filePath(reference));
    if (!reference.isEmpty() && QFileInfo(asFolder).isDir() && inside(asFolder) && asFolder != m_root)
        from = asFolder;
    else
        from = notePath(reference);
    QString name = QFileInfo(from).isFile() ? Vault::noteFileName(str(args, QStringLiteral("name")))
                                            : Vault::cleanName(str(args, QStringLiteral("name")));
    if (name.isEmpty())
        fail(QStringLiteral("Give the new name."));
    if (QFileInfo(from).isFile() && !Vault::isNoteFile(name))
        name += QLatin1Char('.') + QFileInfo(from).suffix();
    const QString to = QDir(QFileInfo(from).absolutePath()).filePath(name);
    if (to == from)
        return {{QStringLiteral("path"), relative(from)}, {QStringLiteral("message"), QStringLiteral("Nothing to rename.")}};
    QJsonObject result = relocate(from, to);
    result.insert(QStringLiteral("message"), QStringLiteral("Renamed %1 to %2; updated %3 link(s) in %4 note(s)")
                                                 .arg(relative(from), relative(to))
                                                 .arg(result.value(QStringLiteral("linksUpdated")).toInt())
                                                 .arg(result.value(QStringLiteral("notesUpdated")).toInt()));
    return result;
}

QJsonObject NotesApi::tidyNames(const QJsonObject &args) {
    const QString folderArg = str(args, QStringLiteral("folder"));
    const QString scope = folderArg.isEmpty() ? QString() : folderPath(folderArg, false);
    const bool apply = flag(args, QStringLiteral("apply"), false);
    QStringList paths;
    for (const QVariant &entry : m_index.notes()) {
        const QString path = entry.toMap().value(QStringLiteral("path")).toString();
        if (scope.isEmpty() || path.startsWith(scope + QLatin1Char('/')))
            paths.append(path);
    }
    std::sort(paths.begin(), paths.end());

    QJsonArray renamed;
    QJsonArray skipped;
    QStringList lines;
    QSet<QString> taken;
    int links = 0;
    int notes = 0;
    for (const QString &from : std::as_const(paths)) {
        const QFileInfo info(from);
        const QString name = Vault::noteFileName(info.completeBaseName());
        if (name.isEmpty() || name == info.completeBaseName())
            continue;
        const QString to = QDir(info.absolutePath()).filePath(name + QLatin1Char('.') + info.suffix());
        if (QFileInfo::exists(to) || taken.contains(to)) {
            skipped.append(QJsonObject{{QStringLiteral("from"), relative(from)}, {QStringLiteral("to"), relative(to)},
                                       {QStringLiteral("reason"), QStringLiteral("a note with that name already exists")}});
            lines.append(QStringLiteral("  skipped %1 (%2 already exists)").arg(relative(from), relative(to)));
            continue;
        }
        taken.insert(to);
        QJsonObject item{{QStringLiteral("from"), relative(from)}, {QStringLiteral("to"), relative(to)}};
        if (apply) {
            const QJsonObject moved = relocate(from, to);
            links += moved.value(QStringLiteral("linksUpdated")).toInt();
            notes += moved.value(QStringLiteral("notesUpdated")).toInt();
            item.insert(QStringLiteral("linksUpdated"), moved.value(QStringLiteral("linksUpdated")));
        }
        renamed.append(item);
        lines.append(QStringLiteral("  %1 -> %2").arg(relative(from), relative(to)));
    }

    QString message;
    if (renamed.isEmpty() && skipped.isEmpty())
        message = QStringLiteral("Every note file name already uses “%1”.").arg(Vault::fileNameSeparator());
    else if (apply)
        message = QStringLiteral("Renamed %1 note(s); updated %2 link(s) in %3 note(s).\n%4")
                      .arg(renamed.size()).arg(links).arg(notes).arg(lines.join(QLatin1Char('\n')));
    else
        message = QStringLiteral("Would rename %1 note(s) (nothing changed yet; add --apply to do it):\n%2")
                      .arg(renamed.size()).arg(lines.join(QLatin1Char('\n')));
    return {{QStringLiteral("applied"), apply},
            {QStringLiteral("separator"), Vault::fileNameSeparator()},
            {QStringLiteral("renamed"), renamed},
            {QStringLiteral("skipped"), skipped},
            {QStringLiteral("linksUpdated"), links},
            {QStringLiteral("message"), message}};
}

QJsonObject NotesApi::setupLayout(const QJsonObject &args) {
    const bool apply = flag(args, QStringLiteral("apply"), false);
    const QDir root(m_root);
    QStringList lines;
    QJsonArray moved;
    QJsonArray skipped;
    QSet<QString> moveTargets;

    // 1. The older folders move in, links and all.
    const QStringList top = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const Layout::Move &move : Layout::moves()) {
        QString from;
        for (const QString &name : top) {
            if (name.compare(move.from, Qt::CaseInsensitive) == 0)
                from = root.filePath(name);
        }
        if (from.isEmpty())
            continue;
        const QString to = root.filePath(move.to);
        // A layout folder made earlier holds only its .keep; that one is
        // taken away so the old folder can take its place.
        const bool onlyKeep = QFileInfo(to).isDir()
                              && QDir(to).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                                     == QStringList{QStringLiteral(".keep")};
        if (QFileInfo::exists(to) && !onlyKeep) {
            skipped.append(QJsonObject{{QStringLiteral("from"), relative(from)}, {QStringLiteral("to"), move.to},
                                       {QStringLiteral("reason"), QStringLiteral("%1 already exists").arg(move.to)}});
            lines.append(QStringLiteral("  skipped %1/ (%2/ already exists; move its notes with omazettel move)")
                             .arg(relative(from), move.to));
            continue;
        }
        moveTargets.insert(move.to.section(QLatin1Char('/'), 0, 0));
        QJsonObject item{{QStringLiteral("from"), relative(from)}, {QStringLiteral("to"), move.to}};
        if (apply) {
            if (onlyKeep) {
                QFile::remove(QDir(to).filePath(QStringLiteral(".keep")));
                QDir().rmdir(to);
            }
            const QJsonObject done = relocate(from, to);
            item.insert(QStringLiteral("linksUpdated"), done.value(QStringLiteral("linksUpdated")));
        }
        moved.append(item);
        lines.append(QStringLiteral("  move %1/ -> %2/").arg(relative(from), move.to));
    }

    // Transcripts go in a transcripts folder beside their meeting notes.
    QJsonArray transcripts;
    QStringList notes;
    for (const QVariant &entry : m_index.notes())
        notes.append(entry.toMap().value(QStringLiteral("path")).toString());
    std::sort(notes.begin(), notes.end());
    for (const QString &note : std::as_const(notes)) {
        const QString rel = relative(note);
        if (!Moc::isTranscript(rel) || rel.contains(QLatin1String("transcripts/"), Qt::CaseInsensitive))
            continue;
        // Where it ends up, after the folder moves above.
        QString target = rel;
        for (const QJsonValue &m : std::as_const(moved)) {
            const QString from = m.toObject().value(QStringLiteral("from")).toString() + QLatin1Char('/');
            if (target.startsWith(from))
                target = m.toObject().value(QStringLiteral("to")).toString() + QLatin1Char('/') + target.mid(from.size());
        }
        const QFileInfo info(target);
        const QString to = (info.path() == QLatin1String(".") ? QString() : info.path() + QLatin1Char('/'))
                           + QStringLiteral("transcripts/") + info.fileName();
        if (QFileInfo::exists(root.filePath(to)))
            continue;
        if (apply)
            relocate(root.filePath(target), root.filePath(to));
        transcripts.append(QJsonObject{{QStringLiteral("from"), target}, {QStringLiteral("to"), to}});
        lines.append(QStringLiteral("  move %1 -> %2").arg(target, to));
    }

    // 2. The folders and rules files that are missing.
    QJsonArray made;
    if (apply) {
        for (const QString &entry : Layout::create(m_root))
            made.append(entry);
    } else {
        for (const QString &folder : Layout::folders()) {
            if (!QFileInfo(root.filePath(folder)).isDir() && !moveTargets.contains(folder))
                made.append(folder + QLatin1Char('/'));
        }
        for (const char *file : {Layout::rulesFile, Layout::howIWorkFile}) {
            if (!QFileInfo::exists(root.filePath(QString::fromLatin1(file))))
                made.append(QString::fromLatin1(file));
        }
    }
    for (const QJsonValue &entry : std::as_const(made))
        lines.append(QStringLiteral("  make %1").arg(entry.toString()));

    // 3. Templates that put notes in an older folder put them in the new one.
    QJsonArray templates;
    for (const QVariant &entry : m_templates.list()) {
        const QString path = entry.toMap().value(QStringLiteral("path")).toString();
        QString text = read(path);
        const QString before = text;
        for (const Layout::Move &move : Layout::moves()) {
            const QRegularExpression folderRe(
                QStringLiteral("(<!--\\s*omazettel:[^\\n]*?folder=\")%1(/[^\"]*)?(\")")
                    .arg(QRegularExpression::escape(move.from)),
                QRegularExpression::CaseInsensitiveOption);
            text.replace(folderRe, QStringLiteral("\\1%1\\2\\3").arg(move.to));
        }
        if (text == before)
            continue;
        templates.append(entry.toMap().value(QStringLiteral("name")).toString());
        lines.append(QStringLiteral("  point the %1 template at its new folder").arg(entry.toMap().value(QStringLiteral("name")).toString()));
        if (apply)
            write(path, text);
    }
    if (apply && !templates.isEmpty())
        m_templates.reload();

    // 4. Everything else stays where it is; say so, so nothing is a surprise.
    QJsonArray left;
    QStringList known = Layout::folders();
    for (const Layout::Move &move : Layout::moves())
        known.append(move.from);
    for (const QString &name : root.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        bool isKnown = false;
        for (const QString &k : std::as_const(known))
            isKnown = isKnown || name.compare(k, Qt::CaseInsensitive) == 0;
        if (!isKnown)
            left.append(name);
    }

    const bool nothing = moved.isEmpty() && made.isEmpty() && templates.isEmpty() && transcripts.isEmpty();
    QString message;
    if (nothing)
        message = QStringLiteral("The notes folder already has the layout.");
    else if (apply)
        message = QStringLiteral("Set up the folder layout:\n%1").arg(lines.join(QLatin1Char('\n')));
    else
        message = QStringLiteral("Would set up the folder layout (nothing changed yet; add --apply to do it):\n%1")
                      .arg(lines.join(QLatin1Char('\n')));
    if (!left.isEmpty()) {
        QStringList names;
        for (const QJsonValue &name : std::as_const(left))
            names.append(name.toString());
        message += QStringLiteral("\nLeft where they are: %1. Move them yourself when you like, e.g. "
                                  "omazettel move \"%2\" 02-context (reference) or 03-projects (active work).")
                       .arg(names.join(QStringLiteral(", ")), names.first());
    }
    return {{QStringLiteral("applied"), apply},
            {QStringLiteral("moved"), moved},
            {QStringLiteral("made"), made},
            {QStringLiteral("transcripts"), transcripts},
            {QStringLiteral("templates"), templates},
            {QStringLiteral("skipped"), skipped},
            {QStringLiteral("leftInPlace"), left},
            {QStringLiteral("message"), message}};
}

QJsonObject NotesApi::updateMaps(const QJsonObject &args) {
    const bool apply = flag(args, QStringLiteral("apply"), false);
    if (!Layout::present(m_root))
        return {{QStringLiteral("applied"), apply}, {QStringLiteral("maps"), QJsonArray()},
                {QStringLiteral("message"), QStringLiteral("Maps of content are made in the AI-first layout's "
                                                           "03-projects; this notes folder doesn't have it "
                                                           "(omazettel setup-layout gives it one).")}};
    QJsonArray maps;
    for (const QString &map : Moc::update(m_root, !apply)) {
        if (apply)
            m_index.updateFile(QDir(m_root).filePath(map));
        maps.append(map);
    }
    QStringList names;
    for (const QJsonValue &map : std::as_const(maps))
        names.append(QStringLiteral("  ") + map.toString());
    QString message;
    if (maps.isEmpty())
        message = QStringLiteral("The maps of content are up to date.");
    else if (apply)
        message = QStringLiteral("Updated %1 map(s):\n%2").arg(maps.size()).arg(names.join(QLatin1Char('\n')));
    else
        message = QStringLiteral("Would update %1 map(s) (add --apply to write them):\n%2").arg(maps.size()).arg(names.join(QLatin1Char('\n')));
    if (!Moc::enabled())
        message += QStringLiteral("\nKeeping them up to date is off in Settings.");
    return {{QStringLiteral("applied"), apply}, {QStringLiteral("maps"), maps}, {QStringLiteral("message"), message}};
}

QJsonObject NotesApi::moveNote(const QJsonObject &args) {
    const QString reference = str(args, QStringLiteral("note"));
    QString from;
    const QString asFolder = QDir::cleanPath(QDir(m_root).filePath(reference));
    if (!reference.isEmpty() && QFileInfo(asFolder).isDir() && inside(asFolder) && asFolder != m_root)
        from = asFolder;
    else
        from = notePath(reference);
    const QString into = folderPath(str(args, QStringLiteral("folder")), true);
    if (into == from || into.startsWith(from + QLatin1Char('/')))
        fail(QStringLiteral("A folder cannot move into itself."));
    const QString to = QDir(into).filePath(QFileInfo(from).fileName());
    if (to == from)
        return {{QStringLiteral("path"), relative(from)}, {QStringLiteral("message"), QStringLiteral("Already there.")}};
    return relocate(from, to);
}

QJsonObject NotesApi::trashNote(const QJsonObject &args) {
    const QString path = notePath(str(args, QStringLiteral("note")));
    const QString name = relative(path);
    if (!QFile::moveToTrash(path))
        fail(QStringLiteral("Could not move %1 to the trash.").arg(name));
    m_index.updateFile(path);
    return {{QStringLiteral("path"), name}, {QStringLiteral("message"), QStringLiteral("Moved %1 to the trash").arg(name)}};
}

// ── Export, sync, transcription ─────────────────────────────────────────────

QJsonObject NotesApi::exportNote(const QJsonObject &args) {
    Exporter exporter;
    exporter.setRoot(m_root);
    exporter.ensureDefaults();
    if (!exporter.pandocAvailable())
        fail(QStringLiteral("Export needs pandoc (sudo pacman -S pandoc)."));

    const QString reference = str(args, QStringLiteral("note"));
    const QString asFolder = QDir::cleanPath(QDir(m_root).filePath(reference));
    const bool isFolder = !reference.isEmpty() && QFileInfo(asFolder).isDir() && inside(asFolder);
    const QString source = isFolder ? asFolder : notePath(reference);

    const QVariantList profiles = exporter.profiles();
    if (profiles.isEmpty())
        fail(QStringLiteral("There are no export profiles."));
    QString profilePath;
    const QString wanted = str(args, QStringLiteral("profile"));
    QStringList names;
    for (const QVariant &entry : profiles) {
        const QVariantMap profile = entry.toMap();
        names.append(profile.value(QStringLiteral("name")).toString());
        if (!wanted.isEmpty()
                && (profile.value(QStringLiteral("name")).toString().compare(wanted, Qt::CaseInsensitive) == 0
                    || profile.value(QStringLiteral("format")).toString().compare(wanted, Qt::CaseInsensitive) == 0
                    || profile.value(QStringLiteral("path")).toString() == wanted))
            profilePath = profile.value(QStringLiteral("path")).toString();
    }
    if (!wanted.isEmpty() && profilePath.isEmpty())
        fail(QStringLiteral("There is no export profile “%1”. Profiles: %2.").arg(wanted, names.join(QStringLiteral(", "))));
    if (profilePath.isEmpty() && isFolder)
        profilePath = exporter.folderSettings(source).value(QStringLiteral("profile")).toString();
    if (profilePath.isEmpty() || !QFileInfo(profilePath).isFile())
        profilePath = exporter.lastProfile();
    if (profilePath.isEmpty() || !QFileInfo(profilePath).isFile())
        profilePath = profiles.first().toMap().value(QStringLiteral("path")).toString();

    QString output = str(args, QStringLiteral("output"));
    if (output.startsWith(QLatin1String("~/")))
        output = QDir::homePath() + output.mid(1);
    if (!output.isEmpty())
        output = QFileInfo(output).absoluteFilePath();
    else
        output = isFolder ? exporter.defaultFolderOutputPath(source, profilePath)
                          : exporter.defaultOutputPath(source, profilePath);

    bool ok = false;
    QString message;
    QEventLoop loop;
    QObject::connect(&exporter, &Exporter::finished, &loop,
                     [&](bool success, const QString &, const QString &text) {
                         ok = success;
                         message = text;
                         loop.quit();
                     });
    bool started = false;
    if (isFolder) {
        const QVariantMap settings = exporter.folderSettings(source);
        QStringList paths;
        for (const QVariant &entry : exporter.folderNotes(source)) {
            if (entry.toMap().value(QStringLiteral("included")).toBool())
                paths.append(entry.toMap().value(QStringLiteral("path")).toString());
        }
        if (paths.isEmpty())
            fail(QStringLiteral("%1 has no notes to export.").arg(relative(source)));
        started = exporter.exportFolder(source, paths, settings.value(QStringLiteral("title")).toString(),
                                        settings.value(QStringLiteral("author")).toString(), profilePath, output);
    } else {
        started = exporter.exportNote(source, profilePath, output);
    }
    if (!started)
        fail(exporter.error().isEmpty() ? QStringLiteral("Export could not start.") : exporter.error());
    loop.exec();
    if (!ok)
        fail(QStringLiteral("Export failed: %1").arg(message.trimmed()));
    return {{QStringLiteral("source"), relative(source)},
            {QStringLiteral("profile"), QFileInfo(profilePath).completeBaseName()},
            {QStringLiteral("output"), output},
            {QStringLiteral("warnings"), message.trimmed()},
            {QStringLiteral("message"), QStringLiteral("Exported to %1").arg(output)}};
}

QJsonObject NotesApi::sync(const QJsonObject &args) {
    Sync sync;
    sync.setRoot(m_root);
    if (!sync.isRepo())
        fail(QStringLiteral("Sync is off for this notes folder. Turn it on in OmaZettel: … menu → Sync & history."));
    const QString author = NoteTemplates::systemAuthor();
    const QString branch = sync.branch().isEmpty() ? QStringLiteral("main") : sync.branch();
    QString token = str(args, QStringLiteral("token"));
    if (token.isEmpty())
        token = qEnvironmentVariable("OMAZETTEL_GIT_TOKEN");
    token = token.trimmed();

    const auto outcome = [this](const Sync::Result &result, const QString &what) {
        QJsonArray updated;
        for (const QString &path : result.updated)
            updated.append(relative(path));
        QJsonArray conflicts;
        for (const QString &path : result.conflicts)
            conflicts.append(relative(path));
        if (!result.ok && conflicts.isEmpty()) {
            QString message = result.message;
            if (!result.details.trimmed().isEmpty())
                message += QStringLiteral("\n\ngit said:\n") + result.details.trimmed();
            if (result.kind == QLatin1String("private-email"))
                message += QStringLiteral("\n\nFix it with: omazettel sync --fix-email");
            message += QStringLiteral("\n(Log: %1)").arg(Sync::logPath());
            fail(message);
        }
        return QJsonObject{{QStringLiteral("ok"), result.ok}, {QStringLiteral("action"), what},
                           {QStringLiteral("updated"), updated}, {QStringLiteral("conflicts"), conflicts},
                           {QStringLiteral("message"), result.message}};
    };

    if (args.contains(QStringLiteral("remote"))) {
        if (!sync.setRemote(str(args, QStringLiteral("remote"))))
            fail(sync.lastError());
        sync.refresh();
        if (!token.isEmpty() && !Sync::storeToken(sync.repoRoot(), sync.remoteUrl(), str(args, QStringLiteral("user")), token, &token))
            fail(token);
        if (sync.remoteUrl().isEmpty())
            return {{QStringLiteral("message"), QStringLiteral("Removed the remote.")}};
        return outcome(Sync::testRemoteBlocking(sync.repoRoot(), branch), QStringLiteral("remote"));
    }
    if (!str(args, QStringLiteral("create")).isEmpty()) {
        const QString provider = str(args, QStringLiteral("create")).toLower();
        const QString name = str(args, QStringLiteral("name"), QFileInfo(m_root).fileName());
        const bool isPrivate = !flag(args, QStringLiteral("public"), false);
        if (provider == QLatin1String("github") && token.isEmpty()) {
            if (!sync.ghAvailable())
                fail(QStringLiteral("Making a GitHub repository needs the GitHub CLI (gh) or --token."));
            QProcess gh;
            gh.setWorkingDirectory(sync.repoRoot());
            gh.start(QStandardPaths::findExecutable(QStringLiteral("gh")),
                     {QStringLiteral("repo"), QStringLiteral("create"), Vault::cleanName(name).replace(QLatin1Char(' '), QLatin1Char('-')),
                      isPrivate ? QStringLiteral("--private") : QStringLiteral("--public"), QStringLiteral("--source"), sync.repoRoot(),
                      QStringLiteral("--remote"), QStringLiteral("origin"), QStringLiteral("--push")});
            gh.waitForFinished(180000);
            if (gh.exitCode() != 0)
                fail(QStringLiteral("gh could not make the repository: %1").arg(QString::fromUtf8(gh.readAllStandardError()).trimmed()));
            return {{QStringLiteral("ok"), true}, {QStringLiteral("message"), QStringLiteral("Made %1 on GitHub and sent the notes.").arg(name)}};
        }
        return outcome(Sync::createRemoteBlocking(sync.repoRoot(), branch, provider, str(args, QStringLiteral("server")), token, name,
                                                  isPrivate, author), QStringLiteral("create"));
    }
    if (!token.isEmpty()) {
        if (!Sync::storeToken(sync.repoRoot(), sync.remoteUrl(), str(args, QStringLiteral("user")), token, &token))
            fail(token);
        return outcome(Sync::testRemoteBlocking(sync.repoRoot(), branch), QStringLiteral("token"));
    }
    if (flag(args, QStringLiteral("check"), false))
        return outcome(Sync::testRemoteBlocking(sync.repoRoot(), branch), QStringLiteral("check"));
    if (flag(args, QStringLiteral("fixEmail"), false))
        return outcome(Sync::useNoReplyBlocking(sync.repoRoot(), branch, !sync.remoteUrl().isEmpty(), author), QStringLiteral("fix-email"));

    QJsonArray copies;
    for (const QVariant &copy : sync.conflictCopies())
        copies.append(relative(copy.toMap().value(QStringLiteral("path")).toString()));
    QJsonArray gitConflicts;
    for (const QString &path : sync.gitConflicts())
        gitConflicts.append(relative(path));
    if (flag(args, QStringLiteral("status"), false)) {
        QString ahead;
        QProcess git;
        git.setWorkingDirectory(sync.repoRoot());
        git.start(QStandardPaths::findExecutable(QStringLiteral("git")),
                  {QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("@{upstream}..HEAD")});
        git.waitForFinished(10000);
        ahead = git.exitCode() == 0 ? QString::fromUtf8(git.readAllStandardOutput()).trimmed() : QStringLiteral("?");
        return {{QStringLiteral("remote"), sync.remoteUrl()},
                {QStringLiteral("host"), sync.hostKind()},
                {QStringLiteral("branch"), sync.branch()},
                {QStringLiteral("commitAs"), QStringLiteral("%1 <%2>").arg(sync.commitName(), sync.commitEmail())},
                {QStringLiteral("pendingChanges"), sync.changeCount()},
                {QStringLiteral("unsentVersions"), ahead},
                {QStringLiteral("conflicts"), gitConflicts},
                {QStringLiteral("conflictCopies"), copies},
                {QStringLiteral("message"), QStringLiteral("%1 change(s) not saved yet, %2 version(s) not sent; %3 conflict(s). Remote: %4 (%5)")
                                                .arg(sync.changeCount()).arg(ahead).arg(gitConflicts.size() + copies.size())
                                                .arg(sync.remoteUrl().isEmpty() ? QStringLiteral("none") : sync.remoteUrl(), sync.hostKind())}};
    }
    if (!gitConflicts.isEmpty())
        fail(QStringLiteral("Resolve the conflicts in OmaZettel first (… menu → Sync & history)."));
    return outcome(Sync::runSync(sync.repoRoot(), branch, !sync.remoteUrl().isEmpty(), author), QStringLiteral("sync"));
}

QJsonObject NotesApi::transcribe(const QJsonObject &args) {
    QString error;
    const QString download = str(args, QStringLiteral("downloadModel"));
    if (!download.isEmpty()) {
        QString path;
        if (!Transcriber::downloadModel(download, &path, &error))
            fail(error);
        return {{QStringLiteral("model"), path}, {QStringLiteral("message"), QStringLiteral("Downloaded %1").arg(path)}};
    }
    QString file = str(args, QStringLiteral("file"));
    if (file.isEmpty())
        fail(QStringLiteral("Name the recording or transcript file."));
    if (file.startsWith(QLatin1String("~/")))
        file = QDir::homePath() + file.mid(1);
    const bool fromVoxtype = file.startsWith(QLatin1String("voxtype:"));
    // Check the AI before a long transcription, not after.
    if (args.contains(QStringLiteral("note")) && str(args, QStringLiteral("ai")) != QLatin1String("none")) {
        const QString trouble = Assistant::problem(Assistant::choose(str(args, QStringLiteral("ai"))));
        if (!trouble.isEmpty())
            fail(trouble + QStringLiteral("\n(Or --ai none for a note with the transcript only.)"));
    }
    static const QStringList textKinds{QStringLiteral("vtt"), QStringLiteral("srt"), QStringLiteral("sbv"), QStringLiteral("txt"),
                                       QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("docx"),
                                       QStringLiteral("odt"), QStringLiteral("rtf"), QStringLiteral("html")};
    if (!fromVoxtype && !textKinds.contains(QFileInfo(file).suffix().toLower())) {
        progress(QStringLiteral("Transcribing %1 with %2 (%3)…").arg(QFileInfo(file).fileName(),
                 Transcriber::engine().isEmpty() ? QStringLiteral("whisper") : Transcriber::engine(),
                 Transcriber::engineModel(str(args, QStringLiteral("model")))));
    }
    const Transcriber::Result result = Transcriber::transcribe(fromVoxtype ? file : QFileInfo(file).absoluteFilePath(),
                                                               str(args, QStringLiteral("model")),
                                                               str(args, QStringLiteral("language")),
                                                               flag(args, QStringLiteral("timestamps"), false), &error);
    if (!error.isEmpty())
        fail(error);
    if (args.contains(QStringLiteral("note")))
        return meetingNote(result.text, result.speakers, QFileInfo(file).fileName(), args);
    QJsonObject out{{QStringLiteral("file"), QFileInfo(file).absoluteFilePath()},
                    {QStringLiteral("engine"), result.engine},
                    {QStringLiteral("speakers"), toJsonArray(result.speakers)},
                    {QStringLiteral("words"), int(result.text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size())},
                    {QStringLiteral("text"), result.text}};
    if (!result.model.isEmpty())
        out.insert(QStringLiteral("model"), result.model);
    return out;
}

// ── AI ──────────────────────────────────────────────────────────────────────

QJsonObject NotesApi::aiStatus(const QJsonObject &args) {
    if (args.contains(QStringLiteral("set"))) {
        const QString wanted = str(args, QStringLiteral("set")).trimmed();
        const bool reset = wanted.isEmpty() || wanted == QLatin1String("default") || wanted == QLatin1String("omarchy");
        if (!reset) {
            const Assistant::Choice choice = Assistant::choose(wanted);
            if (!Assistant::supported().contains(choice.provider) && choice.provider != QLatin1String("none"))
                fail(Assistant::problem(choice));
        }
        Assistant::save(reset ? QString() : wanted);
    }
    const Assistant::Choice choice = Assistant::choose(QString());
    const QString trouble = Assistant::problem(choice);
    QJsonObject out{{QStringLiteral("provider"), choice.provider},
                    {QStringLiteral("name"), Assistant::displayName(choice.provider)},
                    {QStringLiteral("model"), choice.model},
                    {QStringLiteral("origin"), choice.origin},
                    {QStringLiteral("ready"), trouble.isEmpty()},
                    {QStringLiteral("problem"), trouble},
                    {QStringLiteral("omarchyDefaultAgent"), Assistant::omarchyDefaultAgent()},
                    {QStringLiteral("installedAgents"), toJsonArray(Assistant::installedAgents())}};
    if (trouble.isEmpty() && flag(args, QStringLiteral("test"), false)) {
        progress(QStringLiteral("Asking %1…").arg(Assistant::displayName(choice.provider)));
        QString error;
        const QString answer = Assistant::complete(choice, QStringLiteral("Reply with only this JSON: {\"ok\": true}"), &error);
        const QJsonObject parsed = error.isEmpty() ? Assistant::extractJson(answer, &error) : QJsonObject();
        out.insert(QStringLiteral("test"), error.isEmpty() && parsed.value(QStringLiteral("ok")).toBool()
                                               ? QStringLiteral("answered") : (error.isEmpty() ? QStringLiteral("answered oddly: ") + answer.left(200) : error));
    }
    const QString speech = Transcriber::problem();
    out.insert(QStringLiteral("transcription"), QJsonObject{
        {QStringLiteral("engine"), Transcriber::engine()},
        {QStringLiteral("model"), Transcriber::engineModel()},
        {QStringLiteral("ready"), speech.isEmpty()},
        {QStringLiteral("problem"), speech}});
    return out;
}

QJsonObject NotesApi::meetingNote(const QString &transcript, const QStringList &speakers, const QString &source,
                                  const QJsonObject &args) {
    if (transcript.trimmed().isEmpty())
        fail(QStringLiteral("%1 has no words in it to make a note from.").arg(source));
    const QString templateName = str(args, QStringLiteral("template"), QStringLiteral("Meeting"));
    const QString title = str(args, QStringLiteral("note")).trimmed();
    const bool keepTranscript = flag(args, QStringLiteral("transcript"), true);
    const QString ai = str(args, QStringLiteral("ai"));

    // The template's headings are what the AI fills.
    QStringList headings;
    for (const QJsonValue &value : listTemplates({}).value(QStringLiteral("templates")).toArray()) {
        if (value.toObject().value(QStringLiteral("name")).toString().compare(templateName, Qt::CaseInsensitive) == 0) {
            for (const QJsonValue &heading : value.toObject().value(QStringLiteral("headings")).toArray())
                headings.append(heading.toString());
        }
    }
    if (headings.isEmpty())
        headings = {QStringLiteral("Agenda"), QStringLiteral("Notes"), QStringLiteral("Decisions"), QStringLiteral("Action items")};

    QJsonObject create{{QStringLiteral("template"), templateName},
                       {QStringLiteral("linkNotes"), false}};
    if (!str(args, QStringLiteral("folder")).isEmpty())
        create.insert(QStringLiteral("folder"), str(args, QStringLiteral("folder")));
    QJsonObject sections;
    QJsonObject meta;
    QStringList attendees = speakers;
    QString writer;

    if (ai != QLatin1String("none")) {
        const Assistant::Choice choice = Assistant::choose(ai);
        writer = Assistant::displayName(choice.provider) + (choice.model.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(choice.model));
        // Existing notes the meeting may be about, so it can link them.
        QStringList names;
        for (const QVariant &entry : m_index.notes()) {
            const QString folder = entry.toMap().value(QStringLiteral("folder")).toString();
            if (folder.startsWith(QLatin1String("Daily")) || folder.startsWith(QLatin1String("Meetings"))
                    || folder.startsWith(Layout::dailyFolder(m_root)) || folder.startsWith(Layout::meetingsFolder(m_root))
                    || folder.startsWith(QLatin1String(Layout::systemFolder)))
                continue;
            names.append(entry.toMap().value(QStringLiteral("name")).toString());
            if (names.size() >= 400)
                break;
        }
        QStringList guide;
        for (const QString &heading : std::as_const(headings)) {
            const QString key = heading.toLower();
            QString what = QStringLiteral("Markdown for this section, from the transcript; empty string if nothing fits");
            if (key == QLatin1String("agenda"))
                what = QStringLiteral("bullet list (\"- \") of the topics discussed, in order");
            else if (key == QLatin1String("notes"))
                what = QStringLiteral("a concise summary grouped by topic: short paragraphs or bullets, with names, numbers and dates as said");
            else if (key == QLatin1String("decisions"))
                what = QStringLiteral("bullet list of decisions actually made; empty string if none");
            else if (key.contains(QLatin1String("action")))
                what = QStringLiteral("task list, one per line: \"- [ ] Owner: task (due date)\"; leave out Owner or due date when not said; empty string if none");
            guide.append(QStringLiteral("    \"%1\": %2").arg(heading, what));
        }
        const QString prompt = QStringLiteral(
            "You write meeting notes from a transcript. Answer with only a JSON object, no other text and no code fence:\n"
            "{\n"
            "  \"title\": a short title for the meeting (at most 8 words, no date),\n"
            "  \"summary\": one or two sentences on what the meeting covered and concluded,\n"
            "  \"attendees\": [names of the people who took part],\n"
            "  \"tags\": [up to 5 short lowercase topic tags, no #],\n"
            "  \"sections\": {\n%1\n  }\n"
            "}\n\n"
            "Rules: use only what the transcript says; never invent names, owners, dates or decisions. "
            "Speaker labels like \"Speaker 1\" are not names. Write in the language of the transcript. "
            "%2\n\n"
            "Transcript (from %3):\n<<<\n%4\n>>>\n")
            .arg(guide.join(QStringLiteral(",\n")),
                 names.isEmpty() ? QString()
                                 : QStringLiteral("When the meeting clearly discusses one of these existing notes, link it as [[Name]] "
                                                  "(exactly as written here, only these): %1.").arg(names.join(QStringLiteral(", "))),
                 source, transcript.trimmed());
        progress(QStringLiteral("Writing the meeting note with %1…").arg(writer));
        QString error;
        const QString answer = Assistant::complete(choice, prompt, &error);
        if (!error.isEmpty())
            fail(error + QStringLiteral("\n(Or --ai none for a note with the transcript only.)"));
        const QJsonObject note = Assistant::extractJson(answer, &error);
        if (!error.isEmpty())
            fail(error);
        const QJsonObject written = note.value(QStringLiteral("sections")).toObject();
        for (const QString &heading : std::as_const(headings)) {
            for (auto it = written.constBegin(); it != written.constEnd(); ++it) {
                const bool filled = it.value().isArray() ? !it.value().toArray().isEmpty()
                                                         : !it.value().toString().trimmed().isEmpty();
                if (filled && it.key().compare(heading, Qt::CaseInsensitive) == 0)
                    sections.insert(heading, it.value());
            }
        }
        if (title.isEmpty())
            create.insert(QStringLiteral("title"), note.value(QStringLiteral("title")).toString(QStringLiteral("Meeting")));
        const QStringList named = stringList(note.value(QStringLiteral("attendees")));
        if (!named.isEmpty())
            attendees = named;
        const QStringList tags = stringList(note.value(QStringLiteral("tags")));
        if (!tags.isEmpty())
            create.insert(QStringLiteral("tags"), toJsonArray(tags));
        if (!note.value(QStringLiteral("summary")).toString().trimmed().isEmpty())
            meta.insert(QStringLiteral("abstract"), note.value(QStringLiteral("summary")).toString().trimmed());
    }
    if (!title.isEmpty())
        create.insert(QStringLiteral("title"), title);
    else if (!create.contains(QStringLiteral("title")))
        create.insert(QStringLiteral("title"), QFileInfo(source).completeBaseName());
    if (!attendees.isEmpty())
        meta.insert(QStringLiteral("attendees"), toJsonArray(attendees));
    create.insert(QStringLiteral("sections"), sections);
    create.insert(QStringLiteral("meta"), meta);

    QJsonObject result = createNote(create);
    // The template's "**Attendees:**" line, filled in.
    if (!attendees.isEmpty()) {
        const QString path = QDir(m_root).filePath(result.value(QStringLiteral("path")).toString());
        QString text = read(path);
        static const QRegularExpression attendeesRe(QStringLiteral("(?m)^(\\*\\*Attendees:\\*\\*)[ \\t]*$"));
        if (attendeesRe.match(text).hasMatch()) {
            text.replace(attendeesRe, QStringLiteral("\\1 ") + attendees.join(QStringLiteral(", ")));
            write(path, text);
        }
    }
    const QString path = QDir(m_root).filePath(result.value(QStringLiteral("path")).toString());
    // The transcript goes in a note of its own, in a transcripts folder
    // beside the meeting note, each linking the other. The meeting note is
    // what people look for; the transcript stays out of the way.
    if (keepTranscript) {
        const QString meetingName = QFileInfo(path).completeBaseName();
        const QString meetingFolder = QFileInfo(result.value(QStringLiteral("path")).toString()).path();
        const QString folder = meetingFolder == QLatin1String(".") ? QStringLiteral("transcripts")
                                                                   : meetingFolder + QStringLiteral("/transcripts");
        QJsonObject meta{{QStringLiteral("source"), source},
                         {QStringLiteral("meeting"), QStringLiteral("[[%1]]").arg(meetingName)},
                         {QStringLiteral("tags"), QJsonArray{QStringLiteral("transcript")}}};
        const QString date = readMeta(read(path)).value(QStringLiteral("date")).toString();
        if (!date.isEmpty())
            meta.insert(QStringLiteral("date"), date);
        const QJsonObject made = createNote({
            {QStringLiteral("title"), meetingName + QStringLiteral(" transcript")},
            {QStringLiteral("template"), QStringLiteral("none")},
            {QStringLiteral("folder"), folder},
            {QStringLiteral("content"), QStringLiteral("Transcript of [[%1]], from %2.\n\n%3\n")
                                            .arg(meetingName, source, transcript.trimmed())},
            {QStringLiteral("meta"), meta},
            {QStringLiteral("linkNotes"), false}});
        const QString transcriptPath = made.value(QStringLiteral("path")).toString();
        write(path, replaceSection(read(path), QStringLiteral("Transcript"),
                                   QStringLiteral("[[%1]]").arg(QFileInfo(transcriptPath).completeBaseName())));
        m_index.updateFile(path);
        result.insert(QStringLiteral("transcriptNote"), transcriptPath);
    }
    QJsonArray actions;
    for (const Task &task : tasks(read(path)))
        actions.append(task.text);
    result.insert(QStringLiteral("source"), source);
    result.insert(QStringLiteral("writtenBy"), writer.isEmpty() ? QStringLiteral("no AI (transcript only)") : writer);
    result.insert(QStringLiteral("actionItems"), actions);
    result.insert(QStringLiteral("message"), QStringLiteral("Created %1%2%3").arg(result.value(QStringLiteral("path")).toString(),
                  writer.isEmpty() ? QString() : QStringLiteral(" (written by %1)").arg(writer),
                  result.contains(QStringLiteral("transcriptNote"))
                      ? QStringLiteral("\nTranscript in %1").arg(result.value(QStringLiteral("transcriptNote")).toString())
                      : QString()));
    return result;
}

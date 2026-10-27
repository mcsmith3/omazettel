#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

#include "noteindex.h"
#include "notetemplates.h"

// The notes folder as a set of tools: finding, reading, writing and
// organising notes without the window. `omazettel <verb>` on the command
// line and `omazettel mcp` (for AI assistants) both run these, so they work
// the same way and the same as the app: templates, [[links]] that make their
// notes, links rewritten when notes move, git sync.
//
// Every tool takes a JSON object of arguments and answers with a JSON
// object. Failures throw NotesApi::Error with a message meant for people.
class NotesApi {
public:
    struct Error {
        QString message;
    };

    struct Param {
        QString name;        // camelCase; on the command line --kebab-case.
        QString type;        // string, integer, boolean, array, object.
        QString description;
        bool required = false;
        // Taken from the command line's bare words, in order. The last
        // positional string takes every word left ("search meeting notes").
        bool positional = false;
        // On the command line the flag may stand alone (--note with no title).
        bool optionalValue = false;
    };

    struct Tool {
        QString name;  // For MCP: search_notes.
        QString verb;  // For the command line: search.
        QString description;
        QList<Param> params;
        bool readOnly = true;
        bool destructive = false;
        std::function<QJsonObject(NotesApi &, const QJsonObject &)> run;
    };

    NotesApi();
    ~NotesApi();

    static const QList<Tool> &tools();
    static const Tool *toolForVerb(const QString &verb);
    static const Tool *toolNamed(const QString &name);
    static QJsonObject inputSchema(const Tool &tool);

    // Opens a notes folder: folder if given, else $OMAZETTEL_VAULT, else the
    // folder the app had open last.
    bool open(const QString &folder, QString *error);
    QString root() const { return m_root; }

    // Where long steps (transcribing, the AI writing) report what they are
    // doing; the command line prints it on standard error.
    static void setProgress(std::function<void(const QString &)> progress);

    // Runs a tool, after picking up changes made since the last call.
    QJsonObject call(const Tool &tool, const QJsonObject &arguments);

    // ── Text helpers, public for the tests ──────────────────────────────────
    struct FrontMatter {
        bool present = false;
        QString inner;     // Between the --- lines.
        int innerStart = 0;
        int innerEnd = 0;  // Where the closing --- line starts.
        int bodyStart = 0; // After the closing line.
    };
    static FrontMatter frontMatter(const QString &text);
    // Sets (or, for null, removes) top-level keys in the YAML header, adding
    // one if the note has none.
    static QString setMeta(const QString &text, const QJsonObject &meta);
    // The YAML header's top-level keys, read simply: strings, numbers,
    // booleans and lists.
    static QJsonObject readMeta(const QString &text);

    struct Section {
        bool found = false;
        int headingStart = 0;
        int bodyStart = 0;
        int end = 0;
        int level = 0;
    };
    // The heading (with or without its #s, case ignored) and what follows it
    // up to the next heading of the same or a higher level.
    static Section findSection(const QString &text, const QString &heading);
    // Adds block after what the section has (or at the end of the note when
    // heading is empty), making the section if the note has none. Empty
    // template bullets ("- ", "- [ ] ") at the end make way for it.
    static QString appendText(const QString &text, const QString &block,
                              const QString &heading = QString());
    static QString replaceSection(const QString &text, const QString &heading,
                                  const QString &body);
    // Puts block where a template's {{cursor}} was: in place of an empty
    // bullet line, or at that spot.
    static QString insertAt(const QString &text, int position, const QString &block);

    struct Task {
        int line = 0; // 1-based.
        bool done = false;
        QString text;
    };
    static QList<Task> tasks(const QString &text);

private:
    // Tools.
    QJsonObject info(const QJsonObject &args);
    QJsonObject listNotes(const QJsonObject &args);
    QJsonObject search(const QJsonObject &args);
    QJsonObject readNote(const QJsonObject &args);
    QJsonObject createNote(const QJsonObject &args);
    QJsonObject appendToNote(const QJsonObject &args);
    QJsonObject replaceSectionTool(const QJsonObject &args);
    QJsonObject setMetadata(const QJsonObject &args);
    QJsonObject writeNote(const QJsonObject &args);
    QJsonObject dailyNote(const QJsonObject &args);
    QJsonObject capture(const QJsonObject &args);
    QJsonObject listTags(const QJsonObject &args);
    QJsonObject listTasks(const QJsonObject &args);
    QJsonObject linkReport(const QJsonObject &args);
    QJsonObject renameNote(const QJsonObject &args);
    QJsonObject tidyNames(const QJsonObject &args);
    QJsonObject setupLayout(const QJsonObject &args);
    QJsonObject updateMaps(const QJsonObject &args);
    QJsonObject moveNote(const QJsonObject &args);
    QJsonObject trashNote(const QJsonObject &args);
    QJsonObject listTemplates(const QJsonObject &args);
    QJsonObject exportNote(const QJsonObject &args);
    QJsonObject sync(const QJsonObject &args);
    QJsonObject transcribe(const QJsonObject &args);
    QJsonObject aiStatus(const QJsonObject &args);
    QJsonObject meetingNote(const QString &transcript, const QStringList &speakers,
                            const QString &source, const QJsonObject &args);

    // Paths.
    QString relative(const QString &path) const;
    QString notePath(const QString &reference) const;
    QString folderPath(const QString &reference, bool create) const;
    bool inside(const QString &path) const;

    // Files.
    QString read(const QString &path) const;
    void write(const QString &path, const QString &text);
    // After a note is written: re-index it and make the notes its [[links]]
    // name, when that is on. Returns the notes made.
    QJsonArray afterWrite(const QString &path, const QJsonObject &args);
    QJsonObject noteSummary(const QString &path) const;
    QString makeNote(const QString &title, const QString &templateName, const QString &folder,
                     const QDateTime &when, bool *existed, int *cursor = nullptr);
    QJsonObject relocate(const QString &from, const QString &to);
    void refreshIndex(bool force = false);

    QString m_root;
    NoteIndex m_index;
    NoteTemplates m_templates;
    qint64 m_lastScan = 0;
};

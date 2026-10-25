#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// Templates for new notes (meeting notes, daily notes, ...): Markdown files
// in <notes>/.omazettel/note-templates/. Each can start with a pandoc YAML
// header (title, author, date, tags, ...) so new notes are ready to export,
// and use placeholders such as {{title}}, {{date}} and {{cursor}}.
//
// An optional first line sets how the note is made, and is left out of it:
//   <!-- omazettel: name="{{date}}" folder="Daily" existing="open" -->
// name: the file name pattern (default "{{title}}"); folder: where it goes,
// relative to the notes folder (default: the selected folder); existing:
// "open" opens a note of that name if there is one (default: number a new one).
class NoteTemplates : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
    Q_PROPERTY(QString defaultTemplate READ defaultTemplate WRITE setDefaultTemplate NOTIFY changed)
    Q_PROPERTY(QString author READ author WRITE setAuthor NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    struct Rendered {
        QString fileName;  // Without extension.
        QString folder;    // Relative to the notes folder, or empty.
        bool openExisting = false;
        QString content;
        int cursor = -1;   // Where {{cursor}} was, or -1.
    };

    explicit NoteTemplates(QObject *parent = nullptr);

    QString folder() const;
    QString defaultTemplate() const;
    void setDefaultTemplate(const QString &name);
    QString author() const;
    void setAuthor(const QString &author);
    int revision() const { return m_revision; }

    Q_INVOKABLE void setRoot(const QString &notesRoot);
    // Writes the starter templates if the templates folder does not exist.
    Q_INVOKABLE void ensureDefaults();
    // [{name, path, needsTitle}], by name.
    Q_INVOKABLE QVariantList list() const;
    Q_INVOKABLE bool needsTitle(const QString &name) const;
    Q_INVOKABLE QString pathOf(const QString &name) const;
    // Makes a note from a template. title may be empty when the template
    // does not use it. targetFolder is used unless the template names one.
    // exactName, when set, is the file name to use (for [[link]] targets).
    // Returns {path, cursor, existed} or {} with error set.
    Q_INVOKABLE QVariantMap create(const QString &name, const QString &title,
                                   const QString &targetFolder, const QString &exactName = QString());
    // The same, filling {{date}} and the rest from when (the command line's
    // `daily --date`) instead of now.
    QVariantMap createAt(const QString &name, const QString &title, const QString &targetFolder,
                         const QString &exactName, const QDateTime &when);
    Q_INVOKABLE QString lastError() const { return m_lastError; }
    Q_INVOKABLE void reload() { ++m_revision; emit changed(); }

    // Helpers, public for the tests.
    static Rendered render(const QString &templateText, const QString &title,
                           const QString &author, const QString &folderName,
                           const QDateTime &now);
    static QString yamlEscaped(const QString &value);
    static QString systemAuthor();
    // The built-in templates; their folders follow the notes folder's layout
    // (Layout::dailyFolder and the rest).
    static QList<QPair<QString, QString>> starterTemplates(const QString &notesRoot = QString());

signals:
    void changed();

private:
    QString m_root;
    QString m_lastError;
    int m_revision = 0;
};

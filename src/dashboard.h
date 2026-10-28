#pragma once

#include <QDate>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class NoteIndex;

// The dashboard: how much was written each day (from git history when the
// notes folder has it, else file dates), open tasks, notes that need
// attention, projects, tags and recent notes. The app shows it in the middle
// of the window; `omazettel dashboard` and agents get the same numbers.
class Dashboard : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap data READ data NOTIFY dataChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    // While the dashboard is on screen it follows changes to the notes.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    // What the dashboard needs from the note index, copied so it can be
    // used away from the thread that owns the index.
    struct Snapshot {
        struct Note {
            QString path; // Relative to the notes folder.
            QString title;
            QStringList linkedFrom; // Relative paths of notes linking here.
            QStringList missing;    // [[link]] targets that match no note.
            QStringList tags;
        };
        QList<Note> notes;
    };

    explicit Dashboard(NoteIndex *index, QObject *parent = nullptr);
    ~Dashboard() override;

    QVariantMap data() const { return m_data; }
    bool loading() const { return m_watcher.isRunning(); }
    bool active() const { return m_active; }
    void setActive(bool active);

    Q_INVOKABLE void setRoot(const QString &root);
    Q_INVOKABLE void refresh();
    // Ticks off the open task on line (1-based) of the note at path
    // (relative), if that line still holds it. Returns "" or why not.
    Q_INVOKABLE QString completeTask(const QString &path, int line, const QString &text);

    static Snapshot snapshot(const NoteIndex &index, const QString &root);
    // Everything the dashboard shows, for the days up to today.
    static QJsonObject build(const QString &root, const Snapshot &snapshot, int days, const QDate &today);
    // Changes "- [ ]" to "- [x]" on line (1-based) when it is the open task
    // text. Returns the new text, or "" when the line is not that task.
    static QString tickTask(const QString &noteText, int line, const QString &taskText);

signals:
    void dataChanged();
    void loadingChanged();
    void activeChanged();

private:
    void start();

    NoteIndex *m_index;
    QString m_root;
    QVariantMap m_data;
    bool m_active = false;
    bool m_again = false;
    QTimer m_refreshTimer;
    QFutureWatcher<QJsonObject> m_watcher;
};

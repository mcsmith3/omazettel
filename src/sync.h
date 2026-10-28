#pragma once

#include <QDateTime>
#include <functional>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

// Keeping the notes folder in step with other devices and people, without
// real-time co-editing:
//
//  - Git: the notes folder (or the repository holding it) is saved as
//    versions and synced with a remote: commit, fetch, merge (never rebase),
//    push. When the same note changed on both sides, the merge stops and the
//    note is listed as a conflict to resolve (keep mine, keep theirs, keep
//    both, or edit). Every note's history can be compared and restored.
//  - Synced folders (Syncthing, Nextcloud, Dropbox, ownCloud): the conflict
//    copies those tools leave are listed and resolved the same way.
//
// Git runs without prompts (GIT_TERMINAL_PROMPT=0, ssh BatchMode), so a
// missing login shows as an error rather than a hang.
class Sync : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool gitAvailable READ gitAvailable CONSTANT)
    Q_PROPERTY(bool ghAvailable READ ghAvailable CONSTANT)
    Q_PROPERTY(bool isRepo READ isRepo NOTIFY changed)
    Q_PROPERTY(QString repoRoot READ repoRoot NOTIFY changed)
    Q_PROPERTY(QString remoteUrl READ remoteUrl NOTIFY changed)
    Q_PROPERTY(QString branch READ branch NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // "off" (not a repository), "local" (no remote), "clean", "changes",
    // "error" or "conflict".
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QDateTime lastSync READ lastSync NOTIFY changed)
    Q_PROPERTY(int changeCount READ changeCount NOTIFY changed)
    Q_PROPERTY(QStringList gitConflicts READ gitConflicts NOTIFY changed)
    Q_PROPERTY(QVariantList conflictCopies READ conflictCopies NOTIFY changed)
    Q_PROPERTY(bool autoSync READ autoSync WRITE setAutoSync NOTIFY settingsChanged)
    Q_PROPERTY(int intervalMinutes READ intervalMinutes WRITE setIntervalMinutes NOTIFY settingsChanged)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // After a failed sync: git's own words, and what kind of failure it was
    // (private-email, auth, ssh-key, host-key, forbidden, too-large,
    // rejected, behind, certificate, network, not-found, other).
    Q_PROPERTY(QString details READ details NOTIFY changed)
    Q_PROPERTY(QString errorKind READ errorKind NOTIFY changed)
    // The remote's server and what runs it: github, gitlab, forgejo (also
    // Codeberg), gitea, gogs or other.
    Q_PROPERTY(QString remoteHost READ remoteHost NOTIFY changed)
    Q_PROPERTY(QString hostKind READ hostKind NOTIFY changed)
    Q_PROPERTY(bool remoteIsHttps READ remoteIsHttps NOTIFY changed)
    // The name and email versions are saved under, in this repository.
    Q_PROPERTY(QString commitName READ commitName NOTIFY changed)
    Q_PROPERTY(QString commitEmail READ commitEmail NOTIFY changed)
    // A repository around the notes folder that covers the whole home
    // folder, which version history leaves alone ("" when there is none).
    Q_PROPERTY(QString outerRepository READ outerRepository NOTIFY changed)
    Q_PROPERTY(QString logPath READ logPath CONSTANT)

public:
    explicit Sync(QObject *parent = nullptr);
    ~Sync() override;

    bool gitAvailable() const;
    bool ghAvailable() const;
    bool isRepo() const { return !m_repoRoot.isEmpty(); }
    QString repoRoot() const { return m_repoRoot; }
    QString remoteUrl() const { return m_remoteUrl; }
    QString branch() const { return m_branch; }
    bool busy() const { return m_watcher.isRunning(); }
    QString state() const;
    QString message() const { return m_message; }
    QDateTime lastSync() const { return m_lastSync; }
    int changeCount() const { return m_changeCount; }
    QStringList gitConflicts() const { return m_gitConflicts; }
    QVariantList conflictCopies() const { return m_conflictCopies; }
    bool autoSync() const { return m_autoSync; }
    void setAutoSync(bool enabled);
    int intervalMinutes() const { return m_intervalMinutes; }
    void setIntervalMinutes(int minutes);
    int revision() const { return m_revision; }
    QString details() const { return m_details; }
    QString errorKind() const { return m_errorKind; }
    QString remoteHost() const { return hostOf(m_remoteUrl); }
    QString hostKind() const;
    bool remoteIsHttps() const { return m_remoteUrl.startsWith(QLatin1String("http")); }
    QString commitName() const { return m_commitName; }
    QString commitEmail() const { return m_commitEmail; }
    QString outerRepository() const { return m_outerRepository; }
    static QString logPath();
    // The git repository that holds the notes folder, or "" when there is
    // none. A repository of the whole home folder (or above it) doesn't count
    // as the notes' own; ignored, when given, then names it.
    static QString repositoryOf(const QString &notesRoot, QString *ignored = nullptr);
    // Why a remote's address won't work, with the fix, or "" when it looks
    // right (https://host/you/notes.git, git@host:you/notes.git, a path).
    static QString checkRemoteAddress(const QString &url);

    Q_INVOKABLE void setRoot(const QString &notesRoot);
    // Re-reads the repository's status and the folder's conflict copies.
    Q_INVOKABLE void refresh();

    // ── Setting up ──────────────────────────────────────────────────────────
    // Starts version history in the notes folder, with a first version.
    Q_INVOKABLE bool enable();
    Q_INVOKABLE bool setRemote(const QString &url);
    // Makes a GitHub repository with gh, connects it and pushes (in the
    // background). Emits synced().
    Q_INVOKABLE bool createGitHubRepo(const QString &name, bool isPrivate);
    // Makes a repository on GitHub (gh, or a token), GitLab, Forgejo,
    // Codeberg, Gitea or Gogs, connects it, keeps the token for git, and
    // pushes (in the background). server is the address of a self-hosted
    // one (default gitlab.com, codeberg.org). Emits synced().
    Q_INVOKABLE bool createRemoteRepo(const QString &provider, const QString &server, const QString &token,
                                      const QString &name, bool isPrivate);
    // Keeps a username and access token for the https remote in git's
    // credential store (the desktop keyring when there is one).
    Q_INVOKABLE bool saveToken(const QString &username, const QString &token);
    // Checks the remote can be read and written, without changing it, in
    // the background. Emits synced() with the verdict.
    Q_INVOKABLE bool testRemote();
    // Saves versions under this name and email in this repository.
    Q_INVOKABLE bool setCommitIdentity(const QString &name, const QString &email);
    // For GitHub's "push would publish a private email": switches this
    // repository to the account's no-reply address, re-signs the versions
    // not yet pushed with it, and syncs (in the background).
    Q_INVOKABLE bool useNoReplyEmail();

    // ── Syncing ─────────────────────────────────────────────────────────────
    // Commit, then fetch, merge and push when there is a remote, in the
    // background. Emits synced(ok, files the merge changed). Save the open
    // note first.
    Q_INVOKABLE bool syncNow();

    // ── Conflicts ───────────────────────────────────────────────────────────
    // {kind: "git"|"copy", path, mine, theirs, tool, label}. For git, path is
    // the note; for a copy, path is the copy.
    Q_INVOKABLE QVariantMap conflict(const QString &path) const;
    // choice: "mine", "theirs", "both" or (git only) "edited".
    Q_INVOKABLE bool resolve(const QString &path, const QString &choice);
    Q_INVOKABLE bool abortMerge();

    // ── History ─────────────────────────────────────────────────────────────
    // A note's saved versions, newest first:
    // [{hash, date (ISO), author, message, path}] (path at that version).
    Q_INVOKABLE QVariantList history(const QString &path) const;
    Q_INVOKABLE QString versionText(const QString &hash, const QString &path) const;
    // Line differences between two texts (TextDiff::lines).
    Q_INVOKABLE QVariantList diff(const QString &before, const QString &after) const;
    Q_INVOKABLE QString lastError() const { return m_error; }

    struct Result {
        bool ok = false;
        QString message;
        QStringList updated;   // Absolute paths the merge changed.
        QStringList conflicts; // Absolute paths.
        QString details;       // git's words, when it failed.
        QString kind;          // The kind of failure (see errorKind).
    };
    struct Explained {
        QString message;
        QString kind;
    };
    // What a failed git step's error output means, and what to do about it.
    static Explained explain(const QString &errorOutput, const QString &remoteUrl, const QString &step,
                             int exitCode = 1);
    static QString hostOf(const QString &url);
    static QString hostKindOf(const QString &url);
    // Blocking versions, for worker threads and the command line.
    static Result testRemoteBlocking(const QString &repoRoot, const QString &branch);
    static Result useNoReplyBlocking(const QString &repoRoot, const QString &branch, bool hasRemote,
                                     const QString &fallbackAuthor);
    static Result createRemoteBlocking(const QString &repoRoot, const QString &branch, const QString &provider,
                                       const QString &server, const QString &token, const QString &name,
                                       bool isPrivate, const QString &fallbackAuthor);
    static bool storeToken(const QString &repoRoot, const QString &url, const QString &username,
                           const QString &token, QString *error);
    // Adds a line to the sync log (credentials in addresses are hidden).
    static void log(const QString &repoRoot, const QString &what, const Result &result);
    // The whole sync, blocking; runs on a worker thread.
    static Result runSync(const QString &repoRoot, const QString &branch, bool hasRemote,
                          const QString &fallbackAuthor);
    static QStringList scanConflictCopies(const QString &root, QVariantList *details);

signals:
    void changed();
    void busyChanged();
    void settingsChanged();
    // Time for an automatic sync: save the open note, then call syncNow().
    void autoSyncDue();
    void synced(bool ok, const QStringList &updatedFiles);

private:
    bool git(const QStringList &args, QString *out = nullptr, QString *err = nullptr,
             int timeoutMs = 15000) const;
    QString relative(const QString &path) const;
    void startWorker(std::function<Result()> work);

    QString m_root;
    QString m_repoRoot;
    QString m_remoteUrl;
    QString m_branch;
    QString m_message;
    QString m_details;
    QString m_errorKind;
    QString m_commitName;
    QString m_commitEmail;
    QString m_outerRepository;
    QString m_hostKindSetting;
    QString m_error;
    QDateTime m_lastSync;
    int m_changeCount = 0;
    bool m_lastFailed = false;
    QStringList m_gitConflicts;
    QVariantList m_conflictCopies;
    bool m_autoSync = true;
    int m_intervalMinutes = 5;
    int m_revision = 0;
    QTimer m_timer;
    QFutureWatcher<Result> m_watcher;
};

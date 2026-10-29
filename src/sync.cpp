#include "sync.h"
#include <QUrl>
#include <QJsonObject>
#include <QJsonDocument>

#include "notetemplates.h"
#include "textdiff.h"
#include "vault.h"
#include "zettel.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QtConcurrent>

namespace {
const QString autoSyncSetting = QStringLiteral("sync/autoSync");
const QString intervalSetting = QStringLiteral("sync/intervalMinutes");

QProcessEnvironment gitEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Never wait on a prompt nobody can see.
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    if (!env.contains(QStringLiteral("GIT_SSH_COMMAND")))
        env.insert(QStringLiteral("GIT_SSH_COMMAND"), QStringLiteral("ssh -o BatchMode=yes"));
    env.insert(QStringLiteral("GIT_MERGE_AUTOEDIT"), QStringLiteral("no"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    // Reading the status must not take the lock a sync running at the same
    // time needs.
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    return env;
}

// The repository's index lock, which git leaves behind when it is stopped
// halfway (or crashes), after which every git step fails until it goes.
QString indexLockOf(const QString &dir) {
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessEnvironment(gitEnvironment());
    process.start(QStandardPaths::findExecutable(QStringLiteral("git")),
                  {QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")});
    if (!process.waitForFinished(5000) || process.exitCode() != 0)
        return {};
    const QString gitDir = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    return gitDir.isEmpty() ? QString() : gitDir + QStringLiteral("/index.lock");
}

// Runs git in dir and waits. Returns true on exit code 0.
bool runGit(const QString &dir, const QStringList &args, QString *out, QString *err,
            int timeoutMs, int *exitCode = nullptr, bool retried = false) {
    const QString program = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (program.isEmpty()) {
        if (err)
            *err = QStringLiteral("git is not installed.");
        return false;
    }
    const QDateTime started = QDateTime::currentDateTime();
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessEnvironment(gitEnvironment());
    process.start(program, args);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        // Stopped halfway, git leaves its lock: take away the one it made.
        const QString lock = indexLockOf(dir);
        if (!lock.isEmpty() && QFileInfo::exists(lock) && QFileInfo(lock).lastModified() >= started.addSecs(-1))
            QFile::remove(lock);
        if (err)
            *err = QStringLiteral("git %1 took too long.").arg(args.value(0));
        if (exitCode)
            *exitCode = -1;
        return false;
    }
    const QString errors = QString::fromUtf8(process.readAllStandardError()).trimmed();
    // A lock nobody has touched for ten minutes was left by a git that
    // stopped halfway, not one still at work: remove it and try once more.
    if (!retried && process.exitCode() != 0 && errors.contains(QLatin1String("index.lock"))
            && errors.contains(QLatin1String("File exists"))) {
        const QString lock = indexLockOf(dir);
        if (!lock.isEmpty() && QFileInfo(lock).lastModified().secsTo(QDateTime::currentDateTime()) > 600
                && QFile::remove(lock))
            return runGit(dir, args, out, err, timeoutMs, exitCode, true);
    }
    if (out)
        *out = QString::fromUtf8(process.readAllStandardOutput());
    if (err)
        *err = errors;
    if (exitCode)
        *exitCode = process.exitCode();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QString lastLine(const QString &text) {
    const QStringList lines = text.trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.last().trimmed();
}

// A friendlier message for the usual network and login failures.
QString explainRemoteError(const QString &err) {
    return Sync::explain(err, QString(), QStringLiteral("sync")).message;
}

QStringList absolutePaths(const QString &root, const QString &names) {
    QStringList paths;
    for (const QString &name : names.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        paths.append(QDir(root).filePath(name.trimmed()));
    return paths;
}

bool writeText(const QString &path, const QString &text) {
    QSaveFile file(path);
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    file.write(text.toUtf8());
    return file.commit();
}

QString readText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

// Name.md -> "Name (other version).md", numbered if taken.
QString otherVersionPath(const QString &path) {
    const QFileInfo info(path);
    return Vault::uniquePath(info.absolutePath(), info.completeBaseName() + QStringLiteral(" (other version)"),
                             info.suffix());
}

// git -c user.name/email when the repository has none configured.
QStringList identityArgs(const QString &dir, const QString &fallbackAuthor) {
    QString name;
    QString email;
    runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name")}, &name, nullptr, 5000);
    runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email")}, &email, nullptr, 5000);
    QStringList args;
    if (name.trimmed().isEmpty())
        args << QStringLiteral("-c") << QStringLiteral("user.name=%1").arg(fallbackAuthor.isEmpty() ? QStringLiteral("OmaZettel") : fallbackAuthor);
    if (email.trimmed().isEmpty())
        args << QStringLiteral("-c") << QStringLiteral("user.email=%1@%2")
                    .arg(qEnvironmentVariable("USER", QStringLiteral("omazettel")), QSysInfo::machineHostName());
    return args;
}
}

Sync::Sync(QObject *parent) : QObject(parent) {
    QSettings settings;
    m_autoSync = settings.value(autoSyncSetting, true).toBool();
    m_intervalMinutes = qBound(1, settings.value(intervalSetting, 5).toInt(), 240);
    m_timer.setInterval(m_intervalMinutes * 60000);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        if (m_autoSync && !m_remoteUrl.isEmpty() && !busy() && m_gitConflicts.isEmpty())
            emit autoSyncDue();
    });
    m_timer.start();
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this]() {
        const Result result = m_watcher.result();
        m_lastFailed = !result.ok;
        m_message = result.message;
        m_details = result.ok ? QString() : result.details;
        m_errorKind = result.ok ? QString() : result.kind;
        if (result.ok)
            m_lastSync = QDateTime::currentDateTime();
        refresh();
        emit busyChanged();
        emit synced(result.ok, result.updated);
    });
}

Sync::~Sync() {
    m_watcher.waitForFinished();
}

bool Sync::gitAvailable() const {
    return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

bool Sync::ghAvailable() const {
    return !QStandardPaths::findExecutable(QStringLiteral("gh")).isEmpty();
}

QString Sync::state() const {
    if (!m_gitConflicts.isEmpty())
        return QStringLiteral("conflict");
    if (m_repoRoot.isEmpty())
        return QStringLiteral("off");
    if (m_lastFailed)
        return QStringLiteral("error");
    if (m_remoteUrl.isEmpty())
        return QStringLiteral("local");
    return m_changeCount > 0 ? QStringLiteral("changes") : QStringLiteral("clean");
}

void Sync::setAutoSync(bool enabled) {
    if (m_autoSync == enabled)
        return;
    m_autoSync = enabled;
    QSettings().setValue(autoSyncSetting, enabled);
    emit settingsChanged();
}

void Sync::setIntervalMinutes(int minutes) {
    minutes = qBound(1, minutes, 240);
    if (m_intervalMinutes == minutes)
        return;
    m_intervalMinutes = minutes;
    QSettings().setValue(intervalSetting, minutes);
    m_timer.setInterval(minutes * 60000);
    emit settingsChanged();
}

bool Sync::git(const QStringList &args, QString *out, QString *err, int timeoutMs) const {
    return runGit(m_repoRoot.isEmpty() ? m_root : m_repoRoot, args, out, err, timeoutMs);
}

QString Sync::relative(const QString &path) const {
    return QDir(m_repoRoot).relativeFilePath(path);
}

void Sync::setRoot(const QString &notesRoot) {
    if (m_root == notesRoot)
        return;
    m_root = notesRoot;
    m_lastFailed = false;
    m_message.clear();
    m_details.clear();
    m_errorKind.clear();
    m_lastSync = QDateTime();
    refresh();
}

void Sync::refresh() {
    m_commitName.clear();
    m_commitEmail.clear();
    m_hostKindSetting.clear();
    m_repoRoot.clear();
    m_remoteUrl.clear();
    m_branch.clear();
    m_changeCount = 0;
    m_gitConflicts.clear();
    m_conflictCopies.clear();
    m_outerRepository.clear();
    if (!m_root.isEmpty()) {
        m_repoRoot = repositoryOf(m_root, &m_outerRepository);
        if (!m_repoRoot.isEmpty()) {
            QString out;
            if (git({QStringLiteral("symbolic-ref"), QStringLiteral("--short"), QStringLiteral("HEAD")}, &out))
                m_branch = out.trimmed();
            if (git({QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin")}, &out))
                m_remoteUrl = out.trimmed();
            if (git({QStringLiteral("status"), QStringLiteral("--porcelain")}, &out))
                m_changeCount = int(out.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size());
            if (git({QStringLiteral("diff"), QStringLiteral("--name-only"), QStringLiteral("--diff-filter=U")}, &out))
                m_gitConflicts = absolutePaths(m_repoRoot, out);
            m_commitName = git({QStringLiteral("config"), QStringLiteral("user.name")}, &out) ? out.trimmed() : QString();
            m_commitEmail = git({QStringLiteral("config"), QStringLiteral("user.email")}, &out) ? out.trimmed() : QString();
            m_hostKindSetting = git({QStringLiteral("config"), QStringLiteral("omazettel.hostkind")}, &out) ? out.trimmed() : QString();
        }
        scanConflictCopies(m_root, &m_conflictCopies);
    }
    ++m_revision;
    emit changed();
}

QStringList Sync::scanConflictCopies(const QString &root, QVariantList *details) {
    QStringList copies;
    QDirIterator it(root, {QStringLiteral("*.md"), QStringLiteral("*.markdown")},
                    QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.mid(root.size()).contains(QLatin1String("/.")))
            continue;
        QString tool;
        const QString original = Zettel::conflictOriginal(QFileInfo(path).fileName(), &tool);
        if (original.isEmpty())
            continue;
        copies.append(path);
        if (details) {
            const QString originalPath = QFileInfo(path).absoluteDir().filePath(original);
            details->append(QVariantMap{{QStringLiteral("copy"), path},
                                        {QStringLiteral("original"), originalPath},
                                        {QStringLiteral("originalExists"), QFileInfo(originalPath).isFile()},
                                        {QStringLiteral("tool"), tool}});
        }
    }
    copies.sort();
    return copies;
}

bool Sync::enable() {
    m_error.clear();
    if (m_root.isEmpty() || !m_repoRoot.isEmpty())
        return !m_repoRoot.isEmpty();
    QString err;
    if (!runGit(m_root, {QStringLiteral("init"), QStringLiteral("-b"), QStringLiteral("main")}, nullptr, &err, 15000)) {
        m_error = lastLine(err);
        return false;
    }
    const QStringList identity = identityArgs(m_root, NoteTemplates::systemAuthor());
    runGit(m_root, {QStringLiteral("add"), QStringLiteral("-A")}, nullptr, nullptr, 60000);
    runGit(m_root, identity + QStringList{QStringLiteral("commit"), QStringLiteral("--allow-empty"),
                                          QStringLiteral("-m"), QStringLiteral("Start OmaZettel notes history")},
           nullptr, &err, 60000);
    refresh();
    return isRepo();
}

QString Sync::repositoryOf(const QString &notesRoot, QString *ignored) {
    if (ignored)
        ignored->clear();
    QString top;
    if (notesRoot.isEmpty()
            || !runGit(notesRoot, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, &top, nullptr, 5000))
        return {};
    const QString repository = QFileInfo(top.trimmed()).canonicalFilePath();
    const QString notes = QFileInfo(notesRoot).canonicalFilePath();
    const QString home = QFileInfo(QDir::homePath()).canonicalFilePath();
    // A repository of the whole home folder (kept for settings files, or
    // made by a stray git init) is not the notes' own: a sync would save and
    // send everything in the home folder.
    const bool aboveHome = repository == QLatin1String("/")
        || (!home.isEmpty() && (repository == home || home.startsWith(repository + QLatin1Char('/'))));
    if (repository != notes && aboveHome) {
        if (ignored)
            *ignored = repository;
        return {};
    }
    return repository;
}

QString Sync::checkRemoteAddress(const QString &url) {
    const QString address = url.trimmed();
    if (address.isEmpty())
        return {};
    if (address.contains(QRegularExpression(QStringLiteral("\\s"))))
        return QStringLiteral("A remote's address has no spaces in it.");
    // https://github.com:you/notes.git: the SSH address's colon in an https
    // one, which git reads as a port number.
    static const QRegularExpression colonPath(
        QStringLiteral("^(https?://)([^/@]+@)?([^/:@]+):(?!\\d+(?:/|$))(.+)$"));
    const QRegularExpressionMatch colon = colonPath.match(address);
    if (colon.hasMatch())
        return QStringLiteral("In an https address a slash follows the server's name: %1%2%3/%4")
            .arg(colon.captured(1), colon.captured(2), colon.captured(3), colon.captured(4));
    // github.com/you/notes.git: git would take it for a folder on this computer.
    static const QRegularExpression noScheme(QStringLiteral("^[A-Za-z0-9.-]+\\.[A-Za-z]{2,}/\\S+$"));
    if (noScheme.match(address).hasMatch() && !QFileInfo::exists(address))
        return QStringLiteral("Add https:// at the start: https://%1").arg(address);
    return {};
}

bool Sync::setRemote(const QString &url) {
    m_error.clear();
    if (m_repoRoot.isEmpty())
        return false;
    QString err;
    const QString trimmed = url.trimmed();
    m_error = checkRemoteAddress(trimmed);
    if (!m_error.isEmpty())
        return false;
    bool ok;
    if (trimmed.isEmpty())
        ok = git({QStringLiteral("remote"), QStringLiteral("remove"), QStringLiteral("origin")}, nullptr, &err);
    else if (m_remoteUrl.isEmpty())
        ok = git({QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), trimmed}, nullptr, &err);
    else
        ok = git({QStringLiteral("remote"), QStringLiteral("set-url"), QStringLiteral("origin"), trimmed}, nullptr, &err);
    if (!ok)
        m_error = lastLine(err);
    m_lastFailed = false;
    refresh();
    return ok;
}

void Sync::startWorker(std::function<Result()> work) {
    m_watcher.setFuture(QtConcurrent::run(std::move(work)));
    emit busyChanged();
}

bool Sync::createGitHubRepo(const QString &name, bool isPrivate) {
    m_error.clear();
    if (busy() || m_repoRoot.isEmpty() || !m_remoteUrl.isEmpty() || !ghAvailable()) {
        m_error = !ghAvailable() ? QStringLiteral("The GitHub CLI (gh) is not installed.")
                                 : QStringLiteral("This notes folder already has a remote.");
        return false;
    }
    const QString repoRoot = m_repoRoot;
    const QString repoName = Vault::cleanName(name).replace(QLatin1Char(' '), QLatin1Char('-'));
    const QString branch = m_branch;
    startWorker([repoRoot, repoName, isPrivate, branch]() {
        Result result;
        QProcess gh;
        gh.setWorkingDirectory(repoRoot);
        gh.setProcessEnvironment(gitEnvironment());
        gh.start(QStandardPaths::findExecutable(QStringLiteral("gh")),
                 {QStringLiteral("repo"), QStringLiteral("create"), repoName,
                  isPrivate ? QStringLiteral("--private") : QStringLiteral("--public"),
                  QStringLiteral("--source"), repoRoot, QStringLiteral("--remote"), QStringLiteral("origin"),
                  QStringLiteral("--push")});
        if (!gh.waitForFinished(120000) || gh.exitCode() != 0) {
            const QString err = QString::fromUtf8(gh.readAllStandardError());
            result.message = err.contains(QLatin1String("auth login"))
                ? QStringLiteral("gh is not logged in. Run `gh auth login` in a terminal, then try again.")
                : QStringLiteral("Could not create the repository: %1").arg(lastLine(err));
            result.details = err.trimmed();
            result.kind = QStringLiteral("other");
            log(repoRoot, QStringLiteral("create on GitHub"), result);
            return result;
        }
        result.ok = true;
        result.message = QStringLiteral("Created %1 on GitHub and pushed the notes.").arg(repoName);
        Q_UNUSED(branch);
        return result;
    });
    return true;
}

bool Sync::syncNow() {
    m_error.clear();
    if (busy() || m_repoRoot.isEmpty())
        return false;
    if (!m_gitConflicts.isEmpty()) {
        m_error = QStringLiteral("Resolve the conflicts first.");
        return false;
    }
    const QString repoRoot = m_repoRoot;
    const QString branch = m_branch;
    const bool hasRemote = !m_remoteUrl.isEmpty();
    const QString author = NoteTemplates::systemAuthor();
    startWorker([repoRoot, branch, hasRemote, author]() {
        return runSync(repoRoot, branch, hasRemote, author);
    });
    return true;
}

Sync::Result Sync::runSync(const QString &repoRoot, const QString &branch, bool hasRemote,
                           const QString &fallbackAuthor) {
    Result result;
    QString out;
    QString err;
    int code = 0;
    const QStringList identity = identityArgs(repoRoot, fallbackAuthor);

    // 1. Save this device's changes as a version.
    if (!runGit(repoRoot, {QStringLiteral("add"), QStringLiteral("-A")}, nullptr, &err, 60000)) {
        const Explained why = explain(err, QString(), QStringLiteral("add"));
        result.message = why.message;
        result.kind = why.kind;
        result.details = err;
        return result;
    }
    runGit(repoRoot, {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--quiet")},
           nullptr, nullptr, 30000, &code);
    bool committed = false;
    if (code == 1) {
        const QString message = QStringLiteral("Notes from %1, %2")
            .arg(QSysInfo::machineHostName(), QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        if (!runGit(repoRoot, identity + QStringList{QStringLiteral("commit"), QStringLiteral("-m"), message},
                    nullptr, &err, 60000)) {
            const Explained why = explain(err, QString(), QStringLiteral("commit"));
            result.message = QStringLiteral("Could not save a version: %1").arg(why.message);
            result.kind = why.kind;
            result.details = err;
            return result;
        }
        committed = true;
    }
    if (!hasRemote) {
        result.ok = true;
        result.message = committed ? QStringLiteral("Saved a version (no remote to sync with).")
                                   : QStringLiteral("Nothing new to save.");
        return result;
    }

    QString remoteUrl;
    runGit(repoRoot, {QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin")}, &remoteUrl, nullptr, 5000);
    remoteUrl = remoteUrl.trimmed();

    // 2. Get the other side's versions.
    if (!runGit(repoRoot, {QStringLiteral("fetch"), QStringLiteral("origin")}, nullptr, &err, 90000, &code)) {
        const Explained why = explain(err, remoteUrl, QStringLiteral("fetch"), code);
        result.message = why.message;
        result.kind = why.kind;
        result.details = err;
        log(repoRoot, QStringLiteral("sync"), result);
        return result;
    }

    // 3. Merge them in.
    const QString remoteBranch = QStringLiteral("origin/") + branch;
    const bool remoteHasBranch = runGit(repoRoot, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                                   QStringLiteral("--quiet"), remoteBranch},
                                        nullptr, nullptr, 10000);
    bool pulled = false;
    if (remoteHasBranch) {
        QString before;
        runGit(repoRoot, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &before, nullptr, 10000);
        if (!runGit(repoRoot, identity + QStringList{QStringLiteral("merge"), QStringLiteral("--no-edit"), remoteBranch},
                    &out, &err, 120000)) {
            QString unmerged;
            runGit(repoRoot, {QStringLiteral("diff"), QStringLiteral("--name-only"), QStringLiteral("--diff-filter=U")},
                   &unmerged, nullptr, 10000);
            result.conflicts = absolutePaths(repoRoot, unmerged);
            if (!result.conflicts.isEmpty()) {
                result.message = result.conflicts.size() == 1
                    ? QStringLiteral("A note changed here and elsewhere. Choose which version to keep.")
                    : QStringLiteral("%1 notes changed here and elsewhere. Choose which versions to keep.")
                          .arg(result.conflicts.size());
            } else {
                result.message = QStringLiteral("Could not merge: %1").arg(lastLine(err + QLatin1Char('\n') + out));
                result.details = (err + QLatin1Char('\n') + out).trimmed();
                result.kind = QStringLiteral("other");
            }
            log(repoRoot, QStringLiteral("sync"), result);
            return result;
        }
        QString changedNames;
        runGit(repoRoot, {QStringLiteral("diff"), QStringLiteral("--name-only"), before.trimmed(), QStringLiteral("HEAD")},
               &changedNames, nullptr, 10000);
        result.updated = absolutePaths(repoRoot, changedNames);
        pulled = !result.updated.isEmpty();
    }

    // 4. Send this side's versions.
    QString ahead = QStringLiteral("1");
    if (remoteHasBranch)
        runGit(repoRoot, {QStringLiteral("rev-list"), QStringLiteral("--count"), remoteBranch + QStringLiteral("..HEAD")},
               &ahead, nullptr, 10000);
    bool pushed = false;
    if (ahead.trimmed() != QLatin1String("0")) {
        if (!runGit(repoRoot, {QStringLiteral("push"), QStringLiteral("-u"), QStringLiteral("origin"), branch},
                    nullptr, &err, 120000, &code)) {
            const Explained why = explain(err, remoteUrl, QStringLiteral("push"), code);
            result.message = why.message;
            result.kind = why.kind;
            result.details = err;
            result.ok = false;
            log(repoRoot, QStringLiteral("sync"), result);
            return result;
        }
        pushed = true;
    }

    result.ok = true;
    QStringList parts;
    if (pulled)
        parts << (result.updated.size() == 1 ? QStringLiteral("1 file updated")
                                             : QStringLiteral("%1 files updated").arg(result.updated.size()));
    if (pushed)
        parts << QStringLiteral("changes sent");
    result.message = parts.isEmpty() ? QStringLiteral("Up to date.") : QStringLiteral("Synced: ") + parts.join(QStringLiteral(", ")) + QLatin1Char('.');
    log(repoRoot, QStringLiteral("sync"), result);
    return result;
}

QVariantMap Sync::conflict(const QString &path) const {
    if (m_gitConflicts.contains(path)) {
        const QString rel = relative(path);
        QString mine;
        QString theirs;
        const bool hasMine = git({QStringLiteral("show"), QStringLiteral(":2:") + rel}, &mine);
        const bool hasTheirs = git({QStringLiteral("show"), QStringLiteral(":3:") + rel}, &theirs);
        return {{QStringLiteral("kind"), QStringLiteral("git")},
                {QStringLiteral("path"), path},
                {QStringLiteral("label"), rel},
                {QStringLiteral("mine"), hasMine ? mine : QString()},
                {QStringLiteral("theirs"), hasTheirs ? theirs : QString()},
                {QStringLiteral("mineDeleted"), !hasMine},
                {QStringLiteral("theirsDeleted"), !hasTheirs},
                {QStringLiteral("tool"), QStringLiteral("git")}};
    }
    for (const QVariant &entry : m_conflictCopies) {
        const QVariantMap copy = entry.toMap();
        if (copy.value(QStringLiteral("copy")).toString() != path)
            continue;
        const QString original = copy.value(QStringLiteral("original")).toString();
        return {{QStringLiteral("kind"), QStringLiteral("copy")},
                {QStringLiteral("path"), path},
                {QStringLiteral("label"), QDir(m_root).relativeFilePath(original)},
                {QStringLiteral("mine"), readText(original)},
                {QStringLiteral("theirs"), readText(path)},
                {QStringLiteral("mineDeleted"), !QFileInfo(original).isFile()},
                {QStringLiteral("theirsDeleted"), false},
                {QStringLiteral("tool"), copy.value(QStringLiteral("tool"))}};
    }
    return {};
}

bool Sync::resolve(const QString &path, const QString &choice) {
    m_error.clear();
    const QVariantMap item = conflict(path);
    if (item.isEmpty()) {
        m_error = QStringLiteral("That conflict is no longer there.");
        return false;
    }
    const QString mine = item.value(QStringLiteral("mine")).toString();
    const QString theirs = item.value(QStringLiteral("theirs")).toString();

    if (item.value(QStringLiteral("kind")) == QLatin1String("copy")) {
        const QString original = QFileInfo(path).absoluteDir().filePath(
            Zettel::conflictOriginal(QFileInfo(path).fileName()));
        bool ok = false;
        if (choice == QLatin1String("mine")) {
            ok = QFile::moveToTrash(path);
        } else if (choice == QLatin1String("theirs")) {
            // The original goes to the trash, so nothing is lost.
            ok = (!QFileInfo(original).exists() || QFile::moveToTrash(original)) && QFile::rename(path, original);
        } else if (choice == QLatin1String("both")) {
            ok = QFile::rename(path, otherVersionPath(original));
        }
        if (!ok)
            m_error = QStringLiteral("Could not resolve %1.").arg(QFileInfo(path).fileName());
        refresh();
        return ok;
    }

    // Git: write the chosen text, mark it resolved, and finish the merge
    // when nothing is left.
    const QString rel = relative(path);
    QString err;
    bool ok = true;
    if (choice == QLatin1String("mine")) {
        if (item.value(QStringLiteral("mineDeleted")).toBool())
            ok = git({QStringLiteral("rm"), QStringLiteral("--quiet"), rel}, nullptr, &err);
        else
            ok = writeText(path, mine) && git({QStringLiteral("add"), rel}, nullptr, &err);
    } else if (choice == QLatin1String("theirs")) {
        if (item.value(QStringLiteral("theirsDeleted")).toBool())
            ok = git({QStringLiteral("rm"), QStringLiteral("--quiet"), rel}, nullptr, &err);
        else
            ok = writeText(path, theirs) && git({QStringLiteral("add"), rel}, nullptr, &err);
    } else if (choice == QLatin1String("both")) {
        const QString other = otherVersionPath(path);
        ok = writeText(path, mine) && writeText(other, theirs)
            && git({QStringLiteral("add"), rel, relative(other)}, nullptr, &err);
    } else if (choice == QLatin1String("edited")) {
        static const QRegularExpression markers(QStringLiteral("^(<{7}|={7}|>{7})( |$)"),
                                                QRegularExpression::MultilineOption);
        if (markers.match(readText(path)).hasMatch()) {
            m_error = QStringLiteral("The note still has <<<<<<< ======= >>>>>>> lines; remove them first.");
            return false;
        }
        ok = git({QStringLiteral("add"), rel}, nullptr, &err);
    } else {
        return false;
    }
    if (!ok) {
        m_error = err.isEmpty() ? QStringLiteral("Could not write %1.").arg(rel) : lastLine(err);
        return false;
    }

    QString unmerged;
    git({QStringLiteral("diff"), QStringLiteral("--name-only"), QStringLiteral("--diff-filter=U")}, &unmerged);
    if (unmerged.trimmed().isEmpty()) {
        const QStringList identity = identityArgs(m_repoRoot, NoteTemplates::systemAuthor());
        if (!git(identity + QStringList{QStringLiteral("commit"), QStringLiteral("--no-edit")}, nullptr, &err, 60000)) {
            m_error = QStringLiteral("Could not finish the merge: %1").arg(lastLine(err));
            refresh();
            return false;
        }
        m_lastFailed = false;
        m_message = QStringLiteral("Conflicts resolved.");
    }
    refresh();
    return true;
}

bool Sync::abortMerge() {
    QString err;
    const bool ok = git({QStringLiteral("merge"), QStringLiteral("--abort")}, nullptr, &err);
    if (!ok)
        m_error = lastLine(err);
    if (ok) {
        m_lastFailed = false;
        m_message = QStringLiteral("Merge undone; nothing from the other side was taken.");
    }
    refresh();
    return ok;
}

QVariantList Sync::history(const QString &path) const {
    QVariantList result;
    if (m_repoRoot.isEmpty())
        return result;
    QString out;
    if (!git({QStringLiteral("log"), QStringLiteral("--follow"), QStringLiteral("--name-only"),
              QStringLiteral("--format=%x1e%H%x1f%an%x1f%aI%x1f%s"), QStringLiteral("--"), relative(path)}, &out, nullptr, 30000))
        return result;
    for (const QString &block : out.split(QChar(0x1e), Qt::SkipEmptyParts)) {
        const QStringList lines = block.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (lines.isEmpty())
            continue;
        const QStringList fields = lines.first().split(QChar(0x1f));
        if (fields.size() < 4)
            continue;
        result.append(QVariantMap{{QStringLiteral("hash"), fields.at(0)},
                                  {QStringLiteral("author"), fields.at(1)},
                                  {QStringLiteral("date"), fields.at(2)},
                                  {QStringLiteral("message"), fields.at(3)},
                                  {QStringLiteral("path"), lines.size() > 1 ? lines.at(1).trimmed() : relative(path)}});
    }
    return result;
}

QString Sync::versionText(const QString &hash, const QString &path) const {
    QString out;
    const QString rel = QDir::isAbsolutePath(path) ? relative(path) : path;
    git({QStringLiteral("show"), hash + QLatin1Char(':') + rel}, &out, nullptr, 30000);
    return out;
}

QVariantList Sync::diff(const QString &before, const QString &after) const {
    return TextDiff::lines(before, after);
}

// ── Explaining failures ─────────────────────────────────────────────────────

namespace {

QString redact(QString text) {
    static const QRegularExpression credentialRe(QStringLiteral("(https?://)[^/@\\s]+@"));
    return text.replace(credentialRe, QStringLiteral("\\1***@"));
}

// The line of git's output that says the most: a server's own error, then
// git's, skipping the generic "failed to push some refs".
QString telling(const QString &text) {
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &prefix : {QStringLiteral("remote: error"), QStringLiteral("! ["), QStringLiteral("remote: "),
                                  QStringLiteral("fatal:"), QStringLiteral("error:")}) {
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (!line.startsWith(prefix) || line.contains(QLatin1String("failed to push some refs")))
                continue;
            QString cleaned = line;
            cleaned.remove(QRegularExpression(QStringLiteral("^(remote: (error: )?|fatal: |error: )")));
            if (!cleaned.trimmed().isEmpty() && !cleaned.trimmed().startsWith(QLatin1String("----")))
                return cleaned.trimmed();
        }
    }
    return lines.isEmpty() ? QString() : lines.last().trimmed();
}

QString keysPage(const QString &kind, const QString &host) {
    if (kind == QLatin1String("github"))
        return QStringLiteral("https://github.com/settings/keys");
    if (kind == QLatin1String("gitlab"))
        return QStringLiteral("https://%1/-/user_settings/ssh_keys").arg(host);
    if (kind == QLatin1String("gogs"))
        return QStringLiteral("https://%1/user/settings/ssh").arg(host);
    if (kind == QLatin1String("gitea") || kind == QLatin1String("forgejo"))
        return QStringLiteral("https://%1/user/settings/keys").arg(host);
    return QStringLiteral("the SSH keys page of your account on %1").arg(host);
}

QString tokenPage(const QString &kind, const QString &host) {
    if (kind == QLatin1String("github"))
        return QStringLiteral("run `gh auth login`, then `gh auth setup-git`, in a terminal");
    if (kind == QLatin1String("gitlab"))
        return QStringLiteral("make a personal access token with write_repository at https://%1/-/user_settings/personal_access_tokens "
                              "and save it under Sign in with a token (or run `glab auth login`)").arg(host);
    if (kind == QLatin1String("gitea") || kind == QLatin1String("forgejo") || kind == QLatin1String("gogs"))
        return QStringLiteral("make an access token (with repository write access) at https://%1/user/settings/applications "
                              "and save it under Sign in with a token").arg(host);
    return QStringLiteral("save a username and access token under Sign in with a token");
}

struct Http {
    bool ok = false;
    int status = 0;
    QJsonObject json;
    QString error;
};

// A JSON request with curl; headers (which hold the token) go on standard
// input, out of the process list.
Http httpJson(const QString &method, const QString &url, const QStringList &headers, const QJsonObject &body) {
    Http result;
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty()) {
        result.error = QStringLiteral("curl is not installed.");
        return result;
    }
    QByteArray config;
    for (const QString &header : headers)
        config += "header = \"" + header.toUtf8() + "\"\n";
    if (!body.isEmpty())
        config += "data = \"" + QJsonDocument(body).toJson(QJsonDocument::Compact).replace('\\', "\\\\").replace('"', "\\\"") + "\"\n";
    QProcess process;
    process.start(curl, {QStringLiteral("-sS"), QStringLiteral("--max-time"), QStringLiteral("60"), QStringLiteral("-X"), method,
                         QStringLiteral("-H"), QStringLiteral("Content-Type: application/json"),
                         QStringLiteral("-H"), QStringLiteral("Accept: application/json"),
                         QStringLiteral("-w"), QStringLiteral("\n%{http_code}"), QStringLiteral("-K"), QStringLiteral("-"), url});
    if (!process.waitForStarted(10000)) {
        result.error = QStringLiteral("curl did not start.");
        return result;
    }
    process.write(config.isEmpty() ? QByteArray("\n") : config);
    process.closeWriteChannel();
    process.waitForFinished(70000);
    const QString out = QString::fromUtf8(process.readAllStandardOutput());
    const int split = int(out.lastIndexOf(QLatin1Char('\n')));
    result.status = out.mid(split + 1).trimmed().toInt();
    result.json = QJsonDocument::fromJson(out.left(qMax(0, split)).toUtf8()).object();
    if (process.exitCode() != 0) {
        result.error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return result;
    }
    result.ok = result.status >= 200 && result.status < 300;
    if (!result.ok) {
        const QJsonValue message = result.json.value(QStringLiteral("message"));
        result.error = message.isString() ? message.toString()
                     : message.isObject() || message.isArray() ? QString::fromUtf8(QJsonDocument(message.isObject() ? QJsonDocument(message.toObject()) : QJsonDocument(message.toArray())).toJson(QJsonDocument::Compact))
                     : result.json.value(QStringLiteral("error")).toString();
        if (result.error.isEmpty())
            result.error = QStringLiteral("HTTP %1").arg(result.status);
    }
    return result;
}

QString serverUrl(QString server, const QString &fallback) {
    server = server.trimmed();
    if (server.isEmpty())
        server = fallback;
    if (!server.startsWith(QLatin1String("http")))
        server.prepend(QStringLiteral("https://"));
    while (server.endsWith(QLatin1Char('/')))
        server.chop(1);
    return server;
}

}

QString Sync::hostOf(const QString &url) {
    QString text = url.trimmed();
    if (text.isEmpty())
        return QString();
    static const QRegularExpression schemeRe(QStringLiteral("^[a-z+]+://(?:[^@/]+@)?([^/:]+)"));
    QRegularExpressionMatch match = schemeRe.match(text);
    if (match.hasMatch())
        return match.captured(1).toLower();
    static const QRegularExpression scpRe(QStringLiteral("^(?:[^@]+@)?([^:/]+):"));
    match = scpRe.match(text);
    return match.hasMatch() ? match.captured(1).toLower() : QString();
}

QString Sync::hostKindOf(const QString &url) {
    const QString host = hostOf(url);
    if (host == QLatin1String("github.com") || host.endsWith(QLatin1String(".github.com")))
        return QStringLiteral("github");
    if (host == QLatin1String("codeberg.org") || host.contains(QLatin1String("forgejo")))
        return QStringLiteral("forgejo");
    if (host.contains(QLatin1String("gitlab")))
        return QStringLiteral("gitlab");
    if (host.contains(QLatin1String("gitea")))
        return QStringLiteral("gitea");
    if (host.contains(QLatin1String("gogs")))
        return QStringLiteral("gogs");
    return QStringLiteral("other");
}

QString Sync::hostKind() const {
    return m_hostKindSetting.isEmpty() ? hostKindOf(m_remoteUrl) : m_hostKindSetting;
}

Sync::Explained Sync::explain(const QString &errorOutput, const QString &remoteUrl, const QString &step, int exitCode) {
    const QString err = errorOutput.trimmed();
    const QString lower = err.toLower();
    const QString host = hostOf(remoteUrl).isEmpty() ? QStringLiteral("the remote") : hostOf(remoteUrl);
    const QString kind = hostKindOf(remoteUrl);
    const bool https = remoteUrl.startsWith(QLatin1String("http"));
    const QString line = telling(err);

    if (lower.contains(QLatin1String("gh007")) || lower.contains(QLatin1String("would publish a private email")))
        return {QStringLiteral("GitHub refused the push: these versions are saved under your private email address, and your "
                               "GitHub settings block pushes that would publish it. Use your GitHub no-reply address for "
                               "these notes and sync again."),
                QStringLiteral("private-email")};
    if (lower.contains(QLatin1String("gh001")) || lower.contains(QLatin1String("large file"))
            || lower.contains(QLatin1String("file size limit")) || lower.contains(QLatin1String("exceeds")))
        return {QStringLiteral("%1 refused a file that is too large: %2. Move it out of the notes folder (or list it "
                               "in .gitignore) and sync again.").arg(host, line),
                QStringLiteral("too-large")};
    if (lower.contains(QLatin1String("protected branch")) || lower.contains(QLatin1String("pre-receive hook declined"))
            || lower.contains(QLatin1String("push declined")))
        return {QStringLiteral("%1 refused the push: %2").arg(host, line), QStringLiteral("rejected")};
    if (lower.contains(QLatin1String("[rejected]")) && (lower.contains(QLatin1String("fetch first"))
                                                       || lower.contains(QLatin1String("non-fast-forward"))))
        return {QStringLiteral("%1 has versions this device does not have yet. Sync again to bring them in first.").arg(host),
                QStringLiteral("behind")};
    if (lower.contains(QLatin1String("took too long")))
        return {QStringLiteral("git %1 took too long and was stopped. A very large notes folder, or one that iCloud Drive or "
                               "another sync service is also syncing, can do this.").arg(step),
                QStringLiteral("slow")};
    if (lower.contains(QLatin1String("index.lock")) && lower.contains(QLatin1String("file exists")))
        return {QStringLiteral("Another git command is working in this notes folder, or one stopped halfway and left its "
                               "lock file behind (.git/index.lock). If no git command is running, delete that file and "
                               "sync again; OmaZettel removes it itself once it is ten minutes old."),
                QStringLiteral("lock")};
    if (lower.contains(QLatin1String("host key verification failed")))
        return {QStringLiteral("This computer does not know %1's SSH key yet. Run once in a terminal: ssh -T git@%1 "
                               "and answer yes.").arg(host),
                QStringLiteral("host-key")};
    if (lower.contains(QLatin1String("permission denied (publickey")))
        return {QStringLiteral("%1 did not accept this computer's SSH key. Add your public key (~/.ssh/id_ed25519.pub) at %2, "
                               "or use the https address and sign in with a token.").arg(host, keysPage(kind, host)),
                QStringLiteral("ssh-key")};
    if ((lower.contains(QLatin1String("permission to")) && lower.contains(QLatin1String("denied")))
            || lower.contains(QLatin1String("the requested url returned error: 403"))
            || lower.contains(QLatin1String("you are not allowed to push")))
        return {QStringLiteral("Signed in to %1, but this account cannot write to the repository (%2). Sign in as an "
                               "account with write access.").arg(host, line),
                QStringLiteral("forbidden")};
    if (lower.contains(QLatin1String("authentication failed")) || lower.contains(QLatin1String("could not read username"))
            || lower.contains(QLatin1String("could not read password")) || lower.contains(QLatin1String("terminal prompts disabled"))
            || lower.contains(QLatin1String("invalid username or password")) || lower.contains(QLatin1String("error: 401"))
            || lower.contains(QLatin1String("http basic: access denied")))
        return {https ? QStringLiteral("Git could not sign in to %1. To fix it, %2.").arg(host, tokenPage(kind, host))
                      : QStringLiteral("Git could not sign in to %1 over SSH. Add your public key at %2.").arg(host, keysPage(kind, host)),
                QStringLiteral("auth")};
    if (lower.contains(QLatin1String("ssl certificate problem")) || lower.contains(QLatin1String("unable to get local issuer"))
            || lower.contains(QLatin1String("self-signed certificate")) || lower.contains(QLatin1String("certificate verify failed")))
        return {QStringLiteral("%1's https certificate is not trusted by this computer (a self-signed or private one?). "
                               "Trust its certificate (sudo trust anchor <file.crt>) or use an SSH address.").arg(host),
                QStringLiteral("certificate")};
    if (lower.contains(QLatin1String("could not resolve host")) || lower.contains(QLatin1String("network is unreachable"))
            || lower.contains(QLatin1String("connection refused")) || lower.contains(QLatin1String("connection timed out"))
            || lower.contains(QLatin1String("failed to connect")) || lower.contains(QLatin1String("took too long"))
            || lower.contains(QLatin1String("operation timed out")))
        return {QStringLiteral("Could not reach %1 (%2). Changes are saved here and will sync when it can be reached.").arg(host, line),
                QStringLiteral("network")};
    if (lower.contains(QLatin1String("repository not found")) || lower.contains(QLatin1String("does not appear to be a git repository"))
            || lower.contains(QLatin1String("not found")))
        return {QStringLiteral("There is no repository at %1, or this account cannot see it. Check the address under Remote.").arg(remoteUrl.isEmpty() ? host : remoteUrl),
                QStringLiteral("not-found")};
    if (line.isEmpty())
        return {QStringLiteral("git %1 stopped (exit %2) without saying why. The sync log has the details.").arg(step).arg(exitCode),
                QStringLiteral("other")};
    return {QStringLiteral("git %1 failed: %2").arg(step, line), QStringLiteral("other")};
}

// ── Log ─────────────────────────────────────────────────────────────────────

QString Sync::logPath() {
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/omazettel/sync.log");
}

void Sync::log(const QString &repoRoot, const QString &what, const Result &result) {
    const QString path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    // Kept short: the last ~200 KB.
    if (QFileInfo(path).size() > 400 * 1024) {
        QFile old(path);
        if (old.open(QIODevice::ReadOnly)) {
            const QByteArray tail = old.readAll().right(200 * 1024);
            old.close();
            QSaveFile trimmed(path);
            if (trimmed.open(QIODevice::WriteOnly)) {
                trimmed.write(tail);
                trimmed.commit();
            }
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    QString entry = QStringLiteral("%1  %2  %3  %4: %5\n")
                        .arg(QDateTime::currentDateTime().toString(Qt::ISODate), repoRoot, what,
                             result.ok ? QStringLiteral("ok") : QStringLiteral("FAILED"), result.message);
    if (!result.ok && !result.details.trimmed().isEmpty()) {
        for (const QString &line : result.details.trimmed().split(QLatin1Char('\n')))
            entry += QStringLiteral("    ") + line + QLatin1Char('\n');
    }
    file.write(redact(entry).toUtf8());
}

// ── Remotes on other hosts ──────────────────────────────────────────────────

bool Sync::storeToken(const QString &repoRoot, const QString &url, const QString &username, const QString &token,
                      QString *error) {
    if (!url.startsWith(QLatin1String("https://")) && !url.startsWith(QLatin1String("http://"))) {
        *error = QStringLiteral("Tokens are for https addresses; this remote uses SSH, which signs in with your SSH key.");
        return false;
    }
    if (token.trimmed().isEmpty()) {
        *error = QStringLiteral("Paste the access token.");
        return false;
    }
    static const QRegularExpression partsRe(QStringLiteral("^(https?)://(?:[^@/]+@)?([^/]+)"));
    const QRegularExpressionMatch parts = partsRe.match(url);
    if (!parts.hasMatch()) {
        *error = QStringLiteral("%1 is not an https address.").arg(url);
        return false;
    }
    // Somewhere to keep it: the helper git already uses for this server, else
    // the desktop keyring (libsecret), else git's plain-text store.
    QString helper;
    runGit(repoRoot, {QStringLiteral("config"), QStringLiteral("--get-urlmatch"), QStringLiteral("credential.helper"),
                      parts.captured(1) + QStringLiteral("://") + parts.captured(2)},
           &helper, nullptr, 5000);
    if (helper.trimmed().isEmpty()) {
        const QString libsecret = QStringLiteral("/usr/lib/git-core/git-credential-libsecret");
        runGit(repoRoot, {QStringLiteral("config"), QStringLiteral("credential.helper"),
                          QFileInfo::exists(libsecret) ? libsecret : QStringLiteral("store")},
               nullptr, nullptr, 5000);
    }
    QProcess process;
    process.setWorkingDirectory(repoRoot);
    process.setProcessEnvironment(gitEnvironment());
    process.start(QStandardPaths::findExecutable(QStringLiteral("git")), {QStringLiteral("credential"), QStringLiteral("approve")});
    if (!process.waitForStarted(5000)) {
        *error = QStringLiteral("git did not start.");
        return false;
    }
    const QString user = username.trimmed().isEmpty() ? QStringLiteral("oauth2") : username.trimmed();
    process.write(QStringLiteral("protocol=%1\nhost=%2\nusername=%3\npassword=%4\n\n")
                      .arg(parts.captured(1), parts.captured(2), user, token.trimmed()).toUtf8());
    process.closeWriteChannel();
    process.waitForFinished(15000);
    if (process.exitCode() != 0) {
        *error = QStringLiteral("git could not keep the token: %1").arg(lastLine(QString::fromUtf8(process.readAllStandardError())));
        return false;
    }
    // The username goes in the address, so git asks the helper for this one.
    if (!url.contains(QLatin1Char('@'))) {
        QString withUser = url;
        withUser.insert(url.indexOf(QStringLiteral("://")) + 3, QUrl::toPercentEncoding(user) + QLatin1Char('@'));
        runGit(repoRoot, {QStringLiteral("remote"), QStringLiteral("set-url"), QStringLiteral("origin"), withUser}, nullptr, nullptr, 5000);
    }
    return true;
}

Sync::Result Sync::testRemoteBlocking(const QString &repoRoot, const QString &branch) {
    Result result;
    QString url;
    runGit(repoRoot, {QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin")}, &url, nullptr, 5000);
    url = url.trimmed();
    if (url.isEmpty()) {
        result.message = QStringLiteral("There is no remote to test.");
        return result;
    }
    QString err;
    int code = 0;
    if (!runGit(repoRoot, {QStringLiteral("ls-remote"), QStringLiteral("--heads"), QStringLiteral("origin")}, nullptr, &err, 60000, &code)) {
        const Explained why = explain(err, url, QStringLiteral("ls-remote"), code);
        result.message = QStringLiteral("Cannot read the remote. ") + why.message;
        result.kind = why.kind;
        result.details = err;
        log(repoRoot, QStringLiteral("test"), result);
        return result;
    }
    // Writing: a dry-run push asks the server for write access without
    // sending anything.
    const QString ref = branch.isEmpty() ? QStringLiteral("HEAD") : branch;
    if (!runGit(repoRoot, {QStringLiteral("push"), QStringLiteral("--dry-run"), QStringLiteral("origin"),
                           QStringLiteral("HEAD:refs/heads/") + (branch.isEmpty() ? QStringLiteral("main") : branch)},
                nullptr, &err, 60000, &code)) {
        const Explained why = explain(err, url, QStringLiteral("push"), code);
        // Being behind is fine for a test: reading and writing both work.
        if (why.kind != QLatin1String("behind")) {
            result.message = QStringLiteral("Can read the remote, but not write to it. ") + why.message;
            result.kind = why.kind;
            result.details = err;
            log(repoRoot, QStringLiteral("test"), result);
            return result;
        }
    }
    Q_UNUSED(ref);
    result.ok = true;
    result.message = QStringLiteral("Connected to %1: signed in, can read and write.").arg(hostOf(url));
    log(repoRoot, QStringLiteral("test"), result);
    return result;
}

Sync::Result Sync::useNoReplyBlocking(const QString &repoRoot, const QString &branch, bool hasRemote,
                                      const QString &fallbackAuthor) {
    Result result;
    const QString gh = QStandardPaths::findExecutable(QStringLiteral("gh"));
    if (gh.isEmpty()) {
        result.message = QStringLiteral("Finding your no-reply address needs the GitHub CLI (gh). Or set the email yourself "
                                        "to ID+username@users.noreply.github.com (see github.com/settings/emails).");
        return result;
    }
    QProcess process;
    process.setProcessEnvironment(gitEnvironment());
    process.start(gh, {QStringLiteral("api"), QStringLiteral("user"), QStringLiteral("--jq"),
                       QStringLiteral("\"\\(.id)+\\(.login)@users.noreply.github.com\"")});
    process.waitForFinished(30000);
    const QString email = QString::fromUtf8(process.readAllStandardOutput()).trimmed().remove(QLatin1Char('"'));
    if (process.exitCode() != 0 || !email.contains(QLatin1String("noreply"))) {
        result.message = QStringLiteral("Could not ask GitHub for your no-reply address: %1")
                             .arg(lastLine(QString::fromUtf8(process.readAllStandardError())));
        return result;
    }
    QString err;
    runGit(repoRoot, {QStringLiteral("config"), QStringLiteral("user.email"), email}, nullptr, nullptr, 5000);
    QString name;
    runGit(repoRoot, {QStringLiteral("config"), QStringLiteral("user.name")}, &name, nullptr, 5000);
    if (name.trimmed().isEmpty())
        runGit(repoRoot, {QStringLiteral("config"), QStringLiteral("user.name"), fallbackAuthor.isEmpty() ? QStringLiteral("OmaZettel") : fallbackAuthor}, nullptr, nullptr, 5000);

    // Re-sign the versions GitHub has not seen with the new address.
    const QString remoteBranch = QStringLiteral("origin/") + branch;
    const bool remoteHasBranch = runGit(repoRoot, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"), remoteBranch},
                                        nullptr, nullptr, 10000);
    QStringList rebase{QStringLiteral("rebase"), QStringLiteral("--rebase-merges"), QStringLiteral("--exec"),
                       QStringLiteral("git commit --amend --no-edit --reset-author --allow-empty --no-verify")};
    rebase << (remoteHasBranch ? remoteBranch : QStringLiteral("--root"));
    if (!runGit(repoRoot, rebase, nullptr, &err, 120000)) {
        runGit(repoRoot, {QStringLiteral("rebase"), QStringLiteral("--abort")}, nullptr, nullptr, 30000);
        result.message = QStringLiteral("Switched to %1, but could not re-sign the earlier versions: %2").arg(email, lastLine(err));
        result.details = err;
        log(repoRoot, QStringLiteral("no-reply email"), result);
        return result;
    }
    Result synced = runSync(repoRoot, branch, hasRemote, fallbackAuthor);
    if (synced.ok)
        synced.message = QStringLiteral("Now saving versions as %1. %2").arg(email, synced.message);
    return synced;
}

Sync::Result Sync::createRemoteBlocking(const QString &repoRoot, const QString &branch, const QString &providerName,
                                        const QString &server, const QString &token, const QString &name,
                                        bool isPrivate, const QString &fallbackAuthor) {
    Result result;
    const QString provider = providerName.trimmed().toLower();
    const QString repoName = Vault::cleanName(name).replace(QLatin1Char(' '), QLatin1Char('-'));
    if (repoName.isEmpty()) {
        result.message = QStringLiteral("Name the repository.");
        return result;
    }
    if (token.trimmed().isEmpty()) {
        result.message = QStringLiteral("Paste an access token for the server (it needs permission to create repositories).");
        return result;
    }
    QString cloneUrl;
    QString user;
    QString kind = provider;
    Http answer;
    if (provider == QLatin1String("github")) {
        answer = httpJson(QStringLiteral("POST"), QStringLiteral("https://api.github.com/user/repos"),
                          {QStringLiteral("Authorization: Bearer ") + token.trimmed()},
                          {{QStringLiteral("name"), repoName}, {QStringLiteral("private"), isPrivate}});
        cloneUrl = answer.json.value(QStringLiteral("clone_url")).toString();
        user = answer.json.value(QStringLiteral("owner")).toObject().value(QStringLiteral("login")).toString();
    } else if (provider == QLatin1String("gitlab")) {
        const QString base = serverUrl(server, QStringLiteral("gitlab.com"));
        answer = httpJson(QStringLiteral("POST"), base + QStringLiteral("/api/v4/projects"),
                          {QStringLiteral("PRIVATE-TOKEN: ") + token.trimmed()},
                          {{QStringLiteral("name"), repoName}, {QStringLiteral("path"), repoName},
                           {QStringLiteral("visibility"), isPrivate ? QStringLiteral("private") : QStringLiteral("public")}});
        cloneUrl = answer.json.value(QStringLiteral("http_url_to_repo")).toString();
        user = answer.json.value(QStringLiteral("owner")).toObject().value(QStringLiteral("username")).toString();
        if (user.isEmpty())
            user = answer.json.value(QStringLiteral("namespace")).toObject().value(QStringLiteral("path")).toString();
    } else if (provider == QLatin1String("gitea") || provider == QLatin1String("forgejo") || provider == QLatin1String("codeberg")
               || provider == QLatin1String("gogs")) {
        const QString base = serverUrl(server, provider == QLatin1String("codeberg") ? QStringLiteral("codeberg.org") : QString());
        if (base == QLatin1String("https://")) {
            result.message = QStringLiteral("Give the server's address, e.g. https://git.example.com");
            return result;
        }
        if (provider == QLatin1String("codeberg"))
            kind = QStringLiteral("forgejo");
        answer = httpJson(QStringLiteral("POST"), base + QStringLiteral("/api/v1/user/repos"),
                          {QStringLiteral("Authorization: token ") + token.trimmed()},
                          {{QStringLiteral("name"), repoName}, {QStringLiteral("private"), isPrivate}});
        cloneUrl = answer.json.value(QStringLiteral("clone_url")).toString();
        const QJsonObject owner = answer.json.value(QStringLiteral("owner")).toObject();
        user = owner.value(QStringLiteral("login")).toString();
        if (user.isEmpty())
            user = owner.value(QStringLiteral("username")).toString();
    } else {
        result.message = QStringLiteral("OmaZettel can make repositories on GitHub, GitLab, Codeberg, Forgejo, Gitea and Gogs. "
                                        "For anything else, make the repository there and paste its address under Remote.");
        return result;
    }
    if (!answer.ok || cloneUrl.isEmpty()) {
        const QString why = answer.error.isEmpty() ? QStringLiteral("no repository address came back") : answer.error;
        result.message = answer.status == 401 || answer.status == 403
            ? QStringLiteral("The server did not accept the token (%1). Check it is current and may create repositories.").arg(why)
            : answer.status == 409 || answer.status == 422 || why.contains(QLatin1String("already"))
                ? QStringLiteral("Could not make %1: %2 (a repository of that name may already exist; paste its address under Remote instead).").arg(repoName, why)
                : QStringLiteral("Could not make %1: %2").arg(repoName, why);
        result.details = QString::fromUtf8(QJsonDocument(answer.json).toJson());
        result.kind = QStringLiteral("other");
        log(repoRoot, QStringLiteral("create on ") + provider, result);
        return result;
    }
    QString err;
    QString existing;
    const bool hasOrigin = runGit(repoRoot, {QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin")}, &existing, nullptr, 5000);
    runGit(repoRoot, {QStringLiteral("remote"), hasOrigin ? QStringLiteral("set-url") : QStringLiteral("add"), QStringLiteral("origin"), cloneUrl},
           nullptr, &err, 5000);
    runGit(repoRoot, {QStringLiteral("config"), QStringLiteral("omazettel.hostkind"), kind}, nullptr, nullptr, 5000);
    if (!storeToken(repoRoot, cloneUrl, user, token, &err)) {
        result.message = QStringLiteral("Made %1, but could not keep the token for git: %2").arg(cloneUrl, err);
        return result;
    }
    Result synced = runSync(repoRoot, branch, true, fallbackAuthor);
    synced.message = synced.ok ? QStringLiteral("Made %1 and sent the notes there.").arg(cloneUrl)
                               : QStringLiteral("Made %1, but the first sync failed: %2").arg(cloneUrl, synced.message);
    return synced;
}

// ── The same, from the window ───────────────────────────────────────────────

bool Sync::createRemoteRepo(const QString &provider, const QString &server, const QString &token,
                            const QString &name, bool isPrivate) {
    m_error.clear();
    if (busy() || m_repoRoot.isEmpty())
        return false;
    if (provider == QLatin1String("github") && token.trimmed().isEmpty())
        return createGitHubRepo(name, isPrivate);
    const QString repoRoot = m_repoRoot;
    const QString branch = m_branch.isEmpty() ? QStringLiteral("main") : m_branch;
    const QString author = NoteTemplates::systemAuthor();
    startWorker([=]() { return createRemoteBlocking(repoRoot, branch, provider, server, token, name, isPrivate, author); });
    return true;
}

bool Sync::saveToken(const QString &username, const QString &token) {
    m_error.clear();
    if (m_repoRoot.isEmpty() || m_remoteUrl.isEmpty()) {
        m_error = QStringLiteral("Save the remote's address first.");
        return false;
    }
    const bool ok = storeToken(m_repoRoot, m_remoteUrl, username, token, &m_error);
    refresh();
    return ok;
}

bool Sync::testRemote() {
    m_error.clear();
    if (busy() || m_repoRoot.isEmpty() || m_remoteUrl.isEmpty())
        return false;
    const QString repoRoot = m_repoRoot;
    const QString branch = m_branch;
    startWorker([=]() { return testRemoteBlocking(repoRoot, branch); });
    return true;
}

bool Sync::setCommitIdentity(const QString &name, const QString &email) {
    m_error.clear();
    if (m_repoRoot.isEmpty())
        return false;
    QString err;
    bool ok = true;
    if (!name.trimmed().isEmpty())
        ok = git({QStringLiteral("config"), QStringLiteral("user.name"), name.trimmed()}, nullptr, &err) && ok;
    if (!email.trimmed().isEmpty())
        ok = git({QStringLiteral("config"), QStringLiteral("user.email"), email.trimmed()}, nullptr, &err) && ok;
    if (!ok)
        m_error = lastLine(err);
    refresh();
    return ok;
}

bool Sync::useNoReplyEmail() {
    m_error.clear();
    if (busy() || m_repoRoot.isEmpty())
        return false;
    const QString repoRoot = m_repoRoot;
    const QString branch = m_branch;
    const bool hasRemote = !m_remoteUrl.isEmpty();
    const QString author = NoteTemplates::systemAuthor();
    startWorker([=]() { return useNoReplyBlocking(repoRoot, branch, hasRemote, author); });
    return true;
}

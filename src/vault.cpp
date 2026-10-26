#include "vault.h"

#include "layout.h"
#include "notestree.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSettings>

namespace {
const QString folderSetting = QStringLiteral("notes/folder");
const QString recentSetting = QStringLiteral("notes/recentFolders");
const QString lastNoteSetting = QStringLiteral("notes/lastNote");
const QString sidebarVisibleSetting = QStringLiteral("notes/sidebarVisible");
const QString sidebarWidthSetting = QStringLiteral("notes/sidebarWidth");
const QString panelVisibleSetting = QStringLiteral("notes/panelVisible");
const int maxRecentFolders = 8;
const QStringList noteSuffixes{QStringLiteral("md"), QStringLiteral("markdown")};
}

Vault::Vault(QObject *parent) : QObject(parent), m_model(new NotesTree(this)) {
    QSettings settings;
    m_recentFolders = settings.value(recentSetting).toStringList();
    m_sidebarVisible = settings.value(sidebarVisibleSetting, true).toBool();
    m_sidebarWidth = settings.value(sidebarWidthSetting, 260).toInt();
    m_panelVisible = settings.value(panelVisibleSetting, true).toBool();
    m_autoCreateLinks = settings.value(QStringLiteral("notes/autoCreateLinks"), true).toBool();

    const QString saved = settings.value(folderSetting).toString();
    if (!saved.isEmpty() && QFileInfo(saved).isDir())
        openFolder(saved);
}

QString Vault::rootName() const {
    return m_rootPath.isEmpty() ? QString() : QFileInfo(m_rootPath).fileName();
}

QAbstractItemModel *Vault::model() const {
    return m_model;
}

// The tree's top level is the notes folder's contents.
QModelIndex Vault::rootIndex() const {
    return QModelIndex();
}

void Vault::setSidebarVisible(bool visible) {
    if (m_sidebarVisible == visible)
        return;
    m_sidebarVisible = visible;
    QSettings().setValue(sidebarVisibleSetting, visible);
    emit sidebarVisibleChanged();
}

void Vault::setPanelVisible(bool visible) {
    if (m_panelVisible == visible)
        return;
    m_panelVisible = visible;
    QSettings().setValue(panelVisibleSetting, visible);
    emit panelVisibleChanged();
}

QString Vault::fileNameSeparator() {
    const QString value = QSettings().value(QStringLiteral("notes/fileNameSeparator"), QStringLiteral("-")).toString();
    return value == QLatin1String("_") || value == QLatin1String(" ") ? value : QStringLiteral("-");
}

void Vault::setFileNameSeparator(const QString &separator) {
    const QString value = separator == QLatin1String("_") || separator == QLatin1String(" ") ? separator : QStringLiteral("-");
    if (value == fileNameSeparator())
        return;
    QSettings().setValue(QStringLiteral("notes/fileNameSeparator"), value);
    emit fileNameSeparatorChanged();
}

QString Vault::folderLayout() const {
    return Layout::choice();
}

void Vault::setFolderLayout(const QString &layout) {
    if (layout == folderLayout())
        return;
    Layout::setChoice(layout);
    emit folderLayoutChanged();
}

bool Vault::isNewFolder() const {
    return isOpen() && Layout::isEmpty(m_rootPath)
           && !QFileInfo::exists(QDir(m_rootPath).filePath(QStringLiteral(".omazettel/note-templates")));
}

void Vault::setAutoCreateLinks(bool enabled) {
    if (m_autoCreateLinks == enabled)
        return;
    m_autoCreateLinks = enabled;
    QSettings().setValue(QStringLiteral("notes/autoCreateLinks"), enabled);
    emit autoCreateLinksChanged();
}

void Vault::setSidebarWidth(int width) {
    width = qBound(180, width, 600);
    if (m_sidebarWidth == width)
        return;
    m_sidebarWidth = width;
    QSettings().setValue(sidebarWidthSetting, width);
    emit sidebarWidthChanged();
}

bool Vault::openFolder(const QString &path) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isDir()) {
        setError(QStringLiteral("%1 is not a folder.").arg(path));
        return false;
    }

    const QString root = info.canonicalFilePath();
    if (root != m_rootPath) {
        m_rootPath = root;
        m_model->setRoot(root);
        emit rootPathChanged();
    }
    QSettings().setValue(folderSetting, root);
    addRecent(root);
    setError(QString());
    return true;
}

bool Vault::openFolderUrl(const QUrl &url) {
    return openFolder(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

void Vault::closeFolder() {
    if (m_rootPath.isEmpty())
        return;
    m_rootPath.clear();
    m_model->setRoot(QString());
    QSettings settings;
    settings.remove(folderSetting);
    settings.remove(lastNoteSetting);
    emit rootPathChanged();
}

void Vault::forgetRecent(const QString &path) {
    if (m_recentFolders.removeAll(path) == 0)
        return;
    QSettings().setValue(recentSetting, m_recentFolders);
    emit recentFoldersChanged();
}

void Vault::addRecent(const QString &path) {
    QStringList recent = m_recentFolders;
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > maxRecentFolders)
        recent.removeLast();
    if (recent == m_recentFolders)
        return;
    m_recentFolders = recent;
    QSettings().setValue(recentSetting, m_recentFolders);
    emit recentFoldersChanged();
}

QString Vault::filePath(const QModelIndex &index) const {
    return m_model->filePath(index);
}

bool Vault::isFolder(const QModelIndex &index) const {
    return m_model->isFolder(index);
}

bool Vault::isFolderPath(const QString &path) const {
    return !path.isEmpty() && QFileInfo(path).isDir();
}

QString Vault::displayName(const QModelIndex &index) const {
    if (!index.isValid())
        return QString();
    const QFileInfo info(m_model->filePath(index));
    if (info.isDir())
        return info.fileName();
    return noteSuffixes.contains(info.suffix().toLower()) ? info.completeBaseName()
                                                           : info.fileName();
}

QModelIndex Vault::indexForPath(const QString &path) const {
    if (!contains(path))
        return QModelIndex();
    QModelIndex index = m_model->indexForPath(path);
    if (!index.isValid() && QFileInfo::exists(path)) {
        // Made a moment ago, before the folder watcher noticed.
        m_model->refresh();
        index = m_model->indexForPath(path);
    }
    return index;
}

bool Vault::contains(const QString &path) const {
    return !insideOrEmpty(path).isEmpty();
}

QUrl Vault::urlForPath(const QString &path) const {
    return path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path);
}

QString Vault::pathForUrl(const QUrl &url) const {
    return url.isLocalFile() ? QFileInfo(url.toLocalFile()).absoluteFilePath() : QString();
}

QString Vault::prettyPath(const QString &path) const {
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

QString Vault::folderFor(const QString &path) const {
    const QString inside = insideOrEmpty(path);
    if (inside.isEmpty())
        return m_rootPath;
    const QFileInfo info(inside);
    return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

// A path inside the vault, made absolute, or empty for anything outside it
// (including the vault root's own parent reached through "..").
QString Vault::insideOrEmpty(const QString &path) const {
    if (m_rootPath.isEmpty() || path.isEmpty())
        return QString();
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (absolute == m_rootPath || absolute.startsWith(m_rootPath + QLatin1Char('/')))
        return absolute;
    return QString();
}

QString Vault::cleanName(const QString &name) {
    QString cleaned = name;
    // Path separators and control characters cannot be part of one file name.
    for (QChar &c : cleaned) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c.category() == QChar::Other_Control)
            c = QLatin1Char('-');
    }
    cleaned = cleaned.simplified();
    // A leading dot would hide the note from the sidebar.
    while (cleaned.startsWith(QLatin1Char('.')))
        cleaned.remove(0, 1);
    return cleaned.trimmed();
}

QString Vault::noteFileName(const QString &name) {
    QString cleaned = cleanName(name);
    const QString separator = fileNameSeparator();
    if (separator != QLatin1String(" ")) {
        // "Plan - draft" becomes "Plan-draft", not "Plan---draft".
        static const QRegularExpression spaced(QStringLiteral("\\s*([-_])\\s*"));
        cleaned.replace(spaced, QStringLiteral("\\1"));
        cleaned.replace(QLatin1Char(' '), separator);
    }
    return cleaned;
}

QString Vault::matchKey(const QString &name) {
    QString key = name.toLower();
    key.replace(QLatin1Char('-'), QLatin1Char(' ')).replace(QLatin1Char('_'), QLatin1Char(' '));
    return key.simplified();
}

bool Vault::isNoteFile(const QString &path) {
    return noteSuffixes.contains(QFileInfo(path).suffix().toLower());
}

QString Vault::uniquePath(const QString &folder, const QString &baseName,
                          const QString &extension) {
    const QDir dir(folder);
    const QString suffix = extension.isEmpty() ? QString() : QLatin1Char('.') + extension;
    QString candidate = dir.filePath(baseName + suffix);
    for (int n = 2; QFileInfo::exists(candidate); ++n)
        candidate = dir.filePath(QStringLiteral("%1 %2%3").arg(baseName).arg(n).arg(suffix));
    return candidate;
}

QString Vault::createNote(const QString &folder, const QString &title) {
    const QString target = folderFor(folder);
    if (target.isEmpty()) {
        setError(QStringLiteral("Open a notes folder first."));
        return QString();
    }

    QString name = cleanName(title);
    QString extension = QStringLiteral("md");
    if (isNoteFile(name)) {
        extension = QFileInfo(name).suffix();
        name = QFileInfo(name).completeBaseName();
    }
    if (name.isEmpty())
        name = QStringLiteral("Untitled");

    const QString path = uniquePath(target, noteFileName(name), extension);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setError(QStringLiteral("Could not create %1.").arg(QFileInfo(path).fileName()));
        return QString();
    }
    file.write(QStringLiteral("# %1\n\n").arg(name).toUtf8());
    if (!file.commit()) {
        setError(QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName()));
        return QString();
    }
    setError(QString());
    m_model->refresh();
    emit contentsChanged();
    return path;
}

QString Vault::createFolder(const QString &parentFolder, const QString &name) {
    const QString target = folderFor(parentFolder);
    if (target.isEmpty()) {
        setError(QStringLiteral("Open a notes folder first."));
        return QString();
    }
    QString cleaned = cleanName(name);
    if (cleaned.isEmpty())
        cleaned = QStringLiteral("New folder");

    const QString path = uniquePath(target, cleaned, QString());
    if (!QDir().mkpath(path)) {
        setError(QStringLiteral("Could not create the folder %1.").arg(cleaned));
        return QString();
    }
    setError(QString());
    m_model->refresh();
    return path;
}

// Where path would go when renamed to newName, keeping a note's .md or
// .markdown extension when the new name has none.
QString Vault::renameTarget(const QString &path, const QString &newName) {
    const QString inside = insideOrEmpty(path);
    if (inside.isEmpty() || inside == m_rootPath) {
        setError(QStringLiteral("Only notes and folders inside the notes folder can be renamed."));
        return QString();
    }
    const QFileInfo info(inside);
    QString name = info.isFile() ? noteFileName(newName) : cleanName(newName);
    if (name.isEmpty()) {
        setError(QStringLiteral("A name is needed."));
        return QString();
    }
    if (info.isFile() && isNoteFile(inside) && !isNoteFile(name))
        name += QLatin1Char('.') + info.suffix();

    const QString target = QDir(info.absolutePath()).filePath(name);
    if (target != inside && QFileInfo::exists(target)) {
        setError(QStringLiteral("%1 already exists.").arg(name));
        return QString();
    }
    setError(QString());
    return target;
}

QString Vault::renamePath(const QString &path, const QString &newName) {
    const QString target = renameTarget(path, newName);
    if (target.isEmpty())
        return QString();
    const QString from = insideOrEmpty(path);
    if (target == from)
        return target;
    if (!QFile::rename(from, target)) {
        setError(QStringLiteral("Could not rename %1.").arg(QFileInfo(from).fileName()));
        return QString();
    }
    m_model->refresh();
    emit contentsChanged();
    return target;
}

// Where path would land if moved into folder, or empty (with lastError set)
// when the move is not allowed.
QString Vault::moveTarget(const QString &path, const QString &folder) {
    const QString from = insideOrEmpty(path);
    const QString into = insideOrEmpty(folder);
    if (from.isEmpty() || into.isEmpty() || from == m_rootPath || !QFileInfo(into).isDir()) {
        setError(QStringLiteral("Only notes and folders inside the notes folder can be moved."));
        return QString();
    }
    if (into == from || into.startsWith(from + QLatin1Char('/'))) {
        setError(QStringLiteral("A folder cannot be moved into itself."));
        return QString();
    }
    const QString target = QDir(into).filePath(QFileInfo(from).fileName());
    if (target == from) {
        setError(QString());
        return target;
    }
    if (QFileInfo::exists(target)) {
        setError(QStringLiteral("%1 already has something named %2.")
                     .arg(QFileInfo(into).fileName(), QFileInfo(from).fileName()));
        return QString();
    }
    setError(QString());
    return target;
}

QString Vault::movePath(const QString &path, const QString &folder) {
    const QString target = moveTarget(path, folder);
    if (target.isEmpty())
        return QString();
    const QString from = insideOrEmpty(path);
    if (target == from)
        return target;
    if (!QFile::rename(from, target) && !QDir().rename(from, target)) {
        setError(QStringLiteral("Could not move %1.").arg(QFileInfo(from).fileName()));
        return QString();
    }
    m_model->refresh();
    emit contentsChanged();
    return target;
}

bool Vault::trashPath(const QString &path) {
    const QString inside = insideOrEmpty(path);
    if (inside.isEmpty() || inside == m_rootPath) {
        setError(QStringLiteral("Only notes and folders inside the notes folder can be moved to the trash."));
        return false;
    }
    if (!QFile::moveToTrash(inside)) {
        setError(QStringLiteral("Could not move %1 to the trash.").arg(QFileInfo(inside).fileName()));
        return false;
    }
    setError(QString());
    m_model->refresh();
    emit contentsChanged();
    return true;
}

void Vault::rememberLastNote(const QUrl &url) {
    const QString path = url.isLocalFile() ? insideOrEmpty(url.toLocalFile()) : QString();
    if (!path.isEmpty())
        QSettings().setValue(lastNoteSetting, path);
}

QString Vault::lastNotePath() const {
    const QString path = insideOrEmpty(QSettings().value(lastNoteSetting).toString());
    return QFileInfo(path).isFile() ? path : QString();
}

void Vault::setError(const QString &error) {
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

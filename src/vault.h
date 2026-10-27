#pragma once

#include <QModelIndex>
#include <QObject>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QUrl>

class NotesTree;

// The notes folder ("vault"): which folder is open, the folders opened
// before, the Markdown file tree shown in the sidebar, and creating, renaming
// and trashing notes and folders inside it. Notes stay plain .md files, so
// anything else (VS Code, git, pandoc) can work on the same folder.
class Vault : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isOpen READ isOpen NOTIFY rootPathChanged)
    Q_PROPERTY(QString rootPath READ rootPath NOTIFY rootPathChanged)
    Q_PROPERTY(QString rootName READ rootName NOTIFY rootPathChanged)
    Q_PROPERTY(QModelIndex rootIndex READ rootIndex NOTIFY rootPathChanged)
    Q_PROPERTY(QAbstractItemModel *model READ model CONSTANT)
    Q_PROPERTY(QStringList recentFolders READ recentFolders NOTIFY recentFoldersChanged)
    Q_PROPERTY(bool sidebarVisible READ sidebarVisible WRITE setSidebarVisible NOTIFY sidebarVisibleChanged)
    Q_PROPERTY(int sidebarWidth READ sidebarWidth WRITE setSidebarWidth NOTIFY sidebarWidthChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool panelVisible READ panelVisible WRITE setPanelVisible NOTIFY panelVisibleChanged)
    // Whether saving a note makes the notes its [[links]] name.
    Q_PROPERTY(bool autoCreateLinks READ autoCreateLinks WRITE setAutoCreateLinks NOTIFY autoCreateLinksChanged)
    // What stands for a space in new note file names: "-", "_" or " ".
    Q_PROPERTY(QString fileNameSeparator READ fileNameSeparator WRITE setFileNameSeparator NOTIFY fileNameSeparatorChanged)
    // How new, empty notes folders start: "numbered" (AI-first),
    // "traditional", or "" before the first-run question is answered.
    Q_PROPERTY(QString folderLayout READ folderLayout WRITE setFolderLayout NOTIFY folderLayoutChanged)
    // Whether the maps of content in 03-projects are kept up to date.
    Q_PROPERTY(bool mapsOfContent READ mapsOfContent WRITE setMapsOfContent NOTIFY mapsOfContentChanged)

public:
    explicit Vault(QObject *parent = nullptr);

    bool isOpen() const { return !m_rootPath.isEmpty(); }
    QString rootPath() const { return m_rootPath; }
    QString rootName() const;
    QModelIndex rootIndex() const;
    QAbstractItemModel *model() const;
    QStringList recentFolders() const { return m_recentFolders; }
    bool sidebarVisible() const { return m_sidebarVisible; }
    void setSidebarVisible(bool visible);
    int sidebarWidth() const { return m_sidebarWidth; }
    void setSidebarWidth(int width);
    QString lastError() const { return m_lastError; }
    bool panelVisible() const { return m_panelVisible; }
    bool autoCreateLinks() const { return m_autoCreateLinks; }
    static QString fileNameSeparator();
    void setFileNameSeparator(const QString &separator);
    QString folderLayout() const;
    void setFolderLayout(const QString &layout);
    // The open folder is empty and OmaZettel has not set it up yet, so it
    // can still start either way.
    Q_INVOKABLE bool isNewFolder() const;
    bool mapsOfContent() const;
    void setMapsOfContent(bool on);
    // Brings the maps of content up to date now (normally done a moment
    // after notes are added, moved or removed). Returns the maps written.
    Q_INVOKABLE QStringList updateMaps();
    void setAutoCreateLinks(bool enabled);
    void setPanelVisible(bool visible);

    // Opening and closing the notes folder.
    Q_INVOKABLE bool openFolder(const QString &path);
    Q_INVOKABLE bool openFolderUrl(const QUrl &url);
    Q_INVOKABLE void closeFolder();
    Q_INVOKABLE void forgetRecent(const QString &path);
    Q_INVOKABLE void clearError() { setError(QString()); }
    Q_INVOKABLE void showError(const QString &error) { setError(error); }

    // Reading the tree.
    Q_INVOKABLE QString filePath(const QModelIndex &index) const;
    Q_INVOKABLE bool isFolder(const QModelIndex &index) const;
    Q_INVOKABLE bool isFolderPath(const QString &path) const;
    Q_INVOKABLE QString displayName(const QModelIndex &index) const;
    Q_INVOKABLE QModelIndex indexForPath(const QString &path) const;
    Q_INVOKABLE bool contains(const QString &path) const;
    Q_INVOKABLE QUrl urlForPath(const QString &path) const;
    Q_INVOKABLE QString pathForUrl(const QUrl &url) const;
    // A path for display, with the home folder shown as ~.
    Q_INVOKABLE QString prettyPath(const QString &path) const;
    // The folder a new note goes in: the given path if it is a folder, the
    // folder holding it if it is a file, or the vault root.
    Q_INVOKABLE QString folderFor(const QString &path) const;

    // Changing the tree. Each returns the new path, or an empty string and
    // sets lastError.
    Q_INVOKABLE QString createNote(const QString &folder, const QString &title);
    Q_INVOKABLE QString createFolder(const QString &parentFolder, const QString &name);
    Q_INVOKABLE QString renameTarget(const QString &path, const QString &newName);
    Q_INVOKABLE QString renamePath(const QString &path, const QString &newName);
    Q_INVOKABLE bool trashPath(const QString &path);
    Q_INVOKABLE QString moveTarget(const QString &path, const QString &folder);
    Q_INVOKABLE QString movePath(const QString &path, const QString &folder);

    // Remembering which note was open, to reopen it next time.
    Q_INVOKABLE void rememberLastNote(const QUrl &url);
    QString lastNotePath() const;

    // Helpers, public for the tests.
    static QString cleanName(const QString &name);
    // A note's file name: cleanName with spaces swapped for the separator.
    static QString noteFileName(const QString &name);
    // A note name for matching links: lower case, with "-", "_" and spaces alike.
    static QString matchKey(const QString &name);
    static bool isNoteFile(const QString &path);
    static QString uniquePath(const QString &folder, const QString &baseName,
                              const QString &extension);

signals:
    void rootPathChanged();
    void recentFoldersChanged();
    void sidebarVisibleChanged();
    void sidebarWidthChanged();
    void lastErrorChanged();
    void panelVisibleChanged();
    void autoCreateLinksChanged();
    void fileNameSeparatorChanged();
    void folderLayoutChanged();
    void mapsOfContentChanged();
    // Notes or folders were created, renamed, moved or trashed.
    void contentsChanged();

private:
    void setError(const QString &error);
    void addRecent(const QString &path);
    QString insideOrEmpty(const QString &path) const;

    NotesTree *m_model = nullptr;
    QTimer m_mapsTimer;
    QString m_rootPath;
    QStringList m_recentFolders;
    bool m_sidebarVisible = true;
    int m_sidebarWidth = 260;
    QString m_lastError;
    bool m_panelVisible = true;
    bool m_autoCreateLinks = true;
};

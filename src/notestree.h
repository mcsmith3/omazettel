#pragma once

#include <QFileSystemWatcher>
#include <QModelIndex>
#include <QSet>
#include <QStandardItemModel>
#include <QString>
#include <QTimer>

// The sidebar's tree of the notes folder: its folders and Markdown notes,
// folders first, each level in natural order ("2" before "10"); hidden
// entries such as .git are left out.
//
// The whole folder is read at once when it opens, so the tree is complete and
// the same every time (Qt's QFileSystemModel fills in folders in the
// background, so the tree it shows depends on timing). Later changes, from
// the app or from anything else, are found by watching the folders; only the
// rows that changed are added or removed, so open folders stay open.
class NotesTree : public QStandardItemModel {
    Q_OBJECT

public:
    enum Role {
        FilePathRole = Qt::UserRole + 1,
        FileNameRole,
        IsFolderRole,
    };

    explicit NotesTree(QObject *parent = nullptr);

    // An empty path empties the tree.
    void setRoot(const QString &path);
    QString root() const { return m_root; }

    // Reads the folder again now, rather than when the watcher next fires.
    void refresh();

    QModelIndex indexForPath(const QString &path) const;
    QString filePath(const QModelIndex &index) const;
    bool isFolder(const QModelIndex &index) const;

    QHash<int, QByteArray> roleNames() const override;

    // Natural order: runs of digits compare as numbers, the rest without
    // regard to case. Works in any locale, including C.
    static int naturalCompare(const QString &a, const QString &b);

private:
    void syncFolder(QStandardItem *parent, const QString &folder, QSet<QString> &seen);

    QString m_root;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
};

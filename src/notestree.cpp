#include "notestree.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace {
const QStringList noteSuffixes{QStringLiteral("md"), QStringLiteral("markdown")};

struct Entry {
    QString name;
    bool folder = false;
};

bool before(const Entry &a, const Entry &b) {
    if (a.folder != b.folder)
        return a.folder;
    const int order = NotesTree::naturalCompare(a.name, b.name);
    return order != 0 ? order < 0 : a.name < b.name;
}
}

NotesTree::NotesTree(QObject *parent) : QStandardItemModel(parent) {
    // Saving one note can touch a folder several times; read it once after.
    m_settle.setSingleShot(true);
    m_settle.setInterval(150);
    connect(&m_settle, &QTimer::timeout, this, &NotesTree::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this]() { m_settle.start(); });
}

void NotesTree::setRoot(const QString &path) {
    m_settle.stop();
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    clear();
    m_root = path;
    refresh();
}

void NotesTree::refresh() {
    if (m_root.isEmpty())
        return;
    QSet<QString> seen;
    syncFolder(invisibleRootItem(), m_root, seen);
    // Stop watching folders that are gone, and watch new ones.
    QStringList stale;
    for (const QString &watched : m_watcher.directories()) {
        if (!seen.contains(watched))
            stale.append(watched);
    }
    if (!stale.isEmpty())
        m_watcher.removePaths(stale);
    QStringList fresh;
    for (const QString &folder : std::as_const(seen)) {
        if (!m_watcher.directories().contains(folder))
            fresh.append(folder);
    }
    if (!fresh.isEmpty())
        m_watcher.addPaths(fresh);
}

void NotesTree::syncFolder(QStandardItem *parent, const QString &folder, QSet<QString> &seen) {
    // A folder reached twice (through a symbolic link) is shown once.
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    if (canonical.isEmpty() || seen.contains(canonical))
        return;
    seen.insert(canonical);

    QList<Entry> wanted;
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : entries) {
        if (info.fileName().startsWith(QLatin1Char('.')))
            continue;
        if (info.isDir())
            wanted.append({info.fileName(), true});
        else if (noteSuffixes.contains(info.suffix().toLower()))
            wanted.append({info.fileName(), false});
    }
    std::sort(wanted.begin(), wanted.end(), before);

    // Remove the rows that are gone, then insert the new ones in place, so a
    // row that stays keeps its children (and the tree its open folders).
    QSet<QString> wantedKeys;
    for (const Entry &entry : std::as_const(wanted))
        wantedKeys.insert(entry.name + (entry.folder ? QStringLiteral("/") : QString()));
    for (int row = parent->rowCount() - 1; row >= 0; --row) {
        const QStandardItem *child = parent->child(row);
        const QString key = child->data(FileNameRole).toString()
                            + (child->data(IsFolderRole).toBool() ? QStringLiteral("/") : QString());
        if (!wantedKeys.contains(key))
            parent->removeRow(row);
    }
    for (int row = 0; row < wanted.size(); ++row) {
        const Entry &entry = wanted.at(row);
        QStandardItem *child = row < parent->rowCount() ? parent->child(row) : nullptr;
        const bool same = child && child->data(FileNameRole).toString() == entry.name
                          && child->data(IsFolderRole).toBool() == entry.folder;
        if (!same) {
            const QString path = QDir(folder).filePath(entry.name);
            child = new QStandardItem(entry.name);
            child->setEditable(false);
            child->setData(path, FilePathRole);
            child->setData(entry.name, FileNameRole);
            child->setData(entry.folder, IsFolderRole);
            parent->insertRow(row, child);
        }
        if (entry.folder)
            syncFolder(child, child->data(FilePathRole).toString(), seen);
    }
}

QModelIndex NotesTree::indexForPath(const QString &path) const {
    if (m_root.isEmpty())
        return QModelIndex();
    const QString relative = QDir(m_root).relativeFilePath(QDir::cleanPath(path));
    if (relative.isEmpty() || relative == QLatin1String(".") || relative.startsWith(QLatin1String("..")))
        return QModelIndex();
    const QStandardItem *item = invisibleRootItem();
    for (const QString &part : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const QStandardItem *next = nullptr;
        for (int row = 0; row < item->rowCount() && !next; ++row) {
            if (item->child(row)->data(FileNameRole).toString() == part)
                next = item->child(row);
        }
        if (!next)
            return QModelIndex();
        item = next;
    }
    return item->index();
}

QString NotesTree::filePath(const QModelIndex &index) const {
    return index.isValid() ? index.data(FilePathRole).toString() : QString();
}

bool NotesTree::isFolder(const QModelIndex &index) const {
    return index.isValid() && index.data(IsFolderRole).toBool();
}

QHash<int, QByteArray> NotesTree::roleNames() const {
    return {{Qt::DisplayRole, "display"},
            {FilePathRole, "filePath"},
            {FileNameRole, "fileName"},
            {IsFolderRole, "isFolder"}};
}

int NotesTree::naturalCompare(const QString &a, const QString &b) {
    static thread_local QCollator collator = [] {
        QCollator made;
        made.setCaseSensitivity(Qt::CaseInsensitive);
        return made;
    }();
    int i = 0;
    int j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digits = a.at(i).isDigit() && b.at(j).isDigit();
        int ie = i;
        int je = j;
        while (ie < a.size() && a.at(ie).isDigit() == digits && (digits || !a.at(ie).isDigit()))
            ++ie;
        while (je < b.size() && b.at(je).isDigit() == digits && (digits || !b.at(je).isDigit()))
            ++je;
        if (ie == i || je == j) {
            // One side has digits here and the other does not.
            const int order = collator.compare(a.mid(i, 1), b.mid(j, 1));
            if (order != 0)
                return order;
            ++i;
            ++j;
            continue;
        }
        if (digits) {
            QString x = a.mid(i, ie - i);
            QString y = b.mid(j, je - j);
            while (x.size() > 1 && x.startsWith(QLatin1Char('0')))
                x.remove(0, 1);
            while (y.size() > 1 && y.startsWith(QLatin1Char('0')))
                y.remove(0, 1);
            if (x.size() != y.size())
                return x.size() < y.size() ? -1 : 1;
            if (const int order = x.compare(y))
                return order < 0 ? -1 : 1;
        } else if (const int order = collator.compare(a.mid(i, ie - i), b.mid(j, je - j))) {
            return order;
        }
        i = ie;
        j = je;
    }
    if (i < a.size() || j < b.size())
        return i < a.size() ? 1 : -1;
    return 0;
}

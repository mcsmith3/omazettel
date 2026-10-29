// OmaZettel additions to Backend. Kept apart from backend.cpp, which tracks
// upstream Omawrite, so merges stay simple.

#include "backend.h"
#include "apptheme.h"
#include "markdownhighlighter.h"
#include "taskdates.h"
#include "zettel.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QRegularExpression>

#include <QColor>
#include <QDir>
#include <QSettings>
#include <QTextCursor>
#include <QTimer>
#include <QFile>
#include <QFileInfo>

// Saves now what auto-save would save shortly, so switching to another note
// does not stop to ask about changes auto-save was about to keep anyway.
// Returns true when nothing is left unsaved. Untitled drafts, and files whose
// auto-save is off (outside changes, unusual encodings), still go through the
// unsaved-changes prompt.
bool Backend::flushAutoSave() {
    if (!m_modified)
        return true;
    if (!m_fileUrl.isLocalFile() || m_autoSaveSuspended || m_autoSaveHeld)
        return false;
    autoSave();
    return !m_modified;
}

// Renames or moves the open note's file and keeps editing it at its new path,
// without the file watcher reporting the old path as deleted.
bool Backend::moveCurrentFile(const QUrl &newUrl) {
    if (!m_fileUrl.isLocalFile() || !newUrl.isLocalFile())
        return false;

    const QString from = m_fileUrl.toLocalFile();
    const QString to = newUrl.toLocalFile();
    if (QFileInfo(from).absoluteFilePath() == QFileInfo(to).absoluteFilePath())
        return true;

    if (m_modified && !flushAutoSave()) {
        setStatus(QStringLiteral("Save %1 before renaming it.").arg(fileName()));
        return false;
    }
    if (QFileInfo::exists(to)) {
        setStatus(QStringLiteral("%1 already exists.").arg(QFileInfo(to).fileName()));
        return false;
    }

    QDir().mkpath(QFileInfo(to).absolutePath());
    unwatchFiles();
    if (!QFile::rename(from, to)) {
        watchCurrentFile();
        setStatus(QStringLiteral("Could not rename %1.").arg(fileName()));
        return false;
    }

    setFileUrl(newUrl);
    watchCurrentFile();
    setStatus(QStringLiteral("Renamed to %1").arg(fileName()));
    return true;
}

// Points the open note at the path it now has after something else moved it,
// such as renaming the folder that holds it.
void Backend::followMovedFile(const QUrl &newUrl) {
    if (!newUrl.isLocalFile())
        return;
    unwatchFiles();
    setFileUrl(newUrl);
    watchCurrentFile();
}

// Replaces the open note with an empty untitled document in this window, for
// when the note is moved to the trash. Callers deal with unsaved changes first.
void Backend::startNewDocument() {
    unwatchFiles();
    m_autoSaveTimer.stop();
    m_autoSaveSuspended = false;
    loadDocumentText(QString());
    clearRecovery();
    setKnownFileContents(QByteArray(), false);
    setFileUrl(QUrl());
    setModified(false);
    setStatus(QString());
}

QString Backend::wikiLinkAt(int position) const {
    if (!m_document)
        return QString();
    const QTextBlock block = m_document->findBlock(position);
    if (!block.isValid())
        return QString();
    const int column = position - block.position();
    for (const Zettel::WikiLink &link : Zettel::wikiLinks(block.text())) {
        if (column >= link.start && column <= link.start + link.length)
            return link.target;
    }
    return QString();
}

QVariantMap Backend::wikiQueryAt(int position) const {
    if (!m_document || insideFence(position))
        return {};
    const QTextBlock block = m_document->findBlock(position);
    if (!block.isValid())
        return {};
    const QString before = block.text().left(position - block.position());
    static const QRegularExpression openRe(QStringLiteral("\\[\\[([^\\[\\]|#\\n]*)$"));
    const QRegularExpressionMatch match = openRe.match(before);
    if (!match.hasMatch())
        return {};
    // Not inside inline code.
    for (const auto &range : Zettel::codeRanges(before + QStringLiteral("`"))) {
        if (match.capturedStart() >= range.first && match.capturedStart() < range.second)
            return {};
    }
    const QString after = block.text().mid(position - block.position());
    return {{QStringLiteral("query"), match.captured(1)},
            {QStringLiteral("start"), block.position() + int(match.capturedStart(1))},
            {QStringLiteral("closed"), after.startsWith(QStringLiteral("]]"))}};
}

QString Backend::wikiHeadingAt(int position) const {
    if (!m_document)
        return QString();
    const QTextBlock block = m_document->findBlock(position);
    if (!block.isValid())
        return QString();
    const int column = position - block.position();
    for (const Zettel::WikiLink &link : Zettel::wikiLinks(block.text())) {
        if (column >= link.start && column <= link.start + link.length)
            return link.heading;
    }
    return QString();
}

QVariantList Backend::outline() const {
    QVariantList result;
    if (!m_document)
        return result;
    for (const Zettel::Heading &heading : Zettel::outline(m_document->toPlainText())) {
        result.append(QVariantMap{{QStringLiteral("level"), heading.level},
                                  {QStringLiteral("text"), heading.text},
                                  {QStringLiteral("position"), heading.offset}});
    }
    return result;
}

int Backend::headingPosition(const QString &text) const {
    if (!m_document || text.trimmed().isEmpty())
        return -1;
    const QString wanted = text.trimmed();
    for (const Zettel::Heading &heading : Zettel::outline(m_document->toPlainText())) {
        if (heading.text.compare(wanted, Qt::CaseInsensitive) == 0)
            return heading.offset;
    }
    return -1;
}

// OmaZettel's colours: the theme's omazettel.toml, the user's template or
// the built-in one (AppTheme::load), or the fixed Oma palette with the
// theme's accent. Runs at the end of loadOmarchyTheme().
void Backend::applyAppTheme() {
    AppTheme::Result theme = m_omaPalette ? AppTheme::omaPalette(m_themeAccent) : AppTheme::load();
    if (!m_omaPalette) {
        // What the theme file leaves out comes from what Omawrite already read.
        const auto seed = [&theme](const QString &name, const QString &value) {
            if (!theme.colors.contains(name) && QColor(value).isValid())
                theme.colors.insert(name, QColor(value).name());
        };
        seed(QStringLiteral("editor.background"), m_themeBackground);
        seed(QStringLiteral("editor.foreground"), m_themeForeground);
        seed(QStringLiteral("ui.accent"), m_themeAccent);
        seed(QStringLiteral("editor.selection"), m_themeSelection);
        seed(QStringLiteral("editor.selection_foreground"), m_themeSelectionForeground);
        seed(QStringLiteral("editor.code_background"), m_themeCodeBackground);
        const QColor page(theme.colors.value(QStringLiteral("editor.background")));
        m_darkMode = !page.isValid() || (0.299 * page.redF() + 0.587 * page.greenF() + 0.114 * page.blueF()) < 0.5;
        AppTheme::complete(theme.colors, m_darkMode);
    } else {
        m_darkMode = true;
    }
    m_themeTokens = theme.colors;
    m_themeSource = theme.source;
    m_themeBackground = theme.colors.value(QStringLiteral("editor.background"));
    m_themeForeground = theme.colors.value(QStringLiteral("editor.foreground"));
    m_themeAccent = theme.colors.value(QStringLiteral("ui.accent"));
    m_themeSelection = theme.colors.value(QStringLiteral("editor.selection"));
    m_themeSelectionForeground = theme.colors.value(QStringLiteral("editor.selection_foreground"));
    m_themeCodeBackground = theme.colors.value(QStringLiteral("editor.code_background"));
    applyHighlighterColors();
}

void Backend::applyHighlighterColors() {
    if (!m_highlighter)
        return;
    QHash<QString, QColor> colors;
    for (auto it = m_themeTokens.cbegin(); it != m_themeTokens.cend(); ++it) {
        if (it.key().startsWith(QLatin1String("editor.")))
            colors.insert(it.key().mid(7), QColor(it.value()));
    }
    m_highlighter->setZettelColors(colors);
}

QVariantMap Backend::themeTokens() const {
    QVariantMap map;
    for (auto it = m_themeTokens.cbegin(); it != m_themeTokens.cend(); ++it)
        map.insert(it.key(), it.value());
    return map;
}

// Also watch the user's template, so editing it recolours the app at once.
void Backend::watchAppTheme() {
    const QString user = AppTheme::userTemplatePath();
    const QString folder = QFileInfo(user).absolutePath();
    if (QFileInfo(folder).isDir() && !m_themeWatcher.directories().contains(folder))
        m_themeWatcher.addPath(folder);
    if (QFileInfo(user).isFile() && !m_themeWatcher.files().contains(user))
        m_themeWatcher.addPath(user);
    const QString themed = AppTheme::themeDir() + QStringLiteral("/omazettel.toml");
    if (QFileInfo(themed).isFile() && !m_themeWatcher.files().contains(themed))
        m_themeWatcher.addPath(themed);
}

QString Backend::customizeThemeTemplate() {
    const QString user = AppTheme::userTemplatePath();
    if (!QFileInfo(user).isFile()) {
        QDir().mkpath(QFileInfo(user).absolutePath());
        QFile file(user);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            file.write(AppTheme::builtinTemplate().toUtf8());
    }
    watchAppTheme();
    return user;
}

void Backend::setOmaPalette(bool enabled) {
    if (m_omaPalette == enabled)
        return;
    m_omaPalette = enabled;
    QSettings().setValue(QStringLiteral("appearance/omaPalette"), enabled);
    loadOmarchyTheme();
    emit themeColorsChanged();
}

QVariantMap Backend::citationQueryAt(int position) const {
    if (!m_document || insideFence(position))
        return {};
    const QTextBlock block = m_document->findBlock(position);
    if (!block.isValid())
        return {};
    const QString before = block.text().left(position - block.position());
    // Inside an open [ ... with @ starting the word at the caret.
    static const QRegularExpression citeRe(
        QStringLiteral("\\[(?:[^\\[\\]]*[;\\s-])?@([\\w:.#$%&+?<>~/-]*)$"));
    const QRegularExpressionMatch match = citeRe.match(before);
    if (!match.hasMatch())
        return {};
    return {{QStringLiteral("query"), match.captured(1)},
            {QStringLiteral("start"), block.position() + int(match.capturedStart(1))}};
}

void Backend::pauseFileWatching() {
    unwatchFiles();
    holdAutoSave(true);
}

bool Backend::resumeFileWatching() {
    holdAutoSave(false);
    if (!m_fileUrl.isLocalFile())
        return false;
    if (diskMatchesKnownContents()) {
        watchCurrentFile();
        return false;
    }
    const bool exists = QFileInfo::exists(m_fileUrl.toLocalFile());
    if (exists && !m_modified) {
        const QString status = m_status;
        open(m_fileUrl);
        setStatus(QStringLiteral("Updated %1 from sync").arg(fileName()));
        Q_UNUSED(status);
        return true;
    }
    watchCurrentFile();
    reportExternalChange(!exists);
    return false;
}

// Ticking a task in the editor dates it, "- [x] Send it (10/29/2026)", and
// unticking it takes the date off. Only an edit inside a task's box counts, so
// pasting or typing a done task keeps the text as written. The change goes in
// once the editor has finished with this one, as part of the same undo step.
void Backend::stampTaskDate(const QString &before, const QString &after) {
    if (!m_document)
        return;
    const int shorter = int(qMin(before.size(), after.size()));
    int start = 0;
    while (start < shorter && before.at(start) == after.at(start))
        ++start;
    int endBefore = int(before.size());
    int endAfter = int(after.size());
    while (endBefore > start && endAfter > start && before.at(endBefore - 1) == after.at(endAfter - 1)) {
        --endBefore;
        --endAfter;
    }
    if (before.mid(start, endBefore - start).contains(QLatin1Char('\n'))
        || after.mid(start, endAfter - start).contains(QLatin1Char('\n')))
        return;
    const int lineStart = start == 0 ? 0 : int(after.lastIndexOf(QLatin1Char('\n'), start - 1)) + 1;
    int lineEnd = int(after.indexOf(QLatin1Char('\n'), endAfter));
    if (lineEnd < 0)
        lineEnd = int(after.size());
    const QString line = after.mid(lineStart, lineEnd - lineStart);
    const int mark = TaskDates::markPosition(line);
    // The edit touched only the box's mark: " " to "x", or the mark typed or
    // deleted on its own.
    if (mark < 0 || start - lineStart < mark || endAfter - lineStart > mark + 1 || endBefore - lineStart > mark + 1)
        return;
    const TaskDates::Edit edit = TaskDates::forBox(line, QDate::currentDate());
    if (edit.at < 0)
        return;
    QPointer<QTextDocument> document = m_document;
    QTimer::singleShot(0, this, [document, lineStart, line, edit]() {
        if (!document)
            return;
        const QString now = document->toPlainText();
        if (now.mid(lineStart, line.size()) != line
            || (lineStart + line.size() < now.size() && now.at(lineStart + line.size()) != QLatin1Char('\n')))
            return;
        QTextCursor cursor(document);
        cursor.setPosition(lineStart + edit.at);
        cursor.setPosition(lineStart + edit.at + edit.remove, QTextCursor::KeepAnchor);
        cursor.joinPreviousEditBlock();
        cursor.insertText(edit.insert);
        cursor.endEditBlock();
    });
}

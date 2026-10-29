#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

class MarkdownHighlighter;
class QFont;
class QTextDocument;
class QWindow;
class QLockFile;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale NOTIFY textScaleChanged)
    Q_PROPERTY(QString themeBackground READ themeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeForeground READ themeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelection READ themeSelection NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelectionForeground READ themeSelectionForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString editorFont READ editorFont WRITE setEditorFont NOTIFY editorFontChanged)
    Q_PROPERTY(QStringList availableFonts READ availableFonts CONSTANT)
    // OmaZettel: the shared Oma palette for the page and text (the accent
    // still follows the Omarchy theme).
    Q_PROPERTY(bool omaPalette READ omaPalette WRITE setOmaPalette NOTIFY themeColorsChanged)
    // Every named colour (see themed/omazettel.toml.tpl): "editor.link",
    // "ui.panel", ... and where they came from.
    Q_PROPERTY(QVariantMap themeTokens READ themeTokens NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSource READ themeSource NOTIFY themeColorsChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    void setParentWindow(QWindow *window);

    QUrl fileUrl() const { return m_fileUrl; }
    QString fileName() const;

    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    // What the desktop asks for, times what the user asked for on top of it.
    qreal textScale() const { return m_desktopTextScale * m_textZoom; }
    void setDesktopTextScale(qreal textScale);
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeSelection() const { return m_themeSelection; }
    QString themeCodeBackground() const { return m_themeCodeBackground; }
    QString themeSelectionForeground() const { return m_themeSelectionForeground; }
    QString editorFont() const { return m_editorFont; }
    void setEditorFont(const QString &family);
    QStringList availableFonts() const;
    static QString defaultEditorFont();
    // Shortcut text as written in the code ("Ctrl+Shift+H", "Ctrl+Click /
    // Ctrl+Enter") for the platform: on macOS Ctrl is Command, so it reads
    // "⇧⌘H". keys() does this on macOS only; macKeys() always, for tests.
    Q_INVOKABLE QString keys(const QString &text) const;
    static QString macKeys(const QString &text);
    static QStringList selectableFontFamilies(const QStringList &families);
    static int countWords(const QString &text);
    static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);
    static QString tomlValue(const QString &text);
    static QFont printFont(const QFont &editorFont, qreal screenDpi);

    Q_INVOKABLE void attachDocument(QObject *textDocument);
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void open(const QUrl &url);
    Q_INVOKABLE void openOrStartFile(const QUrl &url);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAsDialog();
    Q_INVOKABLE void saveAs(const QUrl &url);
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void reloadFromDisk();
    Q_INVOKABLE void keepExternalVersion();
    Q_INVOKABLE void increaseTextSize();
    Q_INVOKABLE void decreaseTextSize();
    Q_INVOKABLE void resetTextSize();
    Q_INVOKABLE void holdAutoSave(bool held);
    Q_INVOKABLE void printDocument();
    Q_INVOKABLE void copyAsHtml();
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE QString clipboardUrl() const;
    Q_INVOKABLE QString clipboardText() const;
    Q_INVOKABLE bool editorTextChanged();
    Q_INVOKABLE void setCaretPosition(int position);
    Q_INVOKABLE QString linkUrlAt(int position) const;
    Q_INVOKABLE QVariantMap emphasisSpanAt(int position, const QString &marker) const;
    Q_INVOKABLE int cursorPositionStep(int position, int direction) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE QVariantList searchMatches(const QString &query) const;
    Q_INVOKABLE void replaceRange(int start, int end, const QString &text);
    Q_INVOKABLE bool insideFence(int position) const;
    Q_INVOKABLE QVariantMap windowGeometry() const;
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height, bool maximized);
    Q_INVOKABLE void documentFontChanged();

    // OmaZettel additions, implemented in backend_notes.cpp so upstream
    // Omawrite changes to backend.cpp merge cleanly.
    Q_INVOKABLE bool flushAutoSave();
    Q_INVOKABLE bool moveCurrentFile(const QUrl &newUrl);
    Q_INVOKABLE void startNewDocument();
    Q_INVOKABLE void followMovedFile(const QUrl &newUrl);
    bool omaPalette() const { return m_omaPalette; }
    void setOmaPalette(bool enabled);
    QVariantMap themeTokens() const;
    QString themeSource() const { return m_themeSource; }
    // Copies the built-in template to ~/.config/omarchy/themed/ (if it is not
    // there yet) to customise, and returns its path.
    Q_INVOKABLE QString customizeThemeTemplate();
    // The [[wiki link]] target at a document position, or "".
    Q_INVOKABLE QString wikiLinkAt(int position) const;
    // The [[ query being typed before position on its line, or null when
    // the caret is not inside an unfinished [[.
    Q_INVOKABLE QVariantMap wikiQueryAt(int position) const;
    // The citation key being typed before position inside [...@, or {}.
    Q_INVOKABLE QVariantMap citationQueryAt(int position) const;
    Q_INVOKABLE void showStatus(const QString &status) { setStatus(status); }
    // Around a sync, which may rewrite the open note: stop watching and
    // saving it, then pick up what is on disk. Resuming reloads quietly when
    // the note had no unsaved edits (returns true), and asks as usual when it
    // had. Callers restore the caret after a reload.
    Q_INVOKABLE void pauseFileWatching();
    Q_INVOKABLE bool resumeFileWatching();
    // When the open note changes on disk and the editor has no unsaved edits,
    // load the new version quietly (reloadedQuietly) instead of asking. Notes
    // change outside the app often: agents, the dashboard, sync, scripts.
    Q_INVOKABLE void setReloadsCleanFiles(bool on) { m_reloadsCleanFiles = on; }
    // The #heading of the [[link]] at a position, or "".
    Q_INVOKABLE QString wikiHeadingAt(int position) const;
    // The open note's headings: [{level, text, position}].
    Q_INVOKABLE QVariantList outline() const;
    // Where the first heading reading text starts (case does not matter), or -1.
    Q_INVOKABLE int headingPosition(const QString &text) const;

signals:
    void fileUrlChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void darkModeChanged();
    void textScaleChanged();
    void themeColorsChanged();
    void editorFontChanged();
    void saveFailed();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void externalChangeDetected(bool deleted, bool locallyModified);
    // The open note was reloaded from disk; caret is where it was before.
    void reloadedQuietly(int caret);

private:
    friend class OmaZettelTest;

    void loadDocumentText(const QString &text);
    void stampTaskDate(const QString &before, const QString &after);
    qreal steppedTextZoom(int direction) const;
    void setTextZoom(qreal zoom);
    void setFileUrl(const QUrl &url);
    void setModified(bool modified);
    void setStatus(const QString &status);
    void saveTo(const QUrl &url);
    QUrl suggestedSaveUrl() const;
    QString currentDocumentText() const;
    void setWordCount(int words);
    void refreshWordCount();
    void scheduleWordCount();
    void applyDocumentTypography();
    void reapplyTypographyToChange();
    void scheduleRecovery();
    void writeRecovery();
    void scheduleAutoSave();
    void autoSave();
    void suspendAutoSave();
    void restoreRecovery();
    void clearRecovery();
    bool diskMatchesKnownContents() const;
    void reportExternalChange(bool deleted);
    static bool savesUnchanged(const QByteArray &raw);
    void setKnownFileContents(const QByteArray &contents, bool known);
    void unwatchFiles();
    void watchCurrentFile();
    void checkWatchedFile();
    void loadOmarchyTheme();
    void applyAppTheme();
    void applyHighlighterColors();
    void watchAppTheme();
    void watchOmarchyTheme();

    QUrl m_fileUrl;
    bool m_modified = false;
    QString m_status;
    int m_wordCount = 0;
    bool m_darkMode = true;
    // The desktop's light/dark setting, used when the Omarchy theme names none.
    bool m_systemDarkMode = true;
    qreal m_desktopTextScale = 1.0;
    qreal m_textZoom = 1.0;
    bool m_loading = false;
    bool m_formattingTypography = false;
    int m_lastChangePos = 0;
    int m_lastChangeAdded = 0;
    QTimer m_wordCountTimer;
    // Counting a large document stalls typing, so counts run on a worker
    // thread. A load outdates the count in flight, and its result is dropped.
    QFutureWatcher<int> m_wordCountWatcher;
    bool m_wordCountStale = false;
    bool m_wordCountAgain = false;
    QTimer m_recoveryTimer;
    QTimer m_autoSaveTimer;
    QFileSystemWatcher m_fileWatcher;
    QTimer m_fileCheckTimer;
    bool m_fileMissing = false;
    QPointer<QTextDocument> m_document;
    QPointer<QWindow> m_parentWindow;
    QPointer<MarkdownHighlighter> m_highlighter;
    int m_caretPosition = 0;
    QString m_lastDocumentText;
    QByteArray m_lastKnownFileContents;
    QString m_lastKnownFileText;
    bool m_hasKnownFileContents = false;
    bool m_autoSaveSuspended = false;
    bool m_autoSaveHeld = false;
    bool m_reloadsCleanFiles = false;
    QString m_recoveryPath;
    std::unique_ptr<QLockFile> m_recoveryLock;

    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QString m_themeCodeBackground;
    QString m_themeSelectionForeground;
    QString m_editorFont;
    bool m_omaPalette = false;
    QHash<QString, QString> m_themeTokens;
    QString m_themeSource;
    QFileSystemWatcher m_themeWatcher;
    QTimer m_themeReloadTimer;
};

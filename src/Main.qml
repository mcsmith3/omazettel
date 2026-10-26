import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import QtQuick.Window
import "EditorMutations.js" as EditorMutations

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 520
    visible: true
    title: (backend.modified ? "* " : "") + backend.fileName + " - OmaZettel"

    readonly property bool darkMode: backend.darkMode
    readonly property color pageColor: backend.themeBackground
    readonly property color textColor: backend.themeForeground
    // OmaZettel chrome colours, by name from the theme file (or the Oma
    // palette): see themed/omazettel.toml.tpl.
    readonly property var themeTokens: backend.themeTokens
    readonly property bool omaPalette: backend.omaPalette
    readonly property color mutedColor: themeTokens["ui.muted"] || (darkMode ? "#909191" : "#aeb1b5")
    readonly property color panelColor: themeTokens["ui.panel"] || pageColor
    readonly property color raisedColor: themeTokens["ui.raised"] || pageColor
    readonly property color fieldColor: themeTokens["ui.field"] || pageColor
    readonly property color borderColor: themeTokens["ui.border"] || mutedColor
    readonly property color caretColor: themeTokens["editor.cursor"] || textColor
    readonly property color selectionFill: backend.themeSelection
    readonly property color selectionTextColor: backend.themeSelectionForeground
    // The desktop's text size knob (GNOME's text-scaling-factor, which
    // `omarchy display text size` drives) anchored so its 12px default leaves
    // the app at the sizes it was designed around, times the user's own
    // text size set with Ctrl+= / Ctrl+- / Ctrl+wheel.
    readonly property real textScale: backend.textScale
    readonly property int editorFontPixelSize: scaledSize(20)
    readonly property int footerHeight: scaledSize(32)
    // Never wider than the Flickable's viewport, whatever the floor asks for:
    // a tiling compositor can resize the window below its minimum width.
    readonly property int editorWidth: Math.min(
        Math.round(writerFontMetrics.averageCharacterWidth * 65),
        Math.max(360, contentWidth - Math.round(writerFontMetrics.averageCharacterWidth * 20)),
        Math.max(0, contentWidth - 48))
    // OmaZettel: the notes sidebar takes the left edge; the writing area is
    // what is left.
    readonly property bool sidebarShown: vault.sidebarVisible
    readonly property int sidebarWidth: sidebarShown ? Math.min(vault.sidebarWidth, Math.round(width * 0.5)) : 0
    readonly property bool panelShown: vault.isOpen && vault.panelVisible
    readonly property int panelWidth: panelShown ? Math.min(scaledSize(280), Math.round(width * 0.3)) : 0
    readonly property int contentWidth: width - sidebarWidth - panelWidth
    // Notes visited, for Back (Alt+Left) and Forward (Alt+Right).
    property var backStack: []
    property var forwardStack: []
    property bool navigatingHistory: false
    property bool closeConfirmed: false
    property bool searchOpen: false
    property bool searchUpdating: false
    property var searchMatches: []
    property int searchMatchIndex: -1
    property url pendingOpenUrl
    property string pendingAction: ""
    property bool replaceOpen: false
    property bool awaitingPendingSave: false

    Material.theme: darkMode ? Material.Dark : Material.Light
    Material.accent: backend.themeAccent
    color: pageColor

    onClosing: function(close) {
        if (closeConfirmed || !backend.modified)
            return;

        close.accepted = false;
        pendingAction = "close";
        if (!unsavedChangesDialog.opened)
            unsavedChangesDialog.open();
    }

    function requestOpen(url) {
        if (!backend.modified) {
            backend.open(url);
            return;
        }
        pendingOpenUrl = url;
        pendingAction = "open";
        unsavedChangesDialog.open();
    }

    // ── OmaZettel: notes folder actions ─────────────────────────────────────

    function currentPath() {
        return vault.pathForUrl(backend.fileUrl);
    }

    // Opening another note saves the current one first when auto-save would
    // have saved it anyway, and asks only when it would not.
    function openNote(url) {
        if (vault.pathForUrl(url) === currentPath())
            return;
        if (!navigatingHistory && currentPath().length > 0) {
            backStack = backStack.concat([backend.fileUrl]).slice(-100);
            forwardStack = [];
        }
        if (backend.flushAutoSave())
            backend.open(url);
        else
            requestOpen(url);
    }

    // A file named when OmaZettel was started again (`omazettel note.md`,
    // or opening a Markdown file from the file manager) while this window
    // was running. Like openNote(), it saves or asks about the open note
    // first; a file that does not exist yet starts a new one there.
    function openFromLaunch(url) {
        if (vault.pathForUrl(url) === currentPath())
            return;
        if (currentPath().length > 0) {
            backStack = backStack.concat([backend.fileUrl]).slice(-100);
            forwardStack = [];
        }
        if (backend.flushAutoSave() || !backend.modified)
            backend.openOrStartFile(url);
        else
            requestOpen(url);
    }

    function goBack() {
        if (backStack.length === 0)
            return;
        var url = backStack[backStack.length - 1];
        backStack = backStack.slice(0, -1);
        if (currentPath().length > 0)
            forwardStack = forwardStack.concat([backend.fileUrl]);
        navigatingHistory = true;
        openNote(url);
        navigatingHistory = false;
    }

    function goForward() {
        if (forwardStack.length === 0)
            return;
        var url = forwardStack[forwardStack.length - 1];
        forwardStack = forwardStack.slice(0, -1);
        if (currentPath().length > 0)
            backStack = backStack.concat([backend.fileUrl]);
        navigatingHistory = true;
        openNote(url);
        navigatingHistory = false;
    }

    // ── Outline and search results ──────────────────────────────────────────

    // The open note's headings, re-read shortly after typing stops.
    property var outlineHeadings: []

    Timer {
        id: outlineDelay
        interval: 300
        onTriggered: win.outlineHeadings = backend.outline()
    }

    // Puts the caret at position (selecting length characters) and scrolls
    // there.
    function showPosition(position, length) {
        if (position < 0)
            return;
        var start = Math.min(position, editor.length);
        if (length > 0)
            editor.select(start, Math.min(start + length, editor.length));
        else
            editor.cursorPosition = start;
        editorFlick.ensureCursorVisible();
        editor.forceActiveFocus();
    }

    function openSearchResult(path, position, length) {
        openNote(vault.urlForPath(path));
        if (currentPath() === path)
            showPosition(position, length);
    }

    function showNotesSearch() {
        if (!vault.isOpen)
            return;
        vault.sidebarVisible = true;
        var selected = editor.selectedText;
        sidebar.showSearch(selected.indexOf("\u2029") < 0 && selected.length < 80 ? selected : "");
    }

    // ── Making notes ────────────────────────────────────────────────────────

    // Notes made from links and the quick switcher use this template when
    // it exists (it carries the pandoc header), else a plain heading.
    readonly property string linkTemplate: "Note"

    // Makes a note from a template, or a blank one when templateName is ""
    // or missing. Returns {path, cursor, existed} or null.
    function createNoteFile(templateName, title, folder, exactName) {
        noteTemplates.ensureDefaults();
        if (templateName.length > 0 && noteTemplates.pathOf(templateName).length > 0) {
            var result = noteTemplates.create(templateName, title, folder, exactName || "");
            if (result.path === undefined) {
                vault.showError(noteTemplates.lastError());
                return null;
            }
            return result;
        }
        var path = vault.createNote(folder, exactName || title);
        return path.length > 0 ? { path: path, cursor: -1, existed: false } : null;
    }

    function openCreated(result) {
        if (!result)
            return;
        noteIndex.updateFile(result.path);
        openNote(vault.urlForPath(result.path));
        if (result.cursor >= 0 && currentPath() === result.path)
            editor.cursorPosition = Math.min(result.cursor, editor.length);
        editor.forceActiveFocus();
    }

    function openDailyNote() {
        if (!vault.isOpen)
            return;
        noteTemplates.ensureDefaults();
        if (noteTemplates.pathOf("Daily").length === 0) {
            vault.showError("There is no Daily template in .omazettel/note-templates.");
            return;
        }
        openCreated(createNoteFile("Daily", "", vault.rootPath, ""));
    }

    // A [[link]] target as a folder and a note name: "Projects/Plan" goes in
    // Projects, "Plan" next to the linking note. Null for targets that would
    // leave the notes folder.
    function linkTargetPlace(target, fromPath) {
        if (target.indexOf("..") >= 0)
            return null;
        var slash = target.lastIndexOf("/");
        if (slash < 0)
            return { folder: vault.folderFor(fromPath), name: target };
        return { folder: vault.rootPath + "/" + target.substring(0, slash).replace(/^\/+/, ""),
                 name: target.substring(slash + 1) };
    }

    // Opens the note a [[link]] names, making it when there is none yet.
    function followWikiLink(target, heading) {
        if (!vault.isOpen)
            return;
        if (target.length === 0 && heading && heading.length > 0) {
            showPosition(backend.headingPosition(heading), 0);
            return;
        }
        var path = noteIndex.resolve(target, currentPath());
        if (path.length === 0) {
            var place = linkTargetPlace(target, currentPath());
            var result = place ? createNoteFile(linkTemplate, place.name, place.folder, place.name) : null;
            if (!result)
                return;
            noteIndex.updateFile(result.path);
            path = result.path;
        }
        openNote(vault.urlForPath(path));
        if (heading && heading.length > 0 && currentPath() === path)
            showPosition(backend.headingPosition(heading), 0);
    }

    // After a save: every [[link]] in the note that points nowhere gets its
    // note made, except the one the caret is in (it may be half typed).
    function createMissingLinkedNotes() {
        var path = currentPath();
        if (!vault.autoCreateLinks || !noteIndex.covers(path))
            return;
        var missing = noteIndex.missingTargets(path);
        if (missing.length === 0)
            return;
        var typing = backend.wikiLinkAt(editor.cursorPosition);
        var made = [];
        for (var i = 0; i < missing.length; ++i) {
            if (missing[i] === typing)
                continue;
            var place = linkTargetPlace(missing[i], path);
            var result = place ? createNoteFile(linkTemplate, place.name, place.folder, place.name) : null;
            if (result) {
                noteIndex.updateFile(result.path);
                made.push(place.name);
            }
        }
        if (made.length === 1)
            backend.showStatus("Created linked note " + made[0]);
        else if (made.length > 1)
            backend.showStatus("Created " + made.length + " linked notes");
    }

    // Before notes move: rewrites [[links]] to them everywhere, the open note
    // in the editor (so it is not changed under it on disk) and the rest on
    // disk. moves maps old paths to new ones.
    function updateLinksFor(moves) {
        var links = 0;
        var notes = 0;
        var open = currentPath();
        if (open.length > 0 && noteIndex.covers(open)) {
            var text = editor.text;
            var rewritten = noteIndex.rewriteText(text, open, moves);
            if (rewritten !== text) {
                var caret = editor.cursorPosition;
                links += noteIndex.lastRewriteCount();
                notes += 1;
                EditorMutations.replaceRange(editor, 0, text.length, rewritten);
                editor.cursorPosition = Math.min(caret, editor.length);
            }
        }
        links += noteIndex.rewriteLinks(moves, open);
        notes += noteIndex.lastRewriteFiles();
        if (links > 0)
            backend.showStatus("Updated " + links + (links === 1 ? " link" : " links")
                               + " in " + notes + (notes === 1 ? " note" : " notes"));
    }

    function movesFor(path, target) {
        if (vault.isFolderPath(path))
            return noteIndex.movesForFolder(path, target);
        var moves = {};
        moves[path] = target;
        return moves;
    }

    // Ctrl+click or Ctrl+Enter: a [[wiki link]] opens its note, a web link
    // opens in the browser, a link to a local .md file opens here.
    function followLinkAt(position) {
        var target = backend.wikiLinkAt(position);
        var heading = backend.wikiHeadingAt(position);
        if (target.length > 0 || heading.length > 0) {
            followWikiLink(target, heading);
            return true;
        }
        var url = backend.linkUrlAt(position);
        if (url.length === 0)
            return false;
        if (/^[a-z][a-z0-9+.-]*:/i.test(url) && !/^file:/i.test(url)) {
            Qt.openUrlExternally(url);
            return true;
        }
        var local = decodeURIComponent(url.replace(/^file:\/\//i, "").split("#")[0]);
        if (local.charAt(0) !== "/")
            local = vault.folderFor(currentPath()) + "/" + local;
        openNote(vault.urlForPath(local));
        return true;
    }

    // Below the caret, or above it when the window ends first.
    function placeCompleter(popup) {
        var caret = editor.cursorRectangle;
        popup.x = Math.max(0, Math.min(caret.x, editor.width - popup.width));
        var below = editor.mapToItem(win.contentItem, 0, caret.y + caret.height + 4).y;
        var room = win.height - win.footerHeight - below;
        var height = Math.min(popup.implicitHeight, win.scaledSize(280));
        popup.y = room >= height ? caret.y + caret.height + 4 : caret.y - height - 4;
    }

    // Shows or updates the [[ completer at the caret.
    function updateLinkCompletion() {
        if (!vault.isOpen || editor.selectionStart !== editor.selectionEnd) {
            linkCompleter.close();
            citeCompleter.close();
            return;
        }
        // [@ citations, when a bibliography is loaded.
        var cite = citations.count > 0 ? backend.citationQueryAt(editor.cursorPosition) : ({});
        if (cite.query !== undefined) {
            linkCompleter.close();
            citeCompleter.query = cite.query;
            citeCompleter.queryStart = cite.start;
            placeCompleter(citeCompleter);
            if (!citeCompleter.opened)
                citeCompleter.open();
            return;
        }
        citeCompleter.close();
        var context = backend.wikiQueryAt(editor.cursorPosition);
        if (context.query === undefined) {
            linkCompleter.close();
            return;
        }
        linkCompleter.query = context.query;
        linkCompleter.queryStart = context.start;
        linkCompleter.closedLink = context.closed;
        placeCompleter(linkCompleter);
        if (!linkCompleter.opened)
            linkCompleter.open();
    }

    function newNote(folder) {
        if (!vault.isOpen) {
            backend.newWindow();
            return;
        }
        noteTemplates.ensureDefaults();
        nameDialog.askForNote(noteTemplates.list(), noteTemplates.defaultTemplate, function(title, templateName) {
            if (templateName.length > 0)
                noteTemplates.defaultTemplate = templateName;
            openCreated(createNoteFile(templateName, title, folder, ""));
        });
    }

    function newFolder(folder) {
        nameDialog.askForName("New folder", "", "Create", function(name) {
            var path = vault.createFolder(folder, name);
            if (path.length > 0) {
                sidebar.reveal(path);
                sidebar.selectedPath = path;
            }
        });
    }

    // Renaming the open note, or a folder holding it, keeps it open at its
    // new path, and links to the renamed notes follow them.
    function renameItem(path) {
        var name = path.substring(path.lastIndexOf("/") + 1);
        nameDialog.askForName("Rename", name, "Rename", function(newName) {
            var target = vault.renameTarget(path, newName);
            if (target.length === 0 || target === path)
                return;
            relocate(path, target, function() { return vault.renamePath(path, newName); });
        });
    }

    // Moving the open note, or a folder holding it, keeps it open at its new
    // path, and links that name a path follow it.
    function moveItem(path, folder) {
        var target = vault.moveTarget(path, folder);
        if (target.length === 0 || target === path)
            return;
        relocate(path, target, function() { return vault.movePath(path, folder); });
    }

    // Shared by rename and move: update links, then move the files, keeping
    // the open note open.
    function relocate(path, target, move) {
        var open = currentPath();
        var isOpenNote = path === open;
        var holdsOpenNote = open.length > 0 && open.indexOf(path + "/") === 0;
        // The open note has to be saved first; if auto-save cannot do it,
        // ask for a save and leave everything where it is.
        if ((isOpenNote || holdsOpenNote) && !backend.flushAutoSave()) {
            backend.save();
            return;
        }
        // Links are rewritten before anything moves, while the index still
        // knows the old paths.
        updateLinksFor(movesFor(path, target));
        if (isOpenNote) {
            if (backend.moveCurrentFile(vault.urlForPath(target)))
                sidebar.reveal(target);
            noteIndex.scheduleRescan();
            return;
        }
        if (holdsOpenNote)
            backend.flushAutoSave();
        var moved = move();
        if (moved.length === 0)
            return;
        if (holdsOpenNote)
            backend.followMovedFile(vault.urlForPath(moved + open.substring(path.length)));
        sidebar.reveal(moved);
    }

    function trashItem(path) {
        var name = path.substring(path.lastIndexOf("/") + 1);
        var what = vault.isFolderPath(path) ? "the folder " + name + " and everything in it"
                                            : name.replace(/\.(md|markdown)$/i, "");
        nameDialog.confirm("Move to trash", "Move " + what + " to the trash?", "Move to trash", function() {
            var open = currentPath();
            var holdsOpenNote = open.length > 0 && (open === path || open.indexOf(path + "/") === 0);
            if (holdsOpenNote) {
                if (!backend.flushAutoSave()) {
                    backend.save();
                    return;
                }
                backend.startNewDocument();
            }
            vault.trashPath(path);
        });
    }

    // Export a note with pandoc. The open note exports what is in the editor.
    function exportNote(path) {
        if (path.length === 0) {
            backend.showStatus("Save the note before exporting it.");
            return;
        }
        var isOpen = path === currentPath();
        exportDialog.openFor(path, isOpen ? editor.text : "", isOpen);
    }

    // ── Command palette ─────────────────────────────────────────────────────

    // Every action, for Ctrl+Shift+P. name is what is searched and shown,
    // title the shortcut. Actions that need a notes folder or an open note
    // are left out when there is none.
    function commandList() {
        var hasFolder = vault.isOpen;
        var hasNote = currentPath().length > 0;
        var all = [
            { id: "newNote", name: "New note", title: "Ctrl+N", when: hasFolder },
            { id: "daily", name: "Today's daily note", title: "Ctrl+Shift+D", when: hasFolder },
            { id: "newFolder", name: "New folder", title: "", when: hasFolder },
            { id: "goto", name: "Go to note", title: "Ctrl+O", when: hasFolder },
            { id: "search", name: "Search all notes", title: "Ctrl+Shift+F", when: hasFolder },
            { id: "find", name: "Find in note", title: "Ctrl+F", when: true },
            { id: "replace", name: "Find and replace", title: "Ctrl+H", when: true },
            { id: "back", name: "Back", title: "Alt+Left", when: backStack.length > 0 },
            { id: "forward", name: "Forward", title: "Alt+Right", when: forwardStack.length > 0 },
            { id: "export", name: "Export note", title: "Ctrl+Shift+E", when: hasNote },
            { id: "exportFolder", name: "Export this folder", title: "", when: hasFolder },
            { id: "exportAll", name: "Export the whole notes folder", title: "", when: hasFolder },
            { id: "history", name: "Note history", title: "", when: hasNote && sync.isRepo },
            { id: "syncNow", name: sync.remoteUrl.length > 0 ? "Sync now" : "Save a version now", title: "Ctrl+Alt+S", when: sync.isRepo },
            { id: "syncDialog", name: "Sync and history settings", title: "", when: hasFolder },
            { id: "citations", name: "Citations", title: "", when: hasFolder },
            { id: "templates", name: "Edit note templates", title: "", when: hasFolder },
            { id: "rename", name: "Rename note", title: "", when: hasNote && noteIndex.covers(currentPath()) },
            { id: "trash", name: "Move note to trash", title: "", when: hasNote && noteIndex.covers(currentPath()) },
            { id: "sidebar", name: "Show or hide sidebar", title: "Ctrl+\\", when: true },
            { id: "panel", name: "Show or hide backlinks, tags and outline", title: "Ctrl+Shift+B", when: hasFolder },
            { id: "palette", name: backend.omaPalette ? "Use Omarchy theme colors" : "Use Oma color palette", title: "", when: true },
            { id: "settings", name: "Settings", title: "Ctrl+,", when: true },
            { id: "chooseFolder", name: hasFolder ? "Change notes folder" : "Choose notes folder", title: "", when: true },
            { id: "openFile", name: "Open file", title: "Ctrl+Shift+O", when: true },
            { id: "newWindow", name: "New window", title: "Ctrl+Shift+N", when: true },
            { id: "print", name: "Print", title: "Ctrl+P", when: true },
            { id: "copyHtml", name: "Copy as HTML", title: "Ctrl+Shift+C", when: true },
            { id: "fullscreen", name: "Full screen", title: "F11", when: true },
            { id: "shortcuts", name: "Keyboard shortcuts", title: "F1", when: true }
        ];
        return all.filter(function(command) { return command.when; }).map(function(command) {
            return { id: command.id, name: command.name, title: command.title, folder: "", path: "" };
        });
    }

    function runCommand(id) {
        var folder = sidebar.targetFolder;
        switch (id) {
        case "newNote": newNote(folder); break;
        case "daily": openDailyNote(); break;
        case "newFolder": newFolder(folder); break;
        case "goto": quickSwitcher.open(); break;
        case "search": showNotesSearch(); break;
        case "find": openSearch(false); break;
        case "replace": openSearch(true); break;
        case "back": goBack(); break;
        case "forward": goForward(); break;
        case "export": exportNote(currentPath()); break;
        case "exportFolder": exportDialog.openForFolder(vault.folderFor(currentPath()), noteTemplates.author); break;
        case "exportAll": exportDialog.openForFolder(vault.rootPath, noteTemplates.author); break;
        case "history": openHistory(currentPath()); break;
        case "syncNow": syncNotes(true); break;
        case "syncDialog": syncDialog.open(); break;
        case "citations": citationsDialog.open(); break;
        case "templates":
            noteTemplates.ensureDefaults();
            Qt.openUrlExternally(vault.urlForPath(noteTemplates.folder));
            break;
        case "rename": renameItem(currentPath()); break;
        case "trash": trashItem(currentPath()); break;
        case "sidebar": vault.sidebarVisible = !vault.sidebarVisible; break;
        case "panel": vault.panelVisible = !vault.panelVisible; break;
        case "palette": backend.omaPalette = !backend.omaPalette; break;
        case "settings": settingsDialog.open(); break;
        case "chooseFolder": chooseFolder(); break;
        case "openFile": backend.openDialog(); break;
        case "newWindow": backend.newWindow(); break;
        case "print": backend.printDocument(); break;
        case "copyHtml": backend.copyAsHtml(); break;
        case "fullscreen": toggleFullScreen(); break;
        case "shortcuts": shortcutsDialog.open(); break;
        }
    }

    // ── Sync ────────────────────────────────────────────────────────────────

    // Saves the open note, then syncs; the open note is reloaded afterwards
    // if the sync changed it.
    // Whether the running sync was asked for (so a failure opens Sync &
    // history with the reason) rather than automatic.
    property bool manualSync: false

    function syncNotes(manual) {
        if (!sync.isRepo || sync.busy)
            return;
        win.manualSync = manual === true;
        backend.flushAutoSave();
        backend.pauseFileWatching();
        if (!sync.syncNow()) {
            backend.resumeFileWatching();
            if (sync.lastError().length > 0)
                backend.showStatus(sync.lastError());
        }
    }

    function afterFilesChanged() {
        var caret = editor.cursorPosition;
        if (backend.resumeFileWatching())
            editor.cursorPosition = Math.min(caret, editor.length);
        noteIndex.scheduleRescan();
    }

    function resolveConflict(path, choice) {
        if (choice === "edited" && path === currentPath() && !backend.flushAutoSave())
            backend.save();
        backend.pauseFileWatching();
        var ok = sync.resolve(path, choice);
        afterFilesChanged();
        if (!ok) {
            backend.showStatus(sync.lastError());
            return;
        }
        backend.showStatus(sync.gitConflicts.length > 0 ? "Resolved; " + sync.gitConflicts.length + " left."
                                                        : "Resolved.");
        if (sync.gitConflicts.length === 0 && sync.remoteUrl.length > 0 && choice !== "copy")
            syncNotes();
    }

    function openHistory(path) {
        if (path !== currentPath())
            openNote(vault.urlForPath(path));
        if (currentPath() === path)
            historyDialog.openFor(path, editor.text);
    }

    // The first empty notes folder asks how to start: AI-first or traditional.
    function askLayoutIfNew() {
        if (vault.folderLayout === "" && vault.isNewFolder())
            layoutChoiceDialog.open();
    }

    Connections {
        target: vault
        function onRootPathChanged() { Qt.callLater(win.askLayoutIfNew); }
    }

    function chooseFolder() {
        if (vault.isOpen)
            folderDialog.currentFolder = vault.urlForPath(vault.rootPath);
        folderDialog.open();
    }

    function completePendingAction() {
        var action = pendingAction;
        pendingAction = "";
        if (action === "close") {
            closeConfirmed = true;
            close();
        } else if (action === "open") {
            backend.open(pendingOpenUrl);
        }
    }

    FontMetrics {
        id: writerFontMetrics
        font.family: backend.editorFont
        font.pixelSize: win.editorFontPixelSize
    }

    // Every hardcoded size in the interface is expressed at text scale 1.
    function scaledSize(pixels) {
        return Math.max(1, Math.round(pixels * win.textScale));
    }

    function toggleFullScreen() {
        win.visibility = win.visibility === Window.FullScreen
            ? Window.Windowed
            : Window.FullScreen;
    }

    // Only the search field and the next and previous controls select a
    // match. When the document changes underneath, the matches follow along
    // without taking the writer's caret.
    function updateSearch(selectMatch) {
        searchDelay.stop();
        var previous = searchMatchIndex >= 0 ? searchMatches[searchMatchIndex] : 0;
        searchMatches = backend.searchMatches(searchField.text);
        searchMatchIndex = selectMatch ? (searchMatches.length > 0 ? 0 : -1)
                                       : matchAtOrAfter(previous);
        showSearchMatch(selectMatch);
    }

    function matchAtOrAfter(position) {
        for (var i = 0; i < searchMatches.length; ++i) {
            if (searchMatches[i] >= position)
                return i;
        }
        return searchMatches.length > 0 ? 0 : -1;
    }

    function showSearchMatch(selectMatch) {
        var start = searchMatchIndex >= 0 ? searchMatches[searchMatchIndex] : -1;
        searchUpdating = true;
        backend.setSearchHighlight(searchField.text, start);
        if (selectMatch && start >= 0) {
            editor.select(start, start + searchField.text.length);
            editorFlick.ensureCursorVisible();
        }
        searchUpdating = false;
    }

    // Typing in the search field waits for a pause before searching, so
    // anything that acts on the matches catches up with the field first.
    function flushSearch() {
        if (searchDelay.running)
            updateSearch(true);
    }

    function moveSearch(direction) {
        // A pending search lands on its first match, which is where the
        // writer meant to go, rather than one past it.
        if (searchDelay.running) {
            updateSearch(true);
            return;
        }
        if (searchMatches.length === 0)
            return;
        searchMatchIndex = (searchMatchIndex + direction + searchMatches.length)
                           % searchMatches.length;
        showSearchMatch(true);
    }

    // The matches stop following the field and the text while the bar is
    // closed, and a search still waiting for a pause is dropped on closing,
    // so opening the bar searches again for what the field holds.
    function openSearch(withReplace) {
        searchOpen = true;
        if (withReplace)
            replaceOpen = true;
        searchField.forceActiveFocus();
        searchField.selectAll();
        // A search still waiting in an open bar lands on its first match.
        updateSearch(searchDelay.running);
    }

    function closeSearch() {
        searchDelay.stop();
        // Hand focus to the editor before hiding the bar. Hiding the field
        // that holds focus leaves the input method with nothing focused for
        // a moment, and fcitx5 can then miss the next Ctrl+F.
        Qt.inputMethod.reset();
        editor.forceActiveFocus();
        searchOpen = false;
        searchUpdating = true;
        backend.setSearchHighlight("", -1);
        editor.deselect();
        searchUpdating = false;
        replaceOpen = false;
    }

    // Restyling a large document for each letter typed makes typing a word
    // stall, so the search waits until the typing pauses.
    Timer {
        id: searchDelay
        interval: 150
        onTriggered: win.updateSearch(true)
    }

    Shortcut {
        sequence: "Ctrl+S"
        onActivated: backend.save()
    }

    Shortcut {
        sequence: "Ctrl+H"
        onActivated: win.openSearch(true)
    }

    Shortcut {
        sequence: "Ctrl+B"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("**")
    }

    Shortcut {
        sequence: "Ctrl+I"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("*")
    }

    Shortcut {
        sequence: "Ctrl+Shift+X"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("~~")
    }

    Shortcut {
        sequence: "Ctrl+K"
        context: Qt.WindowShortcut
        onActivated: editor.insertLink()
    }

    Shortcut {
        sequences: ["Ctrl+?", "Ctrl+/", "F1"]
        onActivated: shortcutsDialog.open()
    }

    Shortcut {
        // Ctrl++ is a different shortcut to Qt, not a shifted Ctrl+=, and it is
        // the only one a layout that puts + on its own key can reach.
        sequences: ["Ctrl+=", "Ctrl++"]
        onActivated: backend.increaseTextSize()
    }

    Shortcut {
        sequence: "Ctrl+-"
        onActivated: backend.decreaseTextSize()
    }

    Shortcut {
        sequence: "Ctrl+0"
        onActivated: backend.resetTextSize()
    }

    // OmaZettel: Ctrl+O goes to a note in the notes folder; opening any
    // file moves to Ctrl+Shift+O.
    Shortcut {
        sequence: "Ctrl+O"
        onActivated: vault.isOpen ? quickSwitcher.open() : backend.openDialog()
    }

    Shortcut {
        sequence: "Ctrl+Shift+O"
        onActivated: backend.openDialog()
    }

    // OmaZettel: Ctrl+N makes a note when a notes folder is open; a new
    // window moves to Ctrl+Shift+N.
    Shortcut {
        sequence: "Ctrl+N"
        onActivated: win.newNote(sidebar.targetFolder)
    }

    Shortcut {
        sequence: "Ctrl+Shift+N"
        onActivated: backend.newWindow()
    }

    Shortcut {
        sequence: "Ctrl+\\"
        onActivated: vault.sidebarVisible = !vault.sidebarVisible
    }

    Shortcut {
        sequence: "Ctrl+Shift+P"
        onActivated: {
            commandPalette.sourceItems = win.commandList();
            commandPalette.open();
        }
    }

    Shortcut {
        sequence: "Ctrl+,"
        onActivated: settingsDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+Alt+S"
        onActivated: win.syncNotes(true)
    }

    Shortcut {
        sequence: "Ctrl+Shift+F"
        onActivated: win.showNotesSearch()
    }

    Shortcut {
        sequence: "Ctrl+Shift+E"
        onActivated: win.exportNote(win.currentPath())
    }

    Shortcut {
        sequence: "Ctrl+Shift+D"
        onActivated: win.openDailyNote()
    }

    Shortcut {
        sequence: "Ctrl+Shift+B"
        onActivated: vault.panelVisible = !vault.panelVisible
    }

    Shortcut {
        sequence: "Alt+Left"
        onActivated: win.goBack()
    }

    Shortcut {
        sequence: "Alt+Right"
        onActivated: win.goForward()
    }

    Shortcut {
        sequences: ["Ctrl+Return", "Ctrl+Enter"]
        onActivated: win.followLinkAt(editor.cursorPosition)
    }

    Shortcut {
        sequence: "Ctrl+Shift+S"
        onActivated: backend.saveAsDialog()
    }

    Shortcut {
        sequence: "Ctrl+P"
        onActivated: backend.printDocument()
    }

    Shortcut {
        sequence: "Ctrl+Shift+C"
        onActivated: backend.copyAsHtml()
    }

    Shortcut {
        sequences: ["Meta+F", "F11"]
        onActivated: toggleFullScreen()
    }

    Shortcut {
        sequence: "Ctrl+Z"
        context: Qt.WindowShortcut
        onActivated: editor.undo()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        context: Qt.WindowShortcut
        onActivated: editor.redo()
    }

    Shortcut {
        sequence: "Ctrl+F"
        onActivated: win.openSearch(false)
    }

    Shortcut {
        sequence: "Ctrl+G"
        enabled: win.searchOpen
        onActivated: win.moveSearch(1)
    }

    Connections {
        target: backend

        function onOpenDialogRequested() {
            openFileDialog.open();
        }

        function onSaveDialogRequested(suggestedUrl) {
            saveFileDialog.selectedFile = suggestedUrl;
            saveFileDialog.open();
        }

        function onSaveSucceeded() {
            // Only a save the writer chose in the prompt carries out the
            // pending close or open, never an auto-save that lands meanwhile.
            const confirmed = win.awaitingPendingSave;
            win.awaitingPendingSave = false;
            if (confirmed && win.pendingAction !== "")
                win.completePendingAction();
        }

        function onSaveFailed() {
            // A failed save leaves the document as it was, so the close or
            // open it was for is off; a later save must not carry it out.
            win.awaitingPendingSave = false;
            win.pendingAction = "";
        }

        function onExternalChangeDetected(deleted, locallyModified) {
            externalChangeDialog.deleted = deleted;
            externalChangeDialog.locallyModified = locallyModified;
            externalChangeDialog.open();
        }
    }

    Dialogs.FileDialog {
        id: openFileDialog
        title: "Open File"
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        onAccepted: win.requestOpen(selectedFile)
    }

    Dialogs.FileDialog {
        id: saveFileDialog
        title: "Save File"
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        onAccepted: backend.saveAs(selectedFile)
        onRejected: {
            win.awaitingPendingSave = false;
            win.pendingAction = "";
        }
    }

    UnsavedChangesDialog {
        id: unsavedChangesDialog
        objectName: "unsavedChangesDialog"
        fileName: backend.fileName
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.textColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height

        // Discard must still mean discard, so nothing is auto-saved while
        // the writer decides.
        onAboutToShow: backend.holdAutoSave(true)
        onClosed: backend.holdAutoSave(false)

        onDiscardRequested: {
            backend.discardRecovery();
            win.completePendingAction();
        }

        onSaveRequested: {
            win.awaitingPendingSave = true;
            backend.save();
        }
        onCancelRequested: win.pendingAction = ""
    }

    ExternalChangeDialog {
        id: externalChangeDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.textColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height

        onKeepRequested: backend.keepExternalVersion()
        onReloadRequested: backend.reloadFromDisk()
    }

    LayoutChoiceDialog {
        id: layoutChoiceDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.textColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height
        onChosen: function(layout) {
            vault.folderLayout = layout;
            noteTemplates.ensureDefaults();
        }
    }

    Dialogs.FolderDialog {
        id: folderDialog
        title: "Choose Notes Folder"
        onAccepted: {
            if (vault.openFolderUrl(selectedFolder))
                vault.sidebarVisible = true;
        }
    }

    NameDialog {
        id: nameDialog
        raisedColor: win.raisedColor
        fieldColor: win.fieldColor
        borderColor: win.borderColor
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.textColor
        activeButtonColor: backend.themeAccent
        selectionFill: win.selectionFill
        selectionTextColor: win.selectionTextColor
        containerWidth: win.width
        containerHeight: win.height
        onClosed: editor.forceActiveFocus()
    }

    Sidebar {
        id: sidebar
        panelColor: win.panelColor
        borderColor: win.borderColor
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: win.sidebarWidth
        visible: win.sidebarShown
        darkMode: win.darkMode
        textScale: win.textScale
        pageColor: win.pageColor
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        currentPath: vault.pathForUrl(backend.fileUrl)

        onOpenRequested: function(fileUrl) { win.openNote(fileUrl); }
        onNewNoteRequested: function(folder) { win.newNote(folder); }
        onNewFolderRequested: function(folder) { win.newFolder(folder); }
        onRenameRequested: function(path) { win.renameItem(path); }
        onTrashRequested: function(path) { win.trashItem(path); }
        onMoveRequested: function(path, folder) { win.moveItem(path, folder); }
        onDailyNoteRequested: win.openDailyNote()
        onExportRequested: function(path) { win.exportNote(path); }
        onExportFolderRequested: function(path) { exportDialog.openForFolder(path, noteTemplates.author); }
        onCitationsRequested: citationsDialog.open()
        onSyncDialogRequested: syncDialog.open()
        onSyncNowRequested: win.syncNotes(true)
        onHistoryRequested: function(path) { win.openHistory(path); }
        onSettingsRequested: settingsDialog.open()
        onSearchResultRequested: function(path, position, length) { win.openSearchResult(path, position, length); }
        onEditTemplateRequested: function(path) { win.openNote(vault.urlForPath(path)); }
        onAuthorRequested: nameDialog.askForName("Author name", noteTemplates.author, "Save",
                                                 function(name) { noteTemplates.author = name; })
        onChooseFolderRequested: win.chooseFolder()
    }

    NotePanel {
        id: notePanel
        panelColor: win.panelColor
        borderColor: win.borderColor
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: win.panelWidth
        visible: win.panelShown
        darkMode: win.darkMode
        textScale: win.textScale
        pageColor: win.pageColor
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        currentPath: vault.pathForUrl(backend.fileUrl)
        onOpenRequested: function(fileUrl) { win.openNote(fileUrl); }
        onTagRequested: function(tag) {
            vault.sidebarVisible = true;
            sidebar.showTag(tag);
        }
        headings: win.outlineHeadings
        cursorPosition: editor.cursorPosition
        onHeadingRequested: function(position) { win.showPosition(position, 0); }
    }

    ExportDialog {
        id: exportDialog
        raisedColor: win.raisedColor
        fieldColor: win.fieldColor
        borderColor: win.borderColor
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        selectionFill: win.selectionFill
        selectionTextColor: win.selectionTextColor
        containerWidth: win.width
        containerHeight: win.height
        askName: function(heading, initial, callback) {
            nameDialog.askForName(heading, initial, "Create", callback);
        }
        onEditRequested: function(path) {
            close();
            win.openNote(vault.urlForPath(path));
        }
        onClosed: editor.forceActiveFocus()
    }

    SettingsDialog {
        id: settingsDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        raisedColor: win.raisedColor
        fieldColor: win.fieldColor
        borderColor: win.borderColor
        containerWidth: win.width
        containerHeight: win.height
        onSyncDialogRequested: { close(); syncDialog.open(); }
        onCitationsRequested: { close(); citationsDialog.open(); }
        onTemplatesRequested: win.runCommand("templates")
        onEditFileRequested: function(path) { close(); win.openNote(vault.urlForPath(path)); }
        onClosed: editor.forceActiveFocus()
    }

    SyncDialog {
        id: syncDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        raisedColor: win.raisedColor
        fieldColor: win.fieldColor
        borderColor: win.borderColor
        containerWidth: win.width
        containerHeight: win.height
        onSyncRequested: win.syncNotes(true)
        onCompareRequested: function(path) { conflictDialog.openFor(path); }
        onEditConflictRequested: function(path) { close(); win.openNote(vault.urlForPath(path)); }
        onResolveRequested: function(path, choice) { win.resolveConflict(path, choice); }
        onClosed: editor.forceActiveFocus()
    }

    ConflictDialog {
        id: conflictDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        raisedColor: win.raisedColor
        borderColor: win.borderColor
        containerWidth: win.width
        containerHeight: win.height
        onResolveRequested: function(path, choice) { win.resolveConflict(path, choice); }
    }

    HistoryDialog {
        id: historyDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        raisedColor: win.raisedColor
        borderColor: win.borderColor
        containerWidth: win.width
        containerHeight: win.height
        onRestoreRequested: function(text) {
            EditorMutations.replaceRange(editor, 0, editor.length, text);
            editor.cursorPosition = 0;
            backend.showStatus("Restored an earlier version; Ctrl+Z takes it back.");
        }
        onClosed: editor.forceActiveFocus()
    }

    Connections {
        target: sync
        function onSynced(ok, updatedFiles) {
            win.afterFilesChanged();
            backend.showStatus(ok ? sync.message : "Sync failed: " + sync.message);
            // A failure someone asked about, or one that needs them, opens
            // Sync & history with the reason.
            if (sync.state === "conflict" || (!ok && (win.manualSync || sync.errorKind === "private-email"
                                                      || sync.errorKind === "auth" || sync.errorKind === "ssh-key")))
                syncDialog.open();
            win.manualSync = false;
        }
        function onAutoSyncDue() {
            win.syncNotes();
        }
    }

    // Sync shortly after starting, when a remote is set up.
    Timer {
        interval: 1500
        running: true
        onTriggered: {
            if (sync.isRepo && sync.remoteUrl.length > 0 && sync.autoSync)
                win.syncNotes();
        }
    }

    // Keep the sync status's change count current after saves.
    Timer {
        id: syncRefreshDelay
        interval: 2000
        onTriggered: sync.refresh()
    }

    CitationsDialog {
        id: citationsDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        raisedColor: win.raisedColor
        borderColor: win.borderColor
        containerWidth: win.width
        containerHeight: win.height
        onClosed: editor.forceActiveFocus()
    }

    NotePicker {
        id: commandPalette
        objectName: "commandPalette"
        hasField: true
        showSubtitle: false
        sourceItems: []
        placeholder: "Run a command…"
        showShortcut: true
        emptyText: "No matching command."
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        selectionFill: win.selectionFill
        selectionTextColor: win.selectionTextColor
        raisedColor: win.raisedColor
        borderColor: win.borderColor
        x: Math.round((win.width - width) / 2)
        y: Math.round(win.height * 0.15)
        onPicked: function(command, typed) {
            if (command)
                Qt.callLater(win.runCommand, command.id);
        }
        onClosed: editor.forceActiveFocus()
    }

    NotePicker {
        id: quickSwitcher
        raisedColor: win.raisedColor
        borderColor: win.borderColor
        objectName: "quickSwitcher"
        hasField: true
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        selectionFill: win.selectionFill
        selectionTextColor: win.selectionTextColor
        x: Math.round((win.width - width) / 2)
        y: Math.round(win.height * 0.15)
        onPicked: function(note, typed) {
            if (note) {
                win.openNote(vault.urlForPath(note.path));
            } else if (typed.length > 0) {
                win.openCreated(win.createNoteFile(win.linkTemplate, typed, sidebar.targetFolder, typed));
            }
        }
        onClosed: editor.forceActiveFocus()
    }

    Connections {
        target: backend
        function onSaveSucceeded() {
            noteIndex.updateFile(win.currentPath());
            win.createMissingLinkedNotes();
            if (sync.isRepo)
                syncRefreshDelay.restart();
        }
    }

    Connections {
        target: backend
        function onFileUrlChanged() {
            win.outlineHeadings = backend.outline();
            vault.rememberLastNote(backend.fileUrl);
            sidebar.reveal(win.currentPath());
        }
    }

    ShortcutsDialog {
        id: shortcutsDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height
    }

    Item {
        anchors.fill: parent
        anchors.leftMargin: win.sidebarWidth
        anchors.rightMargin: win.panelWidth

        Flickable {
            id: editorFlick
            objectName: "editorScroll"
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            // Inset the scrolling area by the footer strip at both ends, so
            // scrolled text never runs under the footer buttons, status, or
            // word count, and stops just as far short of the top edge.
            anchors.topMargin: win.footerHeight
            anchors.bottomMargin: win.footerHeight
            clip: true
            contentWidth: width
            contentHeight: Math.max(height, editor.y + editor.implicitHeight + 220)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                // Wheel scrolling moves contentY directly rather than
                // flicking the Flickable, so the bar has to be told about
                // that activity; linger briefly after the last event.
                active: hovered || pressed || wheelScroll.running || scrollLinger.running
            }

            // Also lingers after paging through the document from the
            // keyboard, so the bar shows where the jump landed. Writing past
            // the bottom edge scrolls too, but leaves the bar hidden.
            Timer {
                id: scrollLinger
                objectName: "scrollLinger"
                interval: 600
            }

            // Flickable turns a wheel notch into a flick sized by the small
            // application font, which crawls next to a browser. Reproduce
            // Chromium's wheel physics instead (cc::ScrollOffsetAnimationCurve):
            // each notch moves 3 lines of 40px towards a running target, the
            // animation gets shorter as the outstanding distance grows, and a
            // notch landing mid-animation carries the current velocity into
            // the new curve, so sustained spinning keeps picking up speed.
            readonly property real wheelStep: win.scaledSize(120)

            FrameAnimation {
                id: wheelScroll
                running: false

                property real startY: 0
                property real targetY: 0
                property real duration: 0.2
                // Cubic bezier easing; ease-in-out (0.42, 0, 0.58, 1) for a
                // fresh scroll, with y1 tilted on retarget so the curve's
                // initial slope matches the velocity it inherits.
                property real cx1: 0.42
                property real cy1: 0
                readonly property real cx2: 0.58
                readonly property real cy2: 1

                onTriggered: {
                    var x = elapsedTime / duration;
                    if (x >= 1) {
                        editorFlick.contentY = editorFlick.snapToPixel(targetY);
                        stop();
                        return;
                    }
                    editorFlick.contentY = editorFlick.snapToPixel(
                        startY + (targetY - startY) * curveY(solveCurve(x)));
                }

                function begin(from, to, dur, slope) {
                    startY = from;
                    targetY = to;
                    duration = dur;
                    cx1 = 0.42;
                    cy1 = 0.42 * Math.max(-1000, Math.min(1000, slope));
                    restart();
                }

                function retarget(newTarget) {
                    var s = solveCurve(Math.min(1, elapsedTime / duration));
                    var pos = startY + (targetY - startY) * curveY(s);
                    var delta = newTarget - pos;
                    if (Math.abs(delta) < 0.5) {
                        editorFlick.contentY = newTarget;
                        stop();
                        return;
                    }

                    var velocity = curveDY(s) / Math.max(1e-6, curveDX(s))
                        * (targetY - startY) / duration;
                    var dur = editorFlick.wheelDuration(delta);
                    // When already moving faster than the eased curve would,
                    // bound the duration by the time to target at the current
                    // velocity; the 2.5x covers the ease-out tail.
                    if (velocity !== 0 && delta / velocity > 0)
                        dur = Math.min(dur, delta / velocity * 2.5);
                    begin(pos, newTarget, dur, velocity * dur / delta);
                }

                // Cubic bezier through (0,0), (cx1,cy1), (cx2,cy2), (1,1),
                // evaluated by Newton-solving the curve parameter from x.
                function curveX(s) { return 3 * s * (1 - s) * ((1 - s) * cx1 + s * cx2) + s * s * s; }
                function curveY(s) { return 3 * s * (1 - s) * ((1 - s) * cy1 + s * cy2) + s * s * s; }
                function curveDX(s) { return 3 * (1 - s) * (1 - s) * cx1 + 6 * (1 - s) * s * (cx2 - cx1) + 3 * s * s * (1 - cx2); }
                function curveDY(s) { return 3 * (1 - s) * (1 - s) * cy1 + 6 * (1 - s) * s * (cy2 - cy1) + 3 * s * s * (1 - cy2); }

                function solveCurve(x) {
                    var s = x;
                    for (var i = 0; i < 8; ++i) {
                        var error = curveX(s) - x;
                        if (Math.abs(error) < 0.001)
                            break;
                        var d = curveDX(s);
                        if (Math.abs(d) < 1e-6)
                            break;
                        s = Math.max(0, Math.min(1, s - error / d));
                    }
                    return s;
                }
            }

            WheelHandler {
                // Wayland compositors route every pointer's scroll through
                // one seat device that Qt classifies as a touchpad, so the
                // device type cannot tell a mouse wheel from two-finger
                // scrolling. Distinguish by event shape instead: discrete
                // wheel notches arrive with only angleDelta set, while
                // finger scrolling carries pixel-precise pixelDelta.
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function(wheel) {
                    if (wheel.modifiers & Qt.ControlModifier) {
                        editorFlick.zoomByWheel(wheel);
                        wheel.accepted = true;
                        return;
                    }

                    scrollLinger.restart();
                    if (wheel.pixelDelta.y !== 0)
                        editorFlick.scrollTo(editorFlick.clampContentY(editorFlick.contentY - wheel.pixelDelta.y));
                    else
                        editorFlick.scrollByWheel(wheel);
                    wheel.accepted = true;
                }
            }

            onMovementStarted: wheelScroll.stop()
            onContentYChanged: editor.hoveredLinkUrl = ""

            // Ctrl+wheel changes the text size, as it does in a browser. A
            // wheel notch is a whole step, but a touchpad trickles in pixels,
            // so bank the fractions and step once a whole one is scrolled.
            property real zoomNotches: 0

            function zoomByWheel(wheel) {
                // Finger scrolling synthesizes an angleDelta too, so pixelDelta
                // is the one to test first, as in onWheel.
                var notches = wheel.pixelDelta.y !== 0
                    ? wheel.pixelDelta.y / 50
                    : wheel.angleDelta.y / 120;
                if (zoomNotches * notches < 0)
                    zoomNotches = 0;

                zoomNotches += notches;
                while (zoomNotches >= 1) {
                    backend.increaseTextSize();
                    zoomNotches -= 1;
                }
                while (zoomNotches <= -1) {
                    backend.decreaseTextSize();
                    zoomNotches += 1;
                }
            }

            function scrollByWheel(wheel) {
                // High-resolution wheels report fractional notches; feed
                // those through the same animated path, like Chromium does
                // for every wheel-source event.
                var notches = wheel.angleDelta.y / 120;
                if (notches === 0)
                    return;

                if (wheelScroll.running) {
                    wheelScroll.retarget(clampContentY(wheelScroll.targetY - notches * wheelStep));
                    return;
                }

                var target = clampContentY(contentY - notches * wheelStep);
                if (target !== contentY)
                    wheelScroll.begin(contentY, target, wheelDuration(target - contentY), 0);
            }

            // Chromium's inverse-delta duration: 200ms for a single notch,
            // ramping down to 100ms once 480px are outstanding.
            function wheelDuration(delta) {
                var pixels = Math.abs(delta) / win.textScale;
                return Math.max(6, Math.min(12, 14 - pixels / 60)) / 60;
            }

            function clampContentY(y) {
                return Math.max(0, Math.min(Math.max(0, contentHeight - height), y));
            }

            // Whole device pixels keep natively hinted glyphs from
            // re-rasterizing mid-animation, which reads as shimmer.
            function snapToPixel(y) {
                return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio;
            }

            // Jump to a position, abandoning any wheel animation still running.
            function scrollTo(y) {
                wheelScroll.stop();
                contentY = snapToPixel(y);
            }

            // Keep the editing caret within the viewport so writing past the
            // bottom edge scrolls the page along with the text.
            function ensureCursorVisible() {
                var margin = win.editorFontPixelSize * 2;
                var cursorTop = editor.y + editor.cursorRectangle.y;
                var cursorBottom = cursorTop + editor.cursorRectangle.height;
                var maxContentY = Math.max(0, contentHeight - height);

                if (cursorBottom + margin > contentY + height)
                    scrollTo(Math.min(maxContentY, cursorBottom + margin - height));
                else if (cursorTop - margin < contentY)
                    scrollTo(Math.max(0, cursorTop - margin));
            }

            TextEdit {
                id: editor
                objectName: "sourceEditor"
                x: Math.round((editorFlick.width - width) / 2)
                // The resting position counts from the window's top edge, so
                // take back the top inset the Flickable already adds.
                y: Math.max(0, Math.max(42, Math.round(win.height * 0.05)) - win.footerHeight)
                width: win.editorWidth
                height: Math.max(editorFlick.height - y - 96, implicitHeight + 20)
                text: ""
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                activeFocusOnPress: true
                color: win.textColor
                selectedTextColor: win.selectionTextColor
                selectionColor: win.selectionFill
                font.family: backend.editorFont
                font.pixelSize: win.editorFontPixelSize
                font.weight: Font.Normal
                // Native rendering hints glyphs to the pixel grid, which is
                // crispest at whole scale factors but misplaces and unevenly
                // rasterizes glyphs at fractional ones (and goes stale when
                // the compositor delivers the fractional scale after the
                // first frame). Fall back to Qt's scalable renderer there.
                renderType: Screen.devicePixelRatio % 1 === 0 ? TextEdit.NativeRendering : TextEdit.QtRendering
                cursorDelegate: Rectangle {
                    width: 1
                    color: win.caretColor
                }
                onCursorRectangleChanged: editorFlick.ensureCursorVisible()
                onCursorPositionChanged: {
                    backend.setCaretPosition(cursorPosition);
                    win.updateLinkCompletion();
                }
                onFontChanged: backend.documentFontChanged()

                // One edit command, so a single undo takes the change back.
                function replaceText(start, end, replacement) {
                    backend.replaceRange(start, end, replacement);
                }

                function replaceSelectionWith(replacement) {
                    EditorMutations.replaceRange(editor, selectionStart, selectionEnd, replacement);
                }

                function wrapSelection(marker) {
                    forceActiveFocus();
                    EditorMutations.toggleWrap(editor, marker,
                                               backend.emphasisSpanAt(cursorPosition, marker));
                }

                function insertLink() {
                    forceActiveFocus();
                    var start = selectionStart;
                    var end = selectionEnd;
                    var selected = text.slice(start, end);
                    var url = backend.clipboardUrl();
                    var label = selected.length > 0 ? selected : "link text";
                    var markdown = markdownLink(label, url.length > 0 ? url : "https://");
                    var labelLength = escapeMarkdownLinkText(label).length;
                    if (selected.length === 0) {
                        EditorMutations.replaceRange(editor, start, end, markdown,
                                                     1, 1 + labelLength);
                    } else if (url.length === 0) {
                        EditorMutations.replaceRange(editor, start, end, markdown,
                                                     labelLength + 3,
                                                     markdown.length - 1);
                    } else {
                        EditorMutations.replaceRange(editor, start, end, markdown);
                    }
                }

                function smartReturn(softBreak) {
                    if (softBreak) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    var lineStart = text.lastIndexOf("\n", cursorPosition - 1) + 1;
                    var line = text.slice(lineStart, cursorPosition);
                    if (backend.insideFence(cursorPosition)) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    var match = line.match(/^(\s*)([-+*]|\d+[.)]|>+)\s+(.*)$/);
                    if (match) {
                        if (match[3].length === 0) {
                            EditorMutations.replaceRange(editor, lineStart,
                                                         cursorPosition, "\n");
                        } else {
                            var marker = match[2];
                            if (/^\d/.test(marker))
                                marker = (parseInt(marker) + 1) + marker.slice(-1);
                            replaceSelectionWith("\n" + match[1] + marker + " ");
                        }
                        return;
                    }
                    // A new paragraph stands apart from the one above it by a
                    // blank line, which is the second break here. A line that
                    // is already blank has nothing to stand apart from, so
                    // that break would only be a gap nobody asked for.
                    // The break lands on what the selection leaves behind, which
                    // is not the caret's line when it was dragged right to left.
                    var start = selectionStart;
                    var end = selectionEnd;
                    var head = text.slice(text.lastIndexOf("\n", start - 1) + 1, start);
                    var lineEnd = text.indexOf("\n", end);
                    var rest = lineEnd < 0 ? text.slice(end)
                                           : text.slice(end, lineEnd);
                    if (/^\s*$/.test(head) && /^\s*$/.test(rest)) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    replaceSelectionWith("\n\n");
                }

                // Tab nests the list items the caret or selection touches four
                // spaces deeper, and Shift+Tab lifts them back out by the same
                // step. Lines that are neither items, their indented
                // continuations, nor blank, and code fences, leave Tab alone.
                function indentListItems(outdent) {
                    // "* * *" and "- - -" are rules, not items.
                    var listItem = function(line) {
                        return /^[ \t]*(?:[-+*]|\d+[.)])[ \t]/.test(line)
                            && !/^ {0,3}([-*])([ \t]*\1){2,}[ \t]*$/.test(line);
                    };
                    // A tab runs to the next four-column stop.
                    var columns = function(line) {
                        return line.match(/^[ \t]*/)[0].split("").reduce(function(column, c) {
                            return c === "\t" ? column + 4 - column % 4 : column + 1;
                        }, 0);
                    };

                    // A selection that ends at the start of a line leaves it be.
                    // lastIndexOf would read -1 as 0, a newline right there.
                    var start = selectionStart > 0 ? text.lastIndexOf("\n", selectionStart - 1) + 1 : 0;
                    var end = text.indexOf("\n", Math.max(start, selectionEnd - 1));
                    if (end < 0)
                        end = text.length;
                    var lines = text.slice(start, end).split("\n");
                    // The selection starts at an item and holds only lines that
                    // belong to one: other items, blanks, and indented lines.
                    // A fence indented under an item moves along with it whole;
                    // a selection that starts or ends inside code would split it.
                    var filled = lines.filter(function(line) { return /\S/.test(line); });
                    if (!listItem(filled[0] || "")
                        || filled.some(function(line) { return /^\S/.test(line) && !listItem(line); })
                        || backend.insideFence(start)
                        || backend.insideFence(end))
                        return false;

                    // Every line moves by the same step, so the nesting within
                    // the selection is kept. Four spaces nest under any marker
                    // up to "99.", and Shift+Tab lifts no further than the
                    // shallowest line can go.
                    var step = outdent ? -Math.min.apply(null, [4].concat(filled.map(columns))) : 4;
                    if (step === 0)
                        return true;
                    var moved = lines.map(function(line) {
                        var lead = line.match(/^[ \t]*/)[0];
                        return /\S/.test(line)
                            ? " ".repeat(columns(line) + step) + line.slice(lead.length)
                            : line;
                    });
                    var nested = moved.join("\n");
                    var from = Math.max(start, selectionStart + moved[0].length - lines[0].length);
                    // Neither end of the selection leaves its line, even when
                    // the indent it sat in is gone.
                    var lastStart = start + nested.length - moved[moved.length - 1].length;
                    var to = Math.max(from, lastStart, selectionEnd + nested.length - (end - start));
                    var forward = cursorPosition === selectionEnd;
                    replaceText(start, end, nested);
                    select(forward ? from : to, forward ? to : from);
                    return true;
                }

                function escapeMarkdownLinkText(linkText) {
                    return linkText.replace(/\\/g, "\\\\")
                                   .replace(/\[/g, "\\[")
                                   .replace(/\]/g, "\\]");
                }

                function escapeMarkdownLinkDestination(linkUrl) {
                    return linkUrl.replace(/\\/g, "\\\\")
                                  .replace(/\(/g, "\\(")
                                  .replace(/\)/g, "\\)");
                }

                function markdownLink(label, url) {
                    return "[" + escapeMarkdownLinkText(label) + "]("
                        + escapeMarkdownLinkDestination(url) + ")";
                }

                function pasteClipboardUrlAsMarkdownLink() {
                    var start = selectionStart;
                    var end = selectionEnd;
                    if (start === end)
                        return false;

                    var url = backend.clipboardUrl();
                    if (url === "")
                        return false;

                    var selected = text.slice(start, end);
                    var leading = selected.match(/^\s*/)[0];
                    var trailing = selected.match(/\s*$/)[0];
                    var linkText = selected.slice(leading.length,
                                                  selected.length - trailing.length);
                    if (linkText === "")
                        return false;

                    replaceSelectionWith(leading + markdownLink(linkText, url) + trailing);
                    return true;
                }

                function pasteClipboardAsPlainText() {
                    var pastedText = backend.clipboardText();
                    if (pastedText.length > 0)
                        replaceSelectionWith(pastedText);
                }

                function moveCursorVisibly(direction) {
                    if (selectionStart !== selectionEnd) {
                        cursorPosition = direction > 0 ? selectionEnd : selectionStart;
                        return;
                    }

                    var pos = backend.cursorPositionStep(cursorPosition, direction);
                    cursorPosition = Math.max(0, Math.min(length, pos));
                }

                function movePage(direction, extendSelection) {
                    var pageStep = Math.max(win.editorFontPixelSize,
                                            editorFlick.height - win.editorFontPixelSize * 2);
                    var rect = cursorRectangle;
                    var targetY = rect.y + rect.height / 2 + direction * pageStep;
                    var target = positionAt(rect.x, Math.max(0, targetY));
                    if (extendSelection)
                        moveCursorSelection(target, TextEdit.SelectCharacters);
                    else
                        cursorPosition = target;
                }

                function deleteParagraphBreakBehindCursor() {
                    if (selectionStart !== selectionEnd || cursorPosition < 2)
                        return false;

                    // getText reads just the two characters, where text would
                    // copy the whole document on every Backspace.
                    if (getText(cursorPosition - 2, cursorPosition) !== "\n\n")
                        return false;

                    var start = cursorPosition - 2;
                    var lineEnd = text.indexOf("\n", cursorPosition);
                    var line = lineEnd < 0 ? text.slice(cursorPosition)
                                           : text.slice(cursorPosition, lineEnd);
                    // Something on the caret's line and the pair above it is a
                    // paragraph break and nothing else, either the one Return
                    // wrote to end a paragraph or the one the writer is now
                    // closing to join what it separates. Both breaks go.
                    if (/^\s*$/.test(line)) {
                        // On a blank line Return writes a single break, so up
                        // here the pair may be that break and one that was
                        // already in the document. Above a blank line Return
                        // writes a single break too, and so wrote neither of
                        // these.
                        var above = text.slice(text.lastIndexOf("\n", start - 1) + 1, start);
                        if (/^\s*$/.test(above))
                            return false;
                        // Past that the two Returns leave the same text and the
                        // same caret, and no reading of either says which was
                        // pressed. What decides instead is that the gap is left
                        // standing: both breaks go only while a blank line of
                        // it survives them. The end of the document below the
                        // caret leaves it none...
                        if (lineEnd < 0)
                            return false;
                        // ...and so does the next paragraph, which the pair
                        // would otherwise be pulled up against.
                        var belowEnd = text.indexOf("\n", lineEnd + 1);
                        var below = belowEnd < 0 ? text.slice(lineEnd + 1)
                                                 : text.slice(lineEnd + 1, belowEnd);
                        if (!/^\s*$/.test(below))
                            return false;
                    }

                    remove(start, cursorPosition);
                    cursorPosition = start;
                    return true;
                }

                Keys.priority: Keys.BeforeItem
                Keys.onPressed: function(event) {
                    // OmaZettel: the [[ completer takes its navigation keys.
                    if (linkCompleter.handleKey(event) || citeCompleter.handleKey(event)) {
                        event.accepted = true;
                        return;
                    }
                    var pasteKey = (event.key === Qt.Key_V)
                        && (event.modifiers & Qt.ControlModifier)
                        && !(event.modifiers & (Qt.AltModifier | Qt.MetaModifier | Qt.ShiftModifier));
                    var shiftInsert = (event.key === Qt.Key_Insert)
                        && (event.modifiers & Qt.ShiftModifier)
                        && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier));
                    if (pasteKey || shiftInsert) {
                        if (!pasteClipboardUrlAsMarkdownLink())
                            pasteClipboardAsPlainText();
                        event.accepted = true;
                        return;
                    }

                    var pageKey = event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp;
                    var documentEdgeKey = (event.key === Qt.Key_Home || event.key === Qt.Key_End)
                        && (event.modifiers & Qt.ControlModifier);
                    if (pageKey || documentEdgeKey)
                        scrollLinger.restart();

                    var returnKey = event.key === Qt.Key_Return || event.key === Qt.Key_Enter;
                    var commandModifier = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier);
                    if (returnKey && !commandModifier) {
                        smartReturn(event.modifiers & Qt.ShiftModifier);
                        event.accepted = true;
                    } else if (!commandModifier && event.key === Qt.Key_Backspace
                               && deleteParagraphBreakBehindCursor()) {
                        event.accepted = true;
                    } else if (!commandModifier && !(event.modifiers & Qt.ShiftModifier)
                               && event.key === Qt.Key_Right) {
                        moveCursorVisibly(1);
                        event.accepted = true;
                    } else if (!commandModifier && !(event.modifiers & Qt.ShiftModifier)
                               && event.key === Qt.Key_Left) {
                        moveCursorVisibly(-1);
                        event.accepted = true;
                    } else if (!commandModifier
                               && (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                               && indentListItems(event.key === Qt.Key_Backtab
                                                  || (event.modifiers & Qt.ShiftModifier))) {
                        event.accepted = true;
                    } else if (!commandModifier
                               && (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp)) {
                        movePage(event.key === Qt.Key_PageDown ? 1 : -1,
                                 event.modifiers & Qt.ShiftModifier);
                        event.accepted = true;
                    }
                }

                onTextChanged: {
                    hoveredLinkUrl = "";
                    outlineDelay.restart();
                    if (win.searchUpdating)
                        return;
                    var contentChanged = backend.editorTextChanged();
                    if (win.searchOpen && contentChanged)
                        win.updateSearch(false);
                }

                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    text: "# Start writing"
                    visible: editor.length === 0 && !editor.activeFocus
                    color: win.mutedColor
                    font.family: editor.font.family
                    font.pixelSize: editor.font.pixelSize
                    font.weight: editor.font.weight
                }

                property string hoveredLinkUrl: ""
                property real linkHoverX: 0
                property real linkHoverY: 0

                // positionAt() snaps to the nearest caret, even past the end of a
                // line, so only count a character the pointer is inside: the one
                // on either side of that caret, whichever way the text runs.
                function linkUrlUnder(x, y) {
                    var caret = positionAt(x, y);
                    for (var position = Math.max(0, caret - 1);
                         position <= caret && position < length; ++position) {
                        var box = positionToRectangle(position);
                        var next = positionToRectangle(position + 1);
                        // A hidden marker has no width; only a line's last
                        // character needs an estimate.
                        var edge = next.y === box.y
                            ? next.x : box.x + writerFontMetrics.averageCharacterWidth;
                        if (y >= box.y && y < box.y + box.height
                                && x >= Math.min(box.x, edge) && x < Math.max(box.x, edge))
                            return backend.linkUrlAt(position);
                    }
                    return "";
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    hoverEnabled: true
                    cursorShape: Qt.IBeamCursor
                    onPositionChanged: (mouse) => {
                        editor.linkHoverX = mouse.x;
                        editor.linkHoverY = mouse.y;
                        editor.hoveredLinkUrl = editor.linkUrlUnder(mouse.x, mouse.y);
                    }
                    onExited: editor.hoveredLinkUrl = ""
                }

                ToolTip {
                    visible: editor.hoveredLinkUrl.length > 0
                    x: editor.linkHoverX + 12
                    y: editor.linkHoverY + 20
                    contentItem: Label {
                        objectName: "linkTooltipText"
                        // A destination is document text; AutoText would render
                        // one that looks like HTML instead of showing it.
                        textFormat: Text.PlainText
                        text: editor.hoveredLinkUrl
                        font.family: backend.editorFont
                        font.pixelSize: win.scaledSize(12)
                        elide: Text.ElideMiddle
                    }
                    width: Math.min(implicitWidth, win.width - 32)
                }

                // OmaZettel: Ctrl+click follows a link.
                TapHandler {
                    acceptedModifiers: Qt.ControlModifier
                    onTapped: function(eventPoint) {
                        win.followLinkAt(editor.positionAt(eventPoint.position.x, eventPoint.position.y));
                    }
                }

                NotePicker {
                    id: linkCompleter
                    raisedColor: win.raisedColor
                    borderColor: win.borderColor
                    objectName: "linkCompleter"
                    hasField: false
                    property int queryStart: 0
                    property bool closedLink: false
                    darkMode: win.darkMode
                    textScale: win.textScale
                    textColor: win.textColor
                    mutedColor: win.mutedColor
                    accentColor: backend.themeAccent
                    onPicked: function(note, typed) {
                        if (!note)
                            return;
                        var end = editor.cursorPosition + (closedLink ? 2 : 0);
                        EditorMutations.replaceRange(editor, queryStart, end, note.name + "]]");
                    }
                }

                NotePicker {
                    id: citeCompleter
                    objectName: "citeCompleter"
                    hasField: false
                    showSubtitle: true
                    width: win.scaledSize(420)
                    sourceItems: citations.revision >= 0 ? citations.entries() : []
                    property int queryStart: 0
                    darkMode: win.darkMode
                    textScale: win.textScale
                    textColor: win.textColor
                    mutedColor: win.mutedColor
                    accentColor: backend.themeAccent
                    raisedColor: win.raisedColor
                    borderColor: win.borderColor
                    onPicked: function(entry, typed) {
                        if (entry)
                            EditorMutations.replaceRange(editor, queryStart, editor.cursorPosition, entry.name);
                    }
                }

                Component.onCompleted: {
                    backend.attachDocument(textDocument);
                    forceActiveFocus();
                }
            }
        }

        Row {
            id: footerStatus
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.bottomMargin: 10
            spacing: 12
            opacity: 0.55

            FooterIconButton {
                objectName: "saveButton"
                iconName: "save"
                iconColor: win.mutedColor
                tooltip: "Save"
                onClicked: backend.save()
            }

            FooterIconButton {
                objectName: "openButton"
                iconName: "open"
                iconColor: win.mutedColor
                tooltip: "Open"
                onClicked: backend.openDialog()
            }

            FooterIconButton {
                id: fontButton
                objectName: "fontButton"
                iconName: "font"
                iconColor: win.mutedColor
                tooltip: "Font"
                onClicked: fontPicker.opened ? fontPicker.close() : fontPicker.open()
            }

            Label {
                text: backend.status
                color: win.mutedColor
                font.family: Qt.application.font.family
                font.pixelSize: win.scaledSize(11)
                visible: text !== ""
                elide: Text.ElideRight
                width: Math.min(360, win.contentWidth / 3)
                height: win.scaledSize(16)
                verticalAlignment: Text.AlignVCenter
            }
        }

        FontPicker {
            id: fontPicker
            objectName: "fontPicker"
            // Parented to its button, so a press on the button is not a press
            // outside that closes it only for the click to open it again.
            parent: fontButton
            x: -8
            y: -height - 10
            width: win.scaledSize(320)
            maximumHeight: Math.min(win.scaledSize(440), win.height - 80)
            fonts: backend.availableFonts
            currentFont: backend.editorFont
            defaultFont: Qt.application.font.family
            darkMode: win.darkMode
            textScale: win.textScale
            textColor: win.textColor
            mutedColor: win.mutedColor
            highlightColor: backend.themeAccent
            onFontChosen: function(family) { backend.editorFont = family; }
            onClosed: editor.forceActiveFocus()
        }

        Label {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 12
            anchors.bottomMargin: 10
            text: backend.wordCount + (backend.wordCount === 1 ? " Word" : " Words")
            color: win.mutedColor
            opacity: 0.75
            font.family: Qt.application.font.family
            font.pixelSize: win.scaledSize(11)
        }


        Pane {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 12
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: win.scaledSize(win.replaceOpen ? 104 : 56)
            visible: win.searchOpen
            z: 10
            leftPadding: 16
            rightPadding: 8
            topPadding: 0
            bottomPadding: 0
            Material.elevation: 8
            // Reaches here from either field or the buttons beside them.
            Keys.onEscapePressed: function(event) {
                win.closeSearch();
                event.accepted = true;
            }

            background: Rectangle {
                radius: 9
                color: win.raisedColor
            }

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    TextInput {
                        id: searchField
                        objectName: "searchField"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: win.replaceOpen ? parent.height / 2 : parent.height
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        activeFocusOnTab: true
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.selectionTextColor
                        font.pixelSize: win.scaledSize(17)
                        clip: true
                        onTextChanged: searchDelay.restart()
                        Keys.onReturnPressed: function(event) {
                            win.moveSearch((event.modifiers & Qt.ShiftModifier) ? -1 : 1);
                            event.accepted = true;
                        }
                    }

                    TextInput {
                        id: replaceField
                        objectName: "replaceField"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.height / 2
                        visible: win.replaceOpen
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        activeFocusOnTab: true
                        clip: true
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.selectionTextColor
                        font.pixelSize: win.scaledSize(17)
                        Keys.onReturnPressed: replaceCurrentButton.clicked()
                    }

                    Label {
                        anchors.verticalCenter: replaceField.verticalCenter
                        text: "Replace with"
                        visible: win.replaceOpen && replaceField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }

                    Label {
                        anchors.verticalCenter: searchField.verticalCenter
                        text: "Find"
                        visible: searchField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }
                }

                Label {
                    Layout.preferredWidth: win.scaledSize(58)
                    Layout.fillHeight: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: win.searchMatches.length === 0
                        ? "0/0"
                        : (win.searchMatchIndex + 1) + "/" + win.searchMatches.length
                    color: win.darkMode ? win.textColor : "#62635f"
                    font.pixelSize: win.scaledSize(16)
                }

                Button {
                    id: replaceCurrentButton
                    objectName: "replaceButton"
                    visible: win.replaceOpen
                    text: "Replace"
                    onClicked: {
                        win.flushSearch();
                        if (win.searchMatchIndex < 0) return;
                        var start = win.searchMatches[win.searchMatchIndex];
                        var replacement = replaceField.text;
                        EditorMutations.replaceRange(editor, start,
                                                     start + searchField.text.length,
                                                     replacement);
                        // Carry on past what was just written, so a replacement
                        // the query also matches is not found again.
                        win.searchMatches = backend.searchMatches(searchField.text);
                        win.searchMatchIndex = win.matchAtOrAfter(start + replacement.length);
                        win.showSearchMatch(true);
                    }
                }

                Button {
                    objectName: "replaceAllButton"
                    visible: win.replaceOpen
                    text: "All"
                    onClicked: {
                        win.flushSearch();
                        var query = searchField.text;
                        var matches = win.searchMatches;
                        if (query.length === 0 || matches.length === 0) return;
                        // One replacement over the span of every match, so a
                        // single undo takes them all back.
                        var first = matches[0];
                        var replaced = "";
                        var position = first;
                        for (var i = 0; i < matches.length; ++i) {
                            replaced += editor.text.slice(position, matches[i]) + replaceField.text;
                            position = matches[i] + query.length;
                        }
                        EditorMutations.replaceRange(editor, first, position, replaced);
                        win.updateSearch(false);
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 34
                    color: win.borderColor
                }

                SearchIconButton {
                    iconName: "up"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.moveSearch(-1)
                }

                SearchIconButton {
                    iconName: "down"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.moveSearch(1)
                }

                SearchIconButton {
                    iconName: "close"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.closeSearch()
                }
            }
        }
    }

    Component.onCompleted: {
        var geometry = backend.windowGeometry();
        if (geometry.x >= 0) x = geometry.x;
        if (geometry.y >= 0) y = geometry.y;
        width = geometry.width;
        height = geometry.height;
        if (geometry.maximized) showMaximized();
        Qt.callLater(askLayoutIfNew);
    }

    Component.onDestruction: backend.saveWindowGeometry(x, y, width, height, visibility === Window.Maximized)

}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// OmaZettel's settings in one place.
Dialog {
    id: root
    objectName: "settingsDialog"

    property bool darkMode: true
    property real textScale: 1
    property color textColor: "#d0d0d0"
    property color mutedColor: "#909191"
    property color accentColor: "#428bca"
    property color raisedColor: darkMode ? "#1a1a1a" : "#ffffff"
    property color fieldColor: darkMode ? "#202020" : "#f6f6f6"
    property color borderColor: darkMode ? "#343434" : "#d8d8d8"
    property int containerWidth: 900
    property int containerHeight: 700

    signal syncDialogRequested()
    signal citationsRequested()
    signal templatesRequested()
    signal editFileRequested(string path)

    function scaled(pixels) { return Math.max(1, Math.round(pixels * textScale)); }

    onAboutToShow: {
        authorField.text = noteTemplates.author;
        noteTemplates.ensureDefaults();
    }

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    width: Math.min(scaled(620), containerWidth - 48)
    height: Math.min(implicitHeight, containerHeight - 48)
    x: Math.round((containerWidth - width) / 2)
    y: Math.max(24, Math.round((containerHeight - height) / 3))
    padding: scaled(20)

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.5) }
    background: Rectangle { color: root.raisedColor; border.color: root.borderColor }

    component Heading: Label {
        color: root.textColor
        Layout.topMargin: root.scaled(10)
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(13)
        font.bold: true
    }

    component Small: Label {
        color: root.mutedColor
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(11)
    }

    // A checkbox drawn like the rest of the app.
    component Check: RowLayout {
        id: check
        property bool checked: false
        property string text: ""
        signal toggled()
        spacing: root.scaled(10)
        Layout.fillWidth: true
        Label {
            text: check.checked ? "☑" : "☐"
            color: check.checked ? root.accentColor : root.mutedColor
            font.pixelSize: root.scaled(15)
        }
        Label {
            Layout.fillWidth: true
            text: check.text
            wrapMode: Text.Wrap
            color: root.textColor
            font.family: Qt.application.font.family
            font.pixelSize: root.scaled(12)
        }
        TapHandler { onTapped: check.toggled() }
    }

    component Action: Label {
        id: action
        signal clicked()
        color: actionMouse.containsMouse ? root.textColor : root.accentColor
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(12)
        MouseArea {
            id: actionMouse
            anchors.fill: parent
            anchors.margins: -root.scaled(4)
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.clicked()
        }
    }

    contentItem: Flickable {
        implicitHeight: content.implicitHeight
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: content
            width: parent.width
            spacing: root.scaled(8)

            Label {
                text: "Settings"
                color: root.textColor
                font.family: Qt.application.font.family
                font.pixelSize: root.scaled(16)
                font.bold: true
            }

            // ── Look ────────────────────────────────────────────────────────
            Heading { text: "Look"; Layout.topMargin: root.scaled(4) }
            Check {
                objectName: "themeColorsCheck"
                checked: !backend.omaPalette
                text: "Omarchy theme: colors follow the current theme, like Neovim and VS Code"
                onToggled: backend.omaPalette = false
            }
            Check {
                objectName: "omaPaletteCheck"
                checked: backend.omaPalette
                text: "Oma palette: the fixed dark look of OmaShow, with the theme's accent"
                onToggled: backend.omaPalette = true
            }
            Small {
                visible: !backend.omaPalette
                text: backend.themeSource === "theme file" ? "Colors come from the theme's own omazettel.toml."
                      : backend.themeSource === "your template" ? "Colors come from your template, ~/.config/omarchy/themed/omazettel.toml.tpl."
                      : "Colors come from OmaZettel's built-in template, filled with the theme's colors."
            }
            Action {
                visible: !backend.omaPalette
                text: "Customize theme colors…"
                onClicked: root.editFileRequested(backend.customizeThemeTemplate())
            }
            Small { text: "The writing font is in the footer (the A button); text size follows Ctrl+= and Ctrl+-." }

            // ── Notes ───────────────────────────────────────────────────────
            Heading { text: "Notes" }
            RowLayout {
                Layout.fillWidth: true
                spacing: root.scaled(12)
                Small { text: "Author"; Layout.fillWidth: false; Layout.preferredWidth: root.scaled(110) }
                Rectangle {
                    Layout.fillWidth: true
                    height: root.scaled(30)
                    color: root.fieldColor
                    border.color: authorField.activeFocus ? root.accentColor : root.borderColor
                    TextInput {
                        id: authorField
                        objectName: "authorField"
                        anchors.fill: parent
                        anchors.leftMargin: root.scaled(8)
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        color: root.textColor
                        font.family: Qt.application.font.family
                        font.pixelSize: root.scaled(12)
                        onEditingFinished: noteTemplates.author = text
                    }
                }
            }
            Small { text: "Fills {{author}} in note templates and the author of folder exports." }
            RowLayout {
                Layout.fillWidth: true
                spacing: root.scaled(12)
                Small { text: "New notes use"; Layout.fillWidth: false; Layout.preferredWidth: root.scaled(110) }
                ComboBox {
                    id: templateBox
                    objectName: "defaultTemplateBox"
                    Layout.fillWidth: true
                    readonly property var choices: noteTemplates.revision >= 0 ? noteTemplates.list().map(function(t) { return t.name; }) : []
                    model: choices
                    currentIndex: Math.max(0, choices.indexOf(noteTemplates.defaultTemplate))
                    onActivated: function(index) { noteTemplates.defaultTemplate = choices[index]; }
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(12)
                }
                Action { text: "Edit templates"; onClicked: root.templatesRequested() }
            }
            Check {
                objectName: "autoCreateLinksCheck"
                checked: vault.autoCreateLinks
                text: "Make a note for a new [[link]] when the note is saved"
                onToggled: vault.autoCreateLinks = !vault.autoCreateLinks
            }
            Small { text: "Off: notes are only made when you Ctrl+click a link." }
            RowLayout {
                spacing: root.scaled(8)
                Small { text: "Between words in new note file names" }
                ComboBox {
                    objectName: "fileNameSeparatorBox"
                    readonly property var values: ["-", "_", " "]
                    model: ["Hyphen  (Cutover-plan.md)", "Underscore  (Cutover_plan.md)", "Space  (Cutover plan.md)"]
                    currentIndex: Math.max(0, values.indexOf(vault.fileNameSeparator))
                    onActivated: function(index) { vault.fileNameSeparator = values[index]; }
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(12)
                }
            }
            Small { text: "Links work either way: [[Cutover plan]] finds Cutover-plan.md." }
            RowLayout {
                spacing: root.scaled(8)
                Small { text: "New, empty notes folders start" }
                ComboBox {
                    objectName: "folderLayoutBox"
                    readonly property var values: ["numbered", "traditional"]
                    model: ["AI-first  (numbered folders, rules for agents)", "Traditional  (just your notes)"]
                    currentIndex: vault.folderLayout === "traditional" ? 1 : 0
                    onActivated: function(index) { vault.folderLayout = values[index]; }
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(12)
                }
            }
            Small { text: "A folder that already has notes keeps its folders; omazettel setup-layout moves one to AI-first." }
            Check {
                objectName: "mapsOfContentCheck"
                checked: vault.mapsOfContent
                text: "Keep maps of content for projects"
                onToggled: vault.mapsOfContent = !vault.mapsOfContent
            }
            Small { text: "In AI-first folders: 03-projects/projects-moc links a map for each project, listing its notes (not transcripts)." }

            // ── Export ──────────────────────────────────────────────────────
            Heading { text: "Export" }
            Check {
                checked: exporter.exportLocation === "beside"
                text: "Save exports next to the note"
                onToggled: exporter.exportLocation = "beside"
            }
            Check {
                objectName: "exportsFolderCheck"
                checked: exporter.exportLocation === "folder"
                text: "Save exports in the notes folder's Exports folder, kept out of git sync"
                onToggled: exporter.exportLocation = "folder"
            }
            RowLayout {
                spacing: root.scaled(16)
                Action { text: "Citations…"; onClicked: root.citationsRequested() }
                Action {
                    text: "Export profiles folder"
                    onClicked: {
                        exporter.ensureDefaults();
                        Qt.openUrlExternally(vault.urlForPath(exporter.profilesFolder));
                    }
                }
            }

            // ── Sync ────────────────────────────────────────────────────────
            Heading { text: "Sync" }
            Check {
                checked: sync.autoSync
                text: "Sync automatically when OmaZettel starts and every " + sync.intervalMinutes + " minutes"
                onToggled: sync.autoSync = !sync.autoSync
            }
            Small {
                text: !sync.isRepo ? "Version history is off for this notes folder."
                      : sync.remoteUrl.length > 0 ? "Syncing with " + sync.remoteUrl
                      : "Version history is on; no remote to sync with."
            }
            Action { text: "Sync & history…"; onClicked: root.syncDialogRequested() }

            // ── About ───────────────────────────────────────────────────────
            Heading { text: "About" }
            Small {
                text: "OmaZettel " + Qt.application.version + " · GPL-3.0-or-later\n"
                      + "An independent fork of Omawrite by David Heinemeier Hansson (MIT). "
                      + "Not made or endorsed by Omacom or the Omarchy project."
            }
        }
    }

    footer: Item {
        implicitHeight: doneButton.implicitHeight + root.scaled(20)
        SquareDialogButton {
            id: doneButton
            anchors.right: parent.right
            anchors.rightMargin: root.scaled(20)
            anchors.verticalCenter: parent.verticalCenter
            text: "Done"
            primary: true
            darkMode: root.darkMode
            textScale: root.textScale
            activeColor: root.accentColor
            onClicked: {
                noteTemplates.author = authorField.text;
                root.close();
            }
        }
    }
}

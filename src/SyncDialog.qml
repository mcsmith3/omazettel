import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Sync and version history for the notes folder: turn git on, connect a
// remote on any git server (or make a repository on GitHub, GitLab, Codeberg,
// Forgejo, Gitea or Gogs), sync, see why a sync failed, and resolve
// conflicts from git or from synced-folder tools.
Dialog {
    id: root
    objectName: "syncDialog"

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
    property string feedback: ""
    property bool showDetails: false
    property bool editingIdentity: false
    // Where a new repository is made: github, gitlab, codeberg, forgejo, gitea, gogs.
    property string provider: "github"
    readonly property bool needsServer: provider === "gitlab" || provider === "forgejo" || provider === "gitea" || provider === "gogs"
    readonly property bool needsToken: provider !== "github" || !sync.ghAvailable
    readonly property color errorColor: darkMode ? "#e8a0a0" : "#a03030"

    signal syncRequested()
    signal compareRequested(string path)
    signal editConflictRequested(string path)
    signal resolveRequested(string path, string choice)

    function scaled(pixels) { return Math.max(1, Math.round(pixels * textScale)); }

    function relativeTime(date) {
        if (!date || isNaN(date.getTime()))
            return "";
        var seconds = Math.round((Date.now() - date.getTime()) / 1000);
        if (seconds < 60) return "just now";
        if (seconds < 3600) return Math.round(seconds / 60) + " min ago";
        if (seconds < 86400) return Math.round(seconds / 3600) + " h ago";
        return date.toLocaleDateString();
    }

    onAboutToShow: {
        sync.refresh();
        feedback = "";
        remoteField.text = sync.remoteUrl;
        repoNameField.text = vault.rootName.replace(/\s+/g, "-");
        showDetails = false;
        editingIdentity = false;
        tokenField.text = "";
        createTokenField.text = "";
    }

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    width: Math.min(scaled(640), containerWidth - 48)
    height: Math.min(implicitHeight, containerHeight - 48)
    x: Math.round((containerWidth - width) / 2)
    y: Math.max(24, Math.round((containerHeight - height) / 3))
    padding: scaled(20)

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.5) }
    background: Rectangle { color: root.raisedColor; border.color: root.borderColor }

    component Small: Label {
        color: root.mutedColor
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(11)
    }

    component Heading: Label {
        color: root.textColor
        Layout.topMargin: root.scaled(8)
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(13)
        font.bold: true
    }

    component Action: Label {
        id: action
        signal clicked()
        color: !enabled ? root.mutedColor : (actionMouse.containsMouse ? root.textColor : root.accentColor)
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

    component Field: Rectangle {
        property alias text: input.text
        property alias echoMode: input.echoMode
        property string placeholder: ""
        Layout.fillWidth: true
        height: root.scaled(30)
        color: root.fieldColor
        border.color: input.activeFocus ? root.accentColor : root.borderColor
        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: root.scaled(8)
            anchors.rightMargin: root.scaled(8)
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            selectByMouse: true
            color: root.textColor
            font.family: Qt.application.font.family
            font.pixelSize: root.scaled(12)
            Label {
                anchors.verticalCenter: parent.verticalCenter
                visible: input.text.length === 0 && !input.activeFocus
                text: parent.parent.placeholder
                color: root.mutedColor
                font.family: Qt.application.font.family
                font.pixelSize: root.scaled(12)
            }
        }
    }

    // A choice among a few, shown as words; the chosen one in the accent colour.
    component Choice: Label {
        id: choice
        property bool chosen: false
        signal clicked()
        color: chosen ? root.accentColor : (choiceMouse.containsMouse ? root.textColor : root.mutedColor)
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(12)
        font.underline: chosen
        MouseArea {
            id: choiceMouse
            anchors.fill: parent
            anchors.margins: -root.scaled(3)
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: choice.clicked()
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
                text: "Sync & history"
                color: root.textColor
                font.family: Qt.application.font.family
                font.pixelSize: root.scaled(16)
                font.bold: true
            }

            Small {
                visible: !sync.gitAvailable
                text: "git is not installed. Install it with: sudo pacman -S git"
                color: root.errorColor
            }

            // ── Not yet a repository ────────────────────────────────────────
            Small {
                visible: sync.gitAvailable && !sync.isRepo
                text: "Version history keeps every saved version of your notes with git, so you can see "
                      + "what changed and go back. Add a remote (such as a private GitHub repository) to "
                      + "sync between devices and people."
            }
            Small {
                objectName: "outerRepositoryNote"
                visible: sync.gitAvailable && !sync.isRepo && sync.outerRepository.length > 0
                text: sync.outerRepository + " is a git repository of its own, around your whole home folder. "
                      + "Version history leaves it alone: turning it on starts one just for this notes folder."
            }
            Small {
                visible: sync.gitAvailable && (!sync.isRepo || sync.remoteUrl.length === 0)
                text: "Already syncing these notes from another computer? Don't start a second history here: "
                      + "clone that repository instead (git clone https://github.com/you/notes.git ~/Notes) "
                      + "and choose that folder in OmaZettel."
            }
            Action {
                objectName: "enableHistory"
                visible: sync.gitAvailable && !sync.isRepo
                text: "Turn on version history"
                onClicked: {
                    if (!sync.enable())
                        root.feedback = sync.lastError();
                }
            }

            // ── Repository ──────────────────────────────────────────────────
            GridLayout {
                visible: sync.isRepo
                columns: 2
                columnSpacing: root.scaled(12)
                rowSpacing: root.scaled(4)
                Layout.fillWidth: true
                Small { text: "Folder"; Layout.fillWidth: false }
                Label {
                    Layout.fillWidth: true
                    text: vault.prettyPath(sync.repoRoot) + (sync.branch.length > 0 ? "  (" + sync.branch + ")" : "")
                    elide: Text.ElideMiddle
                    color: root.textColor
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(12)
                }
                Small { text: "Status"; Layout.fillWidth: false }
                Label {
                    objectName: "syncStatus"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: {
                        var s = sync.state;
                        var base = s === "conflict" ? "Conflicts to resolve"
                                 : s === "error" ? "Last sync failed"
                                 : s === "local" ? "Version history on, no remote"
                                 : s === "changes" ? sync.changeCount + (sync.changeCount === 1 ? " change" : " changes") + " not synced yet"
                                 : "Up to date";
                        var when = root.relativeTime(sync.lastSync);
                        return base + (when.length > 0 ? " · synced " + when : "");
                    }
                    color: sync.state === "error" || sync.state === "conflict" ? root.errorColor : root.textColor
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(12)
                }
            }
            Small {
                visible: sync.isRepo && sync.message.length > 0 && sync.state !== "error"
                text: sync.message
            }

            // ── Why the last sync failed ────────────────────────────────────
            Rectangle {
                visible: sync.isRepo && sync.state === "error"
                Layout.fillWidth: true
                implicitHeight: errorColumn.implicitHeight + root.scaled(20)
                color: "transparent"
                border.color: root.errorColor
                radius: root.scaled(4)
                ColumnLayout {
                    id: errorColumn
                    x: root.scaled(10)
                    y: root.scaled(10)
                    width: parent.width - root.scaled(20)
                    spacing: root.scaled(6)
                    Label {
                        objectName: "syncError"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: sync.message.length > 0 ? sync.message : "The last sync failed."
                        color: root.errorColor
                        font.family: Qt.application.font.family
                        font.pixelSize: root.scaled(12)
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: root.scaled(16)
                        Action {
                            objectName: "useNoReplyAction"
                            visible: sync.errorKind === "private-email"
                            text: "Use my GitHub no-reply email and sync again"
                            enabled: !sync.busy
                            onClicked: {
                                if (!sync.useNoReplyEmail())
                                    root.feedback = sync.lastError();
                            }
                        }
                        Action {
                            visible: sync.remoteUrl.length > 0
                            text: "Test connection"
                            enabled: !sync.busy
                            onClicked: sync.testRemote()
                        }
                        Action {
                            visible: sync.details.length > 0
                            text: root.showDetails ? "Hide git's message" : "Show git's message"
                            onClicked: root.showDetails = !root.showDetails
                        }
                        Action {
                            text: "Open the sync log"
                            onClicked: Qt.openUrlExternally("file://" + sync.logPath)
                        }
                    }
                    TextEdit {
                        visible: root.showDetails && sync.details.length > 0
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        text: sync.details
                        color: root.mutedColor
                        font.family: Qt.application.font.family
                        font.pixelSize: root.scaled(11)
                    }
                }
            }

            // ── Who versions are saved as ───────────────────────────────────
            RowLayout {
                visible: sync.isRepo && !root.editingIdentity
                Layout.fillWidth: true
                spacing: root.scaled(12)
                Small {
                    text: "Versions are saved as " + (sync.commitName.length > 0 ? sync.commitName : "(no name)")
                          + " <" + (sync.commitEmail.length > 0 ? sync.commitEmail : "no email") + ">"
                }
                Action {
                    text: "Change"
                    onClicked: {
                        nameField.text = sync.commitName;
                        emailField.text = sync.commitEmail;
                        root.editingIdentity = true;
                    }
                }
            }
            Small {
                visible: sync.isRepo && sync.hostKind === "github" && sync.commitEmail.length > 0
                         && sync.commitEmail.indexOf("noreply") < 0 && sync.state !== "error"
                text: "This email is published with your notes' history on GitHub. If GitHub refuses pushes that "
                      + "show a private email, use your no-reply address (Change, or the fix offered when a sync fails)."
            }
            RowLayout {
                visible: sync.isRepo && root.editingIdentity
                Layout.fillWidth: true
                spacing: root.scaled(8)
                Field { id: nameField; placeholder: "Name" }
                Field { id: emailField; placeholder: "Email" }
                Action {
                    text: "Save"
                    onClicked: {
                        if (sync.setCommitIdentity(nameField.text, emailField.text))
                            root.editingIdentity = false;
                        else
                            root.feedback = sync.lastError();
                    }
                }
                Action { text: "Cancel"; onClicked: root.editingIdentity = false }
            }
            RowLayout {
                visible: sync.isRepo
                spacing: root.scaled(16)
                Action {
                    objectName: "syncNowAction"
                    text: sync.busy ? "Syncing…" : (sync.remoteUrl.length > 0 ? "Sync now" : "Save a version now")
                    enabled: !sync.busy && sync.state !== "conflict"
                    onClicked: root.syncRequested()
                }
                BusyIndicator {
                    visible: sync.busy
                    running: sync.busy
                    implicitWidth: root.scaled(20)
                    implicitHeight: root.scaled(20)
                }
            }

            // ── Remote ──────────────────────────────────────────────────────
            Heading { visible: sync.isRepo; text: "Remote" }
            Small {
                visible: sync.isRepo
                text: "Where versions are synced: any git server, such as GitHub, GitLab, Codeberg, Forgejo, Gitea, "
                      + "Gogs or your own. An SSH address (git@host:you/notes.git) signs in with your SSH key; an "
                      + "https address (https://host/you/notes.git) with gh for GitHub, or an access token below."
            }
            RowLayout {
                visible: sync.isRepo
                Layout.fillWidth: true
                spacing: root.scaled(12)
                Field { id: remoteField; objectName: "remoteField" }
                Action {
                    text: "Save"
                    onClicked: root.feedback = sync.setRemote(remoteField.text)
                               ? (remoteField.text.trim().length > 0 ? "Remote saved. Test the connection to check signing in." : "Remote removed.")
                               : sync.lastError()
                }
                Action {
                    visible: sync.remoteUrl.length > 0
                    text: "Test"
                    enabled: !sync.busy
                    onClicked: sync.testRemote()
                }
            }
            // Signing in to an https remote that gh does not cover.
            Small {
                visible: sync.isRepo && sync.remoteIsHttps && sync.hostKind !== "github"
                text: "Sign in with a token: make an access token with permission to write repositories in "
                      + sync.remoteHost + "'s settings ("
                      + (sync.hostKind === "gitlab" ? "Preferences → Access tokens, scope write_repository"
                         : "Settings → Applications → Access tokens, repository read and write") + "). "
                      + "It is kept in your desktop keyring."
            }
            RowLayout {
                visible: sync.isRepo && sync.remoteIsHttps && sync.hostKind !== "github"
                Layout.fillWidth: true
                spacing: root.scaled(8)
                Field { id: tokenUserField; placeholder: "Username"; Layout.preferredWidth: root.scaled(150); Layout.fillWidth: false }
                Field { id: tokenField; placeholder: "Access token"; echoMode: TextInput.Password }
                Action {
                    text: "Save token"
                    enabled: tokenField.text.trim().length > 0
                    onClicked: {
                        if (sync.saveToken(tokenUserField.text, tokenField.text)) {
                            tokenField.text = "";
                            root.feedback = "Token saved. Testing the connection…";
                            sync.testRemote();
                        } else {
                            root.feedback = sync.lastError();
                        }
                    }
                }
            }
            Small {
                visible: sync.isRepo && sync.remoteUrl.length === 0
                text: "Or make a private repository on:"
            }
            Flow {
                visible: sync.isRepo && sync.remoteUrl.length === 0
                Layout.fillWidth: true
                spacing: root.scaled(14)
                Repeater {
                    model: [{ id: "github", name: "GitHub" }, { id: "gitlab", name: "GitLab" },
                            { id: "codeberg", name: "Codeberg" }, { id: "forgejo", name: "Forgejo" },
                            { id: "gitea", name: "Gitea" }, { id: "gogs", name: "Gogs" }]
                    delegate: Choice {
                        required property var modelData
                        text: modelData.name
                        chosen: root.provider === modelData.id
                        onClicked: {
                            root.provider = modelData.id;
                            serverField.text = modelData.id === "gitlab" ? "https://gitlab.com" : "";
                        }
                    }
                }
            }
            Small {
                visible: sync.isRepo && sync.remoteUrl.length === 0
                text: root.provider === "github"
                      ? (sync.ghAvailable ? "Uses your GitHub CLI login (gh auth login)."
                                          : "Paste a GitHub token that may create repositories (github.com/settings/tokens), or install gh.")
                      : root.provider === "gitlab"
                        ? "GitLab.com or your own GitLab. The token needs the api scope (Preferences → Access tokens)."
                        : root.provider === "codeberg"
                          ? "The token needs repository write access (codeberg.org/user/settings/applications)."
                          : "Your server's address, and a token with repository write access (Settings → Applications)."
            }
            RowLayout {
                visible: sync.isRepo && sync.remoteUrl.length === 0 && root.needsServer
                Layout.fillWidth: true
                spacing: root.scaled(8)
                Field { id: serverField; placeholder: root.provider === "gitlab" ? "https://gitlab.com" : "https://git.example.com" }
            }
            RowLayout {
                visible: sync.isRepo && sync.remoteUrl.length === 0
                Layout.fillWidth: true
                spacing: root.scaled(8)
                Field { id: repoNameField; placeholder: "Repository name"; Layout.preferredWidth: root.scaled(170); Layout.fillWidth: !root.needsToken }
                Field { id: createTokenField; visible: root.needsToken; placeholder: "Access token"; echoMode: TextInput.Password }
                Action {
                    objectName: "createRepoAction"
                    text: "Create"
                    enabled: !sync.busy && repoNameField.text.trim().length > 0
                             && (!root.needsToken || createTokenField.text.trim().length > 0)
                             && (!root.needsServer || serverField.text.trim().length > 0)
                    onClicked: {
                        if (!sync.createRemoteRepo(root.provider, serverField.text, root.needsToken ? createTokenField.text : "",
                                                   repoNameField.text, true))
                            root.feedback = sync.lastError();
                        else
                            createTokenField.text = "";
                    }
                }
            }

            // ── Automatic sync ──────────────────────────────────────────────
            Heading { visible: sync.isRepo && sync.remoteUrl.length > 0; text: "Automatic sync" }
            RowLayout {
                visible: sync.isRepo && sync.remoteUrl.length > 0
                spacing: root.scaled(10)
                Label {
                    text: sync.autoSync ? "☑" : "☐"
                    color: sync.autoSync ? root.accentColor : root.mutedColor
                    font.pixelSize: root.scaled(15)
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: sync.autoSync = !sync.autoSync }
                }
                Label {
                    text: "Sync when OmaZettel starts and every"
                    color: root.textColor
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(12)
                }
                Action { text: "−"; onClicked: sync.intervalMinutes = Math.max(1, sync.intervalMinutes - (sync.intervalMinutes > 10 ? 5 : 1)) }
                Label {
                    text: sync.intervalMinutes + " min"
                    color: root.textColor
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(12)
                }
                Action { text: "+"; onClicked: sync.intervalMinutes = sync.intervalMinutes + (sync.intervalMinutes >= 10 ? 5 : 1) }
            }

            // ── Conflicts ───────────────────────────────────────────────────
            Heading {
                visible: sync.gitConflicts.length > 0 || sync.conflictCopies.length > 0
                text: "Conflicts"
            }
            Small {
                visible: sync.gitConflicts.length > 0
                text: "These notes changed here and on another device since the last sync. Compare and "
                      + "choose a version, or edit the note to combine them and click Done editing."
            }
            Repeater {
                model: sync.gitConflicts
                delegate: RowLayout {
                    required property string modelData
                    Layout.fillWidth: true
                    spacing: root.scaled(14)
                    Label {
                        Layout.fillWidth: true
                        text: vault.prettyPath(modelData).replace(vault.prettyPath(vault.rootPath) + "/", "")
                        elide: Text.ElideMiddle
                        color: root.textColor
                        font.family: Qt.application.font.family
                        font.pixelSize: root.scaled(12)
                    }
                    Action { text: "Compare…"; onClicked: root.compareRequested(modelData) }
                    Action { text: "Edit"; onClicked: root.editConflictRequested(modelData) }
                    Action { text: "Done editing"; onClicked: root.resolveRequested(modelData, "edited") }
                }
            }
            Action {
                visible: sync.gitConflicts.length > 0
                text: "Undo the merge (keep everything as it was here)"
                onClicked: {
                    if (!sync.abortMerge())
                        root.feedback = sync.lastError();
                }
            }

            Small {
                visible: sync.conflictCopies.length > 0
                text: "Copies a sync tool (Syncthing, Nextcloud, Dropbox) left when a note changed on two devices at once:"
            }
            Repeater {
                model: sync.conflictCopies
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: root.scaled(14)
                    Label {
                        Layout.fillWidth: true
                        text: modelData.copy.substring(modelData.copy.lastIndexOf("/") + 1) + "  (" + modelData.tool + ")"
                        elide: Text.ElideMiddle
                        color: root.textColor
                        font.family: Qt.application.font.family
                        font.pixelSize: root.scaled(12)
                    }
                    Action { text: "Compare…"; onClicked: root.compareRequested(modelData.copy) }
                }
            }

            Small {
                visible: root.feedback.length > 0
                text: root.feedback
                color: root.textColor
            }

            Small {
                Layout.topMargin: root.scaled(8)
                text: "Prefer Syncthing, Nextcloud or Dropbox? Point it at this folder; OmaZettel lists any "
                      + "conflict copies it makes here. A note's own history is in its right-click menu."
            }
        }
    }

    footer: Item {
        implicitHeight: closeButton.implicitHeight + root.scaled(20)
        SquareDialogButton {
            id: closeButton
            anchors.right: parent.right
            anchors.rightMargin: root.scaled(20)
            anchors.verticalCenter: parent.verticalCenter
            text: "Close"
            darkMode: root.darkMode
            textScale: root.textScale
            labelColor: root.textColor
            onClicked: root.close()
        }
    }
}

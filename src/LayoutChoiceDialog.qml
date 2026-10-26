import QtQuick
import QtQuick.Controls

// Asked once, the first time an empty notes folder is opened: start it the
// AI-first way (numbered folders and rules for agents) or the traditional
// way (Daily, Meetings and your own folders). Settings can change it later.
Dialog {
    id: root
    objectName: "layoutChoiceDialog"

    property bool darkMode: true
    property color textColor: darkMode ? "#d0d0d0" : "#42464c"
    property color strongTextColor: darkMode ? "#eeeeee" : "#222324"
    property color activeButtonColor: "#428bca"
    property int containerWidth: 520
    property int containerHeight: 420
    property real textScale: 1
    readonly property var themeTokens: typeof backend !== "undefined" && backend !== null ? backend.themeTokens : ({})

    // "numbered" or "traditional".
    signal chosen(string layout)

    modal: true
    focus: true
    // One of the two must be picked; there is no wrong answer to dismiss.
    closePolicy: Popup.NoAutoClose

    onOpened: aiButton.forceActiveFocus()
    width: Math.min(520, containerWidth - 48)
    x: Math.round((containerWidth - width) / 2)
    y: Math.round((containerHeight - height) / 2)
    padding: 20

    function pick(layout) {
        root.close();
        root.chosen(layout);
    }

    Shortcut {
        sequence: "Alt+A"
        enabled: root.opened
        onActivated: aiButton.clicked()
    }

    Shortcut {
        sequence: "Alt+T"
        enabled: root.opened
        onActivated: traditionalButton.clicked()
    }

    background: Rectangle {
        color: root.themeTokens["ui.raised"] || (root.darkMode ? "#1a1a1a" : "#ffffff")
        border.color: root.themeTokens["ui.border"] || (root.darkMode ? "#343434" : "#d8d8d8")
        radius: 0
    }

    component Body: Label {
        width: parent.width
        color: root.textColor
        wrapMode: Text.Wrap
        textFormat: Text.StyledText
        font.family: Qt.application.font.family
        font.pixelSize: Math.round(13 * root.textScale)
    }

    contentItem: Column {
        spacing: 12

        Label {
            text: "How should this notes folder start?"
            color: root.strongTextColor
            font.family: Qt.application.font.family
            font.pixelSize: Math.round(16 * root.textScale)
            font.bold: true
        }

        Body {
            text: "<b>AI-first</b>: numbered folders that you and AI agents both find "
                  + "your way around: 01-system (your rules for agents), 02-context, "
                  + "03-projects (daily and meeting notes go here), 04-outputs and 99-archive."
        }

        Body {
            text: "<b>Traditional</b>: just your notes. Daily, meeting and literature notes "
                  + "get their own folders the first time you make one, and the rest is up to you."
        }

        Body {
            text: "Settings can change this for new folders later."
            opacity: 0.75
        }
    }

    footer: Item {
        implicitHeight: dialogButtons.implicitHeight + 20

        Row {
            id: dialogButtons
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            SquareDialogButton {
                id: traditionalButton
                objectName: "traditionalLayoutButton"
                text: "Traditional"
                formattedText: "<u>T</u>raditional"
                darkMode: root.darkMode
                textScale: root.textScale
                labelColor: root.textColor
                KeyNavigation.right: aiButton
                KeyNavigation.tab: aiButton
                onClicked: root.pick("traditional")
            }

            SquareDialogButton {
                id: aiButton
                objectName: "aiFirstLayoutButton"
                text: "AI-first"
                formattedText: "<u>A</u>I-first"
                primary: true
                darkMode: root.darkMode
                textScale: root.textScale
                activeColor: root.activeButtonColor
                KeyNavigation.left: traditionalButton
                KeyNavigation.tab: traditionalButton
                onClicked: root.pick("numbered")
            }
        }
    }
}

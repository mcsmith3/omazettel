import QtQuick
import QtQuick.Controls
import QtQuick.Window
import OmaZettel

// A PDF from the notes folder, shown in place of the editor: pages one under
// another, zoom, and a way to open it in the desktop's PDF viewer. The
// pages are drawn at the size they are shown, so text stays sharp.
Rectangle {
    id: root
    objectName: "pdfView"

    property string path: ""
    property bool darkMode: true
    property real textScale: 1
    property color pageColor: "#101010"
    property color textColor: "#d0d0d0"
    property color mutedColor: "#909191"
    property color borderColor: Qt.rgba(mutedColor.r, mutedColor.g, mutedColor.b, 0.25)

    // 1 fits the page to the width; more or less zooms from there.
    property real zoom: 1
    readonly property int currentPage: pages.count > 0
        ? Math.max(0, pages.indexAt(pages.width / 2, pages.contentY + pages.height / 3)) : 0

    signal closeRequested()

    function scaled(value) { return Math.round(value * textScale); }
    function zoomBy(factor) { zoom = Math.max(0.25, Math.min(4, zoom * factor)); }

    color: pageColor
    visible: path.length > 0

    onPathChanged: { zoom = 1; pages.contentY = 0; }

    PdfDocument {
        id: document
        objectName: "pdfDocument"
        path: root.path
    }

    // Drawing every page again on each step of a window resize is slow;
    // draw them once the size settles, stretching the old ones meanwhile.
    readonly property real fitWidth: Math.max(100, pages.width - scaled(48))
    readonly property real pageWidth: fitWidth * zoom
    property real renderWidth: 0
    onPageWidthChanged: renderDelay.restart()
    Timer {
        id: renderDelay
        interval: 150
        onTriggered: root.renderWidth = root.pageWidth
    }
    Component.onCompleted: renderWidth = pageWidth

    Shortcut {
        sequences: ["Ctrl+=", "Ctrl++"]
        enabled: root.visible
        onActivated: root.zoomBy(1.25)
    }
    Shortcut {
        sequence: "Ctrl+-"
        enabled: root.visible
        onActivated: root.zoomBy(0.8)
    }
    Shortcut {
        sequence: "Ctrl+0"
        enabled: root.visible
        onActivated: root.zoom = 1
    }

    // ── Toolbar ─────────────────────────────────────────────────────────────
    Rectangle {
        id: toolbar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.scaled(40)
        color: "transparent"

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.borderColor
        }

        Label {
            id: title
            anchors.left: parent.left
            anchors.leftMargin: root.scaled(16)
            anchors.right: tools.left
            anchors.rightMargin: root.scaled(12)
            anchors.verticalCenter: parent.verticalCenter
            text: root.path.split("/").pop()
            elide: Text.ElideMiddle
            color: root.textColor
            font.family: Qt.application.font.family
            font.pixelSize: root.scaled(13)
            font.bold: true
        }

        Row {
            id: tools
            anchors.right: parent.right
            anchors.rightMargin: root.scaled(8)
            anchors.verticalCenter: parent.verticalCenter
            spacing: root.scaled(4)

            component ToolText: Label {
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                color: root.mutedColor
                font.family: Qt.application.font.family
                font.pixelSize: root.scaled(12)
            }
            component ToolButton: Button {
                flat: true
                implicitHeight: root.scaled(28)
                leftPadding: root.scaled(7)
                rightPadding: root.scaled(7)
                topPadding: 0
                bottomPadding: 0
                font.family: Qt.application.font.family
                font.pixelSize: root.scaled(12)
                palette.buttonText: root.textColor
                focusPolicy: Qt.NoFocus
            }

            ToolText {
                objectName: "pdfPageLabel"
                text: document.pageCount > 0 ? (root.currentPage + 1) + " / " + document.pageCount : ""
                rightPadding: root.scaled(8)
            }
            ToolButton { text: "−"; onClicked: root.zoomBy(0.8) }
            ToolButton {
                // Shows the zoom; a click fits the page to the width again.
                text: Math.round(root.zoom * 100) + "%"
                ToolTip.visible: hovered
                ToolTip.text: backend.keys("Fit to width (Ctrl+0)")
                onClicked: root.zoom = 1
            }
            ToolButton { text: "+"; onClicked: root.zoomBy(1.25) }
            ToolButton {
                text: "Open"
                ToolTip.visible: hovered
                ToolTip.text: "Open in the desktop's PDF viewer"
                onClicked: Qt.openUrlExternally(vault.urlForPath(root.path))
            }
            ToolButton {
                objectName: "pdfCloseButton"
                text: "✕"
                onClicked: root.closeRequested()
            }
        }
    }

    // ── Pages ───────────────────────────────────────────────────────────────
    ListView {
        id: pages
        objectName: "pdfPages"
        anchors.top: toolbar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: document.pageCount
        spacing: root.scaled(16)
        topMargin: root.scaled(16)
        bottomMargin: root.scaled(24)
        contentWidth: Math.max(width, root.pageWidth + root.scaled(48))
        flickableDirection: Flickable.AutoFlickIfNeeded
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: 2000
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

        delegate: Item {
            required property int index
            readonly property var pageSize: document.pageSizes[index] || ({ width: 612, height: 792 })
            width: pages.contentWidth
            height: Math.round(root.pageWidth * pageSize.height / pageSize.width)

            Rectangle {
                id: paper
                x: Math.round((parent.width - width) / 2)
                width: Math.round(root.pageWidth)
                height: parent.height
                color: "white"
                border.color: root.borderColor

                Image {
                    anchors.fill: parent
                    asynchronous: true
                    cache: false
                    retainWhileLoading: true
                    smooth: true
                    fillMode: Image.Stretch
                    source: root.renderWidth > 0
                        ? document.pageSource(index, Math.round(root.renderWidth * Screen.devicePixelRatio)) : ""
                }
            }
        }
    }

    Label {
        anchors.centerIn: pages
        visible: document.error.length > 0
        text: document.error
        color: root.mutedColor
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(13)
    }
}

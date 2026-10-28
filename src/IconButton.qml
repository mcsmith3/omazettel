import QtQuick
import QtQuick.Controls
import QtQuick.Window

// Small line icons drawn on a canvas, in the style of FooterIconButton, for
// the sidebar and other OmaZettel chrome.
Item {
    id: control

    property string iconName
    property color iconColor: "#666666"
    property color hoverColor: iconColor
    property string tooltip
    property real textScale: 1
    readonly property int iconSize: Math.max(12, Math.round(16 * textScale))

    signal clicked()

    width: iconSize
    height: iconSize

    ToolTip.visible: hitArea.containsMouse && tooltip.length > 0
    ToolTip.text: tooltip
    ToolTip.delay: 500

    Canvas {
        id: iconCanvas

        readonly property real dpr: Screen.devicePixelRatio
        readonly property color strokeColor: hitArea.containsMouse ? control.hoverColor : control.iconColor
        width: control.width * dpr
        height: control.height * dpr
        transformOrigin: Item.TopLeft
        scale: 1 / dpr
        onDprChanged: requestPaint()
        onStrokeColorChanged: requestPaint()

        onPaint: {
            var ctx = getContext("2d");
            var unit = control.width / 16;
            ctx.setTransform(dpr * unit, 0, 0, dpr * unit, 0, 0);
            ctx.clearRect(0, 0, 16, 16);
            ctx.strokeStyle = strokeColor;
            ctx.fillStyle = strokeColor;
            ctx.lineWidth = 1.4;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.beginPath();
            var name = control.iconName;
            if (name === "dashboard") {
                ctx.rect(2.5, 2.5, 4.5, 5.5);
                ctx.rect(9, 2.5, 4.5, 3);
                ctx.rect(9, 7.5, 4.5, 6);
                ctx.rect(2.5, 10, 4.5, 3.5);
            } else if (name === "note-plus") {
                ctx.moveTo(9.5, 2.5); ctx.lineTo(3.5, 2.5); ctx.lineTo(3.5, 13.5);
                ctx.lineTo(8, 13.5);
                ctx.moveTo(9.5, 2.5); ctx.lineTo(12.5, 5.5); ctx.lineTo(12.5, 8);
                ctx.moveTo(12, 10); ctx.lineTo(12, 15);
                ctx.moveTo(9.5, 12.5); ctx.lineTo(14.5, 12.5);
            } else if (name === "folder-plus") {
                ctx.moveTo(8, 13); ctx.lineTo(1.5, 13); ctx.lineTo(1.5, 3.5);
                ctx.lineTo(5.5, 3.5); ctx.lineTo(7.5, 5.5); ctx.lineTo(13.5, 5.5);
                ctx.lineTo(13.5, 8);
                ctx.moveTo(12, 10); ctx.lineTo(12, 15);
                ctx.moveTo(9.5, 12.5); ctx.lineTo(14.5, 12.5);
            } else if (name === "more") {
                ctx.stroke();
                [3.5, 8, 12.5].forEach(function(x) {
                    ctx.beginPath(); ctx.arc(x, 8, 1.2, 0, Math.PI * 2); ctx.fill();
                });
                return;
            } else if (name === "sync") {
                // A circle of two arrows.
                ctx.arc(8, 8, 5, Math.PI * 1.15, Math.PI * 1.95);
                ctx.moveTo(12.6, 3.6); ctx.lineTo(12.9, 6.4); ctx.lineTo(10.1, 6.2);
                ctx.moveTo(13, 8);
                ctx.arc(8, 8, 5, Math.PI * 0.15, Math.PI * 0.95, false);
                ctx.moveTo(3.4, 12.4); ctx.lineTo(3.1, 9.6); ctx.lineTo(5.9, 9.8);
            } else if (name === "chevron-right") {
                ctx.moveTo(6, 4); ctx.lineTo(10, 8); ctx.lineTo(6, 12);
            } else if (name === "chevron-down") {
                ctx.moveTo(4, 6); ctx.lineTo(8, 10); ctx.lineTo(12, 6);
            } else if (name === "sidebar") {
                ctx.rect(2.5, 3, 11, 10);
                ctx.moveTo(6.5, 3); ctx.lineTo(6.5, 13);
            } else if (name === "folder") {
                ctx.moveTo(1.5, 13); ctx.lineTo(1.5, 3.5); ctx.lineTo(5.5, 3.5);
                ctx.lineTo(7.5, 5.5); ctx.lineTo(14.5, 5.5); ctx.lineTo(14.5, 13);
                ctx.closePath();
            }
            ctx.stroke();
        }

        Connections {
            target: control
            function onIconNameChanged() { iconCanvas.requestPaint(); }
            function onWidthChanged() { iconCanvas.requestPaint(); }
        }
    }

    MouseArea {
        id: hitArea
        anchors.centerIn: parent
        width: control.width + 12
        height: control.height + 12
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: control.clicked()
    }
}

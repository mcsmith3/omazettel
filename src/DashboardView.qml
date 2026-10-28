import QtQuick
import QtQuick.Controls

// The dashboard, shown in place of the editor: how much was written each day
// (a year of days, like a contribution graph), open tasks across the notes,
// projects, notes that need a look, top tags and recent notes. The numbers
// come from `dashboard` (src/dashboard.cpp), which the command line shares.
Rectangle {
    id: root
    objectName: "dashboardView"

    property bool open: false
    property bool darkMode: true
    property real textScale: 1
    property color pageColor: "#101010"
    property color textColor: "#d0d0d0"
    property color mutedColor: "#909191"
    property color accentColor: "#428bca"
    property color raisedColor: pageColor
    property color fieldColor: pageColor
    property color borderColor: Qt.rgba(mutedColor.r, mutedColor.g, mutedColor.b, 0.25)

    signal closeRequested()
    // path is relative to the notes folder.
    signal noteRequested(string path)
    signal taskRequested(var task)
    signal taskCompleted(var task)
    signal tagRequested(string tag)

    readonly property var info: dashboard.data
    readonly property bool ready: info.summary !== undefined
    readonly property var summary: info.summary || ({})

    function scaled(value) { return Math.round(value * textScale); }
    function plural(count, one, many) { return count + " " + (count === 1 ? one : (many || one + "s")); }

    color: pageColor
    visible: open

    onVisibleChanged: if (visible) keyCatcher.forceActiveFocus()

    // Tasks ticked here hide at once, before the notes are read again.
    property var ticked: ({})
    Connections {
        target: dashboard
        function onDataChanged() { root.ticked = ({}); }
    }

    Item {
        id: keyCatcher
        focus: root.visible
        Keys.onEscapePressed: root.closeRequested()
    }

    // ── Dates ───────────────────────────────────────────────────────────────
    function pad(n) { return n < 10 ? "0" + n : "" + n; }
    function isoDate(d) { return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()); }
    function parseDate(text) {
        var parts = (text || "").split("-");
        return parts.length === 3 ? new Date(+parts[0], +parts[1] - 1, +parts[2]) : new Date();
    }
    function addDays(d, days) { var copy = new Date(d.getTime()); copy.setDate(copy.getDate() + days); return copy; }
    function longDate(text) { return parseDate(text).toLocaleDateString(Qt.locale(), "dddd d MMMM yyyy"); }
    function shortDate(text) { return text ? parseDate(text).toLocaleDateString(Qt.locale(), "d MMM") : ""; }

    readonly property var byDate: {
        var map = {};
        (info.activity || []).forEach(function(day) { map[day.date] = day; });
        return map;
    }
    readonly property int maxCount: {
        var most = 0;
        (info.activity || []).forEach(function(day) { most = Math.max(most, day.created + day.edited); });
        return most;
    }
    readonly property int weekStart: Qt.locale().firstDayOfWeek % 7
    readonly property date today: parseDate(info.today)
    // The first cell: the start of the week holding the first day shown.
    readonly property date gridStart: {
        var first = parseDate(info.from);
        return addDays(first, -((first.getDay() - weekStart + 7) % 7));
    }
    readonly property int weeks: ready ? Math.ceil((Math.round((today - gridStart) / 86400000) + 1) / 7) : 53
    property string selectedDay: ""
    property string hoveredDay: ""

    function level(count) { return count <= 0 || maxCount <= 0 ? 0 : Math.max(1, Math.ceil(count / maxCount * 4)); }
    function levelColor(lvl) {
        if (lvl === 0)
            return Qt.rgba(mutedColor.r, mutedColor.g, mutedColor.b, darkMode ? 0.14 : 0.18);
        return Qt.rgba(accentColor.r, accentColor.g, accentColor.b, [0, 0.3, 0.52, 0.76, 1][lvl]);
    }
    function dayText(iso) {
        var day = byDate[iso];
        if (!day)
            return shortDate(iso) + ": nothing written";
        var parts = [];
        if (day.created > 0) parts.push(day.created + " new");
        if (day.edited > 0) parts.push(day.edited + " changed");
        return shortDate(iso) + ": " + parts.join(", ");
    }

    // ── Tasks ───────────────────────────────────────────────────────────────
    // Task text as it reads, without Markdown marks: [[note|label]] shows
    // label, [text](url) shows text, and **, __ and ` go.
    function plainText(text) {
        return text.replace(/\[\[([^\]|]*)\|([^\]]*)\]\]/g, "$2")
                   .replace(/\[\[([^\]]*)\]\]/g, "$1")
                   .replace(/\[([^\]]*)\]\([^)]*\)/g, "$1")
                   .replace(/(\*\*|__)(.+?)\1/g, "$2")
                   .replace(/`([^`]*)`/g, "$1");
    }
    property string taskFilter: ""
    property var expanded: ({})
    readonly property var taskGroups: {
        var query = taskFilter.trim().toLowerCase();
        var groups = [];
        var byPath = {};
        (info.tasks || []).forEach(function(task) {
            if (ticked[task.path + ":" + task.line])
                return;
            if (query.length > 0
                    && (task.text + " " + task.title + " " + task.project).toLowerCase().indexOf(query) < 0)
                return;
            var group = byPath[task.path];
            if (!group) {
                group = { path: task.path, title: task.title, project: task.project, tasks: [] };
                byPath[task.path] = group;
                groups.push(group);
            }
            group.tasks.push(task);
        });
        return groups;
    }
    readonly property int shownTasks: {
        var count = 0;
        taskGroups.forEach(function(group) { count += group.tasks.length; });
        return count;
    }

    // ── Pieces ──────────────────────────────────────────────────────────────
    component Heading: Label {
        color: root.textColor
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(15)
        font.bold: true
        topPadding: root.scaled(8)
    }
    component Muted: Label {
        color: root.mutedColor
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(12)
        wrapMode: Text.Wrap
    }
    // A line of text that opens something when clicked.
    component LinkText: Label {
        id: link
        signal activated()
        property color baseColor: root.textColor
        color: !enabled ? root.mutedColor : linkArea.containsMouse ? root.accentColor : baseColor
        font.family: Qt.application.font.family
        font.pixelSize: root.scaled(13)
        elide: Text.ElideRight
        MouseArea {
            id: linkArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: link.activated()
        }
    }
    component Card: Rectangle {
        color: root.raisedColor
        border.color: root.borderColor
        radius: 0
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
            anchors.left: parent.left
            anchors.leftMargin: root.scaled(16)
            anchors.right: tools.left
            anchors.verticalCenter: parent.verticalCenter
            text: "Dashboard" + (vault.isOpen ? "  ·  " + vault.rootName : "")
            elide: Text.ElideRight
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

            Muted {
                anchors.verticalCenter: parent.verticalCenter
                visible: dashboard.loading
                text: "Reading notes…"
                rightPadding: root.scaled(6)
            }
            ToolButton {
                objectName: "dashboardRefreshButton"
                text: "Refresh"
                onClicked: dashboard.refresh()
            }
            ToolButton {
                objectName: "dashboardCloseButton"
                text: "✕"
                ToolTip.visible: hovered
                ToolTip.text: "Back to the note (Esc)"
                onClicked: root.closeRequested()
            }
        }
    }

    Flickable {
        id: flick
        anchors.top: toolbar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        contentWidth: width
        contentHeight: page.height + root.scaled(48)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
            id: page
            x: Math.round((flick.width - width) / 2)
            y: root.scaled(20)
            width: Math.min(flick.width - root.scaled(48), root.scaled(920))
            spacing: root.scaled(14)

            Muted {
                width: page.width
                visible: !root.ready
                text: !vault.isOpen ? "Choose a notes folder to see its dashboard."
                                    : "Reading your notes…"
            }

            // ── Summary ─────────────────────────────────────────────────────
            Flow {
                id: tiles
                width: page.width
                visible: root.ready
                spacing: root.scaled(10)
                readonly property int columns: page.width > root.scaled(560) ? 4 : 2
                readonly property real tileWidth: Math.floor((width - spacing * (columns - 1)) / columns)

                Repeater {
                    model: [
                        { value: root.summary.notes, label: "notes" },
                        { value: root.summary.openTasks, label: root.summary.openTasks === 1 ? "open task" : "open tasks" },
                        { value: root.summary.streak, label: root.summary.streak === 1 ? "day streak" : "days in a row" },
                        { value: root.summary.createdThisWeek, label: "new this week"
                              + (root.summary.editedThisWeek > 0 ? " · " + root.summary.editedThisWeek + " changed" : "") }
                    ]
                    delegate: Card {
                        required property var modelData
                        width: tiles.tileWidth
                        height: tileColumn.implicitHeight + root.scaled(20)
                        Column {
                            id: tileColumn
                            x: root.scaled(12)
                            y: root.scaled(10)
                            width: parent.width - root.scaled(24)
                            Label {
                                text: modelData.value === undefined ? "–" : modelData.value
                                color: root.textColor
                                font.family: Qt.application.font.family
                                font.pixelSize: root.scaled(24)
                                font.bold: true
                            }
                            Muted { width: parent.width; text: modelData.label }
                        }
                    }
                }
            }

            // ── Activity ────────────────────────────────────────────────────
            Item {
                width: page.width
                height: activityHeading.height
                visible: root.ready
                Heading { id: activityHeading; text: "Activity" }
                Row {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: root.scaled(3)
                    spacing: root.scaled(3)
                    Muted { text: "Less"; rightPadding: root.scaled(3) }
                    Repeater {
                        model: 5
                        delegate: Rectangle {
                            required property int index
                            anchors.verticalCenter: parent.verticalCenter
                            width: root.scaled(10); height: width
                            radius: root.scaled(2)
                            color: root.levelColor(index)
                        }
                    }
                    Muted { text: "More"; leftPadding: root.scaled(3) }
                }
            }

            Item {
                id: heatmap
                objectName: "dashboardHeatmap"
                visible: root.ready
                width: page.width
                height: monthRow.height + root.scaled(4) + 7 * step
                readonly property int labelWidth: root.scaled(30)
                readonly property real step: Math.max(root.scaled(8), Math.min(root.scaled(17),
                                                     Math.floor((width - labelWidth) / Math.max(1, root.weeks))))
                readonly property real cell: step - Math.max(2, Math.round(step / 6))

                // Month names over the week a month starts in.
                Item {
                    id: monthRow
                    x: heatmap.labelWidth
                    height: root.scaled(16)
                    Repeater {
                        model: root.weeks
                        delegate: Muted {
                            required property int index
                            readonly property date first: root.addDays(root.gridStart, index * 7)
                            readonly property date last: root.addDays(first, 6)
                            readonly property bool holdsFirst: first.getDate() === 1 || last.getMonth() !== first.getMonth()
                            x: index * heatmap.step
                            font.pixelSize: root.scaled(10)
                            wrapMode: Text.NoWrap
                            // Over the week a month starts in, and over the
                            // first week when its month has room before the next.
                            visible: holdsFirst || (index === 0 && first.getDate() <= 14)
                            text: (holdsFirst ? last : first).toLocaleDateString(Qt.locale(), "MMM")
                        }
                    }
                }

                Repeater {
                    model: [1, 3, 5]
                    delegate: Muted {
                        required property var modelData
                        y: monthRow.height + root.scaled(4) + modelData * heatmap.step + (heatmap.cell - height) / 2
                        font.pixelSize: root.scaled(10)
                        wrapMode: Text.NoWrap
                        text: Qt.locale().dayName((root.weekStart + modelData) % 7, Locale.ShortFormat)
                    }
                }

                Item {
                    id: grid
                    x: heatmap.labelWidth
                    y: monthRow.height + root.scaled(4)
                    width: root.weeks * heatmap.step
                    height: 7 * heatmap.step

                    Repeater {
                        model: root.weeks * 7
                        delegate: Rectangle {
                            required property int index
                            readonly property int week: Math.floor(index / 7)
                            readonly property int weekday: index % 7
                            readonly property date day: root.addDays(root.gridStart, week * 7 + weekday)
                            readonly property string iso: root.isoDate(day)
                            readonly property var activity: root.byDate[iso]
                            visible: iso >= (root.info.from || "") && iso <= (root.info.today || "")
                            x: week * heatmap.step
                            y: weekday * heatmap.step
                            width: heatmap.cell
                            height: heatmap.cell
                            radius: Math.max(1, Math.round(heatmap.cell / 5))
                            color: root.levelColor(root.level(activity ? activity.created + activity.edited : 0))
                            border.width: iso === root.selectedDay || iso === root.info.today ? 1 : 0
                            border.color: iso === root.selectedDay ? root.textColor : root.mutedColor
                        }
                    }

                    MouseArea {
                        id: gridArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        function dayAt(x, y) {
                            var week = Math.floor(x / heatmap.step);
                            var weekday = Math.floor(y / heatmap.step);
                            if (week < 0 || week >= root.weeks || weekday < 0 || weekday > 6)
                                return "";
                            var iso = root.isoDate(root.addDays(root.gridStart, week * 7 + weekday));
                            return iso >= root.info.from && iso <= root.info.today ? iso : "";
                        }
                        onPositionChanged: function(mouse) { root.hoveredDay = dayAt(mouse.x, mouse.y); }
                        onExited: root.hoveredDay = ""
                        onClicked: function(mouse) {
                            var iso = dayAt(mouse.x, mouse.y);
                            root.selectedDay = iso === root.selectedDay ? "" : iso;
                        }
                        ToolTip.visible: containsMouse && root.hoveredDay.length > 0
                        ToolTip.text: root.dayText(root.hoveredDay)
                        ToolTip.delay: 0
                    }
                }
            }

            Muted {
                width: page.width
                visible: root.ready && root.selectedDay.length === 0
                text: (root.info.history === "git"
                       ? "From the notes folder's version history. "
                       : "From file dates; turn on Sync and history for a fuller record. ")
                      + "Click a day to see what you wrote."
            }

            // The notes of the day clicked.
            Column {
                objectName: "dashboardDayNotes"
                width: page.width
                visible: root.selectedDay.length > 0
                spacing: root.scaled(4)
                readonly property var day: root.byDate[root.selectedDay]
                Label {
                    text: root.selectedDay.length > 0 ? root.longDate(root.selectedDay) : ""
                    color: root.textColor
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(13)
                    font.bold: true
                }
                Muted { visible: !parent.day; text: "Nothing written that day." }
                Repeater {
                    model: parent.day ? parent.day.notes : []
                    delegate: Row {
                        required property var modelData
                        spacing: root.scaled(8)
                        width: page.width
                        Muted {
                            width: root.scaled(56)
                            text: modelData.created ? "new" : "changed"
                            color: modelData.created ? root.accentColor : root.mutedColor
                        }
                        LinkText {
                            width: parent.width - root.scaled(64)
                            text: modelData.title + (modelData.exists ? "" : " (since moved or deleted)")
                            enabled: modelData.exists
                            onActivated: root.noteRequested(modelData.path)
                        }
                    }
                }
            }

            // ── Projects ────────────────────────────────────────────────────
            Heading {
                visible: root.ready && (root.info.projects || []).length > 0
                text: root.info.projectsAre === "projects" ? "Projects" : "Folders"
            }
            Flow {
                id: projectCards
                width: page.width
                spacing: root.scaled(10)
                visible: root.ready
                readonly property int columns: Math.max(1, Math.floor((width + spacing) / (root.scaled(210) + spacing)))
                readonly property real cardWidth: Math.floor((width - spacing * (columns - 1)) / columns)
                Repeater {
                    model: root.info.projects || []
                    delegate: Card {
                        id: projectCard
                        required property var modelData
                        width: projectCards.cardWidth
                        height: projectColumn.implicitHeight + root.scaled(20)
                        border.color: cardArea.containsMouse && modelData.map ? root.accentColor : root.borderColor
                        Column {
                            id: projectColumn
                            x: root.scaled(12)
                            y: root.scaled(10)
                            width: parent.width - root.scaled(24)
                            spacing: root.scaled(2)
                            Label {
                                width: parent.width
                                text: projectCard.modelData.name
                                elide: Text.ElideRight
                                color: root.textColor
                                font.family: Qt.application.font.family
                                font.pixelSize: root.scaled(14)
                                font.bold: true
                            }
                            Muted {
                                width: parent.width
                                text: root.plural(projectCard.modelData.notes, "note") + " · "
                                      + root.plural(projectCard.modelData.openTasks, "open task")
                            }
                            Muted {
                                width: parent.width
                                text: "Last changed " + root.shortDate(projectCard.modelData.lastActivity)
                            }
                        }
                        MouseArea {
                            id: cardArea
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: projectCard.modelData.map.length > 0
                            cursorShape: Qt.PointingHandCursor
                            ToolTip.visible: containsMouse
                            ToolTip.text: "Open the map of content"
                            ToolTip.delay: 600
                            onClicked: root.noteRequested(projectCard.modelData.map)
                        }
                    }
                }
            }

            // ── Open tasks ──────────────────────────────────────────────────
            Item {
                width: page.width
                height: Math.max(tasksHeading.height, taskFilterField.height)
                visible: root.ready
                Heading {
                    id: tasksHeading
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Open tasks" + (root.ready ? "  " + (root.taskFilter.length > 0
                          ? root.shownTasks + " of " + (root.info.tasks || []).length : (root.info.tasks || []).length) : "")
                }
                TextField {
                    id: taskFilterField
                    objectName: "dashboardTaskFilter"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(root.scaled(260), parent.width / 2)
                    placeholderText: "Filter: a name, word or project"
                    font.family: Qt.application.font.family
                    font.pixelSize: root.scaled(12)
                    color: root.textColor
                    placeholderTextColor: root.mutedColor
                    background: Rectangle { color: root.fieldColor; border.color: root.borderColor }
                    onTextChanged: root.taskFilter = text
                    Keys.onEscapePressed: { if (text.length > 0) text = ""; else root.closeRequested(); }
                }
            }

            Muted {
                width: page.width
                visible: root.ready && root.taskGroups.length === 0
                text: root.taskFilter.length > 0 ? "No open tasks match."
                      : "No open tasks. Tasks are list items like - [ ] Send the deck, in any note."
            }

            Column {
                objectName: "dashboardTasks"
                width: page.width
                spacing: root.scaled(12)
                visible: root.ready

                Repeater {
                    model: root.taskGroups
                    delegate: Column {
                        id: group
                        required property var modelData
                        readonly property bool showAll: root.expanded[modelData.path] === true || root.taskFilter.length > 0
                        readonly property int limit: 3
                        width: page.width
                        spacing: root.scaled(3)

                        Item {
                            width: parent.width
                            height: groupTitle.height
                            LinkText {
                                id: groupTitle
                                width: Math.min(implicitWidth, parent.width - projectLabel.width - root.scaled(8))
                                text: group.modelData.title
                                font.bold: true
                                onActivated: root.noteRequested(group.modelData.path)
                            }
                            Muted {
                                id: projectLabel
                                x: groupTitle.width + root.scaled(8)
                                anchors.baseline: groupTitle.baseline
                                text: group.modelData.project
                                wrapMode: Text.NoWrap
                            }
                        }

                        Repeater {
                            model: group.showAll ? group.modelData.tasks : group.modelData.tasks.slice(0, group.limit)
                            delegate: Item {
                                id: taskRow
                                required property var modelData
                                width: group.width
                                height: Math.max(taskText.implicitHeight, box.height + root.scaled(4))

                                Rectangle {
                                    id: box
                                    objectName: "dashboardTaskBox"
                                    x: root.scaled(2)
                                    y: root.scaled(3)
                                    width: root.scaled(14)
                                    height: width
                                    color: boxArea.containsMouse ? Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.25) : "transparent"
                                    border.color: boxArea.containsMouse ? root.accentColor : root.mutedColor
                                    border.width: 1
                                    radius: root.scaled(2)
                                    Label {
                                        anchors.centerIn: parent
                                        visible: boxArea.containsMouse
                                        text: "✓"
                                        color: root.accentColor
                                        font.pixelSize: root.scaled(11)
                                    }
                                    MouseArea {
                                        id: boxArea
                                        anchors.fill: parent
                                        anchors.margins: -root.scaled(4)
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        ToolTip.visible: containsMouse
                                        ToolTip.text: "Mark done"
                                        ToolTip.delay: 600
                                        onClicked: {
                                            var ticked = Object.assign({}, root.ticked);
                                            ticked[taskRow.modelData.path + ":" + taskRow.modelData.line] = true;
                                            root.ticked = ticked;
                                            root.taskCompleted(taskRow.modelData);
                                        }
                                    }
                                }
                                Label {
                                    id: taskText
                                    x: root.scaled(26)
                                    width: parent.width - x
                                    text: root.plainText(taskRow.modelData.text)
                                    wrapMode: Text.Wrap
                                    textFormat: Text.PlainText
                                    color: textArea.containsMouse ? root.accentColor : root.textColor
                                    font.family: Qt.application.font.family
                                    font.pixelSize: root.scaled(13)
                                    MouseArea {
                                        id: textArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.taskRequested(taskRow.modelData)
                                    }
                                }
                            }
                        }

                        LinkText {
                            visible: !group.showAll && group.modelData.tasks.length > group.limit
                            x: root.scaled(26)
                            text: "Show " + (group.modelData.tasks.length - group.limit) + " more"
                            baseColor: root.mutedColor
                            font.pixelSize: root.scaled(12)
                            onActivated: {
                                var expanded = Object.assign({}, root.expanded);
                                expanded[group.modelData.path] = true;
                                root.expanded = expanded;
                            }
                        }
                    }
                }
            }

            // ── Needs a look ────────────────────────────────────────────────
            Heading { visible: root.ready; text: "Needs a look" }
            Column {
                width: page.width
                visible: root.ready
                spacing: root.scaled(6)
                Repeater {
                    model: root.ready ? [
                        { key: "broken", label: "Links to notes that don't exist", items: root.info.attention.broken },
                        { key: "unlinked", label: "Not linked from another note (maps of content don't count)", items: root.info.attention.unlinked },
                        { key: "empty", label: "Empty notes", items: root.info.attention.empty }
                    ] : []
                    delegate: Column {
                        id: attention
                        required property var modelData
                        property bool open: false
                        width: page.width
                        spacing: root.scaled(3)
                        LinkText {
                            objectName: "dashboardAttention_" + attention.modelData.key
                            width: parent.width
                            enabled: attention.modelData.items.length > 0
                            text: (attention.modelData.items.length > 0 ? (attention.open ? "−  " : "+  ") : "✓  ")
                                  + attention.modelData.label + "  " + attention.modelData.items.length
                            onActivated: attention.open = !attention.open
                        }
                        Repeater {
                            model: attention.open ? attention.modelData.items : []
                            delegate: LinkText {
                                required property var modelData
                                x: root.scaled(18)
                                width: page.width - x
                                text: modelData.title + (modelData.target ? "  →  [[" + modelData.target + "]]" : "")
                                      + "   " + modelData.path.split("/").slice(0, -1).join("/")
                                onActivated: root.noteRequested(modelData.path)
                            }
                        }
                    }
                }
            }

            // ── Tags and recent notes ───────────────────────────────────────
            Flow {
                width: page.width
                spacing: root.scaled(24)
                visible: root.ready
                readonly property real columnWidth: page.width > root.scaled(600) ? (page.width - spacing) / 2 : page.width

                Column {
                    width: parent.columnWidth
                    spacing: root.scaled(8)
                    Heading { text: "Top tags" }
                    Muted { visible: (root.info.tags || []).length === 0; text: "No tags yet." }
                    Flow {
                        width: parent.width
                        spacing: root.scaled(6)
                        Repeater {
                            model: root.info.tags || []
                            delegate: Rectangle {
                                required property var modelData
                                width: tagLabel.implicitWidth + root.scaled(16)
                                height: tagLabel.implicitHeight + root.scaled(8)
                                color: tagArea.containsMouse ? Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.2) : root.raisedColor
                                border.color: root.borderColor
                                Label {
                                    id: tagLabel
                                    anchors.centerIn: parent
                                    text: "#" + modelData.tag + "  " + modelData.count
                                    color: root.textColor
                                    font.family: Qt.application.font.family
                                    font.pixelSize: root.scaled(12)
                                }
                                MouseArea {
                                    id: tagArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.tagRequested(modelData.tag)
                                }
                            }
                        }
                    }
                }

                Column {
                    width: parent.columnWidth
                    spacing: root.scaled(4)
                    Heading { text: "Recently changed"; bottomPadding: root.scaled(4) }
                    Repeater {
                        model: root.info.recent || []
                        delegate: Row {
                            required property var modelData
                            width: parent.width
                            spacing: root.scaled(8)
                            LinkText {
                                width: parent.width - when.width - root.scaled(8)
                                text: modelData.title
                                onActivated: root.noteRequested(modelData.path)
                            }
                            Muted {
                                id: when
                                wrapMode: Text.NoWrap
                                text: root.shortDate(modelData.modified.slice(0, 10))
                            }
                        }
                    }
                }
            }
        }
    }
}

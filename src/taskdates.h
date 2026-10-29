#pragma once

// Ticked tasks carry the day they were done: "- [x] Send the deck (10/29/2026)".
// Ticking a task adds the date at the end of its line, and unticking it takes
// the date off again. The dashboard and the editor both use this.

#include <QDate>
#include <QRegularExpression>
#include <QString>

namespace TaskDates {

// A list item task: the box's mark is captured(1), the text after it captured(2).
inline const QRegularExpression &taskLine() {
    static const QRegularExpression re(
        QStringLiteral("^\\s*(?:[-*+]|\\d+[.)])\\s+\\[([ xX])\\](.*)$"));
    return re;
}

// " (MM/dd/yyyy)" at the end of a line, with any spaces after it.
inline const QRegularExpression &dateStamp() {
    static const QRegularExpression re(QStringLiteral(" ?\\(\\d{2}/\\d{2}/\\d{4}\\)\\s*$"));
    return re;
}

inline QString stampFor(const QDate &day) {
    return QStringLiteral(" (") + day.toString(QStringLiteral("MM/dd/yyyy")) + QLatin1Char(')');
}

// What brings a line's date in line with its box: remove `remove`
// characters at `at`, then insert `insert` there. `at` is -1 for nothing.
struct Edit {
    int at = -1;
    int remove = 0;
    QString insert;
};

// Where the box's mark is in a task line, or -1.
inline int markPosition(const QString &line) {
    const QRegularExpressionMatch match = taskLine().match(line);
    return match.hasMatch() ? int(match.capturedStart(1)) : -1;
}

// For a line whose box was just changed: a done task with text gets today's
// date (unless it has one), and an open task loses its date.
inline Edit forBox(const QString &line, const QDate &today) {
    Edit edit;
    const QRegularExpressionMatch task = taskLine().match(line);
    if (!task.hasMatch())
        return edit;
    const QRegularExpressionMatch stamp = dateStamp().match(line);
    if (task.captured(1) != QLatin1String(" ")) {
        if (stamp.hasMatch() || task.captured(2).trimmed().isEmpty())
            return edit;
        int end = int(line.size());
        while (end > 0 && line.at(end - 1).isSpace())
            --end;
        edit.at = end;
        edit.insert = stampFor(today);
    } else if (stamp.hasMatch()) {
        edit.at = int(stamp.capturedStart());
        edit.remove = int(stamp.capturedLength());
    }
    return edit;
}

inline QString applied(const QString &line, const Edit &edit) {
    if (edit.at < 0)
        return line;
    QString result = line;
    result.replace(edit.at, edit.remove, edit.insert);
    return result;
}

} // namespace TaskDates

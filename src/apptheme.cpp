#include "apptheme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace AppTheme {

namespace {
QString readText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

QColor mix(const QColor &a, const QColor &b, double amount) {
    amount = qBound(0.0, amount, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount,
                            a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount);
}
}

QString themeDir() {
    return QDir::homePath() + QStringLiteral("/.local/state/omarchy/current/theme");
}

QString userTemplatePath() {
    return QDir::homePath() + QStringLiteral("/.config/omarchy/themed/omazettel.toml.tpl");
}

QString builtinTemplate() {
    return readText(QStringLiteral(":/themed/omazettel.toml.tpl"));
}

QStringList names() {
    return {QStringLiteral("editor.background"), QStringLiteral("editor.foreground"),
            QStringLiteral("editor.cursor"), QStringLiteral("editor.selection"),
            QStringLiteral("editor.selection_foreground"), QStringLiteral("editor.heading"),
            QStringLiteral("editor.bold"), QStringLiteral("editor.italic"),
            QStringLiteral("editor.link"), QStringLiteral("editor.wikilink"),
            QStringLiteral("editor.tag"), QStringLiteral("editor.citation"),
            QStringLiteral("editor.code"), QStringLiteral("editor.code_background"),
            QStringLiteral("editor.quote"), QStringLiteral("editor.marker"),
            QStringLiteral("editor.search"), QStringLiteral("editor.search_current"),
            QStringLiteral("ui.accent"), QStringLiteral("ui.panel"), QStringLiteral("ui.raised"),
            QStringLiteral("ui.field"), QStringLiteral("ui.border"), QStringLiteral("ui.muted")};
}

QHash<QString, QString> themeColors(const QString &colorsFile) {
    QHash<QString, QString> colors;
    // Omarchy's own resolver knows every alias and derived shade.
    const QString resolver = QStandardPaths::findExecutable(QStringLiteral("omarchy-theme-color"));
    if (!resolver.isEmpty() && QFileInfo::exists(colorsFile)) {
        QProcess process;
        process.start(resolver, {QStringLiteral("--file"), colorsFile, QStringLiteral("--all")});
        if (process.waitForFinished(3000) && process.exitCode() == 0) {
            const QStringList lines = QString::fromUtf8(process.readAllStandardOutput()).split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const int tab = int(line.indexOf(QLatin1Char('\t')));
                if (tab > 0)
                    colors.insert(line.left(tab).trimmed(), line.mid(tab + 1).trimmed());
            }
        }
    }
    if (!colors.isEmpty())
        return colors;

    // Without it: the file's own keys, and the few aliases templates use most.
    // A value may start with "#" (a colour); a "#" after it starts a comment.
    static const QRegularExpression lineRe(QStringLiteral("^\\s*([A-Za-z0-9_]+)\\s*=\\s*\"?(#?[^\"#\\n]+)\"?"),
                                           QRegularExpression::MultilineOption);
    auto it = lineRe.globalMatch(readText(colorsFile));
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        colors.insert(match.captured(1), match.captured(2).trimmed());
    }
    const auto alias = [&colors](const QString &name, const QString &from) {
        if (!colors.contains(name) && colors.contains(from))
            colors.insert(name, colors.value(from));
    };
    alias(QStringLiteral("selection_background"), QStringLiteral("selection"));
    alias(QStringLiteral("cursor"), QStringLiteral("foreground"));
    alias(QStringLiteral("selection_foreground"), QStringLiteral("foreground"));
    alias(QStringLiteral("bright_foreground"), QStringLiteral("foreground"));
    alias(QStringLiteral("light_foreground"), QStringLiteral("foreground"));
    alias(QStringLiteral("dark_foreground"), QStringLiteral("foreground"));
    alias(QStringLiteral("lighter_background"), QStringLiteral("background"));
    alias(QStringLiteral("dark_background"), QStringLiteral("background"));
    alias(QStringLiteral("orange"), QStringLiteral("yellow"));
    return colors;
}

QString render(const QString &templateText, const QHash<QString, QString> &colors) {
    static const QRegularExpression tokenRe(
        QStringLiteral("\\{\\{\\s*(?:(mix|mix_strip|mix_rgb)\\s+([A-Za-z0-9_]+)\\s+([A-Za-z0-9_]+)\\s+([0-9.]+)(%?)"
                       "|([A-Za-z0-9_]+?)(_strip|_rgb)?)\\s*\\}\\}"));
    QString out;
    int last = 0;
    auto it = tokenRe.globalMatch(templateText);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += templateText.mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        QString value;
        QString variant;
        if (!match.captured(1).isEmpty()) {
            const QColor a(colors.value(match.captured(2)));
            const QColor b(colors.value(match.captured(3)));
            double amount = match.captured(4).toDouble();
            if (!match.captured(5).isEmpty() || amount > 1)
                amount /= 100.0;
            if (a.isValid() && b.isValid())
                value = mix(a, b, amount).name();
            variant = match.captured(1).mid(3);
        } else {
            QString name = match.captured(6);
            variant = match.captured(7);
            // "selection_background" is a name, not "selection" + a suffix.
            if (!variant.isEmpty() && colors.contains(name + variant)) {
                name += variant;
                variant.clear();
            }
            value = colors.value(name);
        }
        if (value.isEmpty()) {
            out += match.captured(0);
            continue;
        }
        if (variant == QLatin1String("_strip"))
            value.remove(QLatin1Char('#'));
        else if (variant == QLatin1String("_rgb")) {
            const QColor color(value);
            value = QStringLiteral("%1,%2,%3").arg(color.red()).arg(color.green()).arg(color.blue());
        }
        out += value;
    }
    out += templateText.mid(last);
    return out;
}

QHash<QString, QString> parse(const QString &toml) {
    QHash<QString, QString> colors;
    QString section;
    static const QRegularExpression sectionRe(QStringLiteral("^\\s*\\[([A-Za-z0-9_.-]+)\\]"));
    // Quoted as TOML wants, or bare (#rrggbb) as people write.
    static const QRegularExpression pairRe(QStringLiteral("^\\s*([A-Za-z0-9_-]+)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|(#?[^#\\s]+))"));
    for (const QString &line : toml.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch sectionMatch = sectionRe.match(line);
        if (sectionMatch.hasMatch()) {
            section = sectionMatch.captured(1);
            continue;
        }
        const QRegularExpressionMatch pair = pairRe.match(line);
        if (!pair.hasMatch())
            continue;
        const QString value = (pair.captured(2) + pair.captured(3) + pair.captured(4)).trimmed();
        const QColor color(value);
        if (!color.isValid())
            continue;
        colors.insert(section.isEmpty() ? pair.captured(1) : section + QLatin1Char('.') + pair.captured(1),
                      color.name());
    }
    return colors;
}

void complete(QHash<QString, QString> &colors, bool dark) {
    const auto get = [&colors](const QString &name) { return QColor(colors.value(name)); };
    const auto fill = [&colors](const QString &name, const QColor &value) {
        if (!QColor(colors.value(name)).isValid() && value.isValid())
            colors.insert(name, value.name());
    };
    fill(QStringLiteral("editor.background"), QColor(dark ? QStringLiteral("#101010") : QStringLiteral("#ffffff")));
    fill(QStringLiteral("editor.foreground"), QColor(dark ? QStringLiteral("#eeeeee") : QStringLiteral("#222324")));
    fill(QStringLiteral("ui.accent"), QColor(dark ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2")));
    const QColor background = get(QStringLiteral("editor.background"));
    const QColor foreground = get(QStringLiteral("editor.foreground"));
    const QColor accent = get(QStringLiteral("ui.accent"));

    fill(QStringLiteral("editor.cursor"), foreground);
    fill(QStringLiteral("editor.selection"), mix(background, accent, 0.4));
    fill(QStringLiteral("editor.selection_foreground"), foreground);
    fill(QStringLiteral("editor.heading"), foreground);
    fill(QStringLiteral("editor.bold"), foreground);
    fill(QStringLiteral("editor.italic"), foreground);
    fill(QStringLiteral("editor.link"), accent);
    fill(QStringLiteral("editor.wikilink"), get(QStringLiteral("editor.link")));
    fill(QStringLiteral("editor.tag"), accent);
    fill(QStringLiteral("editor.citation"), accent);
    fill(QStringLiteral("editor.code"), foreground);
    fill(QStringLiteral("editor.code_background"), mix(background, foreground, 0.06));
    fill(QStringLiteral("editor.marker"), QColor(dark ? QStringLiteral("#4f525a") : QStringLiteral("#aeb1b5")));
    fill(QStringLiteral("editor.quote"), get(QStringLiteral("editor.marker")));
    fill(QStringLiteral("editor.search"), QColor(dark ? QStringLiteral("#725b18") : QStringLiteral("#ffe58a")));
    fill(QStringLiteral("editor.search_current"), QColor(dark ? QStringLiteral("#b36b20") : QStringLiteral("#ffad42")));
    fill(QStringLiteral("ui.panel"), dark ? background.lighter(125) : background.darker(104));
    fill(QStringLiteral("ui.raised"), mix(background, foreground, dark ? 0.07 : 0.03));
    fill(QStringLiteral("ui.field"), mix(background, foreground, dark ? 0.10 : 0.05));
    fill(QStringLiteral("ui.border"), mix(background, foreground, 0.18));
    fill(QStringLiteral("ui.muted"), mix(background, foreground, 0.55));
}

Result load() {
    Result result;
    const QString themed = themeDir() + QStringLiteral("/omazettel.toml");
    if (QFileInfo(themed).isFile()) {
        result.colors = parse(readText(themed));
        result.source = QStringLiteral("theme file");
        result.path = themed;
    }
    if (result.colors.isEmpty()) {
        const QString user = userTemplatePath();
        const bool hasUser = QFileInfo(user).isFile();
        const QString text = hasUser ? readText(user) : builtinTemplate();
        result.colors = parse(render(text, themeColors(themeDir() + QStringLiteral("/colors.toml"))));
        result.source = hasUser ? QStringLiteral("your template") : QStringLiteral("built-in template");
        result.path = hasUser ? user : QString();
    }
    return result;
}

Result omaPalette(const QString &accent) {
    Result result;
    result.source = QStringLiteral("Oma palette");
    // OmaShow's Theme.qml: a fixed dark surround; the accent is the theme's.
    result.colors = {
        {QStringLiteral("editor.background"), QStringLiteral("#0b0e13")},
        {QStringLiteral("editor.foreground"), QStringLiteral("#e7edf7")},
        {QStringLiteral("editor.selection_foreground"), QStringLiteral("#e7edf7")},
        {QStringLiteral("editor.code_background"), QStringLiteral("#171e2a")},
        {QStringLiteral("editor.marker"), QStringLiteral("#4f5a6c")},
        {QStringLiteral("editor.quote"), QStringLiteral("#a7b1c1")},
        {QStringLiteral("ui.accent"), QColor(accent).isValid() ? QColor(accent).name() : QStringLiteral("#27c2ff")},
        {QStringLiteral("ui.panel"), QStringLiteral("#111722")},
        {QStringLiteral("ui.raised"), QStringLiteral("#171e2a")},
        {QStringLiteral("ui.field"), QStringLiteral("#1b2431")},
        {QStringLiteral("ui.border"), QStringLiteral("#293241")},
        {QStringLiteral("ui.muted"), QStringLiteral("#727e90")},
    };
    complete(result.colors, true);
    return result;
}

}

#include "cli.h"

#include "mcpserver.h"
#include "notesapi.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <io.h>
#include <stdio.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace {

QString kebab(const QString &camel) {
    QString out;
    for (const QChar c : camel) {
        if (c.isUpper()) {
            out += QLatin1Char('-');
            out += c.toLower();
        } else {
            out += c;
        }
    }
    return out;
}

QString firstSentence(const QString &text) {
    const int end = int(text.indexOf(QStringLiteral(". ")));
    return end < 0 ? text : text.left(end + 1);
}

QString readStdin() {
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(in.readAll());
}

// --flag value as the parameter's type. "-" reads standard input.
QJsonValue convert(const NotesApi::Param &param, const QString &raw, const QJsonValue &existing, QString *error) {
    const QString value = raw == QLatin1String("-") && (param.type == QLatin1String("string") || param.type == QLatin1String("object"))
        ? readStdin() : raw;
    if (param.type == QLatin1String("integer")) {
        bool ok = false;
        const int number = value.toInt(&ok);
        if (!ok)
            *error = QStringLiteral("--%1 takes a number.").arg(kebab(param.name));
        return number;
    }
    if (param.type == QLatin1String("boolean"))
        return value != QLatin1String("false") && value != QLatin1String("no") && value != QLatin1String("0");
    if (param.type == QLatin1String("array")) {
        QJsonArray array = existing.toArray();
        for (const QString &item : value.split(QLatin1Char(','), Qt::SkipEmptyParts))
            array.append(item.trimmed());
        return array;
    }
    if (param.type == QLatin1String("object")) {
        QJsonObject object = existing.toObject();
        if (value.trimmed().startsWith(QLatin1Char('{'))) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8(), &parseError);
            if (!document.isObject()) {
                *error = QStringLiteral("--%1: %2").arg(kebab(param.name), parseError.errorString());
                return object;
            }
            const QJsonObject parsed = document.object();
            for (auto it = parsed.constBegin(); it != parsed.constEnd(); ++it)
                object.insert(it.key(), it.value());
            return object;
        }
        const int equals = int(value.indexOf(QLatin1Char('=')));
        if (equals <= 0) {
            *error = QStringLiteral("--%1 takes key=value or a JSON object.").arg(kebab(param.name));
            return object;
        }
        const QString key = value.left(equals).trimmed();
        const QString text = value.mid(equals + 1);
        // Values may be JSON ([1, 2], true, null, "quoted"); else they are text.
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(QStringLiteral("[%1]").arg(text).toUtf8(), &parseError);
        object.insert(key, parseError.error == QJsonParseError::NoError && document.array().size() == 1
                               ? document.array().first() : QJsonValue(text));
        return object;
    }
    return value;
}

void printJson(const QJsonObject &object) {
    QTextStream(stdout) << QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QString tagList(const QJsonValue &tags) {
    QStringList out;
    for (const QJsonValue &tag : tags.toArray())
        out.append(QLatin1Char('#') + tag.toString());
    return out.join(QLatin1Char(' '));
}

void printPlain(const QString &tool, const QJsonObject &result) {
    QTextStream out(stdout);
    const auto noteLine = [&out](const QJsonObject &note) {
        QString line = note.value(QStringLiteral("path")).toString();
        const QString tags = tagList(note.value(QStringLiteral("tags")));
        if (!tags.isEmpty())
            line += QStringLiteral("  ") + tags;
        out << line << '\n';
    };

    if (tool == QLatin1String("vault_info")) {
        out << "Notes folder: " << result.value(QStringLiteral("vault")).toString() << '\n'
            << "Notes: " << result.value(QStringLiteral("notes")).toInt()
            << ", tags: " << result.value(QStringLiteral("tags")).toInt() << '\n';
        QStringList templates;
        for (const QJsonValue &name : result.value(QStringLiteral("templates")).toArray())
            templates.append(name.toString());
        out << "Note templates: " << templates.join(QStringLiteral(", ")) << '\n';
        QStringList profiles;
        for (const QJsonValue &name : result.value(QStringLiteral("exportProfiles")).toArray())
            profiles.append(name.toString());
        out << "Export profiles: " << profiles.join(QStringLiteral(", "))
            << (result.value(QStringLiteral("pandoc")).toBool() ? "" : " (pandoc is not installed)") << '\n';
        const QJsonObject sync = result.value(QStringLiteral("sync")).toObject();
        if (sync.value(QStringLiteral("enabled")).toBool())
            out << "Sync: " << (sync.value(QStringLiteral("remote")).toString().isEmpty() ? QStringLiteral("history only")
                                                                                           : sync.value(QStringLiteral("remote")).toString())
                << ", " << sync.value(QStringLiteral("pendingChanges")).toInt() << " change(s) waiting\n";
        else
            out << "Sync: off\n";
        const QJsonObject transcription = result.value(QStringLiteral("transcription")).toObject();
        out << "Transcribing recordings: "
            << (transcription.value(QStringLiteral("recordings")).toBool()
                    ? transcription.value(QStringLiteral("engine")).toString() + QStringLiteral(" (")
                          + transcription.value(QStringLiteral("model")).toString() + QStringLiteral(")")
                    : QStringLiteral("not set up (see omazettel help transcribe)"))
            << '\n';
        return;
    }
    if (tool == QLatin1String("list_notes")) {
        for (const QJsonValue &note : result.value(QStringLiteral("notes")).toArray())
            noteLine(note.toObject());
        return;
    }
    if (tool == QLatin1String("search_notes")) {
        const QJsonArray results = result.value(QStringLiteral("results")).toArray();
        if (results.isEmpty())
            out << "No notes match.\n";
        for (const QJsonValue &value : results) {
            const QJsonObject hit = value.toObject();
            out << hit.value(QStringLiteral("path")).toString() << "  (" << hit.value(QStringLiteral("matches")).toInt() << ")\n";
            for (const QJsonValue &snippet : hit.value(QStringLiteral("snippets")).toArray())
                out << "    " << snippet.toString() << '\n';
        }
        return;
    }
    if (tool == QLatin1String("read_note") || tool == QLatin1String("daily_note")
            || (tool == QLatin1String("transcribe") && result.contains(QStringLiteral("text")) && !result.contains(QStringLiteral("message")))) {
        out << result.value(QStringLiteral("text")).toString();
        if (!result.value(QStringLiteral("text")).toString().endsWith(QLatin1Char('\n')))
            out << '\n';
        return;
    }
    if (tool == QLatin1String("list_tags")) {
        for (const QJsonValue &value : result.value(QStringLiteral("tags")).toArray())
            out << '#' << value.toObject().value(QStringLiteral("tag")).toString() << "  "
                << value.toObject().value(QStringLiteral("count")).toInt() << '\n';
        return;
    }
    if (tool == QLatin1String("list_tasks")) {
        const QJsonArray tasks = result.value(QStringLiteral("tasks")).toArray();
        if (tasks.isEmpty())
            out << "No tasks.\n";
        for (const QJsonValue &value : tasks) {
            const QJsonObject task = value.toObject();
            out << (task.value(QStringLiteral("done")).toBool() ? "[x] " : "[ ] ") << task.value(QStringLiteral("text")).toString()
                << "  — " << task.value(QStringLiteral("path")).toString() << ':' << task.value(QStringLiteral("line")).toInt() << '\n';
        }
        return;
    }
    if (tool == QLatin1String("link_report")) {
        const QJsonArray broken = result.value(QStringLiteral("broken")).toArray();
        out << "Broken links: " << broken.size() << '\n';
        for (const QJsonValue &value : broken)
            out << "  " << value.toObject().value(QStringLiteral("path")).toString() << " → [["
                << value.toObject().value(QStringLiteral("target")).toString() << "]]\n";
        const QJsonArray orphans = result.value(QStringLiteral("orphans")).toArray();
        out << "Notes nothing links to: " << orphans.size() << '\n';
        for (const QJsonValue &value : orphans)
            out << "  " << value.toString() << '\n';
        return;
    }
    if (tool == QLatin1String("list_templates")) {
        for (const QJsonValue &value : result.value(QStringLiteral("templates")).toArray()) {
            const QJsonObject item = value.toObject();
            QStringList headings;
            for (const QJsonValue &heading : item.value(QStringLiteral("headings")).toArray())
                headings.append(heading.toString());
            out << item.value(QStringLiteral("name")).toString();
            if (!item.value(QStringLiteral("folder")).toString().isEmpty())
                out << "  (in " << item.value(QStringLiteral("folder")).toString() << ")";
            if (!headings.isEmpty())
                out << "  — " << headings.join(QStringLiteral(", "));
            out << '\n';
        }
        return;
    }
    if (tool == QLatin1String("ai_status")) {
        const QString name = result.value(QStringLiteral("name")).toString();
        if (result.value(QStringLiteral("provider")).toString().isEmpty())
            out << "AI for notes: none chosen\n";
        else
            out << "AI for notes: " << name
                << (result.value(QStringLiteral("model")).toString().isEmpty() ? QString() : QStringLiteral(" (%1)").arg(result.value(QStringLiteral("model")).toString()))
                << "  — " << result.value(QStringLiteral("origin")).toString() << '\n';
        if (result.value(QStringLiteral("ready")).toBool())
            out << "  ready\n";
        else
            out << "  not ready: " << QString(result.value(QStringLiteral("problem")).toString()).replace(QLatin1Char('\n'), QStringLiteral("\n  ")) << '\n';
        if (result.contains(QStringLiteral("test")))
            out << "  test: " << result.value(QStringLiteral("test")).toString() << '\n';
        QStringList agents;
        for (const QJsonValue &agent : result.value(QStringLiteral("installedAgents")).toArray())
            agents.append(agent.toString());
        out << "Agents installed: " << (agents.isEmpty() ? QStringLiteral("none") : agents.join(QStringLiteral(", "))) << '\n';
        const QJsonObject speech = result.value(QStringLiteral("transcription")).toObject();
        out << "Transcribing recordings: ";
        if (speech.value(QStringLiteral("ready")).toBool())
            out << speech.value(QStringLiteral("engine")).toString() << " (" << speech.value(QStringLiteral("model")).toString() << ")\n";
        else
            out << "not ready\n  " << QString(speech.value(QStringLiteral("problem")).toString()).replace(QLatin1Char('\n'), QStringLiteral("\n  ")) << '\n';
        out << "Caption files and transcripts: ready\n";
        return;
    }
    if (result.contains(QStringLiteral("message")))
        out << result.value(QStringLiteral("message")).toString() << '\n';
    const QJsonArray actions = result.value(QStringLiteral("actionItems")).toArray();
    if (!actions.isEmpty()) {
        out << "Action items:\n";
        for (const QJsonValue &value : actions)
            out << "  [ ] " << value.toString() << '\n';
    }
    const QJsonArray made = result.value(QStringLiteral("linkedNotesCreated")).toArray();
    for (const QJsonValue &value : made)
        out << "  made linked note " << value.toString() << '\n';
    if (tool == QLatin1String("sync")) {
        for (const QJsonValue &value : result.value(QStringLiteral("conflicts")).toArray())
            out << "  conflict: " << value.toString() << '\n';
        for (const QJsonValue &value : result.value(QStringLiteral("conflictCopies")).toArray())
            out << "  conflict copy: " << value.toString() << '\n';
    }
}

int runVerb(const NotesApi::Tool &tool, const QStringList &arguments) {
    QJsonObject args;
    QStringList words;
    QString vault;
    int json = -1; // -1: when the output is not a terminal.
    QString error;

    // Whether to answer in JSON is known before anything can go wrong.
    for (int i = 2; i < arguments.size() && arguments.at(i) != QLatin1String("--"); ++i) {
        if (arguments.at(i) == QLatin1String("--json"))
            json = 1;
        else if (arguments.at(i) == QLatin1String("--plain"))
            json = 0;
    }
    // An option is -x or --name; "- [ ] task" and "-" are text.
    static const QRegularExpression optionRe(QStringLiteral("^(-[A-Za-z]|--[A-Za-z])"));

    for (int i = 2; i < arguments.size() && error.isEmpty(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QLatin1String("--")) {
            words.append(arguments.mid(i + 1));
            break;
        }
        if (argument == QLatin1String("-h") || argument == QLatin1String("--help")) {
            QTextStream(stdout) << Cli::verbUsage(tool.verb);
            return 0;
        }
        if (!optionRe.match(argument).hasMatch() || (argument.size() > 2 && !argument.startsWith(QLatin1String("--")))) {
            words.append(argument);
            continue;
        }
        QString name = argument.startsWith(QLatin1String("--")) ? argument.mid(2) : argument.mid(1);
        QString value;
        bool hasValue = false;
        if (const int equals = int(name.indexOf(QLatin1Char('='))); equals > 0) {
            value = name.mid(equals + 1);
            name = name.left(equals);
            hasValue = true;
        }
        // Short forms.
        static const QHash<QString, QString> shortNames{
            {QStringLiteral("t"), QStringLiteral("template")}, {QStringLiteral("f"), QStringLiteral("folder")},
            {QStringLiteral("s"), QStringLiteral("section")}, {QStringLiteral("n"), QStringLiteral("limit")},
            {QStringLiteral("m"), QStringLiteral("meta")}, {QStringLiteral("o"), QStringLiteral("output")},
            {QStringLiteral("p"), QStringLiteral("profile")}};
        if (!argument.startsWith(QLatin1String("--"))) {
            if (!shortNames.contains(name)) {
                error = QStringLiteral("Unknown option %1.").arg(argument);
                break;
            }
            name = shortNames.value(name);
        }
        if (name == QLatin1String("json")) { json = 1; continue; }
        if (name == QLatin1String("plain")) { json = 0; continue; }
        if (name == QLatin1String("vault")) {
            if (!hasValue && i + 1 < arguments.size()) {
                value = arguments.at(++i);
                hasValue = true;
            }
            if (!hasValue) {
                error = QStringLiteral("--vault needs a folder.");
                break;
            }
            vault = value;
            continue;
        }
        bool negated = false;
        const NotesApi::Param *param = nullptr;
        for (const NotesApi::Param &candidate : tool.params) {
            if (kebab(candidate.name) == name) {
                param = &candidate;
            } else if (candidate.type == QLatin1String("boolean") && name == QStringLiteral("no-") + kebab(candidate.name)) {
                param = &candidate;
                negated = true;
            }
        }
        if (!param) {
            error = QStringLiteral("%1 has no option --%2. See: omazettel %1 --help").arg(tool.verb, name);
            break;
        }
        if (param->type == QLatin1String("boolean")) {
            // --flag, --no-flag, --flag=false or --flag false.
            if (!hasValue && i + 1 < arguments.size()
                    && QStringList{QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("yes"),
                                   QStringLiteral("no")}.contains(arguments.at(i + 1))) {
                value = arguments.at(++i);
                hasValue = true;
            }
            args.insert(param->name, negated ? false : convert(*param, hasValue ? value : QStringLiteral("true"), QJsonValue(), &error));
            continue;
        }
        if (!hasValue) {
            // --note alone, or before another option or the file, has no value.
            const bool bare = i + 1 >= arguments.size() || optionRe.match(arguments.at(i + 1)).hasMatch()
                              || (param->optionalValue && QFileInfo::exists(arguments.at(i + 1)))
                              || (param->optionalValue && arguments.at(i + 1).startsWith(QLatin1String("voxtype:")));
            if (bare && param->optionalValue) {
                args.insert(param->name, QString());
                continue;
            }
            if (i + 1 >= arguments.size()) {
                error = QStringLiteral("--%1 needs a value.").arg(name);
                break;
            }
            value = arguments.at(++i);
        }
        args.insert(param->name, convert(*param, value, args.value(param->name), &error));
    }

    // Bare words fill the positional parameters in order; the last one
    // takes all that are left.
    QList<const NotesApi::Param *> positional;
    for (const NotesApi::Param &param : tool.params) {
        if (param.positional && !args.contains(param.name))
            positional.append(&param);
    }
    for (int p = 0; p < positional.size() && !words.isEmpty() && error.isEmpty(); ++p) {
        const NotesApi::Param &param = *positional.at(p);
        const bool last = p == positional.size() - 1;
        if (param.type == QLatin1String("object")) {
            QJsonValue object;
            while (!words.isEmpty() && error.isEmpty())
                object = convert(param, words.takeFirst(), object, &error);
            args.insert(param.name, object);
        } else if (last && param.type == QLatin1String("string") && words.size() > 1) {
            args.insert(param.name, words.join(QLatin1Char(' ')));
            words.clear();
        } else {
            args.insert(param.name, convert(param, words.takeFirst(), QJsonValue(), &error));
        }
    }
    if (error.isEmpty() && !words.isEmpty())
        error = QStringLiteral("Unexpected “%1”. See: omazettel %2 --help").arg(words.join(QLatin1Char(' ')), tool.verb);
    if (error.isEmpty() && tool.name != QLatin1String("transcribe")) {
        for (const NotesApi::Param &param : tool.params) {
            if (param.required && !args.contains(param.name)) {
                error = QStringLiteral("%1 needs %2. See: omazettel %1 --help")
                            .arg(tool.verb, param.positional ? QLatin1Char('<') + param.name + QLatin1Char('>')
                                                             : QStringLiteral("--") + kebab(param.name));
                break;
            }
        }
    }

#ifdef Q_OS_WIN
    const bool terminal = _isatty(_fileno(stdout));
#else
    const bool terminal = isatty(STDOUT_FILENO);
#endif
    const bool asJson = json == 1 || (json == -1 && !terminal);
    const auto report = [asJson](const QString &message) {
        if (asJson)
            printJson({{QStringLiteral("error"), message}});
        else
            QTextStream(stderr) << "omazettel: " << message << '\n';
        return 1;
    };
    if (!error.isEmpty())
        return report(error);

    NotesApi::setProgress([](const QString &message) {
        QTextStream(stderr) << message << '\n';
    });
    NotesApi api;
    if (!api.open(vault, &error))
        return report(error);
    try {
        const QJsonObject result = api.call(tool, args);
        if (asJson)
            printJson(result);
        else
            printPlain(tool.name, result);
    } catch (const NotesApi::Error &failure) {
        return report(failure.message);
    }
    return 0;
}

}

QString Cli::usage() {
    QString text = QStringLiteral(
        "Usage: omazettel [FILE]\n"
        "       omazettel <command> [arguments] [--vault <folder>] [--json|--plain]\n"
        "\n"
        "Without a command, opens FILE (a Markdown file) in the OmaZettel window, or the\n"
        "notes folder used last at the note open last.\n"
        "\n"
        "Commands work on the notes folder the app has open (or --vault, or\n"
        "$OMAZETTEL_VAULT) without opening a window. They print text in a terminal and\n"
        "JSON when piped (or with --json). A value of - reads standard input.\n"
        "\n"
        "Commands:\n");
    for (const NotesApi::Tool &tool : NotesApi::tools())
        text += QStringLiteral("  %1%2\n").arg(tool.verb.leftJustified(12), firstSentence(tool.description));
    text += QStringLiteral(
        "  mcp         Serve these as tools to AI assistants (Model Context Protocol, stdio)\n"
        "  help CMD    Show a command's options\n"
        "\n"
        "Examples:\n"
        "  omazettel search \"quarterly plan\"\n"
        "  omazettel new \"Vendor kickoff\" -t Meeting --meta attendees='[\"Ann\",\"Bo\"]'\n"
        "  omazettel append \"Vendor kickoff\" \"- [ ] Send the contract\" -s \"Action items\"\n"
        "  omazettel capture \"Call the bank about the card\"\n"
        "  omazettel transcribe ~/Videos/standup.mp4 > standup.txt\n"
        "  omazettel transcribe standup.vtt --note \"Atlas standup\"   (an AI writes the note)\n"
        "\n"
        "Options:\n"
        "  -h, --help     Show this message and exit\n"
        "  -v, --version  Show the version and exit\n");
    return text;
}

QString Cli::verbUsage(const QString &verb) {
    const NotesApi::Tool *tool = NotesApi::toolForVerb(verb);
    if (!tool) {
        if (verb == QLatin1String("mcp"))
            return QStringLiteral(
                "Usage: omazettel mcp [--vault <folder>]\n\n"
                "Serves the notes commands as tools to AI assistants over the Model Context\n"
                "Protocol (JSON-RPC on standard input and output). For Claude Code:\n"
                "  claude mcp add --scope user omazettel -- omazettel mcp\n"
                "For Claude Desktop, Cursor and others, add a server with the command\n"
                "\"omazettel\" and the argument \"mcp\".\n");
        return QString();
    }
    QString line = QStringLiteral("Usage: omazettel %1").arg(verb);
    for (const NotesApi::Param &param : tool->params) {
        if (param.positional)
            line += param.required ? QStringLiteral(" <%1>").arg(param.name) : QStringLiteral(" [%1]").arg(param.name);
    }
    line += QStringLiteral(" [options]\n\n");
    QString text = line + tool->description + QStringLiteral("\n");
    if (!tool->params.isEmpty())
        text += QStringLiteral("\nOptions:\n");
    for (const NotesApi::Param &param : tool->params) {
        QString flag = QStringLiteral("--") + kebab(param.name);
        if (param.type == QLatin1String("string") || param.type == QLatin1String("integer"))
            flag += QStringLiteral(" <%1>").arg(param.type == QLatin1String("integer") ? QStringLiteral("n") : QStringLiteral("text"));
        else if (param.type == QLatin1String("array"))
            flag += QStringLiteral(" <a,b>");
        else if (param.type == QLatin1String("object"))
            flag += QStringLiteral(" <key=value|json>");
        text += QStringLiteral("  %1\n      %2\n").arg(flag, param.description);
    }
    text += QStringLiteral("  --vault <folder>\n      The notes folder (default: the one OmaZettel has open)\n"
                           "  --json, --plain\n      Answer in JSON, or as text\n");
    return text;
}

bool Cli::isCommand(const QStringList &arguments) {
    if (arguments.size() < 2)
        return false;
    const QString &first = arguments.at(1);
    static const QRegularExpression wordRe(QStringLiteral("^[a-z][a-z-]*$"));
    return first == QLatin1String("mcp") || first == QLatin1String("help") || NotesApi::toolForVerb(first)
           || (wordRe.match(first).hasMatch() && !QFileInfo::exists(first));
}

std::optional<int> Cli::handleArguments(const QStringList &arguments, const QString &version) {
    if (arguments.size() < 2)
        return std::nullopt;

    const QString &first = arguments.at(1);
    if (first == QLatin1String("-h") || first == QLatin1String("--help")) {
        QTextStream(stdout) << usage();
        return 0;
    }
    if (first == QLatin1String("-v") || first == QLatin1String("--version")) {
        QTextStream(stdout) << QStringLiteral("omazettel %1\n").arg(version);
        return 0;
    }
    if (first == QLatin1String("help")) {
        const QString text = arguments.size() > 2 ? verbUsage(arguments.at(2)) : usage();
        if (text.isEmpty()) {
            QTextStream(stderr) << "omazettel: there is no command " << arguments.at(2) << '\n';
            return 1;
        }
        QTextStream(stdout) << text;
        return 0;
    }
    if (first == QLatin1String("mcp")) {
        QString vault;
        for (int i = 2; i < arguments.size(); ++i) {
            if (arguments.at(i) == QLatin1String("--vault") && i + 1 < arguments.size())
                vault = arguments.at(++i);
            else if (arguments.at(i).startsWith(QLatin1String("--vault=")))
                vault = arguments.at(i).mid(8);
            else if (arguments.at(i) == QLatin1String("-h") || arguments.at(i) == QLatin1String("--help")) {
                QTextStream(stdout) << verbUsage(QStringLiteral("mcp"));
                return 0;
            }
        }
        NotesApi api;
        QString error;
        if (!api.open(vault, &error)) {
            QTextStream(stderr) << "omazettel: " << error << '\n';
            return 1;
        }
        return McpServer::run(api, version);
    }
    if (const NotesApi::Tool *tool = NotesApi::toolForVerb(first))
        return runVerb(*tool, arguments);
    // A bare word that is no file is a mistyped command, not a new document.
    static const QRegularExpression wordRe(QStringLiteral("^[a-z][a-z-]*$"));
    if (wordRe.match(first).hasMatch() && !QFileInfo::exists(first)) {
        QTextStream(stderr) << "omazettel: there is no command " << first << ". See: omazettel --help\n";
        return 1;
    }

    return std::nullopt;
}

#ifdef Q_OS_WIN
namespace {
// The command line as Windows keeps it, in UTF-16. argv is in the ANSI code
// page, which can't hold every note name.
QStringList windowsArguments() {
    int count = 0;
    LPWSTR *list = CommandLineToArgvW(GetCommandLineW(), &count);
    QStringList arguments;
    for (int index = 0; list && index < count; ++index)
        arguments.append(QString::fromWCharArray(list[index]));
    LocalFree(list);
    return arguments;
}

// omazettel.exe is a window app, so Windows gives it no terminal, and the
// terminal handles omazettel.com hands down write nowhere. Join the terminal
// of the program that started this one and write to it. Pipes and files are
// left as they are, so `omazettel tasks | more`, `> file` and MCP still work.
void useParentConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    const auto reopen = [](DWORD which, FILE *stream, const char *device, const char *mode) {
        const HANDLE handle = GetStdHandle(which);
        const DWORD type = handle && handle != INVALID_HANDLE_VALUE ? GetFileType(handle) : FILE_TYPE_UNKNOWN;
        if (type == FILE_TYPE_CHAR || type == FILE_TYPE_UNKNOWN)
            (void)freopen(device, mode, stream);
    };
    reopen(STD_INPUT_HANDLE, stdin, "CONIN$", "r");
    reopen(STD_OUTPUT_HANDLE, stdout, "CONOUT$", "w");
    reopen(STD_ERROR_HANDLE, stderr, "CONOUT$", "w");
}
}
#endif

std::optional<int> Cli::handleArguments(int argc, char *argv[], const QString &version) {
#ifdef Q_OS_WIN
    const QStringList arguments = windowsArguments();
#else
    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));
#endif

    if (!isCommand(arguments)) {
#ifdef Q_OS_WIN
        // --help and --version answer in the terminal too.
        if (arguments.size() > 1 && (arguments.at(1) == QLatin1String("-h") || arguments.at(1) == QLatin1String("--help")
                                     || arguments.at(1) == QLatin1String("-v") || arguments.at(1) == QLatin1String("--version")))
            useParentConsole();
#endif
        return handleArguments(arguments, version);
    }
#ifdef Q_OS_WIN
    useParentConsole();
#endif

    // Commands run without a display, as a plain (non-GUI) application, with
    // the app's name so they read its settings.
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("omazettel"));
    app.setApplicationName(QStringLiteral("omazettel"));
    app.setApplicationVersion(version);
    return handleArguments(arguments, version);
}

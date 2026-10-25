#include "mcpserver.h"

#include "layout.h"
#include "notesapi.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cstdio>
#include <iostream>
#include <string>

namespace {

const QStringList supportedVersions{QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"),
                                    QStringLiteral("2024-11-05")};

QJsonObject reply(const QJsonValue &id, const QJsonObject &result) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
            {QStringLiteral("result"), result}};
}

QJsonObject replyError(const QJsonValue &id, int code, const QString &message) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                  {QStringLiteral("message"), message}}}};
}

QString instructions(const NotesApi &api) {
    return QStringLiteral(
        "OmaZettel notes: plain Markdown files in %1, linked with [[wiki links]] and tagged with #tags or "
        "YAML header tags. Find notes with search_notes or list_notes before reading or changing them; "
        "change part of a note with append_to_note, replace_section or set_metadata rather than "
        "write_note. New notes come from templates (list_templates): Meeting, Daily, Literature note, Note. "
        "For a meeting recording or transcript: transcribe it, then create_note with template Meeting, "
        "filling sections (Agenda, Notes, Decisions, Action items as - [ ] tasks) and meta.attendees. "
        "Link related notes with [[Note name]]; each link to a note that does not exist makes an empty "
        "one unless linkNotes is false, so link only notes that exist or should.%2")
        .arg(api.root(), Layout::present(api.root())
                             ? QStringLiteral("\n\nBefore changing notes, read 01-system/SYSTEM-RULES.md and "
                                              "01-system/HOW-I-WORK.md (vault_info returns both) and follow them. ")
                                   + Layout::guide()
                             : QString());
}

QJsonArray toolList() {
    QJsonArray tools;
    for (const NotesApi::Tool &tool : NotesApi::tools()) {
        QJsonObject annotations{{QStringLiteral("readOnlyHint"), tool.readOnly},
                                {QStringLiteral("openWorldHint"), false}};
        if (!tool.readOnly)
            annotations.insert(QStringLiteral("destructiveHint"), tool.destructive);
        tools.append(QJsonObject{{QStringLiteral("name"), tool.name},
                                 {QStringLiteral("description"), tool.description},
                                 {QStringLiteral("inputSchema"), NotesApi::inputSchema(tool)},
                                 {QStringLiteral("annotations"), annotations}});
    }
    return tools;
}

struct Prompt {
    QString name;
    QString description;
    QList<QPair<QString, QString>> arguments; // name, description; the first is required.
    QString text;                             // With {argument} placeholders.
};

const QList<Prompt> &prompts() {
    static const QList<Prompt> list = {
        {QStringLiteral("meeting_note"),
         QStringLiteral("Turn a meeting recording, caption file or pasted transcript into a meeting note"),
         {{QStringLiteral("source"), QStringLiteral("Path to the recording or transcript, or the transcript text")},
          {QStringLiteral("title"), QStringLiteral("The meeting's title (optional)")}},
         QStringLiteral(
             "Make an OmaZettel meeting note from this meeting: {source}\n\n"
             "1. If it is a file path, call transcribe on it to get the text.\n"
             "2. Call list_templates to see the Meeting template's headings.\n"
             "3. Search the notes (search_notes) for the project, people and topics mentioned, so you can "
             "link existing notes with [[Note name]].\n"
             "4. Call create_note with template \"Meeting\", title \"{title}\" (or a short title you choose), "
             "meta.attendees (the speakers), and sections: Agenda (topics covered), Notes (a concise "
             "summary by topic), Decisions, and Action items as \"- [ ] Owner: task (due date)\" lines. "
             "Set linkNotes to false unless a linked note should exist.\n"
             "5. Tell me the note's path and list the action items.")},
        {QStringLiteral("add_to_note"),
         QStringLiteral("Add something to an existing note, under the right heading"),
         {{QStringLiteral("note"), QStringLiteral("Which note (name, title or path)")},
          {QStringLiteral("content"), QStringLiteral("What to add")}},
         QStringLiteral(
             "Add this to my note \"{note}\":\n\n{content}\n\n"
             "Read the note first (read_note) to see its headings and style. Then use append_to_note with "
             "the section it belongs under (or replace_section if it updates a section), matching the "
             "note's formatting. Do not rewrite the rest of the note.")},
        {QStringLiteral("weekly_review"),
         QStringLiteral("Review the past week's notes, decisions and open tasks"),
         {{QStringLiteral("since"), QStringLiteral("How far back (default 7d)")}},
         QStringLiteral(
             "Review my notes changed since {since}: call list_notes with since and sort modified, read the "
             "meeting and daily notes among them, and list_tasks for open tasks. Summarise what happened, "
             "decisions made and open action items by owner, then offer to append the summary to today's "
             "daily note (daily_note with append, section \"Review\").")},
    };
    return list;
}

QJsonObject promptList() {
    QJsonArray list;
    for (const Prompt &prompt : prompts()) {
        QJsonArray arguments;
        for (int i = 0; i < prompt.arguments.size(); ++i) {
            arguments.append(QJsonObject{{QStringLiteral("name"), prompt.arguments.at(i).first},
                                         {QStringLiteral("description"), prompt.arguments.at(i).second},
                                         {QStringLiteral("required"), i == 0 && prompt.name != QLatin1String("weekly_review")}});
        }
        list.append(QJsonObject{{QStringLiteral("name"), prompt.name},
                                {QStringLiteral("description"), prompt.description},
                                {QStringLiteral("arguments"), arguments}});
    }
    return {{QStringLiteral("prompts"), list}};
}

}

QJsonObject McpServer::handle(NotesApi &api, const QJsonObject &message, const QString &version) {
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    const bool isRequest = message.contains(QStringLiteral("id"));
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();

    if (!isRequest)
        return {}; // notifications/initialized, cancelled, ...

    if (method == QLatin1String("initialize")) {
        QString protocol = params.value(QStringLiteral("protocolVersion")).toString();
        if (!supportedVersions.contains(protocol))
            protocol = supportedVersions.first();
        return reply(id, {{QStringLiteral("protocolVersion"), protocol},
                          {QStringLiteral("capabilities"), QJsonObject{
                               {QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}},
                               {QStringLiteral("prompts"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
                          {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("omazettel")},
                                                                     {QStringLiteral("title"), QStringLiteral("OmaZettel notes")},
                                                                     {QStringLiteral("version"), version}}},
                          {QStringLiteral("instructions"), instructions(api)}});
    }
    if (method == QLatin1String("ping"))
        return reply(id, {});
    if (method == QLatin1String("tools/list"))
        return reply(id, {{QStringLiteral("tools"), toolList()}});
    if (method == QLatin1String("prompts/list"))
        return reply(id, promptList());
    if (method == QLatin1String("prompts/get")) {
        const QString name = params.value(QStringLiteral("name")).toString();
        const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
        for (const Prompt &prompt : prompts()) {
            if (prompt.name != name)
                continue;
            QString text = prompt.text;
            for (const auto &argument : prompt.arguments) {
                QString value = arguments.value(argument.first).toString();
                if (value.isEmpty() && argument.first == QLatin1String("since"))
                    value = QStringLiteral("7d");
                text.replace(QLatin1Char('{') + argument.first + QLatin1Char('}'), value);
            }
            text.replace(QStringLiteral("title \"\""), QStringLiteral("title"));
            return reply(id, {{QStringLiteral("description"), prompt.description},
                              {QStringLiteral("messages"), QJsonArray{QJsonObject{
                                   {QStringLiteral("role"), QStringLiteral("user")},
                                   {QStringLiteral("content"), QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                           {QStringLiteral("text"), text}}}}}}});
        }
        return replyError(id, -32602, QStringLiteral("No prompt named %1").arg(name));
    }
    if (method == QLatin1String("tools/call")) {
        const QString name = params.value(QStringLiteral("name")).toString();
        const NotesApi::Tool *tool = NotesApi::toolNamed(name);
        if (!tool)
            return replyError(id, -32602, QStringLiteral("No tool named %1").arg(name));
        QJsonObject result;
        bool failed = false;
        try {
            result = api.call(*tool, params.value(QStringLiteral("arguments")).toObject());
        } catch (const NotesApi::Error &error) {
            result = {{QStringLiteral("error"), error.message}};
            failed = true;
        }
        const QString text = failed ? result.value(QStringLiteral("error")).toString()
                                    : QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
        QJsonObject answer{{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                              {QStringLiteral("text"), text}}}},
                           {QStringLiteral("isError"), failed}};
        if (!failed)
            answer.insert(QStringLiteral("structuredContent"), result);
        return reply(id, answer);
    }
    return replyError(id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

int McpServer::run(NotesApi &api, const QString &version) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parseError);
        QJsonArray answers;
        const auto answerOne = [&](const QJsonValue &value) {
            if (!value.isObject()) {
                answers.append(replyError(QJsonValue(), -32600, QStringLiteral("Invalid request")));
                return;
            }
            const QJsonObject answer = handle(api, value.toObject(), version);
            if (!answer.isEmpty())
                answers.append(answer);
        };
        if (parseError.error != QJsonParseError::NoError) {
            answers.append(replyError(QJsonValue(), -32700, QStringLiteral("Parse error")));
        } else if (document.isArray()) {
            for (const QJsonValue &value : document.array())
                answerOne(value);
        } else {
            answerOne(document.object());
        }
        if (answers.isEmpty())
            continue;
        const QByteArray out = document.isArray()
            ? QJsonDocument(answers).toJson(QJsonDocument::Compact)
            : QJsonDocument(answers.first().toObject()).toJson(QJsonDocument::Compact);
        std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    return 0;
}

#include "transcriber.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

namespace {

QString readAll(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

QString decodeEntities(QString text) {
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));
    text.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return text;
}

// 00:01:02.345 or 00:01:02,345 or 01:02.345 -> 00:01:02.
QString shortTime(const QString &stamp) {
    QString time = stamp.trimmed();
    time = time.left(time.indexOf(QRegularExpression(QStringLiteral("[.,]"))));
    if (time.count(QLatin1Char(':')) == 1)
        time.prepend(QStringLiteral("00:"));
    return time;
}

// Runs a program to the end. Returns its exit code, or -1 when it could not
// start or crashed.
int run(const QString &program, const QStringList &arguments, QString *errorOutput,
        QByteArray *output = nullptr) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10000))
        return -1;
    process.closeWriteChannel();
    process.waitForFinished(-1);
    if (output)
        *output = process.readAllStandardOutput();
    if (errorOutput)
        *errorOutput = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

QString lastLines(const QString &text, int count = 4) {
    QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.mid(qMax(0, int(lines.size()) - count)).join(QLatin1Char('\n'));
}

QString voxtypeModelsFolder() {
    QString base = qEnvironmentVariable("XDG_DATA_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/voxtype/models");
}

// Voxtype with a model downloaded (by `omarchy voxtype model` or
// `voxtype setup model`).
QString voxtype() {
    const QString found = QStandardPaths::findExecutable(QStringLiteral("voxtype"));
    if (found.isEmpty())
        return QString();
    const QStringList models = QDir(voxtypeModelsFolder()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &model : models) {
        if (model != QLatin1String("CACHEDIR.TAG"))
            return found;
    }
    return QString();
}

QString voxtypeModel() {
    QFile file(QDir::homePath() + QStringLiteral("/.config/voxtype/config.toml"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        static const QRegularExpression modelRe(QStringLiteral("(?m)^\\s*model\\s*=\\s*\"([^\"]+)\""));
        const QRegularExpressionMatch match = modelRe.match(QString::fromUtf8(file.readAll()));
        if (match.hasMatch())
            return match.captured(1);
    }
    return QStringLiteral("its default model");
}

// Voxtype prints what it is doing before the text.
QString voxtypeText(const QByteArray &output) {
    static const QRegularExpression noiseRe(QStringLiteral("^(Loading audio file|Audio format|Processing \\d+ samples|Transcrib)"));
    QStringList kept;
    for (const QString &line : QString::fromUtf8(output).split(QLatin1Char('\n'))) {
        if (!noiseRe.match(line.trimmed()).hasMatch())
            kept.append(line);
    }
    return kept.join(QLatin1Char('\n')).trimmed();
}

// Meeting Recorder, which records meetings and tells their voices apart.
QString meetingRecorder() {
    return QStandardPaths::findExecutable(QStringLiteral("omarchy-meeting-recorder"));
}

// The model Meeting Recorder transcribes with (its config.toml), for messages.
QString meetingRecorderModel() {
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");
    QFile file(base + QStringLiteral("/omarchy-meeting-recorder/config.toml"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        static const QRegularExpression modelRe(QStringLiteral("(?m)^\\s*model\\s*=\\s*\"([^\"]+)\""));
        const QRegularExpressionMatch match = modelRe.match(QString::fromUtf8(file.readAll()));
        if (match.hasMatch())
            return match.captured(1);
    }
    return QStringLiteral("large-v3-turbo");
}

QString manifestIn(const QString &folder) {
    const QStringList found = QDir(folder).entryList({QStringLiteral("*.meeting-recorder")}, QDir::Files, QDir::Name);
    return found.isEmpty() ? QString() : QDir(folder).filePath(found.first());
}

// 61000 -> "01:01", 3723000 -> "1:02:03", as Meeting Recorder writes times.
QString clock(qint64 ms) {
    const qint64 s = ms / 1000;
    return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// A meeting folder of Meeting Recorder's, with its transcript and details.
Transcriber::Result readMeeting(const QString &folder, bool timestamps, QString *error) {
    Transcriber::Result result;
    QJsonObject manifest;
    const QString manifestPath = manifestIn(folder);
    if (!manifestPath.isEmpty())
        manifest = QJsonDocument::fromJson(readAll(manifestPath).toUtf8()).object();
    result.title = manifest.value(QStringLiteral("title")).toString(QFileInfo(manifestPath).completeBaseName());
    const QString transcript = QDir(folder).filePath(QStringLiteral("transcript.md"));
    if (!QFileInfo(transcript).isFile()) {
        *error = QStringLiteral("Meeting Recorder has not finished the transcript of \"%1\" yet (%2).")
                     .arg(result.title, folder);
        return result;
    }
    result.text = Transcriber::fromMeetingRecorder(readAll(transcript), timestamps, &result.speakers);
    result.engine = QStringLiteral("meeting recorder");
    result.model = manifest.value(QStringLiteral("model")).toString();
    result.folder = folder;
    if (manifest.contains(QStringLiteral("started_at")))
        result.started = QDateTime::fromSecsSinceEpoch(manifest.value(QStringLiteral("started_at")).toInteger());
    result.durationSecs = manifest.value(QStringLiteral("duration_secs")).toInt();
    for (const QJsonValue &chapter : manifest.value(QStringLiteral("chapters")).toArray()) {
        const QJsonObject item = chapter.toObject();
        result.chapters.append(QStringLiteral("[%1] %2").arg(clock(item.value(QStringLiteral("start_ms")).toInteger()),
                                                              item.value(QStringLiteral("title")).toString()));
    }
    return result;
}

QString whisperCpp() {
    for (const QString &name : {QStringLiteral("whisper-cli"), QStringLiteral("whisper-cpp")}) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return found;
    }
    return QString();
}

const QStringList preferredModels{
    QStringLiteral("large-v3-turbo"), QStringLiteral("large-v3"), QStringLiteral("medium.en"),
    QStringLiteral("medium"), QStringLiteral("small.en"), QStringLiteral("small"),
    QStringLiteral("base.en"), QStringLiteral("base"), QStringLiteral("tiny.en"),
    QStringLiteral("tiny")};

QStringList modelFolders() {
    return {Transcriber::modelsFolder(), QStringLiteral("/usr/share/whisper.cpp/models"),
            QStringLiteral("/usr/local/share/whisper.cpp/models")};
}

}

bool Transcriber::unnamedSpeaker(const QString &name) {
    static const QRegularExpression defaultRe(QStringLiteral("^(You|Remote|Room|Speaker)( \\d+)?$"));
    return name.trimmed().isEmpty() || defaultRe.match(name.trimmed()).hasMatch();
}

QString Transcriber::meetingsFolder() {
    const QString chosen = qEnvironmentVariable("OMAZETTEL_MEETINGS");
    return chosen.isEmpty() ? QDir::homePath() + QStringLiteral("/Documents/Meetings") : chosen;
}

QString Transcriber::meetingRecorderFolder(const QString &file) {
    if (file.startsWith(QLatin1String("meeting-recorder:"))) {
        // Folders are named "<YYYYMMDDHHMM> <name>": newest first.
        const QString wanted = file.mid(17).trimmed();
        const QDir meetings(meetingsFolder());
        for (const QString &name : meetings.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
            const QString folder = meetings.filePath(name);
            if (manifestIn(folder).isEmpty() || !QFileInfo(QDir(folder).filePath(QStringLiteral("transcript.md"))).isFile())
                continue;
            if (wanted.isEmpty() || wanted == QLatin1String("latest") || name.contains(wanted, Qt::CaseInsensitive))
                return QFileInfo(folder).canonicalFilePath();
        }
        return QString();
    }
    const QFileInfo info(file);
    if (info.isDir())
        return manifestIn(info.absoluteFilePath()).isEmpty() ? QString() : info.canonicalFilePath();
    if (info.isFile() && (info.suffix() == QLatin1String("meeting-recorder") || !manifestIn(info.absolutePath()).isEmpty()))
        return QFileInfo(info.absolutePath()).canonicalFilePath();
    return QString();
}

QString Transcriber::fromMeetingRecorder(const QString &markdown, bool timestamps, QStringList *speakers) {
    // The turns follow "## Transcript" (after the heading and the chapters).
    QString body = markdown;
    const qsizetype start = body.indexOf(QLatin1String("## Transcript"));
    if (start >= 0)
        body = body.mid(start + 13);
    static const QRegularExpression turnRe(QStringLiteral("^\\*\\*\\[([0-9:]+)\\]\\s+(.+?):\\*\\*\\s*(.*)$"));
    QStringList turns;
    QStringList named;
    for (const QString &raw : body.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QRegularExpressionMatch turn = turnRe.match(line);
        if (turn.hasMatch()) {
            const QString speaker = turn.captured(2).trimmed();
            if (!unnamedSpeaker(speaker) && !named.contains(speaker))
                named.append(speaker);
            turns.append((timestamps ? QStringLiteral("[%1] ").arg(turn.captured(1)) : QString())
                         + speaker + QStringLiteral(": ") + turn.captured(3).trimmed());
        } else if (!turns.isEmpty()) {
            turns.last() += QLatin1Char(' ') + line;
        }
    }
    if (speakers)
        *speakers = named;
    return turns.join(QStringLiteral("\n\n"));
}

QString Transcriber::fromCaptions(const QString &captions, bool timestamps, QStringList *speakers) {
    struct Turn {
        QString time;
        QString speaker;
        QString text;
    };
    QList<Turn> turns;
    static const QRegularExpression voiceRe(QStringLiteral("^<v(?:\\.[^\\s>]*)?\\s+([^>]+)>(.*)$"));
    static const QRegularExpression namedRe(QStringLiteral("^([A-Z][^:]{0,40}?):\\s+(.+)$"));
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    static const QRegularExpression indexRe(QStringLiteral("^\\d+$"));

    const QStringList lines = QString(captions).replace(QLatin1String("\r\n"), QLatin1String("\n"))
                                  .split(QLatin1Char('\n'));
    QString time;
    QString lastCue;
    QStringList cue;
    bool inCue = false;
    bool skipping = false;

    const auto flush = [&]() {
        if (cue.isEmpty())
            return;
        QString text = cue.join(QLatin1Char(' ')).simplified();
        cue.clear();
        QString speaker;
        const QRegularExpressionMatch voice = voiceRe.match(text);
        if (voice.hasMatch()) {
            speaker = voice.captured(1).trimmed();
            text = voice.captured(2);
        }
        text = decodeEntities(text.remove(tagRe)).simplified();
        if (speaker.isEmpty()) {
            const QRegularExpressionMatch named = namedRe.match(text);
            if (named.hasMatch()) {
                speaker = named.captured(1).trimmed();
                text = named.captured(2).trimmed();
            }
        }
        if (text.isEmpty())
            return;
        const QString cueText = text;
        // Timed cues of one speaker make one turn; untimed lines with no
        // speaker stay the paragraphs they were.
        if (!turns.isEmpty() && turns.last().speaker == speaker && (inCue || !speaker.isEmpty())) {
            // Rolling captions repeat the cue before, with new words after it.
            if (!lastCue.isEmpty() && text.startsWith(lastCue))
                text = text.mid(lastCue.size()).trimmed();
            lastCue = text.isEmpty() ? lastCue : cueText;
            if (!text.isEmpty() && !turns.last().text.endsWith(text))
                turns.last().text += QLatin1Char(' ') + text;
            return;
        }
        lastCue = cueText;
        turns.append({time, speaker, text});
        if (speakers && !speaker.isEmpty() && !speakers->contains(speaker))
            speakers->append(speaker);
    };

    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            flush();
            inCue = false;
            skipping = false;
            continue;
        }
        if (skipping)
            continue;
        if (!inCue && (line.startsWith(QLatin1String("WEBVTT")) || line.startsWith(QLatin1String("NOTE"))
                       || line == QLatin1String("STYLE") || line == QLatin1String("REGION"))) {
            skipping = true;
            continue;
        }
        if (line.contains(QLatin1String("-->"))) {
            flush();
            time = shortTime(line.left(line.indexOf(QLatin1String("-->"))));
            inCue = true;
            continue;
        }
        if (!inCue) {
            // A cue's number or id comes before its time line.
            if (indexRe.match(line).hasMatch())
                continue;
            // Text with no cue around it: a plain transcript line.
            cue.append(line);
            flush();
            continue;
        }
        cue.append(line);
    }
    flush();

    QStringList paragraphs;
    for (const Turn &turn : std::as_const(turns)) {
        QString paragraph;
        if (timestamps && !turn.time.isEmpty())
            paragraph += QLatin1Char('[') + turn.time + QStringLiteral("] ");
        if (!turn.speaker.isEmpty())
            paragraph += turn.speaker + QStringLiteral(": ");
        paragraphs.append(paragraph + turn.text);
    }
    return paragraphs.join(QStringLiteral("\n\n"));
}

QString Transcriber::engine() {
    if (!meetingRecorder().isEmpty())
        return QStringLiteral("meeting recorder");
    if (!voxtype().isEmpty())
        return QStringLiteral("voxtype");
    if (!whisperCpp().isEmpty())
        return QStringLiteral("whisper.cpp");
    if (!QStandardPaths::findExecutable(QStringLiteral("whisper")).isEmpty())
        return QStringLiteral("whisper");
    return QString();
}

QString Transcriber::engineModel(const QString &model) {
    if (model.isEmpty() && !meetingRecorder().isEmpty())
        return meetingRecorderModel();
    if (model.isEmpty() && !voxtype().isEmpty())
        return voxtypeModel();
    if (!whisperCpp().isEmpty()) {
        const QString path = findModel(model);
        return path.isEmpty() ? QString() : QFileInfo(path).completeBaseName().remove(QStringLiteral("ggml-"));
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("whisper")).isEmpty())
        return model.isEmpty() ? QStringLiteral("base") : model;
    return QString();
}

QString Transcriber::problem(const QString &model) {
    const bool omarchy = QFileInfo::exists(QStringLiteral("/usr/share/omarchy"));
    // Meeting Recorder brings its own model (downloaded on first use).
    if (model.isEmpty() && !meetingRecorder().isEmpty())
        return QString();
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
        return QStringLiteral("Transcribing a recording needs ffmpeg: sudo pacman -S ffmpeg");
    if (model.isEmpty() && !voxtype().isEmpty())
        return QString();
    if (!whisperCpp().isEmpty()) {
        if (!findModel(model).isEmpty())
            return QString();
        return model.isEmpty()
            ? QStringLiteral("whisper.cpp needs a model. Download one with:\n"
                             "  omazettel transcribe --download-model base.en\n"
                             "(small.en or large-v3-turbo are slower but more accurate.)")
            : QStringLiteral("There is no whisper model %1. Download it with:\n"
                             "  omazettel transcribe --download-model %1").arg(model);
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("whisper")).isEmpty())
        return QString();
    if (!QStandardPaths::findExecutable(QStringLiteral("voxtype")).isEmpty())
        return QStringLiteral("Voxtype has no speech model yet. Set one up with: omarchy voxtype model");
    return omarchy
        ? QStringLiteral("Transcribing a recording needs a speech-to-text model. On Omarchy, install Voxtype\n"
                         "(Omarchy's dictation) with: omarchy voxtype install\n"
                         "Or install whisper.cpp (yay -S whisper.cpp) and run: omazettel transcribe --download-model base.en\n"
                         "Caption files (.vtt, .srt) and transcripts (.txt, .docx) work without either.")
        : QStringLiteral("Transcribing a recording needs whisper.cpp: install it, then download a model with:\n"
                         "  omazettel transcribe --download-model base.en\n"
                         "Caption files (.vtt, .srt) and transcripts (.txt, .docx) work without it.");
}

QString Transcriber::modelsFolder() {
    QString base = qEnvironmentVariable("XDG_DATA_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/omazettel/whisper");
}

QString Transcriber::findModel(const QString &wanted) {
    if (!wanted.isEmpty()) {
        if (QFileInfo(wanted).isFile())
            return QFileInfo(wanted).absoluteFilePath();
        for (const QString &folder : modelFolders()) {
            const QString path = folder + QStringLiteral("/ggml-") + wanted + QStringLiteral(".bin");
            if (QFileInfo(path).isFile())
                return path;
        }
        return QString();
    }
    const QString fromEnvironment = qEnvironmentVariable("OMAZETTEL_WHISPER_MODEL");
    if (!fromEnvironment.isEmpty())
        return findModel(fromEnvironment);
    for (const QString &name : preferredModels) {
        const QString path = findModel(name);
        if (!path.isEmpty())
            return path;
    }
    for (const QString &folder : modelFolders()) {
        const QFileInfoList any = QDir(folder).entryInfoList({QStringLiteral("ggml-*.bin")}, QDir::Files);
        if (!any.isEmpty())
            return any.first().absoluteFilePath();
    }
    return QString();
}

bool Transcriber::downloadModel(const QString &name, QString *path, QString *error) {
    static const QRegularExpression nameRe(QStringLiteral("^[a-z0-9.-]+$"));
    if (!nameRe.match(name).hasMatch()) {
        *error = QStringLiteral("Model names look like base.en, small or large-v3-turbo.");
        return false;
    }
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty()) {
        *error = QStringLiteral("Downloading a model needs curl.");
        return false;
    }
    QDir().mkpath(modelsFolder());
    *path = modelsFolder() + QStringLiteral("/ggml-") + name + QStringLiteral(".bin");
    const QString partial = *path + QStringLiteral(".part");
    const QString url = QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%1.bin").arg(name);
    QString errors;
    const int code = run(curl, {QStringLiteral("-fL"), QStringLiteral("--silent"), QStringLiteral("--show-error"),
                                QStringLiteral("-o"), partial, url}, &errors);
    if (code != 0) {
        QFile::remove(partial);
        *error = QStringLiteral("Could not download %1: %2").arg(url, errors.isEmpty() ? QStringLiteral("curl failed") : errors);
        return false;
    }
    QFile::remove(*path);
    if (!QFile::rename(partial, *path)) {
        *error = QStringLiteral("Could not save %1.").arg(*path);
        return false;
    }
    return true;
}

Transcriber::Result Transcriber::transcribe(const QString &file, const QString &model,
                                            const QString &language, bool timestamps, QString *error) {
    Result result;
    const QString meeting = meetingRecorderFolder(file);
    if (!meeting.isEmpty())
        return readMeeting(meeting, timestamps, error);
    if (file.startsWith(QLatin1String("meeting-recorder:"))) {
        *error = QStringLiteral("There is no meeting with a transcript in %1 yet.").arg(meetingsFolder());
        return result;
    }
    if (file.startsWith(QLatin1String("voxtype:"))) {
        const QString id = file.mid(8).isEmpty() ? QStringLiteral("latest") : file.mid(8);
        const QString program = QStandardPaths::findExecutable(QStringLiteral("voxtype"));
        if (program.isEmpty()) {
            *error = QStringLiteral("Voxtype is not installed (omarchy voxtype install).");
            return result;
        }
        QString errors;
        QByteArray output;
        QStringList arguments{QStringLiteral("meeting"), QStringLiteral("export"), id, QStringLiteral("-f"),
                              QStringLiteral("text"), QStringLiteral("--speakers")};
        if (timestamps)
            arguments << QStringLiteral("--timestamps");
        if (run(program, arguments, &errors, &output) != 0) {
            *error = QStringLiteral("Voxtype could not export meeting %1: %2").arg(id, lastLines(errors));
            return result;
        }
        result.text = fromCaptions(QString::fromUtf8(output), false, &result.speakers);
        result.engine = QStringLiteral("voxtype meeting");
        return result;
    }
    const QFileInfo info(file);
    if (!info.isFile()) {
        *error = QStringLiteral("%1 is not a file.").arg(file);
        return result;
    }
    const QString suffix = info.suffix().toLower();

    if (suffix == QLatin1String("vtt") || suffix == QLatin1String("srt") || suffix == QLatin1String("sbv")) {
        result.text = fromCaptions(readAll(file), timestamps, &result.speakers);
        result.engine = QStringLiteral("captions");
        return result;
    }
    if (suffix == QLatin1String("txt") || suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) {
        result.text = readAll(file).trimmed();
        result.engine = QStringLiteral("text");
        return result;
    }
    if (suffix == QLatin1String("docx") || suffix == QLatin1String("odt") || suffix == QLatin1String("rtf")
            || suffix == QLatin1String("html")) {
        const QString pandoc = QStandardPaths::findExecutable(QStringLiteral("pandoc"));
        if (pandoc.isEmpty()) {
            *error = QStringLiteral("Reading .%1 transcripts needs pandoc.").arg(suffix);
            return result;
        }
        QString errors;
        QByteArray output;
        if (run(pandoc, {file, QStringLiteral("-t"), QStringLiteral("plain"), QStringLiteral("--wrap=none")},
                &errors, &output) != 0) {
            *error = QStringLiteral("pandoc could not read %1: %2").arg(info.fileName(), errors);
            return result;
        }
        // Teams' Word transcripts put each speaker and time on a line of
        // their own; the caption reader joins them into turns.
        result.text = fromCaptions(QString::fromUtf8(output), timestamps, &result.speakers);
        result.engine = QStringLiteral("pandoc");
        return result;
    }

    // A recording.
    const QString needed = problem(model);
    if (!needed.isEmpty()) {
        *error = needed;
        return result;
    }
    // Meeting Recorder tells the voices apart ("Speaker 1", "Speaker 2").
    if (const QString recorder = meetingRecorder(); model.isEmpty() && !recorder.isEmpty()) {
        QStringList arguments{QStringLiteral("transcribe-file"), info.absoluteFilePath()};
        if (!language.isEmpty())
            arguments << QStringLiteral("--language") << language;
        QString errors;
        QByteArray output;
        if (run(recorder, arguments, &errors, &output) != 0) {
            *error = QStringLiteral("Meeting Recorder could not transcribe %1: %2").arg(info.fileName(), lastLines(errors));
            return result;
        }
        result.text = fromMeetingRecorder(QString::fromUtf8(output), timestamps, &result.speakers);
        result.engine = QStringLiteral("meeting recorder");
        result.model = meetingRecorderModel();
        return result;
    }
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString vox = model.isEmpty() ? voxtype() : QString();
    const QString cpp = whisperCpp();
    const QString python = QStandardPaths::findExecutable(QStringLiteral("whisper"));
    QTemporaryDir work;
    if (!work.isValid()) {
        *error = QStringLiteral("Could not make a temporary folder.");
        return result;
    }

    if (!vox.isEmpty()) {
        // Voxtype reads 16 kHz mono WAV.
        const QString wav = work.filePath(QStringLiteral("audio.wav"));
        QString errors;
        if (run(ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"),
                         QStringLiteral("error"), QStringLiteral("-i"), file, QStringLiteral("-vn"),
                         QStringLiteral("-ar"), QStringLiteral("16000"), QStringLiteral("-ac"),
                         QStringLiteral("1"), QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), wav},
                &errors) != 0) {
            *error = QStringLiteral("ffmpeg could not read %1: %2").arg(info.fileName(), lastLines(errors));
            return result;
        }
        QByteArray output;
        if (run(vox, {QStringLiteral("-q"), QStringLiteral("transcribe"), wav}, &errors, &output) != 0) {
            *error = QStringLiteral("Voxtype could not transcribe %1: %2").arg(info.fileName(), lastLines(errors));
            return result;
        }
        result.text = voxtypeText(output);
        result.engine = QStringLiteral("voxtype");
        result.model = voxtypeModel();
        return result;
    }

    if (!cpp.isEmpty()) {
        const QString modelPath = findModel(model);
        const QString wav = work.filePath(QStringLiteral("audio.wav"));
        QString errors;
        if (run(ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"),
                         QStringLiteral("error"), QStringLiteral("-i"), file, QStringLiteral("-vn"),
                         QStringLiteral("-ar"), QStringLiteral("16000"), QStringLiteral("-ac"),
                         QStringLiteral("1"), QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), wav},
                &errors) != 0) {
            *error = QStringLiteral("ffmpeg could not read %1: %2").arg(info.fileName(), lastLines(errors));
            return result;
        }
        const QString base = work.filePath(QStringLiteral("transcript"));
        QStringList arguments{QStringLiteral("-m"), modelPath, QStringLiteral("-f"), wav,
                              QStringLiteral("-of"), base, QStringLiteral("-np"),
                              QStringLiteral("-t"), QString::number(qMax(1, QThread::idealThreadCount()))};
        arguments << (timestamps ? QStringLiteral("-osrt") : QStringLiteral("-otxt"));
        if (!language.isEmpty())
            arguments << QStringLiteral("-l") << language;
        if (run(cpp, arguments, &errors) != 0) {
            *error = QStringLiteral("whisper.cpp failed: %1").arg(lastLines(errors));
            return result;
        }
        const QString text = readAll(base + (timestamps ? QStringLiteral(".srt") : QStringLiteral(".txt")));
        result.text = timestamps ? fromCaptions(text, true) : text.simplified().isEmpty() ? QString() : text.trimmed();
        result.engine = QStringLiteral("whisper.cpp");
        result.model = QFileInfo(modelPath).completeBaseName().remove(QStringLiteral("ggml-"));
        return result;
    }

    // OpenAI's whisper (Python).
    const QString name = model.isEmpty() ? QStringLiteral("base") : model;
    QStringList arguments{file, QStringLiteral("--model"), name, QStringLiteral("--output_dir"), work.path(),
                          QStringLiteral("--output_format"), timestamps ? QStringLiteral("srt") : QStringLiteral("txt"),
                          QStringLiteral("--verbose"), QStringLiteral("False")};
    if (!language.isEmpty())
        arguments << QStringLiteral("--language") << language;
    QString errors;
    if (run(python, arguments, &errors) != 0) {
        *error = QStringLiteral("whisper failed: %1").arg(lastLines(errors));
        return result;
    }
    const QString text = readAll(work.filePath(info.completeBaseName()
                                               + (timestamps ? QStringLiteral(".srt") : QStringLiteral(".txt"))));
    result.text = timestamps ? fromCaptions(text, true) : text.trimmed();
    result.engine = QStringLiteral("whisper");
    result.model = name;
    return result;
}

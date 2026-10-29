#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

// Turning a meeting into text a note can be written from: caption files
// (Teams, Zoom and Meet .vtt, .srt), Word transcripts (.docx, through
// pandoc), plain text, or a recording (audio or video, through ffmpeg and
// whisper.cpp, or OpenAI's whisper if that is what is installed).
namespace Transcriber {

struct Result {
    QString text;
    QString engine;       // captions, pandoc, text, whisper.cpp, whisper, meeting recorder.
    QStringList speakers; // Named in the captions, in order of first word.
    QString model;        // The whisper model used, for recordings.
    // From a Meeting Recorder meeting:
    QString title;        // The meeting's name.
    QDateTime started;    // When it began.
    int durationSecs = 0;
    QStringList chapters; // "[12:34] Title", from its agent.
    QString folder;       // The meeting folder, with the recording.
};

// Caption cues as "Speaker: words" paragraphs, one per turn, with the turn's
// start time in front when timestamps is set.
QString fromCaptions(const QString &captions, bool timestamps, QStringList *speakers = nullptr);

// The speech-to-text program found: voxtype (Omarchy's dictation, with its
// model), whisper.cpp or whisper; or "".
QString engine();
// What the engine needs before it can transcribe a recording, with how to
// get it, or "".
QString problem(const QString &model = QString());
// The model the engine will use, for messages.
QString engineModel(const QString &model = QString());
// Where downloaded whisper.cpp models go: ~/.local/share/omazettel/whisper.
QString modelsFolder();
// A model file: wanted as a path or a name (base.en), else
// $OMAZETTEL_WHISPER_MODEL, else the best one already downloaded.
QString findModel(const QString &wanted);
// Downloads a whisper.cpp model (tiny, base.en, small, large-v3-turbo, ...).
bool downloadModel(const QString &name, QString *path, QString *error);

// Meeting Recorder (omarchy-meeting-recorder) keeps each meeting in a folder
// of ~/Documents/Meetings: the audio, transcript.md and a .meeting-recorder
// file with its name, start, speakers and chapters. Its folder, that file,
// a file in the folder (such as transcript.md) or meeting-recorder:latest
// (the newest meeting with a transcript) gives the folder; else "".
QString meetingRecorderFolder(const QString &file);
QString meetingsFolder();
// Meeting Recorder's transcript Markdown ("**[01:23] Ann:** words") as
// "Ann: words" paragraphs, with the time in front when timestamps is set.
// speakers gets the names given (not "You", "Remote 2", "Speaker 1").
QString fromMeetingRecorder(const QString &markdown, bool timestamps, QStringList *speakers = nullptr);
// "You", "Remote 2", "Speaker 1": a voice nobody named yet.
bool unnamedSpeaker(const QString &name);

// file may also be voxtype:latest or voxtype:<id>, a meeting recorded with
// `voxtype meeting start`, which comes with speaker labels; or a Meeting
// Recorder meeting (see meetingRecorderFolder). A recording is transcribed
// with Meeting Recorder when it is installed (it tells the voices apart),
// else Voxtype or whisper.cpp.
// Throws nothing; error is set and the result empty on failure.
Result transcribe(const QString &file, const QString &model, const QString &language,
                  bool timestamps, QString *error);

}

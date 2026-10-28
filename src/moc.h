#pragma once

#include <QString>
#include <QStringList>

// Maps of content (MOCs) for the AI-first layout. 03-projects/projects-moc.md
// links each project's map, and each project folder's map
// (03-projects/vdem/vdem-moc.md) links every note in that project, grouped
// by subfolder. Transcripts are left out; their meeting notes link them.
//
// OmaZettel rewrites only the list between the two marker comments, so
// anything written above or below it stays. The list is sorted the same way
// on every computer, so synced copies agree.
namespace Moc {

// Whether maps are kept up to date (Settings; on by default).
bool enabled();
void setEnabled(bool on);

// The top map's file name, without .md.
QString topName();
// A project folder's map file name, without .md: "vdem" -> "vdem-moc".
QString nameFor(const QString &projectFolder);

// One of the maps OmaZettel keeps: 03-projects/projects-moc.md or a
// project's 03-projects/<project>/<project>-moc.md.
bool isMap(const QString &relativePath);
// A transcript: in a "transcripts" folder, or a note named "... transcript".
bool isTranscript(const QString &relativePath);

// Brings the maps under 03-projects up to date and returns the files it
// wrote (or, with dryRun, would write), relative to root. Writes nothing
// unless the notes folder has the layout. Unchanged maps are not touched.
QStringList update(const QString &root, bool dryRun = false);

}

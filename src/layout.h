#pragma once

#include <QString>
#include <QStringList>

// The numbered folder layout a new notes folder starts with, so people and
// AI agents find things in the same places:
//
//   01-system/     rules agents follow (SYSTEM-RULES.md) and how you work
//                  (HOW-I-WORK.md); agents read these first
//   02-context/    stable background and reference (literature notes)
//   03-projects/   active work, one folder per project; daily and meeting notes
//   04-outputs/    finished work
//   99-archive/    old material, still searched
//
// The numbers give a reading order: rules, context, work, results; 99 keeps
// the archive last. A notes folder made before this keeps its own folders
// until `omazettel setup-layout` moves them.
namespace Layout {

inline constexpr auto systemFolder = "01-system";
inline constexpr auto contextFolder = "02-context";
inline constexpr auto projectsFolder = "03-projects";
inline constexpr auto outputsFolder = "04-outputs";
inline constexpr auto archiveFolder = "99-archive";
inline constexpr auto rulesFile = "01-system/SYSTEM-RULES.md";
inline constexpr auto howIWorkFile = "01-system/HOW-I-WORK.md";

// The five folders, in order.
QStringList folders();

// How new, empty notes folders start, chosen on first run and in Settings:
// "numbered" (AI-first, the layout above), "traditional" (the templates'
// Daily, Meetings and Literature folders and nothing else), or "" when
// nobody has chosen yet.
QString choice();
void setChoice(const QString &value);
// Whether an empty notes folder gets the layout: yes unless traditional
// was chosen, so the command line and agents get it by default.
bool wanted();

// Whether the notes folder uses the layout (it has 01-system and 03-projects).
bool present(const QString &root);

// Whether a notes folder has nothing in it yet but hidden files, so the
// layout can be made there without disturbing anything.
bool isEmpty(const QString &root);

// Makes the folders and starter rules files that are missing; never changes
// a file that is there. Returns what it made, relative to root.
QStringList create(const QString &root);

// Where the built-in templates put their notes: inside the layout when the
// notes folder has it, otherwise the older top-level folders.
QString dailyFolder(const QString &root);
QString meetingsFolder(const QString &root);
QString literatureFolder(const QString &root);

// The older top-level folders setup-layout moves, and where to.
struct Move {
    QString from;
    QString to;
};
QList<Move> moves();

// One paragraph for agents: the folders and how to name files.
QString guide();

QString starterRules();
QString starterHowIWork();

}

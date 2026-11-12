OmaZettel for Windows (early)
=============================

OmaZettel is a notes app for a folder of plain Markdown notes, with
[[wiki links]], tags, a dashboard and git sync.
https://github.com/mcsmith3/omazettel

Running it
----------
Keep this folder somewhere of your own, such as
%LOCALAPPDATA%\Programs\OmaZettel, and open omazettel.exe. Right-click it
and pick "Pin to Start" or "Pin to taskbar" to keep it handy. Not in
Program Files (x86): that is for 32-bit programs, and updating there needs
administrator rights.

Windows may say it protected your PC the first time, because this download
isn't signed: click "More info", then "Run anyway".

The command line
----------------
Add this folder to your PATH (Settings > System > About > Advanced system
settings > Environment Variables > Path > Edit > New). Then in a new
terminal:

    omazettel tasks
    omazettel search "budget"
    omazettel --help

omazettel.com is the command line's half: terminals find it before
omazettel.exe. In Git Bash, type omazettel.com.

What else it uses
-----------------
- Sync: Git for Windows (https://git-scm.com/download/win).
- Export to PDF, Word and HTML: pandoc (winget install JohnMacFarlane.Pandoc).

Notes you already sync from another computer
--------------------------------------------
Do this before opening OmaZettel the first time, so it doesn't start a
separate notes history of its own. Clone them with plain line endings, then
choose that folder in OmaZettel:

    git clone -c core.autocrlf=false https://github.com/you/notes.git %USERPROFILE%\Notes

Without -c core.autocrlf=false, Git for Windows changes every note's line
endings, and OmaZettel won't save notes automatically. Note names with
: * ? " < > | can't be checked out on Windows: on your other computer, run
`omazettel tidy-names --apply` and sync first.

Keep the notes outside OneDrive (the clone above goes to your user folder).
OneDrive and git sync would both change the same files, which can damage
the notes' history and slows OmaZettel down.

If OmaZettel is slow to start, run it from a terminal with timings and send
them to us:

    $env:OMAZETTEL_TIMING = "1"
    Start-Process .\omazettel.exe -RedirectStandardError timing.txt

Updating and removing
---------------------
Update: close OmaZettel and replace this folder with the new download.
Remove: delete this folder. Your notes are untouched. Settings live in the
registry under HKEY_CURRENT_USER\Software\omazettel.

OmaZettel is still early, so keep your notes folder backed up. It is a
community project, not an official Omacom/Omarchy app. GPL-3.0-or-later;
see LICENSE and THIRD-PARTY-NOTICES.md.

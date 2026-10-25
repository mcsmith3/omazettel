# OmaZettel

**Zettelkasten notes for [Omarchy](https://omarchy.org), in plain Markdown files.**

OmaZettel is a native Qt/QML notes app for linking ideas: `[[wiki links]]`,
backlinks, tags, note templates, search, pandoc export with citations, and git
sync with version history. Your notes stay ordinary `.md` files in a folder you
choose, so VS Code, Neovim, git and pandoc keep working on them too. Its colors
follow your Omarchy theme, the way Neovim and VS Code do.

![A note with its outline, tags and backlinks (Tokyo Night theme)](docs/screenshots/notes.png)

OmaZettel is an independent fork of [Omawrite](https://github.com/omacom/omawrite)
by David Heinemeier Hansson, extended into a notes system. It is not made or
endorsed by Omacom or the Omarchy project.

> **Status: 0.8.2, early.** Everything below works and is covered by tests, but
> OmaZettel is young: expect rough edges, and keep your notes folder in git or
> another backup. Bug reports and ideas are welcome in the issues.

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Install](#install)
- [Getting started](#getting-started)
- [Writing](#writing)
- [Folder layout](#folder-layout)
- [Links, backlinks and tags](#links-backlinks-and-tags)
- [Note templates](#note-templates)
- [Search and outline](#search-and-outline)
- [Export](#export)
- [Citations](#citations)
- [Sync and history](#sync-and-history)
- [Command line and AI assistants](#command-line-and-ai-assistants)
- [Colors and themes](#colors-and-themes)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Where things are kept](#where-things-are-kept)
- [Building from source](#building-from-source)
- [Contributing](#contributing)
- [Credits and license](#credits-and-license)

## Features

- **A notes folder, not a database.** A sidebar of your folders and Markdown
  notes: create, rename, move by drag and drop, move to trash.
- **Links that keep working.** `[[Note]]`, `[[folder/Note]]`, `[[Note#Heading]]`
  and `[[Note|label]]`, with completion as you type `[[`. Following a link to a
  note that doesn't exist creates it. Renaming or moving a note rewrites every
  link to it.
- **Backlinks, tags and an outline** for the open note in a side panel, and a
  Tags view of the whole folder.
- **Note templates** (Note, Meeting, Daily, Literature note, or your own) with
  placeholders and a pandoc YAML header, so notes export with the right title,
  author and date. Ctrl+Shift+D opens today's daily note.
- **Search across notes** with phrases and tag filters, ignoring case and
  accents, plus **Ctrl+O** to jump to any note by name and a **command palette**
  (Ctrl+Shift+P).
- **Export with pandoc** to PDF, Word, OpenDocument, HTML, LaTeX and anything
  else pandoc writes, using your own templates and reference documents. Export
  one note or a whole folder as one document.
- **Citations** from a BibTeX or CSL JSON bibliography (Zotero's Better BibTeX
  works well), with `[@key]` completion and a formatted reference list on export.
- **Git sync and version history**: sync with any git remote or a private
  GitHub repository you create in one click, resolve conflicts side by side,
  and see or restore any earlier version of a note. Conflict copies from
  Syncthing, Nextcloud and Dropbox are spotted and resolved the same way.
- **A command line and AI tools.** `omazettel search`, `new`, `append`,
  `daily`, `capture`, `tasks`, `export` and more work from a terminal or a
  script without opening a window. The same commands serve AI assistants as
  an MCP server and a Claude skill: "turn this meeting recording into a note",
  "add this to my Atlas note". Meeting captions and recordings become text
  on your own computer.
- **Your theme's colors**, through an Omarchy theme template, or the fixed
  Oma palette shared with OmaShow and the other Oma apps.
- From Omawrite: a calm editor that hides Markdown markers away from the caret,
  auto-save, crash recovery, and warnings when a file changes underneath you.

## Requirements

OmaZettel is made for **[Omarchy](https://omarchy.org)**, which already has
everything it needs to run. On another **Arch Linux** desktop, install the
packages below. (Other distributions should work from source with the same
pieces, but are untested.)

**To run** (required):

| Package | Why |
| --- | --- |
| `qt6-base` 6.6 or newer | Qt, the toolkit OmaZettel is built with |
| `qt6-declarative` 6.6 or newer | Qt Quick, for the interface |
| `xdg-desktop-portal` and a backend (`xdg-desktop-portal-hyprland` or `-gtk`) | File and folder pickers |
| A monospace font | The editor and interface use your desktop's monospace font (`fc-match monospace`); on Omarchy that is JetBrains Mono, set with `omarchy font set` |

**To build** (required, from the source): `base-devel` (for `gcc`, `make`
and `makepkg`) and `git`.

```sh
sudo pacman -S --needed base-devel git qt6-base qt6-declarative xdg-desktop-portal
```

**For some features** (optional, install what you use):

| Feature | Package | Notes |
| --- | --- | --- |
| Export (PDF, Word, OpenDocument, HTML, …) | `pandoc-cli` | Without it, everything else works; Export says pandoc is missing |
| PDF export | `texlive-xetex` (recommended) or `texlive-latex` | XeLaTeX handles any font and script; pdflatex covers Western text |
| Version history and sync | `git` | Already installed if you built from source |
| Creating a private GitHub repository to sync with | `github-cli` | Then run `gh auth login` and `gh auth setup-git` once |
| Editing Word or OpenDocument export styles | `libreoffice-fresh` | Opens reference documents |
| Citations | none | Use a `.bib` or CSL JSON file, for example from Zotero with Better BibTeX |
| Transcribing meeting recordings | `ffmpeg`, and Voxtype (Omarchy's dictation: `omarchy voxtype install`) or `whisper.cpp` (AUR) | With whisper.cpp, download a model once: `omazettel transcribe --download-model base.en`. Caption files (.vtt, .srt) and text transcripts need neither |
| Meeting notes written by AI | An AI you already have: a coding agent (Claude Code, Codex, Gemini CLI, OpenCode, ...), Ollama, or an API key | On Omarchy, your default agent (`omarchy default agent`) is used. `omazettel ai` shows what is set up |

```sh
sudo pacman -S --needed pandoc-cli texlive-xetex github-cli
```

## Install

OmaStore can install OmaZettel from [its app page](omastore://mcsmith3/omazettel),
or find it by searching for "OmaZettel" in the store. The store installs it in
your home directory. Qt 6 and an XDG desktop portal must already be installed
as listed in [Requirements](#requirements); optional tools are needed only for
their corresponding features.

Or, on Omarchy or Arch Linux, paste this in a terminal (it asks for your password):

```sh
curl -fsSL https://raw.githubusercontent.com/mcsmith3/omazettel/master/install.sh | bash
```

It downloads the latest release's package, checks it against the release's
`SHA256SUMS`, and installs it with pacman, so OmaZettel appears in the app
launcher. On machines without a ready-made package (such as ARM) it builds one
from the release's source instead.

**Update:** run the same line again. **Uninstall:** `sudo pacman -R omazettel`;
your notes are untouched.

Use one way or the other, not both: the store's copy (in `~/.local/bin`) would
hide the system one, and each updates only its own.

To install from a clone instead (for example, to try unreleased changes):

```sh
git clone https://github.com/mcsmith3/omazettel.git
cd omazettel
./bin/install
```

## Getting started

1. Open **OmaZettel** from the app launcher.
2. Click **Choose folder…** and pick a folder of Markdown notes, or an empty
   folder to start one. OmaZettel reopens it, at the note you had open, next time.
3. Press **Ctrl+N** for a new note, pick a template, and give it a title.
4. Type `[[` to link to another note. Ctrl+click a link to follow it.
5. Press **Ctrl+Shift+P** for every command, and **Ctrl+,** for settings.

OmaZettel keeps to one window. Starting it again, running `omazettel note.md`,
or opening a Markdown file from the file manager brings that window to the
front and opens the file there (saving or asking about the open note first).
For a second window anyway, start it with `OMAZETTEL_NEW_INSTANCE=1 omazettel`.

To make OmaZettel the default app for Markdown files:

```sh
xdg-mime default omazettel.desktop text/markdown text/x-markdown
```

## Writing

- Markdown markers (`**`, `_`, `` ` ``, link addresses) fade away when the caret
  is elsewhere and come back when you edit them. Headings, lists, quotes and
  code blocks are styled as you type.
- Once a note has a file, edits are saved a moment after you stop typing, so
  Ctrl+S is rarely needed. Untitled drafts are never saved behind your back, but
  are recovered after a crash.
- If another app changes the open note, OmaZettel asks before replacing your
  text, and stops auto-saving until you choose.
- Ctrl+B, Ctrl+I and Ctrl+Shift+X toggle bold, italic and strikethrough; Ctrl+K
  inserts a link; Tab and Shift+Tab indent list items; Enter continues lists.
- The editor uses your desktop's monospace font, so changing it with
  `omarchy font set` changes OmaZettel too (at its next start). The **A**
  button in the footer picks a different font just for writing. Text follows
  `omarchy display text size`, and Ctrl+= / Ctrl+- / Ctrl+scroll change it on
  top of that.

## Folder layout

A new, empty notes folder starts with a numbered layout, so you and AI agents
find things in the same places:

```
01-system/      SYSTEM-RULES.md (rules agents follow) and HOW-I-WORK.md (about you)
02-context/     stable background and reference; literature notes in 02-context/literature
03-projects/    active work, one folder per project; daily/ and meetings/ live here
04-outputs/     finished work
99-archive/     old material, still searched
```

The numbers set the reading order: rules first, then context, the work, and
the results. 99 keeps the archive at the end. Agents using the
`omazettel` command or the MCP server are told to read the two `01-system`
files before changing anything (`omazettel info` returns both), so put your
own rules and background there. OmaZettel writes starter versions once and
never changes them after that.

File names say when, what and how far along. Anything tied to a day starts with
the date (`2026-10-25-kickoff`; the Daily and Meeting templates do this),
drafts and outputs end with their status (`2026-10-25-q4-plan-final`), and
lasting ideas keep plain topic names so links read well.

A notes folder that already has notes keeps its own folders. To move it to
the layout:

```sh
omazettel setup-layout            # shows what would change, changes nothing
omazettel setup-layout --apply    # does it
```

This moves `Daily`, `Meetings`, `Literature` and `Archive` into the layout,
points the templates there and rewrites links to match. It adds what's
missing and never overwrites a file. Other folders stay where they are, and
it lists them; move each one with `omazettel move <folder> 02-context` (or
`03-projects`). Empty layout folders hold a hidden `.keep` file, so sync keeps
them on every computer.

## Links, backlinks and tags

- **Link** with `[[Note name]]`, `[[folder/Note]]`, `[[Note#Heading]]`,
  `[[#Heading]]` (in the same note) or `[[Note|label]]`, which reads as just
  "label". Typing `[[` suggests notes; Up, Down and Enter pick one.
- **Follow** a link with Ctrl+click, or Ctrl+Enter with the caret in it. Web
  links open in the browser. Alt+Left and Alt+Right go back and forward.
- **Links to new notes** create those notes when the note is saved, next to it
  or at the path the link names, from the Note template. The link you are still
  typing waits. Turn this off in Settings to create notes only on Ctrl+click.
- **File names** use hyphens between words (`2026-10-22-Cutover-plan.md`);
  the title inside the note keeps its spaces. Settings can switch to
  underscores or spaces. Links don't mind which: `[[Cutover plan]]`,
  `[[cutover-plan]]` and `[[Cutover_plan]]` all find the same note, so older
  notes with spaces keep working. To rename those too, run
  `omazettel tidy-names` to see the plan, then `omazettel tidy-names --apply`.
- **Renaming or moving** a note, or a folder of notes, updates every link to it
  and keeps `#heading` and `|label`. The status bar says how many links changed.
- **Tag** with `#tag` (nested: `#project/client`), or `tags: [a, b]` or
  `keywords:` in the YAML header.
- The **right panel** (Ctrl+Shift+B) shows the open note's outline, its tags,
  and its **backlinks**: every note that links to it, with the line it's on.
  The sidebar's **Tags** tab lists every tag with its count.
- The note index notices edits made in other apps when you switch back.

## Note templates

**Ctrl+N** asks for a template and a title; OmaZettel remembers the template you
used last. **Ctrl+Shift+D** opens today's daily note, making it the first time.

Templates are Markdown files in `.omazettel/note-templates/` inside your notes
folder; OmaZettel adds **Note** (a Zettelkasten note with an ID), **Meeting**,
**Daily** and **Literature note** the first time. Edit them from … →
Edit note templates, or add your own. Each starts with a pandoc YAML header:

```markdown
<!-- omazettel: name="{{date}} {{title}}" folder="03-projects/meetings" -->
---
title: "{{title}}"
author: "{{author}}"
date: "{{date}}"
tags: [meeting]
---

## Agenda

- {{cursor}}
```

- **Placeholders:** `{{title}}`, `{{author}}` (set in Settings),
  `{{date}}`, `{{date:dddd d MMMM yyyy}}` (any Qt date format), `{{time}}`,
  `{{datetime}}`, `{{weekday}}`, `{{id}}` (yyyyMMddHHmmss), `{{folder}}` and
  `{{cursor}}`, where typing starts. Values in the YAML header are escaped.
- **The first line** (optional, left out of the note) sets the file `name`
  (default `{{title}}`), the `folder` it goes in, and `existing="open"` to open a
  note of that name instead of making a second one, as the Daily template does.
- A template that doesn't use `{{title}}` doesn't ask for one.

## Search and outline

![Search across notes](docs/screenshots/search.png)

- **Ctrl+Shift+F** searches every note. Words must all appear, in any order;
  `"quoted phrases"` match as written; `#tag` or `tag:name` keeps notes with
  that tag or one under it. Case and accents don't matter. Title matches come
  first, each note shows its matching lines, and clicking one opens the note
  with the match selected.
- **Ctrl+O** jumps to a note by name (Enter on a new name creates it), and
  **Ctrl+F** / **Ctrl+H** find and replace within the note.
- The **outline** in the right panel follows your headings as you type and
  marks the section you're in; click one to go there.

## Export

![The export dialog](docs/screenshots/export.png)

**Ctrl+Shift+E** (or right-click a note → Export…) exports with
[pandoc](https://pandoc.org). Pick a profile and where to save. The note's YAML
header fills in the title, author and date; `[[links]]` become their text;
unsaved changes are included.

The setup follows Zettlr's custom templates and reference documents, but uses
pandoc's own files, so the same export works from a terminal:

- **Profiles** are pandoc [defaults files](https://pandoc.org/MANUAL.html#defaults-files)
  in `.omazettel/export/`: PDF, Word, OpenDocument, HTML and LaTeX source to
  start with. Any pandoc option works (`toc: true`, `number-sections: true`,
  `variables:`, …). **Edit** opens one; **New…** copies it.
- **Templates** (`.tex`, `.html`) lay out PDF and HTML exports; **reference
  documents** (`.docx`, `.odt`, `.pptx`) carry Word, LibreOffice and PowerPoint
  styles. **New ▾** makes one from pandoc's default in `.omazettel/templates/`,
  and **New profile with this** makes a profile that uses it.
- From a terminal: `pandoc -d .omazettel/export/PDF.yaml note.md -o note.pdf`.

**Export a folder** (right-click a folder → Export folder…) to make one document
from all its notes and subfolders, like a Zettlr project: untick notes, reorder
them, and give it a title and author. Each note's title becomes a chapter
heading. Choices are saved in the folder's `.omazettel-project.yaml`.

Exports are saved next to the note, or, if you choose in Settings, in an
`Exports` folder that is kept out of git sync.

PDF export needs a LaTeX engine. The PDF profile uses XeLaTeX when
`texlive-xetex` is installed (any font, any script), and pdflatex otherwise.

## Citations

In … → **Citations…**, choose a bibliography (BibTeX `.bib` or CSL JSON, such as
the file Zotero's Better BibTeX keeps exported; OmaZettel reloads it when it
changes) and, if you like, a citation style (`.csl`, from the
[Zotero Style Repository](https://www.zotero.org/styles); pandoc's default is
Chicago author-date).

Cite with `[@key]`, `[@key, p. 4]` or `[see @one; @two]`. Typing `[@` suggests
references by key, author, year or title. Exports of notes that cite something
format the citations and add a reference list. A note may also name its own
`bibliography:` in its header.

## Sync and history

Your notes stay plain files on your computer. To keep them on other devices, or
share them with a few people taking turns, OmaZettel has git built in (… →
**Sync & history…**, or click the status line at the foot of the sidebar):

1. **Turn on version history.** Every version of every note is kept with git.
2. **Add a remote** on any git server: GitHub, GitLab, Codeberg, Forgejo,
   Gitea, Gogs, or your own. Paste its address, or pick one of those and click
   **Create** to make a private repository there:
   - **GitHub** uses the GitHub CLI (`gh auth login`), or a token.
   - **GitLab** (gitlab.com or self-hosted) needs a personal access token with
     the `api` scope.
   - **Codeberg, Forgejo, Gitea and Gogs** need the server's address (for your
     own) and an access token with repository write access.

   Signing in: an SSH address (`git@host:you/notes.git`) uses your SSH key; an
   https address uses gh for GitHub, or an access token you save under **Sign in
   with a token**, kept in your desktop keyring. **Test** checks that this
   computer can read and write the remote without changing anything. Git never
   stops to ask for a password.
3. **Sync** happens when OmaZettel starts, every 5 minutes (adjustable), and on
   **Ctrl+Alt+S** or the sync button. It saves your changes as a version,
   merges the other side's (never rewriting history) and sends yours. The open
   note is saved first and reloads quietly if the sync changed it.

**When a sync fails**, the sidebar's status line says so; click it (or sync
by hand) and Sync & history says why and what to do: sign in, add an SSH key,
trust a server's certificate, move a file that is too big, and so on.
**Show git's message** has git's own words, and the sync log
(`~/.local/state/omazettel/sync.log`) keeps every sync. If GitHub refuses a push
because it would publish your private email, **Use my GitHub no-reply email**
switches these notes to your `…@users.noreply.github.com` address, re-signs
the versions not sent yet, and syncs. **Versions are saved as** shows and
changes the name and email used.

From a terminal, `omazettel sync --check` tests the connection, `--status`
shows what is waiting, `--fix-email` applies the no-reply fix, and
`--create gitlab --server https://git.example.com --token -` makes a repository
(the token is read from standard input).

**Conflicts.** When the same note changed on two devices, the sync stops and
lists it. **Compare…** shows both versions line by line; choose **Keep mine**,
**Keep theirs**, **Keep both** (theirs saved beside it as
"Note (other version).md"), or **Edit** the note to combine them and click
**Done editing**. **Undo the merge** leaves everything as it was.

**History.** Right-click a note → **History…** lists its saved versions,
compares any of them with the note as it is now, and restores one (Ctrl+Z takes
it back).

**Syncthing, Nextcloud, Dropbox or ownCloud** work too: point them at the notes
folder. The conflict copies they leave (`Note.sync-conflict-….md`,
`Note (conflicted copy …).md`, …) are kept out of links and search, listed in
Sync & history, and resolved the same way.

OmaZettel does not edit a note live alongside someone else. People take turns
and sync, and the conflict screen handles the occasional clash.

On another computer, clone the repository and choose that folder in OmaZettel:

```sh
git clone https://github.com/you/notes.git ~/Notes        # or your GitLab, Forgejo, Gitea… address
```

## Command line and AI assistants

Everything you do to notes in the window can be done from a terminal, a
script or an AI assistant, without opening the window. Commands work on the
notes folder the app has open (or `--vault <folder>`, or `$OMAZETTEL_VAULT`),
print text in a terminal and JSON when piped (or with `--json`), and take `-`
to read a value from standard input.

```sh
omazettel search "quarterly plan"                 # notes, with matching lines
omazettel show "Atlas"                            # a note's text
omazettel new "Vendor kickoff" -t Meeting --meta 'attendees=["Ann","Bo"]'
omazettel append "Vendor kickoff" "- [ ] Send the contract" -s "Action items"
omazettel capture "Call the bank"                 # to today's daily note, under Log
omazettel daily yesterday                         # or a date, or -3
omazettel tasks                                   # open - [ ] tasks in every note
omazettel rename "Atlas" "Atlas Program"          # and every [[link]] to it
omazettel tidy-names [--apply]                    # rename old "file names with spaces"
omazettel setup-layout [--apply]                  # move an older folder to the numbered layout
omazettel export Meetings -p Word                 # a folder as one document
omazettel sync
```

| Command | What it does |
| --- | --- |
| `info` | The notes folder, templates, export profiles, sync and transcription status, and the folder layout with your `01-system` rules for agents |
| `list`, `search`, `show`, `tags`, `tasks`, `links` | Find and read: by folder, tag or date; full text; one note with its links and backlinks; tags; open tasks; broken links and orphans |
| `new`, `append`, `set-section`, `set-meta`, `write` | Write: a note from a template (filling its sections and header), add under a heading, replace a section, change header fields, replace a note |
| `daily`, `capture` | A day's daily note; a quick timestamped line or task in today's |
| `rename`, `move`, `trash` | Organise, rewriting links as the app does |
| `tidy-names` | Renames older notes with spaces in their file names to the current separator, rewriting links; previews unless `--apply` |
| `setup-layout` | Gives the notes folder the [numbered layout](#folder-layout), moving the older Daily, Meetings, Literature and Archive folders in; previews unless `--apply` |
| `export`, `sync` | Pandoc export with your profiles; git sync |
| `transcribe` | Meeting captions (Teams, Zoom, Meet `.vtt`/`.srt`) as `Speaker: words`; `.docx` and `.txt` transcripts; recordings through Voxtype or whisper.cpp. With `--note`, an AI writes the meeting note |
| `ai` | Which AI writes notes, whether it and transcription are ready, and how to fix them; `--set` chooses |

`omazettel help <command>` lists a command's options. As in the app, writing a
`[[link]]` to a note that does not exist makes that note (`--no-link-notes`
skips it).

**Meetings.** One command turns a meeting into a finished note:

```sh
omazettel transcribe standup.vtt --note                    # the AI picks the title
omazettel transcribe ~/Recordings/standup.m4a --note "Atlas standup"
omazettel transcribe voxtype:latest --note                 # Voxtype's last recorded meeting
```

OmaZettel reads the captions or transcript (Teams, Zoom and Meet `.vtt`/`.srt`,
`.docx`, `.txt`), or transcribes a recording (`.m4a`, `.mp3`, `.mp4`, ...) on
your computer. Then an AI writes a Meeting note: title, attendees, a summary,
Agenda, Notes, Decisions and Action items as `- [ ] Owner: task (due)`, and
links to notes the meeting is about. The transcript goes in a note of its own
beside it (`<meeting>-transcript`), and the two link to each other.
`--ai none` makes the notes without an AI summary, `--no-transcript` skips
the transcript note, and `-t` and `-f` choose another template or folder.
Without `--note`, `transcribe` prints the transcript.

*Transcribing recordings* uses Voxtype, Omarchy's dictation, with the model you
set up for it (`omarchy voxtype install`, `omarchy voxtype model`), and
otherwise [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
(`yay -S whisper.cpp`, then `omazettel transcribe --download-model base.en`).
Nothing is uploaded.

*The AI* is one you already have. On Omarchy it is your default agent
(`omarchy default agent claude`), run once with no tools. Choose another with
`--ai`, or save a choice with `omazettel ai --set`:

| `--ai` | Uses |
| --- | --- |
| `claude`, `codex`, `gemini`, `opencode`, `cursor-agent`, `copilot`, `crush`, `pi` | That agent's command line and your sign-in, optionally with a model: `claude:sonnet`, `codex:gpt-5` |
| `ollama:<model>` | A local model through [Ollama](https://ollama.com), e.g. `ollama:llama3.2`. Nothing leaves your computer |
| `anthropic[:<model>]` | The Claude API with `ANTHROPIC_API_KEY` (default model `claude-opus-5-5`) |
| `openai:<model>` | An OpenAI-compatible API with `OPENAI_API_KEY`, or `OPENAI_BASE_URL` for another server |

`omazettel ai` shows which AI will write notes and whether it is ready, the
agents installed, and whether recordings can be transcribed, each with what to
install or sign in to if not. `omazettel ai --test` sends it a tiny request.
The transcript goes to the AI you choose, under that service's terms.

**AI assistants (MCP).** `omazettel mcp` offers every command as a tool to
assistants that speak the [Model Context Protocol](https://modelcontextprotocol.io),
along with prompts for a meeting note, adding to a note and a weekly review.
For Claude Code:

```sh
claude mcp add --scope user omazettel -- omazettel mcp
```

For Claude Desktop, Cursor and others, add a server with the command
`omazettel` and the argument `mcp`. Then ask: "make a meeting note from
~/Downloads/standup.vtt", "add 'Ann sends the budget Friday' to the action
items in my Vendor kickoff note", "what did I decide about Atlas this month?"

**Claude skill.** The package installs a skill that teaches Claude Code (and
other agents that read skills) to use the commands well. Link it into your
skills folder:

```sh
mkdir -p ~/.claude/skills
ln -s /usr/share/omazettel/skills/omazettel ~/.claude/skills/omazettel
```

The assistant reads and writes only through these commands, in the notes
folder, and what it sends to its model is up to the assistant you use.

## Colors and themes

![Catppuccin Latte, Tokyo Night and Gruvbox](docs/screenshots/themes.png)

OmaZettel follows your Omarchy theme the way Neovim, VS Code and the terminals
do: switch themes and it recolors at once, light themes included. Every color
it uses has a name (`editor.background`, `editor.heading`, `editor.link`,
`editor.wikilink`, `editor.tag`, `editor.citation`, `editor.code`, `ui.panel`,
`ui.border`, `ui.accent`, …), and the names are set by an `omazettel.toml` theme
file in Omarchy's template format:

```toml
[editor]
background = "{{ background }}"
link = "{{ blue }}"
tag = "{{ magenta }}"
citation = "{{ yellow }}"
search = "{{ mix background yellow 35% }}"

[ui]
panel = "{{ dark_background }}"
border = "{{ mix background foreground 18% }}"
```

The colors come from the first of these that exists:

1. **The theme's own `omazettel.toml`**, in the theme's folder. Theme authors can
   ship one, as they do `neovim.lua` or `vscode.json`. Omarchy also writes it
   when you switch themes if you have a template in `~/.config/omarchy/themed/`.
2. **Your template**, `~/.config/omarchy/themed/omazettel.toml.tpl`, used for
   every theme. Settings → **Customize theme colors…** creates it from the
   built-in one and opens it; OmaZettel picks up each save at once.
3. **The built-in template** ([`themed/omazettel.toml.tpl`](themed/omazettel.toml.tpl),
   also installed to `/usr/share/omazettel/themed/`), filled with the current
   theme's colors.

`{{ name }}` is any color the theme defines (`background`, `foreground`,
`accent`, `red` … `magenta`, `lighter_background`, `selection_background`,
`color0`–`color15`, …), `{{ mix a b 30% }}` mixes two, and `"#rrggbb"` sets one
directly. Names a file leaves out get sensible defaults.

Prefer one look everywhere? Settings → **Oma palette** uses the fixed dark
palette of OmaShow and the other Oma apps, keeping only the theme's accent.

## Keyboard shortcuts

Press **F1** in the app for the full list.

| Keys | Action |
| --- | --- |
| Ctrl+Shift+P | Command palette |
| Ctrl+, | Settings |
| Ctrl+N | New note (from a template) |
| Ctrl+Shift+D | Today's daily note |
| Ctrl+O | Go to note |
| Ctrl+Shift+F | Search all notes |
| Ctrl+F / Ctrl+H | Find / find and replace in the note |
| Ctrl+click, Ctrl+Enter | Follow a link |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+Shift+E | Export with pandoc |
| Ctrl+Alt+S | Sync now |
| Ctrl+\ | Show or hide the sidebar |
| Ctrl+Shift+B | Show or hide outline, tags and backlinks |
| Ctrl+Shift+O | Open any file |
| Ctrl+Shift+N | New window |
| Ctrl+B / Ctrl+I / Ctrl+K | Bold / italic / link |
| Ctrl+= / Ctrl+- / Ctrl+0 | Text size |
| Ctrl+P | Print |
| F11 or Super+F | Full screen |

## Where things are kept

Everything about your notes lives in the notes folder, so it syncs with them:

| Path (in the notes folder) | What |
| --- | --- |
| `*.md`, subfolders | Your notes |
| `.omazettel/note-templates/` | Note templates |
| `.omazettel/export/` | Export profiles (pandoc defaults files) |
| `.omazettel/templates/` | Export templates and reference documents |
| `.omazettel/citations.yaml` | Bibliography and citation style |
| `<folder>/.omazettel-project.yaml` | A folder export's notes, order and profile |
| `Exports/` | Exports, if you chose that in Settings (git-ignored) |

App settings (window, recent folders, author, sync timing) are in
`~/.config/omazettel/omazettel.conf`.

Downloaded transcription models are in `~/.local/share/omazettel/whisper/`.

`omazettel FILE` opens a file; with no arguments OmaZettel reopens your notes
folder at the last note. `omazettel --help` lists the commands (see
[Command line and AI assistants](#command-line-and-ai-assistants)).

## Building from source

You need the [requirements](#requirements) above: Qt 6.6 or newer
(`qt6-base`, `qt6-declarative`), a C++17 compiler and `make`.

```sh
./bin/build            # builds ./build/omazettel
./bin/test             # runs the tests (offscreen; no display needed)
./build/omazettel      # runs the build
./bin/install          # builds and installs the Arch package
```

The tests cover the editor (from Omawrite) and OmaZettel's notes features,
including real pandoc exports and two devices syncing through a git remote.

### Making a release

1. Set the version in `src/main.cpp` (`OMAZETTEL_VERSION`), `pkgbuild/PKGBUILD`
   (`pkgver`, and `pkgrel=1`) and the status line at the top of this README.
2. Write the release notes in `docs/releases/<version>.md`.
3. Commit, then tag and push: `git tag -a omazettel-v<version> -m "OmaZettel <version>"`
   and `git push origin master omazettel-v<version>`.

The tag starts `.github/workflows/release.yml`. It builds and tests x86_64 and
aarch64 in Arch containers with `bin/release-build` (which refuses a tag that
doesn't match the version in the code), then publishes the release: the
OmaStore tarballs, the pacman packages (also under the version-free names
`install.sh` fetches), `checksums.txt`, `SHA256SUMS` and a build provenance
attestation. To try the build before tagging, run it in the same container:

```sh
docker run --rm -v "$PWD:/w" -w /w archlinux:latest bash -c \
  'pacman -Syu --noconfirm --needed base-devel git qt6-base qt6-declarative ttf-jetbrains-mono && bin/release-build omazettel-v<version>'
```

The code is laid out so changes from upstream Omawrite merge cleanly:
OmaZettel's additions live in their own files (`src/vault.*`,
`src/noteindex.*`, `src/zettel.*`, `src/exporter.*`, `src/sync.*`,
`src/apptheme.*`, `src/backend_notes.cpp`, `src/markdownhighlighter_notes.cpp`
and the QML panels and dialogs), with small hooks in Omawrite's files.
`docs/PLAN.md` records how it was built.

## Contributing

Issues and pull requests are welcome. Please:

- describe what you expected and what happened, with your Omarchy version
  (`omarchy version`) for bugs;
- run `./bin/test` before sending a pull request, and add a test for new
  behavior where you can;
- keep additions out of Omawrite's own files when possible, so upstream
  merges stay simple.

## Credits and license

Copyright (c) 2026 Matthew Smith. GPL-3.0-or-later licence; see [`LICENSE`](LICENSE).

It started from [Omawrite](https://github.com/omacom/omawrite) by David
Heinemeier Hansson, under the MIT license
([`LICENSES/MIT-omawrite.txt`](LICENSES/MIT-omawrite.txt)). Feature ideas
inspired by [Zettlr](https://www.zettlr.com), [Obsidian](https://obsidian.md)
and [Joplin](https://joplinapp.org). The Oma palette comes from
[OmaShow](https://github.com/28allday/omashow). See
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) for details.

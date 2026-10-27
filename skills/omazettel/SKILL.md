---
name: omazettel
description: >
  Reading, searching, writing and organising the person's OmaZettel notes (a
  folder of plain Markdown notes with [[wiki links]], tags and pandoc headers)
  from the command line with `omazettel <command>`, without opening the app.
  Use when asked to turn a meeting recording, Teams/Zoom captions or a
  transcript into a meeting note; to add something to a note ("add X to my
  Atlas note", "put this under Action items"); to capture a thought or task to
  today's daily note; to find, summarise or review notes, tags, open tasks or
  action items; to make a note from a template; to rename or move notes
  keeping links working; or to export a note or folder to PDF or Word.
  Triggers: omazettel, my notes, meeting notes, daily note, zettelkasten,
  transcript to notes, action items, add to my note, capture this, export my
  note. NOT for working on the OmaZettel source code itself.
---

# Working with OmaZettel notes

OmaZettel keeps notes as plain `.md` files in one folder (the "vault"). Every
command below runs without a window and works on the folder the app has open,
or `--vault <folder>`. Always add `--json` when you read the answer; it is one
JSON object, or `{"error": "..."}` with exit code 1.

```
omazettel info                                   # folder, templates, export profiles, sync, transcription
omazettel list [--folder F] [--tag T] [--since 7d] [--sort modified]
omazettel search "words" [--limit N]             # all words; "a phrase"; #tag
omazettel show <note> [--section H]              # text, meta, outline, links, backlinks
omazettel templates                              # each template's headings and header keys
omazettel new <title> [-t Template] [-f Folder] [--content -] [--sections JSON|-] [--meta k=v|JSON] [--tags a,b]
omazettel append <note> <text|-> [-s Heading] [--create]
omazettel set-section <note> <text|-> -s Heading
omazettel set-meta <note> key=value ...           # values may be JSON; null removes
omazettel write <note> - [--create]               # whole note; last resort
omazettel daily [date] [--append text] [-s Log]
omazettel capture <text> [--task]
omazettel tasks [--status open|done|all] [--note N] [--folder F] [--tag T]
omazettel tags | links                            # tag counts; broken links and orphans
omazettel rename <note|folder> <new name>         # rewrites [[links]]
omazettel tidy-names [folder] [--apply]           # spaces in file names -> separator; preview first
omazettel setup-layout [--apply]                  # older folder -> numbered layout; preview first
omazettel maps [--apply]                          # project maps of content; kept up to date on their own
omazettel move <note|folder> <folder>
omazettel trash <note>
omazettel export <note|folder> [-p PDF|Word|HTML|...] [-o file]
omazettel sync [--status]
omazettel transcribe <file> [--timestamps] [--language en]
omazettel transcribe <file> --note [title] [--ai claude|none|...]   # transcript -> finished meeting note
omazettel ai [--test]                             # which AI writes notes; is transcription ready
omazettel help <command>
```

`<note>` is a note's file name, title, or path in the folder
(`Meetings/2026-10-22 Kickoff`). If a name matches several notes the error
lists them; use the path. A value of `-` reads standard input, which is the
safe way to pass long Markdown: use a quoted heredoc (`<<'EOF'`) so the shell
leaves `$`, backticks and `!` alone.

## Ground rules

- **Read the person's rules first.** Run `omazettel info --json` once at the
  start. When `layout.numbered` is true, `layout.rules.text`
  (`01-system/SYSTEM-RULES.md`) and `layout.howIWork.text`
  (`01-system/HOW-I-WORK.md`) are the person's own instructions for agents;
  follow them over the defaults here. Don't edit either file unless asked.
- **Look before you write.** `search` or `list` to find the note, `show` to
  read it, and match its headings and style.
- **Change the part, not the whole.** `append` (to the end, or under a
  heading with `-s`), `set-section` and `set-meta` leave everything else as it
  was. Use `write` only when asked to rewrite a note, and only after `show`.
- **Links make notes.** Like the app, writing a `[[Link]]` to a note that does
  not exist makes an empty note of that name. Link existing notes (check with
  `search`), or pass `--no-link-notes` when you are not sure the note should
  exist. Answers list any `linkedNotesCreated`; mention them.
- **Tasks** are `- [ ] Owner: task (due)`; `tasks` finds them across notes.
- Report the note's `path` back to the person, and the action items you wrote.

## Where notes go

A notes folder with the numbered layout (`layout.numbered` in `info`):

| Folder | What goes there |
| --- | --- |
| `01-system/` | The rules and how-I-work files. Read them; leave them alone |
| `02-context/` | Stable background and reference: people, clients, glossaries; literature notes in `02-context/literature` |
| `03-projects/<project>/` | Active work, one folder per project, starting with a `00-brief` note (goal, people, deadline). Daily notes are in `03-projects/daily`, meetings in `03-projects/meetings` |
| `04-outputs/` | Finished work, ready to send |
| `99-archive/` | Finished or dropped projects: `move` them here rather than `trash` |

**Maps of content.** `03-projects/projects-moc` links each project's map
(`03-projects/<project>/<project>-moc`), which lists every note in the
project except transcripts. To get an overview of a project, `show` its map.
OmaZettel rewrites the list between the `omazettel-map` marker comments after
every change, so never edit inside them; write any overview above or below
with `append` or `set-section`. Answers list any `mapsUpdated`.

**Transcripts** go in `transcripts/` beside their meeting note
(`transcribe --note` does this). Link the meeting note, not the transcript.

Name files so they say when, what and how far along: date first for anything
tied to a day (`2026-10-25-vendor-kickoff`), a status at the end for drafts
and outputs (`2026-10-25-q4-plan-draft-v1`, then `...-final` in `04-outputs`),
and plain topic names for lasting ideas (`zettelkasten-method`). Avoid
`notes`, `final` or `untitled`. The templates already date daily and meeting
notes; pass `-f 03-projects/<project>` to file a meeting under its project.

Without the layout, follow the folders that are there. Suggest
`omazettel setup-layout` (it previews first) only when the person asks how to
organise their notes.

## A meeting note from a recording or transcript

When the person just wants the note, one command does it all: OmaZettel
transcribes, then the person's own AI (Omarchy's default agent, or `--ai`)
writes the Meeting note: summary, Agenda, Notes, Decisions and Action items.
The transcript goes in a separate linked note in a `transcripts/` folder beside it
(`transcripts/<meeting>-transcript`).

```sh
omazettel transcribe ~/Downloads/standup.vtt --note --json        # AI picks the title
omazettel transcribe call.m4a --note "Vendor kickoff" --json
omazettel transcribe voxtype:latest --note --json                 # Voxtype's last meeting
```

The answer has `path`, `transcriptNote`, `actionItems` and `writtenBy`. If it fails with a
message about a missing AI, speech model or sign-in, pass the message on: it
says what to install or run (`omazettel ai` shows the whole picture). Don't
install things yourself. `--ai none` still makes a note with the transcript.

Write the note yourself instead (below) when you are the AI the person is
talking to and want to shape it: they asked for a particular emphasis, it
should link notes you found, or it adds to an existing note.

1. `omazettel transcribe <file> --json` turns Teams/Zoom/Meet captions
   (`.vtt`, `.srt`) into `Speaker: words` turns with `speakers` listed, reads
   `.docx`/`.txt` transcripts, and transcribes recordings (`.mp3`, `.m4a`,
   `.mp4`, ...) on this computer with Voxtype or whisper.cpp. If it says a
   speech model is missing, pass on the install line it gives rather than
   installing things yourself. A transcript pasted into the chat needs no
   transcribing.
2. `omazettel templates --json`: the Meeting template's headings are usually
   Agenda, Notes, Decisions, Action items, and its header has `attendees`.
3. `omazettel search` for the project and people, to link the right notes.
4. Write the note in one call, sections as JSON on standard input:

```sh
omazettel new "Atlas standup" -t Meeting --tags atlas \
  --meta 'attendees=["Ann Lee","Bo Diaz"]' --no-link-notes --json --sections - <<'EOF'
{
  "Agenda": ["Snowflake cutover", "dbt model status"],
  "Notes": "Cutover to Snowflake is planned for Friday ([[Atlas]]).\n\nBo is still finishing the dbt models and tests.",
  "Decisions": ["Cutover stays on Friday unless tests fail Thursday"],
  "Action items": "- [ ] Bo: finish dbt models and tests (Thu)\n- [ ] Ann: confirm the cutover window"
}
EOF
```

A list value becomes bullets; a string goes in as written. Headings the
template lacks are added. The Meeting template names the file
`<date>-<title>` in its meetings folder (`03-projects/meetings` with the
layout, else `Meetings/`); `-f` puts it elsewhere. Keep the summary
faithful to the transcript: no invented decisions, owners or dates.
Whisper does not know who spoke; with a recording, leave owners out or ask.

## Adding to an existing note

"Add X to my Y note": `show Y --json`, pick the heading it belongs under, then

```sh
omazettel append "Y" -s "Action items" --json - <<'EOF'
- [ ] Ann: send the revised budget (Fri)
EOF
```

List items join the list already there; template placeholders (`- `,
`- [ ] `) make way. Without `-s` the text goes at the end. `--create` makes the
note (from the Note template) if there is none. To change a header field:
`set-meta Y status=done 'reviewers=["Bo"]'`.

## Daily notes and capture

`daily` returns today's daily note (made from the Daily template in
`03-projects/daily/`, or `Daily/` without the layout, if needed); `daily yesterday`, `daily 2026-10-01` or `daily -3` for others.
`daily --append "..." -s Focus` adds to a heading (default Log).
`capture "text"` adds `- HH:MM text` under Log in today's note;
`capture --task "text"` adds `- [ ] text`.

## Reviews and questions about the notes

`list --since 7d --sort modified --json`, then `show` the meeting and daily
notes, and `tasks --json` for what is open. Quote note paths so the person can
open them (`omazettel <path>` opens one in the app).

## Organising and exporting

`rename` and `move` rewrite every `[[link]]` to what moved, as the app does;
never move notes with `mv`. `tidy-names` renames notes with spaces in their
file names, and `setup-layout` moves an older folder to the numbered layout;
run either without `--apply` first and show the person the list. `export <note> -p PDF` uses the folder's pandoc
export profiles (`info` lists them) and answers with `output`, the file
written; exporting a folder makes one document of its notes. `sync` saves a
version and pulls and pushes when git sync is set up; `sync --status` only
reports.

## MCP

The same commands are MCP tools (`omazettel mcp`), named `search_notes`,
`create_note`, `append_to_note` and so on, with the same arguments in
camelCase. When those tools are available, prefer them to the shell.

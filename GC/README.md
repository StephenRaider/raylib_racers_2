# General Championship (GC) edition

A college championship between teams that other people send you. Each team is one zip. Drop the zips in
`GC/submissions`, click **Build_GC**, start **GC_Race_viewer**: every team and its two bots are in the
sim, nothing wired by hand.

Not for the public: `GC/teams`, `GC/submissions`, the built bots and the championships are git-ignored, so
nobody sees a team before race day.

## Host: the steps

1. Collect the zips (see `SUBMISSION.md`, which is the page to send to submitters).
2. Put them in `GC/submissions/`. The zip's file name is the team's id (`mech.zip` makes team `mech`).
   You can also put an already unpacked team folder straight into `GC/teams/`.
3. Click **`Build_GC.bat`** (Windows) or run `Build_GC.sh` (Linux/macOS). It
   - builds the tools the first time (needs CMake and a C/C++ compiler),
   - unpacks the zips into `GC/teams/` (refusing zips that climb out of the folder, contain links, or are huge),
   - checks every team and **lists the ones it rejected and why**; the rest go on,
   - compiles every accepted team's bots into `GC/bots/<team>__<bot>.dll` (`.so`),
   - runs a one-lap sandboxed smoke race of all bots and prints crashes and CPU use.
4. Start **`GC_Race_viewer`**. The first screen lists the teams. It has Quick race, Race weekend and
   Championship. There is no Testing, no grid editing, no team stats editing and none of the example bots
   (John F One and the rest) unless a team submitted them in this format.

To add a team later, drop its zip in `GC/submissions` and click `Build_GC` again. Teams already in
`GC/teams` are skipped; delete a team's folder to replace it with a new zip.

## Locked championships

When a championship is created, a SHA-256 digest of every team's files (manifest, bot source, liveries and
the built bot libraries) is stored in the season file. Before each round, and when a season is opened, the
files are checked again. If anything differs, the championship is **marked INVALID in its file for good**:
the viewer shows why and refuses to start more rounds. A round that finishes while a file changed is not
recorded. Restoring the file does not undo it. So: finish building before you press *Start championship*,
and do not run `Build_GC` during a season.

## Rules in force (GC only)

- Every bot runs sandboxed (`rr_bothost`) with a CPU cap of 2 ms per `drive()` call; a bot that crashes,
  hangs or keeps overrunning is retired from the race, the race goes on.
- At most 10 teams (20 cars). Team names, short names and race numbers are unique across the event.
- Stats follow `specs/development.json` (the budget is shown by the check).
- Only source goes in: bots must be C or C++. Compiled files (`.exe`, `.dll`, `.so`) are refused.

## Safety on the hosting PC

The sandbox stops a *running* bot from touching files, the network or starting programs. Linux uses seccomp
(strong); on Windows it is weaker and a bot can still read files you can read. Compiling is not sandboxed.
`rr_gc` rejects `#include` of paths outside the bot folder, `.incbin`/`#embed`, links and any non-source
file, but that is not a substitute for reading the code. Best: host the event on a PC with nothing private
on it (or a VM / WSL), and skim each bot's source.

## Commands (what the scripts call)

```
rr_gc check [--built]   list accepted and rejected teams (--built also needs the bots compiled)
rr_gc import            unpack GC/submissions/*.zip into GC/teams
rr_gc plan              write GC/build/bots.cmake: which bots to compile
rr_gc smoke             one sandboxed lap of every bot: crashes and CPU use
```

Everything that is built lands in this folder: `GC_Race_viewer`, `rr_gc`, `rr_bothost`, `bots/`.
Tracks, cars and sounds come from the repository next to it (`../assets`, `../tracks`, `../specs`), so keep
this folder inside the checkout.

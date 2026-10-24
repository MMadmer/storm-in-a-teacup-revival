# Automatic updates

## Release contract

- The tag is a version in `MAJOR.MINOR.PATCH` form, optionally prefixed with `v`.
- The release carries `storm-in-a-teacup-revival-MAJOR.MINOR.PATCH.zip`, built by `tools/package_release.py`.
  The archive is stored without compression and contains `Mods/AutoUpdate/update-manifest.txt`, whose version
  matches the tag. Every release archive is complete: an update from any older version reads only what it needs.
- The version comes from `project(... VERSION ...)` in `CMakeLists.txt`; `AutoUpdate.dll` compiles it in.
- The release description follows this template. The update dialog shows only the bullet items of
  `## Changes` inside `## EN`, or of `## Изменения` inside `## RU` when the Windows display language is Russian.

```markdown
## EN

## Changes

* [Short user-visible change]

## Installation

Unpack the archive into the folder with `Storm.exe`.
---
## RU

## Изменения

* [Краткое описание изменения, заметного игроку]

## Установка

Распакуйте архив в папку с `Storm.exe`.
```

## Manifest

```
storm-revival-manifest 1
version 1.0.0
file <sha256> <size> <-|config> <path>
```

Paths are relative to the game folder and use `/`. `config` marks a user-editable file (`Mods.ini`, mod settings):
it is installed when missing and never overwritten or removed. The installed copy of the manifest is the record
of the files the mods own.

## Client

`Mods/AutoUpdate/AutoUpdate.dll` asks the GitHub API for the latest release in a background thread as soon as
the mod loader starts. When the release is newer than the compiled version, it reads the end of the archive and
its central directory with HTTP range requests, extracts the manifest, compares every file with the installation
by size and SHA-256, and computes the size of the records that differ. When the release is the installed version,
the installation is checked against its installed manifest instead; missing or damaged files make the client offer
a repair from the same archive. Installations without a manifest (development builds) are not repaired.

The offer is shown before the game creates its Direct3D device, so exclusive fullscreen has not taken the screen
yet; the check gets up to two more seconds to finish there. Declining dismisses the offer until the next launch.
On acceptance the differing files are downloaded, each checked against the manifest, and staged in
`%LOCALAPPDATA%\StormInATeacupRevival\update\new`. A server that ignores range requests sends the whole archive
once; it is accepted only when it matches the SHA-256 digest GitHub reports.

`STORM_UPDATE_URL` replaces the API address for tests; `tools/update_test_server` serves a local release.

## Applier

`StormUpdater.exe` runs from a copy in `%LOCALAPPDATA%\StormInATeacupRevival\update\applier` and holds the
`Local\StormInATeacupRevivalUpdate` mutex until it restarts the game; a game started meanwhile skips the check.
After the game has exited, the applier copies the staged update into `Mods/AutoUpdate/.update` and works only from
that copy. Every file of the new release must be either downloaded and verified or already installed with the
right hash; otherwise nothing is changed. The applier backs up and replaces the changed files, removes the files
the previous manifest listed and the new one does not, and installs the new manifest last. A failure or an
exception restores every touched file; the backup is kept when that does not fully succeed.

A game folder that is not writable is updated by the installed `Mods/AutoUpdate/StormUpdater.exe`, started with
administrator rights after a UAC prompt; the elevated process does not read the user-writable staging folder after
copying it. The game is started again in every case. `Mods/AutoUpdate/update.log` records the result.

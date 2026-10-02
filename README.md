# fsearch-mac

**fsearch-mac** is a fork of [FSearch](https://github.com/cboxdoerfer/fsearch) by Christian Boxdörfer that runs on
macOS with live filesystem monitoring and a headless command line interface. It is distributed under the same license,
the GNU GPL version 2 or (at your option) any later version — see [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md) for
what was changed.

What this fork adds on top of upstream FSearch:

- **macOS build fixes**, and a fix for a crash when a loaded database was disposed (uninitialized mutex).
- **Live monitoring on macOS** through the CoreServices FSEvents API. Monitored folders pick up created, renamed,
  moved, modified and deleted files without rescanning — in the GUI as well as in `fsearch-cli watch`.
- **`fsearch-cli`**, a headless tool to build databases, keep them live and search them, for scripts and for other
  programs that call it as a separate process.

## Installation

### Homebrew (recommended)

```sh
brew tap NomaDamas/fsearch-mac
brew install fsearch-mac
```

This installs `fsearch-cli` (headless) and `fsearch` (GUI) from the [NomaDamas/homebrew-fsearch-mac](https://github.com/NomaDamas/homebrew-fsearch-mac) tap. Grant your terminal *Full Disk Access* in System Settings to index protected folders.

### Build from source

```sh
brew install meson ninja pkg-config gettext itstool gtk+3 pcre2 icu4c
export PKG_CONFIG_PATH="$(brew --prefix icu4c)/lib/pkgconfig:$PKG_CONFIG_PATH"
meson setup build
ninja -C build
meson test -C build
```

This produces `build/src/fsearch` (GUI) and `build/src/fsearch-cli`. Grant the terminal (or whichever program runs
them) *Full Disk Access* in System Settings to index protected folders.

## fsearch-cli

```sh
# Build a database; folders are marked for live monitoring unless --no-monitor is given
fsearch-cli index --db ~/fs.db --include ~/Documents --include ~/Projects --exclude node_modules --exclude .git

# Search it (loads the database file)
fsearch-cli search --db ~/fs.db 'report ext:pdf'
fsearch-cli search --db ~/fs.db --json --limit 20 --sort mtime --desc '*.md'
fsearch-cli search --db ~/fs.db --count --regex '^IMG_\d+\.jpe?g$'

# Keep it live: rescans monitored folders once, then applies filesystem events as they happen,
# saves every --save-interval seconds and on SIGINT/SIGTERM
fsearch-cli watch --db ~/fs.db

# While `watch` runs, search/stats go through its socket (no load time, always current)
fsearch-cli search --db ~/fs.db invoice
fsearch-cli stats --db ~/fs.db
fsearch-cli events --db ~/fs.db   # JSON line per index change
```

`--db` defaults to `$XDG_DATA_HOME/fsearch/fsearch.db` (`~/.local/share/fsearch/fsearch.db`), the database the FSearch
app uses. Queries use FSearch's [search syntax](https://github.com/cboxdoerfer/fsearch/wiki/Search-syntax).

### Indexing a whole Mac

```sh
fsearch-cli index --db ~/fs.db --include / --include /Volumes/SSD1 --one-file-system --dev-excludes
```

`--dev-excludes` drops common development artifact folders (VCS metadata like `.git`, dependency trees like
`node_modules` and `Pods`, virtualenvs, build output like `target`/`dist`, and tool caches). Measured on the same Mac:
the index drops from 18.0M to 8.3M files, the database from 984 MB to 482 MB, and scan-time RAM from 4.3 GB to
2.2 GB.

`--one-file-system` keeps a scan of `/` from crossing into other mounted volumes, so every volume you list with
`--include` is covered exactly once. On top of that, the scan deduplicates directories by (device, inode): APFS
firmlinks (`/Users` and `/System/Volumes/Data/Users` are the same directory), bind mounts and repeated mount points
are indexed once, under the path the scan reaches first. macOS also exposes the root filesystem a second time under
`/.nofollow`, `/.resolve` and `/System/Volumes`; those (and `/dev`) are excluded by default so the canonical path wins
— pass `--no-default-excludes` to see even them. Measured on an
Apple Silicon Mac with ~18M files / ~2.6M folders (system volume plus an external SSD): ~7 minutes, a ~1 GB database,
~4 GB of RAM while scanning, and after a restart of `watch` around 5 seconds until searches are served again. Search
takes a few hundred milliseconds over 20M entries.

### Output

Plain `search` prints one absolute path per line. `--json` prints one object per result followed by a summary line:

```json
{"path":"/Users/me/docs/report.pdf","name":"report.pdf","type":"file","size":48213,"mtime":1790862917}
{"done":true,"num_results":1,"num_returned":1}
```

`events` prints `{"event":"changed","files":N,"folders":N,"time":UNIX}` (also `subscribed` and `scan-finished`).

### Daemon socket protocol

`watch` listens on a Unix socket (`<db>.sock`, mode 0600). A client sends one line and reads until the daemon closes
the connection (except for `subscribe`, which stays open):

| Request | Reply |
|---|---|
| `search\tLIMIT\tFLAGS\tSORT\tasc\|desc\tFORMAT\tQUERY` | results in `FORMAT` (`paths`, `json` or `count`) |
| `stats` | one JSON object |
| `subscribe` | JSON event lines until the client disconnects |
| `save` | `{"done":true}` once the database file is written |
| `rescan` | `{"queued":true}` |

`LIMIT` is 0 for unlimited, `SORT` is `name`, `path`, `size` or `mtime`, and `FLAGS` is a bitmask of
`FsearchQueryFlags` (`src/fsearch_query_flags.h`): 1 match case, 2 auto match case, 4 regex, 8 search in path, 16 auto
search in path, 32 files only, 64 folders only.

### Monitoring notes (macOS)

- One FSEvents stream per indexed folder; changes are applied within about a second.
- FSEvents coalesces events and reports renames without telling which side an event belongs to, so the monitor decides
  by the current state of the path: whatever exists is (re)indexed, whatever is gone is removed.
- When FSEvents reports dropped events, the affected folder is rescanned; when the indexed folder itself disappears,
  its index is cleared and rebuilt once the folder is back.
- Changes made while nothing is watching are picked up by the rescan `watch` performs on start.

---

Upstream README follows.

![Build Status](https://github.com/cboxdoerfer/fsearch/actions/workflows/build_test.yml/badge.svg)
[![Translation status](https://hosted.weblate.org/widgets/fsearch/-/svg-badge.svg)](https://hosted.weblate.org/engage/fsearch/?utm_source=widget)

FSearch is a fast file search utility, inspired by Everything Search Engine. It's written in C and based on GTK3.

* For bug reports and feature requests please use the issue tracker: <https://github.com/cboxdoerfer/fsearch/issues>
* For discussions and questions about FSearch use the discussion
  forum: <https://github.com/cboxdoerfer/fsearch/discussions>
* For everything else related to FSearch you can talk to me on Matrix: <https://matrix.to/#/#fsearch:matrix.org>

![](https://raw.githubusercontent.com/cboxdoerfer/fsearch/master/data/screenshots/02-main_window_menubar.png)
![](https://raw.githubusercontent.com/cboxdoerfer/fsearch/master/data/screenshots/01-main_window_headerbar.png)

## Features

- Instant (as you type) results
- [Advanced search syntax](https://github.com/cboxdoerfer/fsearch/wiki/Search-syntax)
- Wildcard support
- RegEx support
- Filter support (only search for files, folders or everything)
- Include and exclude specific folders to be indexed
- Ability to exclude certain files/folders from index using wildcard expressions
- Fast sort by filename, path, size or modification time
- Customizable interface (e.g., switch between traditional UI with menubar and client-side decorations)

## Requirements

- GTK 3.22
- GLib 2.60
- glibc 2.19 or musl 1.1.15 (other C standard libraries might work too, those are just the ones I verified)
- PCRE2 (libpcre2) 10.21
- ICU 4.4

## Download

It is recommended to install FSearch from one of the **Stable** packages, unless you know what you're doing.

The **Development** packages are primarily intended for testing and adventurous users.


| Distribution                                                                                          | Stable                                                                                                                     | Development                                                                            |
|-------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------|
| Ubuntu                                                                                                | [PPA Stable](https://launchpad.net/~christian-boxdoerfer/+archive/ubuntu/fsearch-stable)                                   | [PPA Daily](https://launchpad.net/~christian-boxdoerfer/+archive/ubuntu/fsearch-daily) |
| Arch Linux                                                                                            | [AUR](https://aur.archlinux.org/packages/fsearch/)                                                                         | [AUR (git)](https://aur.archlinux.org/packages/fsearch-git/)                           |
| Fedora/RHEL/CentOS                                                                                    | [COPR Stable](https://copr.fedorainfracloud.org/coprs/cboxdoerfer/fsearch/)                                                | [COPR Nightly](https://copr.fedorainfracloud.org/coprs/cboxdoerfer/fsearch_nightly/)   |
| Debian                                                                                                | [OpenBuildService](https://software.opensuse.org//download.html?project=home%3Acboxdoerfer&package=fsearch#manualDebian)   |                                                                                        |
| openSUSE                                                                                              | [OpenBuildService](https://software.opensuse.org//download.html?project=home%3Acboxdoerfer&package=fsearch#manualopenSUSE) |                                                                                        |
| Flatpak ([limited features](https://github.com/cboxdoerfer/fsearch/wiki/Flatpak-version-limitations)) | [Flathub](https://flathub.org/apps/details/io.github.cboxdoerfer.FSearch)                                                  |                                                                                        |
| Solus*                                                                                             | [Solus Repository](https://dev.getsol.us/source/fsearch/)                                                                  |                                                                                        |
| FreeBSD*                                                                                           | [FreshPorts](https://www.freshports.org/sysutils/fsearch)                                                                  |                                                                                        |

(*) Not maintained by me

## Roadmap

<https://github.com/cboxdoerfer/fsearch/wiki/Roadmap>

## Build Instructions

<https://github.com/cboxdoerfer/fsearch/wiki/Build-instructions>

## Localization

The localization of FSearch is managed with Weblate.

<https://hosted.weblate.org/projects/fsearch/>

If you want to contribute translations please submit them there, instead of opening pull requests on GitHub. This also includes any suggestions to the English texts — English isn't my first language, so there are likely errors and unusual wordings.

Instructions can be found here:
<https://docs.weblate.org/en/latest/user/basic.html>

And of course: Thank you for taking the time to translate FSearch!

## Current Limitations

* Sorting lots of results by *Type* can be very slow, since gathering that information is expensive, and the data isn't
  indexed. This also means that when the view is sorted by *Type*, searching will reset the sort order to *Name*.
* Using the *Move to Trash* option doesn't update the database index, so trashed files/folders show up in the result
  list as if nothing happened to them.

## Why yet another search utility?

Performance. On Windows I really like to use Everything Search Engine. It provides instant results as you type for all
your files and lots of useful features (regex, filters, bookmarks, ...). On Linux I couldn't find anything that's even
remotely as fast and powerful.

Before I started working on FSearch, I took a look at existing solutions. I tried MATE Search Tool (formerly GNOME
Search Tool), Recoll, Krusader (locate based search), SpaceFM File Search, Nautilus, ANGRYsearch and Catfish, to find
out whether it makes sense to improve those. However, they're not exactly what I was looking for:

- standalone application (not part of a file manager)
- written in a language with C like performance
- no dependencies to any specific desktop environment
- Qt5 or GTK3 based
- small memory usage (both hard drive and RAM)
- target audience: advanced users

## Looking for a command line interface?

I highly recommend [fzf](https://github.com/junegunn/fzf) or the obvious tools: find and (m)locate

## Why GTK3 and not Qt5?

I like both of them, and my long term goal is to provide console, GTK3 and Qt5 interfaces, or at least make it easy for
others to build those. However, for the time being it's only GTK3 because I like C more than C++, and I'm more familiar
with GTK development.

## Questions?

Email: christian.boxdoerfer[AT]posteo.de

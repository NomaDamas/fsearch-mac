# NOTICE

fsearch-mac is a modified version of FSearch.

- Original work: FSearch, Copyright © Christian Boxdörfer and contributors,
  <https://github.com/cboxdoerfer/fsearch>
- Modifications: Copyright © 2026 NomaDamas, <https://github.com/NomaDamas/fsearch-mac>

Both are licensed under the GNU General Public License, version 2 or (at your option) any later version. The full
license text is in [LICENSE](LICENSE). This program comes with ABSOLUTELY NO WARRANTY.

## Changes relative to upstream

Based on upstream commit `d531eb3b` (FSearch 0.3). Changes made by NomaDamas, starting October 2026:

| Date | Files | Change |
|---|---|---|
| 2026-10-01 | `src/fsearch.c`, `src/fsearch_database_file.c` | Portable `<limits.h>` and `off_t` so FSearch builds on macOS. |
| 2026-10-01 | `src/fsearch_database_index.c`, `src/fsearch_folder_monitor_inotify.c` | Initialize mutexes that were cleared without being initialized (crash when disposing a loaded database). |
| 2026-10-01 | `src/fsearch_folder_monitor_fsevents.{c,h}` (new), `src/fsearch_database_index.c`, `src/fsearch_database_scan.{c,h}`, `src/fsearch_database_entry.{c,h}`, `src/fsearch_database_entry_flags.h`, `src/fsearch_folder_monitor_event.{c,h}`, `meson.build`, `src/meson.build` | FSEvents folder monitor backend for macOS. |
| 2026-10-01 | `src/tests/test_folder_monitor_fsevents.c` (new), `src/tests/meson.build` | Test for the FSEvents monitor. |
| 2026-10-01 | `src/fsearch_cli.c` (new), `src/fsearch_database.{c,h}`, `src/meson.build` | `fsearch-cli` headless tool and a read-only database mode. |
| 2026-10-01 | `README.md`, `NOTICE.md` (new), `NEWS` | Documentation for the fork. |

/*
   FSearch - A fast file search utility
   Copyright © 2026 NomaDamas (fsearch-mac)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#pragma once

#include "fsearch_database_entry.h"

#include <glib.h>

// macOS folder monitor backed by the CoreServices FSEvents API.
//
// One FSEvents stream is created per monitor, rooted at the first folder that gets watched (the
// index root). Every further watched folder is only registered by path; incoming FSEvents item
// events are routed to the watched parent folder of the changed item, mirroring how the inotify
// backend reports events relative to a watch descriptor.
typedef struct FsearchFolderMonitorFsevents FsearchFolderMonitorFsevents;

FsearchFolderMonitorFsevents *
fsearch_folder_monitor_fsevents_new(GMainContext *monitor_context, GAsyncQueue *event_queue);

void
fsearch_folder_monitor_fsevents_free(FsearchFolderMonitorFsevents *self);

bool
fsearch_folder_monitor_fsevents_watch(FsearchFolderMonitorFsevents *self,
                                      FsearchDatabaseEntry *folder,
                                      const char *path);

void
fsearch_folder_monitor_fsevents_unwatch(FsearchFolderMonitorFsevents *self, FsearchDatabaseEntry *folder);

FsearchDatabaseEntry *
fsearch_folder_monitor_fsevents_resolve(FsearchFolderMonitorFsevents *self, gpointer handle);

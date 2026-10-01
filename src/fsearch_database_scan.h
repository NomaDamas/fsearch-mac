#pragma once

#include "fsearch_database_exclude_manager.h"
#include "fsearch_folder_monitor_fanotify.h"
#include "fsearch_folder_monitor_inotify.h"
#include "fsearch_folder_monitor_fsevents.h"

bool
db_scan_folder(const char *path,
               FsearchDatabaseEntry *parent,
               DynamicArray *folders,
               DynamicArray *files,
               FsearchDatabaseExcludeManager *exclude_manager,
               FsearchFolderMonitorFanotify *fanotify_monitor,
               FsearchFolderMonitorInotify *inotify_monitor,
               FsearchFolderMonitorFsevents *fsevents_monitor,
               bool one_file_system,
               GCancellable *cancellable,
               void (*status_cb)(const char *, gpointer),
               gpointer status_cb_data);
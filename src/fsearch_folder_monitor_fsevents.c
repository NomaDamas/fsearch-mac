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

#define G_LOG_DOMAIN "fsearch-monitor-fsevents"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include "fsearch_folder_monitor_fsevents.h"

#include "fsearch_database_entry.h"
#include "fsearch_folder_monitor_event.h"

#include <config.h>
#include <glib.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Coalescing latency handed to FSEvents. Low enough to feel live, high enough to batch bursts
// such as `git checkout` or archive extraction into a handful of callbacks.
#define FSEVENTS_LATENCY_SECONDS 0.2

#define FSEVENTS_STRUCTURE_FLAGS                                                                                       \
    (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed)

#define FSEVENTS_ATTRIBUTE_FLAGS                                                                                       \
    (kFSEventStreamEventFlagItemModified | kFSEventStreamEventFlagItemInodeMetaMod                                    \
     | kFSEventStreamEventFlagItemXattrMod | kFSEventStreamEventFlagItemChangeOwner                                    \
     | kFSEventStreamEventFlagItemFinderInfoMod)

#define FSEVENTS_RESCAN_FLAGS                                                                                          \
    (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped                                       \
     | kFSEventStreamEventFlagKernelDropped)

struct FsearchFolderMonitorFsevents {
    // All tables are only accessed with `mutex` held. The FSEvents callback runs on `queue` and
    // never dereferences a database entry; it only consults the path tables.
    GMutex mutex;
    GHashTable *id_to_folder; // id -> FsearchDatabaseEntry * (not owned)
    GHashTable *folder_to_id; // FsearchDatabaseEntry * -> id
    GHashTable *path_to_id;   // full path (owned) -> id
    GHashTable *id_to_path;   // id -> full path (owned by path_to_id)
    uint32_t next_id;

    GAsyncQueue *event_queue;

    FSEventStreamRef stream;
    dispatch_queue_t queue;

    // Path of the stream root as the index sees it, and its canonical form as FSEvents reports
    // it (e.g. /tmp vs. /private/tmp).
    char *root_path;
    char *root_real_path;
};

static void
push_event(FsearchFolderMonitorFsevents *self,
           const char *name,
           uint32_t parent_id,
           FsearchFolderMonitorEventKind kind,
           bool is_dir) {
    g_async_queue_push(self->event_queue,
                       fsearch_folder_monitor_event_new(name,
                                                        GUINT_TO_POINTER(parent_id),
                                                        kind,
                                                        FSEARCH_FOLDER_MONITOR_FSEVENTS,
                                                        is_dir));
}

// Maps a path reported by FSEvents back into the index's path namespace. Returns NULL when the
// path is outside of the stream root.
static char *
translate_event_path(FsearchFolderMonitorFsevents *self, const char *event_path) {
    g_autofree char *path = g_strdup(event_path);
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == G_DIR_SEPARATOR) {
        path[--len] = '\0';
    }

    const char *prefixes[] = {self->root_real_path, self->root_path};
    for (size_t i = 0; i < G_N_ELEMENTS(prefixes); i++) {
        const char *prefix = prefixes[i];
        if (!prefix) {
            continue;
        }
        const size_t prefix_len = strlen(prefix);
        // A prefix that already ends in '/' (the root "/") has no boundary character after it.
        const bool boundary_ok = prefix[prefix_len - 1] == G_DIR_SEPARATOR || path[prefix_len] == '\0'
                                 || path[prefix_len] == G_DIR_SEPARATOR;
        if (strncmp(path, prefix, prefix_len) == 0 && boundary_ok) {
            return g_strconcat(self->root_path, path + prefix_len, NULL);
        }
    }
    return NULL;
}

static bool
lookup_id_locked(FsearchFolderMonitorFsevents *self, const char *path, uint32_t *id_out) {
    gpointer value = NULL;
    if (!g_hash_table_lookup_extended(self->path_to_id, path, NULL, &value)) {
        return false;
    }
    *id_out = GPOINTER_TO_UINT(value);
    return true;
}

// FSEvents told us it lost track of what changed below the root itself (event queue overflow or
// similar). Every direct child of the root is re-scanned, which re-syncs the whole tree.
static void
rescan_root_children_locked(FsearchFolderMonitorFsevents *self, uint32_t root_id) {
    g_autoptr(GDir) dir = g_dir_open(self->root_path, 0, NULL);
    if (!dir) {
        return;
    }
    const char *name = NULL;
    while ((name = g_dir_read_name(dir))) {
        g_autofree char *child = g_build_filename(self->root_path, name, NULL);
        push_event(self, name, root_id, FSEARCH_FOLDER_MONITOR_EVENT_RESCAN, g_file_test(child, G_FILE_TEST_IS_DIR));
    }
}

static void
handle_root_event_locked(FsearchFolderMonitorFsevents *self, uint32_t root_id, FSEventStreamEventFlags flags) {
    struct stat st;
    if (lstat(self->root_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        // The index root itself was deleted or moved away. The index clears itself and polls
        // until the root reappears.
        push_event(self, NULL, root_id, FSEARCH_FOLDER_MONITOR_EVENT_DELETE_SELF, true);
        return;
    }
    if (flags & FSEVENTS_RESCAN_FLAGS) {
        rescan_root_children_locked(self, root_id);
        return;
    }
    if (flags & FSEVENTS_ATTRIBUTE_FLAGS) {
        push_event(self, NULL, root_id, FSEARCH_FOLDER_MONITOR_EVENT_ATTRIB, true);
    }
}

static void
handle_item_event_locked(FsearchFolderMonitorFsevents *self, const char *path, FSEventStreamEventFlags flags) {
    g_autofree char *parent = g_path_get_dirname(path);
    uint32_t parent_id = 0;
    if (!lookup_id_locked(self, parent, &parent_id)) {
        // Below an excluded or unwatched folder.
        return;
    }
    g_autofree char *name = g_path_get_basename(path);

    struct stat st;
    const bool exists = lstat(path, &st) == 0;
    const bool flagged_dir = (flags & kFSEventStreamEventFlagItemIsDir) != 0;
    const bool flagged_file = (flags & kFSEventStreamEventFlagItemIsFile) != 0;
    const bool is_dir = exists ? S_ISDIR(st.st_mode) : flagged_dir;

    if (flags & FSEVENTS_RESCAN_FLAGS) {
        push_event(self, name, parent_id, FSEARCH_FOLDER_MONITOR_EVENT_RESCAN, is_dir);
        return;
    }

    if (flags & FSEVENTS_STRUCTURE_FLAGS) {
        // FSEvents coalesces flags and reports renames once per side without telling which side
        // an event belongs to, so the current state of the path decides: whatever exists now is
        // (re)created, whatever is gone now is deleted. The index turns a create of an already
        // known entry into a rescan, which keeps it consistent either way.
        if (exists) {
            push_event(self, name, parent_id, FSEARCH_FOLDER_MONITOR_EVENT_CREATE, is_dir);
        }
        else if (flagged_dir == flagged_file) {
            // Symlinks and hard links carry neither type flag: drop whichever kind is indexed.
            push_event(self, name, parent_id, FSEARCH_FOLDER_MONITOR_EVENT_DELETE, true);
            push_event(self, name, parent_id, FSEARCH_FOLDER_MONITOR_EVENT_DELETE, false);
        }
        else {
            push_event(self, name, parent_id, FSEARCH_FOLDER_MONITOR_EVENT_DELETE, is_dir);
        }
        return;
    }

    if (exists && (flags & FSEVENTS_ATTRIBUTE_FLAGS)) {
        push_event(self, name, parent_id, FSEARCH_FOLDER_MONITOR_EVENT_ATTRIB, is_dir);
    }
}

static void
fsevents_callback(ConstFSEventStreamRef stream,
                  void *client_info,
                  size_t num_events,
                  void *event_paths,
                  const FSEventStreamEventFlags event_flags[],
                  const FSEventStreamEventId event_ids[]) {
    (void)stream;
    (void)event_ids;
    FsearchFolderMonitorFsevents *self = client_info;
    char **paths = event_paths;

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    uint32_t root_id = 0;
    const bool root_watched = lookup_id_locked(self, self->root_path, &root_id);

    for (size_t i = 0; i < num_events; i++) {
        const FSEventStreamEventFlags flags = event_flags[i];
        if (flags & kFSEventStreamEventFlagHistoryDone) {
            continue;
        }
        g_autofree char *path = translate_event_path(self, paths[i]);
        if (!path) {
            continue;
        }
        if (strcmp(path, self->root_path) == 0 || (flags & kFSEventStreamEventFlagRootChanged)) {
            if (root_watched) {
                handle_root_event_locked(self, root_id, flags);
            }
            continue;
        }
        handle_item_event_locked(self, path, flags);
    }
}

static bool
start_stream_locked(FsearchFolderMonitorFsevents *self, const char *root_path) {
    self->root_path = g_strdup(root_path);
    char real_path[PATH_MAX] = "";
    self->root_real_path = realpath(root_path, real_path) ? g_strdup(real_path) : NULL;

    CFStringRef cf_path = CFStringCreateWithFileSystemRepresentation(kCFAllocatorDefault, root_path);
    CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault, (const void **)&cf_path, 1, &kCFTypeArrayCallBacks);
    CFRelease(cf_path);

    FSEventStreamContext context = {.version = 0, .info = self};
    self->stream = FSEventStreamCreate(kCFAllocatorDefault,
                                       fsevents_callback,
                                       &context,
                                       paths,
                                       kFSEventStreamEventIdSinceNow,
                                       FSEVENTS_LATENCY_SECONDS,
                                       kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer
                                           | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    if (!self->stream) {
        g_debug("failed to create FSEvents stream for: %s", root_path);
        return false;
    }

    self->queue = dispatch_queue_create("io.github.nomadamas.fsearch-mac.fsevents", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(self->stream, self->queue);
    if (!FSEventStreamStart(self->stream)) {
        g_debug("failed to start FSEvents stream for: %s", root_path);
        FSEventStreamInvalidate(self->stream);
        FSEventStreamRelease(self->stream);
        self->stream = NULL;
        return false;
    }
    return true;
}

static void
remove_id_locked(FsearchFolderMonitorFsevents *self, uint32_t id) {
    FsearchDatabaseEntry *folder = g_hash_table_lookup(self->id_to_folder, GUINT_TO_POINTER(id));
    const char *path = g_hash_table_lookup(self->id_to_path, GUINT_TO_POINTER(id));
    if (folder) {
        g_hash_table_remove(self->folder_to_id, folder);
    }
    g_hash_table_remove(self->id_to_folder, GUINT_TO_POINTER(id));
    g_hash_table_remove(self->id_to_path, GUINT_TO_POINTER(id));
    if (path) {
        // Frees `path`, so this must come last.
        g_hash_table_remove(self->path_to_id, path);
    }
}

FsearchFolderMonitorFsevents *
fsearch_folder_monitor_fsevents_new(GMainContext *monitor_context, GAsyncQueue *event_queue) {
    // FSEvents delivers on its own dispatch queue, so unlike inotify no GLib context is needed.
    (void)monitor_context;
    g_return_val_if_fail(event_queue, NULL);

    FsearchFolderMonitorFsevents *self = calloc(1, sizeof(FsearchFolderMonitorFsevents));
    g_assert(self);

    g_mutex_init(&self->mutex);
    self->id_to_folder = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->folder_to_id = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->path_to_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->id_to_path = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->next_id = 1;
    self->event_queue = g_async_queue_ref(event_queue);

    return self;
}

static void
flush_queue_cb(void *context) {
    (void)context;
}

void
fsearch_folder_monitor_fsevents_free(FsearchFolderMonitorFsevents *self) {
    g_return_if_fail(self);

    if (self->stream) {
        FSEventStreamStop(self->stream);
        FSEventStreamInvalidate(self->stream);
        FSEventStreamRelease(self->stream);
        self->stream = NULL;
    }
    if (self->queue) {
        // Wait for a callback that might still be running before tearing down the tables.
        dispatch_sync_f(self->queue, NULL, flush_queue_cb);
        dispatch_release(self->queue);
        self->queue = NULL;
    }

    g_clear_pointer(&self->id_to_path, g_hash_table_unref);
    g_clear_pointer(&self->path_to_id, g_hash_table_unref);
    g_clear_pointer(&self->folder_to_id, g_hash_table_unref);
    g_clear_pointer(&self->id_to_folder, g_hash_table_unref);
    g_clear_pointer(&self->event_queue, g_async_queue_unref);
    g_clear_pointer(&self->root_path, g_free);
    g_clear_pointer(&self->root_real_path, g_free);
    g_mutex_clear(&self->mutex);

    free(self);
}

bool
fsearch_folder_monitor_fsevents_watch(FsearchFolderMonitorFsevents *self,
                                      FsearchDatabaseEntry *folder,
                                      const char *path) {
    g_return_val_if_fail(self, false);
    g_return_val_if_fail(folder, false);
    g_return_val_if_fail(path, false);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    if (!self->stream && !start_stream_locked(self, path)) {
        return false;
    }

    if (g_hash_table_contains(self->folder_to_id, folder)) {
        return true;
    }

    // A path can be re-registered by a new entry (e.g. after a rescan) before the stale entry was
    // unwatched. The newest entry wins; unwatching the stale one later is a no-op.
    uint32_t stale_id = 0;
    if (lookup_id_locked(self, path, &stale_id)) {
        remove_id_locked(self, stale_id);
    }

    const uint32_t id = self->next_id++;
    char *owned_path = g_strdup(path);
    g_hash_table_insert(self->path_to_id, owned_path, GUINT_TO_POINTER(id));
    g_hash_table_insert(self->id_to_path, GUINT_TO_POINTER(id), owned_path);
    g_hash_table_insert(self->id_to_folder, GUINT_TO_POINTER(id), folder);
    g_hash_table_insert(self->folder_to_id, folder, GUINT_TO_POINTER(id));

    db_entry_set_monitored_fsevents(folder);
    return true;
}

void
fsearch_folder_monitor_fsevents_unwatch(FsearchFolderMonitorFsevents *self, FsearchDatabaseEntry *folder) {
    g_return_if_fail(self);
    g_return_if_fail(folder);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    gpointer id = NULL;
    if (g_hash_table_lookup_extended(self->folder_to_id, folder, NULL, &id)) {
        remove_id_locked(self, GPOINTER_TO_UINT(id));
    }
    db_entry_set_unmonitored_fsevents(folder);
}

FsearchDatabaseEntry *
fsearch_folder_monitor_fsevents_resolve(FsearchFolderMonitorFsevents *self, gpointer handle) {
    g_return_val_if_fail(self, NULL);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    return g_hash_table_lookup(self->id_to_folder, handle);
}

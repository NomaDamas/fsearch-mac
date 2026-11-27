/*
 * End-to-end check of the macOS FSEvents folder monitor: a monitored FsearchDatabaseIndex must
 * pick up files and folders that are created, renamed and deleted on disk after the initial scan,
 * without rescanning.
 *
 * FSEvents delivers asynchronously, so every expectation polls the index (processing queued
 * monitor events in between) until it holds or a generous timeout expires.
 */

#include "fsearch_database_chunked_array.h"
#include "fsearch_database_entry.h"
#include "fsearch_database_exclude_manager.h"
#include "fsearch_database_include.h"
#include "fsearch_database_include_manager.h"
#include "fsearch_database_index_store.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>

#define WAIT_TIMEOUT_USEC (10 * G_USEC_PER_SEC)

static bool
store_contains(FsearchDatabaseIndexStore *store, const char *path, bool is_dir) {
    fsearch_database_index_store_lock(store);
    g_autoptr(FsearchDatabaseChunkedArray) entries =
        is_dir ? fsearch_database_index_store_get_folders(store, DATABASE_INDEX_PROPERTY_PATH)
               : fsearch_database_index_store_get_files(store, DATABASE_INDEX_PROPERTY_PATH);
    bool found = false;
    const uint32_t count = entries ? fsearch_database_chunked_array_get_num_entries(entries) : 0;
    for (uint32_t i = 0; i < count && !found; i++) {
        FsearchDatabaseEntry *entry = fsearch_database_chunked_array_get_entry(entries, i);
        g_autoptr(GString) entry_path = db_entry_get_path_full(entry);
        found = g_strcmp0(entry_path->str, path) == 0;
    }
    fsearch_database_index_store_unlock(store);
    return found;
}

static void
wait_for(FsearchDatabaseIndexStore *store, const char *path, bool is_dir, bool expected) {
    const gint64 deadline = g_get_monotonic_time() + WAIT_TIMEOUT_USEC;
    while (store_contains(store, path, is_dir) != expected) {
        if (g_get_monotonic_time() > deadline) {
            g_error("timed out waiting for %s to be %s the index", path, expected ? "in" : "removed from");
        }
        g_usleep(50 * 1000);
    }
}

static void
remove_tree(const char *path) {
    g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const char *name = NULL;
        while ((name = g_dir_read_name(dir))) {
            g_autofree char *child = g_build_filename(path, name, NULL);
            remove_tree(child);
        }
    }
    g_remove(path);
}

static void
test_monitor_tracks_changes(void) {
    // Resolve symlinks (e.g. /var -> /private/var) up front so paths compare verbatim.
    g_autofree char *tmp = g_dir_make_tmp("fsearch-test-fsevents-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *root = realpath(tmp, NULL);
    g_assert_nonnull(root);

    g_autofree char *existing = g_build_filename(root, "existing.txt", NULL);
    g_assert_true(g_file_set_contents(existing, "x", -1, NULL));

    g_autoptr(FsearchDatabaseIncludeManager) includes = fsearch_database_include_manager_new();
    g_autoptr(FsearchDatabaseInclude) include = fsearch_database_include_new(root, TRUE, FALSE, TRUE, FALSE, 0);
    fsearch_database_include_manager_add(includes, include);
    g_autoptr(FsearchDatabaseExcludeManager) excludes = fsearch_database_exclude_manager_new();
    g_autoptr(FsearchDatabaseIndexStore) store =
        fsearch_database_index_store_new(includes, excludes, DATABASE_INDEX_PROPERTY_FLAG_DEFAULT, NULL, NULL);

    fsearch_database_index_store_start(store, NULL);
    fsearch_database_index_store_start_monitoring(store);
    g_assert_true(store_contains(store, existing, false));

    // FSEvents only reports changes made after the stream started.
    g_usleep(500 * 1000);

    g_autofree char *created = g_build_filename(root, "created.txt", NULL);
    g_assert_true(g_file_set_contents(created, "hello", -1, NULL));
    wait_for(store, created, false, true);

    g_autofree char *folder = g_build_filename(root, "folder", NULL);
    g_autofree char *nested = g_build_filename(folder, "nested.md", NULL);
    g_assert_cmpint(g_mkdir(folder, 0755), ==, 0);
    wait_for(store, folder, true, true);
    // A file inside a folder that only appeared after the scan must be tracked as well.
    g_assert_true(g_file_set_contents(nested, "deep", -1, NULL));
    wait_for(store, nested, false, true);

    g_autofree char *renamed = g_build_filename(root, "renamed.txt", NULL);
    g_assert_cmpint(g_rename(created, renamed), ==, 0);
    wait_for(store, renamed, false, true);
    wait_for(store, created, false, false);

    g_autofree char *moved_folder = g_build_filename(root, "moved", NULL);
    g_autofree char *moved_nested = g_build_filename(moved_folder, "nested.md", NULL);
    g_assert_cmpint(g_rename(folder, moved_folder), ==, 0);
    wait_for(store, moved_nested, false, true);
    wait_for(store, nested, false, false);

    g_assert_cmpint(g_remove(existing), ==, 0);
    wait_for(store, existing, false, false);

    g_assert_cmpint(g_remove(moved_nested), ==, 0);
    g_assert_cmpint(g_rmdir(moved_folder), ==, 0);
    wait_for(store, moved_folder, true, false);
    wait_for(store, moved_nested, false, false);

    remove_tree(root);
}

int
main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/folder_monitor/fsevents/tracks_changes", test_monitor_tracks_changes);
    return g_test_run();
}

/*
 * End-to-end check of the macOS FSEvents folder monitor: a monitored FsearchDatabaseIndex must
 * pick up files and folders that are created, renamed and deleted on disk after the initial scan,
 * without rescanning.
 *
 * FSEvents delivers asynchronously, so every expectation polls the index (processing queued
 * monitor events in between) until it holds or a generous timeout expires.
 */

#include "fsearch_database_entry.h"
#include "fsearch_database_exclude_manager.h"
#include "fsearch_database_include.h"
#include "fsearch_database_index.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>

#define WAIT_TIMEOUT_USEC (10 * G_USEC_PER_SEC)

static bool
index_contains(FsearchDatabaseIndex *index, const char *path, bool is_dir) {
    fsearch_database_index_lock(index);
    g_autoptr(DynamicArray) entries = is_dir ? fsearch_database_index_get_folders(index)
                                             : fsearch_database_index_get_files(index);
    bool found = false;
    for (uint32_t i = 0; entries && i < darray_get_num_items(entries) && !found; i++) {
        g_autoptr(GString) entry_path = db_entry_get_path_full(darray_get_item(entries, i));
        found = g_strcmp0(entry_path->str, path) == 0;
    }
    fsearch_database_index_unlock(index);
    return found;
}

static void
wait_for(FsearchDatabaseIndex *index, const char *path, bool is_dir, bool expected) {
    const gint64 deadline = g_get_monotonic_time() + WAIT_TIMEOUT_USEC;
    while (index_contains(index, path, is_dir) != expected) {
        if (g_get_monotonic_time() > deadline) {
            g_error("timed out waiting for %s to be %s the index", path, expected ? "in" : "removed from");
        }
        fsearch_database_index_process_events(index);
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

    g_autoptr(FsearchDatabaseInclude) include = fsearch_database_include_new(root, TRUE, FALSE, TRUE, FALSE, 0);
    g_autoptr(FsearchDatabaseExcludeManager) excludes = fsearch_database_exclude_manager_new();
    g_autoptr(GMainContext) monitor_ctx = g_main_context_new();
    g_autoptr(FsearchDatabaseIndex) index =
        fsearch_database_index_new(include, excludes, DATABASE_INDEX_PROPERTY_FLAG_DEFAULT, monitor_ctx, NULL, NULL);

    g_assert_true(fsearch_database_index_scan(index, NULL));
    fsearch_database_index_start_monitoring(index, true);
    g_assert_true(index_contains(index, existing, false));

    // FSEvents only reports changes made after the stream started.
    g_usleep(500 * 1000);

    g_autofree char *created = g_build_filename(root, "created.txt", NULL);
    g_assert_true(g_file_set_contents(created, "hello", -1, NULL));
    wait_for(index, created, false, true);

    g_autofree char *folder = g_build_filename(root, "folder", NULL);
    g_autofree char *nested = g_build_filename(folder, "nested.md", NULL);
    g_assert_cmpint(g_mkdir(folder, 0755), ==, 0);
    wait_for(index, folder, true, true);
    // A file inside a folder that only appeared after the scan must be tracked as well.
    g_assert_true(g_file_set_contents(nested, "deep", -1, NULL));
    wait_for(index, nested, false, true);

    g_autofree char *renamed = g_build_filename(root, "renamed.txt", NULL);
    g_assert_cmpint(g_rename(created, renamed), ==, 0);
    wait_for(index, renamed, false, true);
    wait_for(index, created, false, false);

    g_autofree char *moved_folder = g_build_filename(root, "moved", NULL);
    g_autofree char *moved_nested = g_build_filename(moved_folder, "nested.md", NULL);
    g_assert_cmpint(g_rename(folder, moved_folder), ==, 0);
    wait_for(index, moved_nested, false, true);
    wait_for(index, nested, false, false);

    g_assert_cmpint(g_remove(existing), ==, 0);
    wait_for(index, existing, false, false);

    g_assert_cmpint(g_remove(moved_nested), ==, 0);
    g_assert_cmpint(g_rmdir(moved_folder), ==, 0);
    wait_for(index, moved_folder, true, false);
    wait_for(index, moved_nested, false, false);

    remove_tree(root);
}

int
main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/folder_monitor/fsevents/tracks_changes", test_monitor_tracks_changes);
    return g_test_run();
}

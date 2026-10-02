/*
   fsearch-cli - headless indexing, monitoring and search for FSearch databases
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

#define G_LOG_DOMAIN "fsearch-cli"

#include "fsearch_database.h"
#include "fsearch_database_entry_info.h"
#include "fsearch_database_exclude.h"
#include "fsearch_database_exclude_manager.h"
#include "fsearch_database_file.h"
#include "fsearch_database_include.h"
#include "fsearch_database_include_manager.h"
#include "fsearch_database_index_properties.h"
#include "fsearch_database_info.h"
#include "fsearch_database_search_info.h"
#include "fsearch_database_work.h"
#include "fsearch_filter_manager.h"
#include "fsearch_query.h"

#include <config.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLI_VIEW_ID 1
#define CLI_DEFAULT_LIMIT 100
#define CLI_ITEM_INFO_FLAGS                                                                                            \
    (FSEARCH_DATABASE_ENTRY_INFO_FLAG_NAME | FSEARCH_DATABASE_ENTRY_INFO_FLAG_PATH_FULL                                \
     | FSEARCH_DATABASE_ENTRY_INFO_FLAG_SIZE | FSEARCH_DATABASE_ENTRY_INFO_FLAG_MODIFICATION_TIME)

// region Shared helpers

static void
die(const char *format, ...) G_GNUC_PRINTF(1, 2) G_GNUC_NORETURN;

static void
die(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("fsearch-cli: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(EXIT_FAILURE);
}

static char *
default_db_path(void) {
    return g_build_filename(g_get_user_data_dir(), "fsearch", "fsearch.db", NULL);
}

static char *
socket_path_for_db(const char *db_path) {
    return g_strconcat(db_path, ".sock", NULL);
}

static char *
absolute_path(const char *path) {
    char resolved[PATH_MAX] = "";
    if (realpath(path, resolved)) {
        return g_strdup(resolved);
    }
    return g_canonicalize_filename(path, NULL);
}

static void
json_append_string(GString *out, const char *str) {
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)str; p && *p; p++) {
        switch (*p) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if (*p < 0x20) {
                g_string_append_printf(out, "\\u%04x", *p);
            }
            else {
                g_string_append_c(out, (char)*p);
            }
        }
    }
    g_string_append_c(out, '"');
}

typedef enum {
    OUTPUT_PATHS,
    OUTPUT_JSON,
    // Only the number of matches; no rows are fetched.
    OUTPUT_COUNT,
} OutputFormat;

typedef struct {
    char *query;
    uint32_t limit;
    FsearchQueryFlags flags;
    FsearchDatabaseIndexProperty sort_order;
    GtkSortType sort_type;
} SearchRequest;

static void
search_request_clear(SearchRequest *request) {
    g_clear_pointer(&request->query, g_free);
}

static FsearchDatabaseIndexProperty
parse_sort_order(const char *name) {
    if (!name || g_strcmp0(name, "name") == 0) {
        return DATABASE_INDEX_PROPERTY_NAME;
    }
    if (g_strcmp0(name, "path") == 0) {
        return DATABASE_INDEX_PROPERTY_PATH;
    }
    if (g_strcmp0(name, "size") == 0) {
        return DATABASE_INDEX_PROPERTY_SIZE;
    }
    if (g_strcmp0(name, "mtime") == 0) {
        return DATABASE_INDEX_PROPERTY_MODIFICATION_TIME;
    }
    return DATABASE_INDEX_PROPERTY_NONE;
}

static const char *
sort_order_to_string(FsearchDatabaseIndexProperty sort_order) {
    switch (sort_order) {
    case DATABASE_INDEX_PROPERTY_PATH:
        return "path";
    case DATABASE_INDEX_PROPERTY_SIZE:
        return "size";
    case DATABASE_INDEX_PROPERTY_MODIFICATION_TIME:
        return "mtime";
    default:
        return "name";
    }
}

// Pumps the default main context until `*done` becomes true. Database signals are delivered there.
static void
wait_until(volatile bool *done) {
    while (!*done) {
        g_main_context_iteration(NULL, TRUE);
    }
}

static void
on_flag_signal(FsearchDatabase *db, gpointer arg1, gpointer user_data) {
    (void)db;
    (void)arg1;
    *(volatile bool *)user_data = true;
}

static FsearchDatabaseEntryInfo *
get_item_info_blocking(FsearchDatabase *db, uint32_t idx) {
    // The store lock is held for short moments by the monitor thread; just retry.
    for (int attempt = 0; attempt < 10000; attempt++) {
        FsearchDatabaseEntryInfo *info = NULL;
        const FsearchResult res = fsearch_database_try_get_item_info(db, CLI_VIEW_ID, idx, CLI_ITEM_INFO_FLAGS, &info);
        if (res == FSEARCH_RESULT_SUCCESS) {
            return info;
        }
        if (res != FSEARCH_RESULT_DB_BUSY) {
            return NULL;
        }
        g_usleep(1000);
    }
    return NULL;
}

static FsearchDatabaseInfo *
get_database_info_blocking(FsearchDatabase *db) {
    for (int attempt = 0; attempt < 10000; attempt++) {
        FsearchDatabaseInfo *info = NULL;
        const FsearchResult res = fsearch_database_try_get_database_info(db, &info);
        if (res == FSEARCH_RESULT_SUCCESS) {
            return info;
        }
        if (res != FSEARCH_RESULT_DB_BUSY) {
            return NULL;
        }
        g_usleep(1000);
    }
    return NULL;
}

typedef struct {
    volatile bool done;
    FsearchDatabaseSearchInfo *info;
} SearchWait;

static void
on_search_finished(FsearchDatabase *db, guint id, FsearchDatabaseSearchInfo *info, gpointer user_data) {
    (void)db;
    SearchWait *wait = user_data;
    if (id != CLI_VIEW_ID) {
        return;
    }
    g_clear_pointer(&wait->info, fsearch_database_search_info_unref);
    wait->info = info ? fsearch_database_search_info_ref(info) : NULL;
    wait->done = true;
}

// Runs a search on `db` and appends the formatted results to `out`. Returns the number of
// matches, which can be larger than the number of printed rows.
static uint32_t
run_search(FsearchDatabase *db, FsearchFilterManager *filters, const SearchRequest *request, OutputFormat format, GString *out) {
    g_autoptr(FsearchQuery) query = fsearch_query_new(request->query, NULL, filters, request->flags, "fsearch-cli");

    SearchWait wait = {};
    const gulong handler = g_signal_connect(db, "search-finished", G_CALLBACK(on_search_finished), &wait);
    g_autoptr(FsearchDatabaseWork) work =
        fsearch_database_work_new_search(CLI_VIEW_ID, query, request->sort_order, request->sort_type);
    fsearch_database_queue_work(db, work);
    wait_until(&wait.done);
    g_signal_handler_disconnect(db, handler);

    const uint32_t num_results = wait.info ? fsearch_database_search_info_get_num_entries(wait.info) : 0;
    g_clear_pointer(&wait.info, fsearch_database_search_info_unref);

    if (format == OUTPUT_COUNT) {
        g_string_append_printf(out, "%u\n", num_results);
        return num_results;
    }

    const uint32_t num_rows = request->limit > 0 ? MIN(request->limit, num_results) : num_results;
    for (uint32_t i = 0; i < num_rows; i++) {
        g_autoptr(FsearchDatabaseEntryInfo) info = get_item_info_blocking(db, i);
        if (!info) {
            continue;
        }
        GString *path = fsearch_database_entry_info_get_path_full(info);
        if (!path) {
            continue;
        }
        if (format == OUTPUT_PATHS) {
            g_string_append(out, path->str);
            g_string_append_c(out, '\n');
            continue;
        }
        const bool is_dir = fsearch_database_entry_info_get_entry_type(info) == DATABASE_ENTRY_TYPE_FOLDER;
        GString *name = fsearch_database_entry_info_get_name(info);
        g_string_append(out, "{\"path\":");
        json_append_string(out, path->str);
        g_string_append(out, ",\"name\":");
        json_append_string(out, name ? name->str : "");
        g_string_append_printf(out,
                               ",\"type\":\"%s\",\"size\":%lld,\"mtime\":%lld}\n",
                               is_dir ? "folder" : "file",
                               (long long)fsearch_database_entry_info_get_size(info),
                               (long long)fsearch_database_entry_info_get_mtime(info));
    }
    if (format == OUTPUT_JSON) {
        g_string_append_printf(out, "{\"done\":true,\"num_results\":%u,\"num_returned\":%u}\n", num_results, num_rows);
    }
    return num_results;
}

static void
append_stats_json(GString *out, FsearchDatabaseInfo *info, const char *db_path, bool live) {
    g_string_append(out, "{\"db\":");
    json_append_string(out, db_path);
    g_string_append_printf(out,
                           ",\"live\":%s,\"files\":%u,\"folders\":%u,\"includes\":[",
                           live ? "true" : "false",
                           info ? fsearch_database_info_get_num_files(info) : 0,
                           info ? fsearch_database_info_get_num_folders(info) : 0);
    g_autoptr(FsearchDatabaseIncludeManager) includes = info ? fsearch_database_info_get_include_manager(info) : NULL;
    g_autoptr(GPtrArray) include_array = includes ? fsearch_database_include_manager_get_includes(includes) : NULL;
    for (uint32_t i = 0; include_array && i < include_array->len; i++) {
        FsearchDatabaseInclude *include = g_ptr_array_index(include_array, i);
        if (i > 0) {
            g_string_append_c(out, ',');
        }
        g_string_append(out, "{\"path\":");
        json_append_string(out, fsearch_database_include_get_path(include));
        g_string_append_printf(out,
                               ",\"monitor\":%s,\"active\":%s,\"last_scan_time\":%lld}",
                               fsearch_database_include_get_monitored(include) ? "true" : "false",
                               fsearch_database_include_get_active(include) ? "true" : "false",
                               (long long)fsearch_database_include_get_last_scan_time(include));
    }
    g_string_append(out, "]}\n");
}

// Opens an existing database file. Includes and excludes are taken from the file itself so it
// loads regardless of how it was created. `monitor` forces folder monitoring on or leaves the
// stored setting alone (when NULL).
static FsearchDatabase *
open_database(const char *db_path, bool read_only, const bool *monitor) {
    FsearchDatabaseIncludeManager *stored_includes = NULL;
    FsearchDatabaseExcludeManager *excludes = NULL;
    FsearchDatabaseIndexPropertyFlags flags = 0;
    if (!fsearch_database_file_load_config(db_path, &stored_includes, &excludes, &flags)) {
        die("can't read database %s (create it with `fsearch-cli index`)", db_path);
    }

    FsearchDatabaseIncludeManager *includes = stored_includes;
    if (monitor) {
        includes = fsearch_database_include_manager_new();
        g_autoptr(GPtrArray) array = fsearch_database_include_manager_get_includes(stored_includes);
        for (uint32_t i = 0; i < array->len; i++) {
            FsearchDatabaseInclude *include = g_ptr_array_index(array, i);
            g_autoptr(FsearchDatabaseInclude) copy =
                fsearch_database_include_new(fsearch_database_include_get_path(include),
                                             fsearch_database_include_get_active(include),
                                             fsearch_database_include_get_one_file_system(include),
                                             *monitor,
                                             fsearch_database_include_get_scan_after_launch(include),
                                             fsearch_database_include_get_rescan_after(include));
            fsearch_database_include_manager_add(includes, copy);
        }
        g_object_unref(stored_includes);
    }

    FsearchDatabase *db = fsearch_database_new(g_file_new_for_path(db_path), includes, excludes);
    fsearch_database_set_read_only(db, read_only);
    g_object_unref(includes);
    g_object_unref(excludes);

    volatile bool loaded = false;
    const gulong handler = g_signal_connect(db, "load-finished", G_CALLBACK(on_flag_signal), (gpointer)&loaded);
    g_autoptr(FsearchDatabaseWork) work = fsearch_database_work_new_load();
    fsearch_database_queue_work(db, work);
    wait_until(&loaded);
    g_signal_handler_disconnect(db, handler);
    return db;
}

// endregion

// region index

static int
cmd_index(int argc, char **argv) {
    g_autofree char *db_path = NULL;
    g_auto(GStrv) include_paths = NULL;
    g_auto(GStrv) exclude_patterns = NULL;
    g_auto(GStrv) exclude_paths = NULL;
    gboolean no_monitor = FALSE;
    gboolean exclude_hidden = FALSE;
    gboolean one_file_system = FALSE;
    gboolean no_default_excludes = FALSE;
    gint64 rescan_after = 0;

    GOptionEntry entries[] = {
        {"db", 'd', 0, G_OPTION_ARG_FILENAME, &db_path, "Database file", "PATH"},
        {"include", 'i', 0, G_OPTION_ARG_FILENAME_ARRAY, &include_paths, "Folder to index (repeatable)", "DIR"},
        {"exclude", 'e', 0, G_OPTION_ARG_STRING_ARRAY, &exclude_patterns, "Wildcard to exclude by name (repeatable)", "GLOB"},
        {"exclude-path", 'x', 0, G_OPTION_ARG_FILENAME_ARRAY, &exclude_paths, "Folder to exclude by full path (repeatable)", "DIR"},
        {"exclude-hidden", 0, 0, G_OPTION_ARG_NONE, &exclude_hidden, "Skip hidden files and folders", NULL},
        {"one-file-system", 0, 0, G_OPTION_ARG_NONE, &one_file_system, "Don't cross filesystem boundaries", NULL},
        {"no-default-excludes", 0, 0, G_OPTION_ARG_NONE, &no_default_excludes, "Don't exclude /dev and the magic namespaces", NULL},
        {"no-monitor", 0, 0, G_OPTION_ARG_NONE, &no_monitor, "Don't mark folders for live monitoring", NULL},
        {"rescan-after", 0, 0, G_OPTION_ARG_INT64, &rescan_after, "Periodic rescan interval in seconds (0 = off)", "SECONDS"},
        {NULL},
    };
    g_autoptr(GOptionContext) context = g_option_context_new("- build a database from folders");
    g_option_context_add_main_entries(context, entries, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        die("%s", error->message);
    }
    if (!include_paths || !include_paths[0]) {
        die("index: at least one --include DIR is required");
    }
    if (!db_path) {
        db_path = default_db_path();
    }
    g_autofree char *db_dir = g_path_get_dirname(db_path);
    g_mkdir_with_parents(db_dir, 0700);

    FsearchDatabaseIncludeManager *includes = fsearch_database_include_manager_new();
    for (char **p = include_paths; *p; p++) {
        g_autofree char *path = absolute_path(*p);
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
            die("index: not a directory: %s", *p);
        }
        g_autoptr(FsearchDatabaseInclude) include =
            fsearch_database_include_new(path, TRUE, one_file_system, !no_monitor, FALSE, rescan_after);
        fsearch_database_include_manager_add(includes, include);
    }

    FsearchDatabaseExcludeManager *excludes = no_default_excludes ? fsearch_database_exclude_manager_new()
                                                                  : fsearch_database_exclude_manager_new_with_defaults();
    fsearch_database_exclude_manager_set_exclude_hidden(excludes, exclude_hidden);
    for (char **p = exclude_patterns; p && *p; p++) {
        g_autoptr(FsearchDatabaseExclude) exclude = fsearch_database_exclude_new(*p,
                                                                                 TRUE,
                                                                                 FSEARCH_DATABASE_EXCLUDE_TYPE_WILDCARD,
                                                                                 FSEARCH_DATABASE_EXCLUDE_MATCH_SCOPE_BASENAME,
                                                                                 FSEARCH_DATABASE_EXCLUDE_TARGET_BOTH);
        fsearch_database_exclude_manager_add(excludes, exclude);
    }
    for (char **p = exclude_paths; p && *p; p++) {
        g_autofree char *path = absolute_path(*p);
        g_autoptr(FsearchDatabaseExclude) exclude = fsearch_database_exclude_new(path,
                                                                                 TRUE,
                                                                                 FSEARCH_DATABASE_EXCLUDE_TYPE_FIXED,
                                                                                 FSEARCH_DATABASE_EXCLUDE_MATCH_SCOPE_FULL_PATH,
                                                                                 FSEARCH_DATABASE_EXCLUDE_TARGET_FOLDERS);
        fsearch_database_exclude_manager_add(excludes, exclude);
    }

    FsearchDatabase *db = fsearch_database_new(g_file_new_for_path(db_path), includes, excludes);
    g_object_unref(includes);
    g_object_unref(excludes);

    g_autoptr(GTimer) timer = g_timer_new();
    fsearch_database_rescan_blocking(db);
    const double scan_time = g_timer_elapsed(timer, NULL);

    g_autoptr(FsearchDatabaseInfo) info = get_database_info_blocking(db);
    const uint32_t files = info ? fsearch_database_info_get_num_files(info) : 0;
    const uint32_t folders = info ? fsearch_database_info_get_num_folders(info) : 0;

    // Disposing saves the database file.
    g_timer_start(timer);
    g_object_unref(db);
    fprintf(stderr,
            "indexed %u files, %u folders in %.2fs (saved in %.2fs) -> %s\n",
            files,
            folders,
            scan_time,
            g_timer_elapsed(timer, NULL),
            db_path);
    return EXIT_SUCCESS;
}

// endregion

// region Daemon client

static GSocketConnection *
connect_daemon(const char *socket_path) {
    if (!g_file_test(socket_path, G_FILE_TEST_EXISTS)) {
        return NULL;
    }
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
    return g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
}

// Sends one request line and copies the daemon's reply to stdout until it closes the connection.
// Returns false when the daemon couldn't be reached.
static bool
daemon_request(const char *socket_path, const char *request, bool stream_forever) {
    g_autoptr(GSocketConnection) connection = connect_daemon(socket_path);
    if (!connection) {
        return false;
    }
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    g_autofree char *line = g_strconcat(request, "\n", NULL);
    if (!g_output_stream_write_all(output, line, strlen(line), NULL, NULL, NULL)) {
        return false;
    }
    g_autoptr(GDataInputStream) input =
        g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    while (true) {
        g_autofree char *reply = g_data_input_stream_read_line_utf8(input, NULL, NULL, NULL);
        if (!reply) {
            break;
        }
        puts(reply);
        if (stream_forever) {
            fflush(stdout);
        }
    }
    return true;
}

// endregion

// region search

static int
cmd_search(int argc, char **argv) {
    g_autofree char *db_path = NULL;
    g_autofree char *socket_path = NULL;
    g_autofree char *sort = NULL;
    gint limit = CLI_DEFAULT_LIMIT;
    gboolean descending = FALSE;
    gboolean regex = FALSE;
    gboolean match_case = FALSE;
    gboolean in_path = FALSE;
    gboolean files_only = FALSE;
    gboolean folders_only = FALSE;
    gboolean json = FALSE;
    gboolean count = FALSE;
    gboolean cold = FALSE;

    GOptionEntry entries[] = {
        {"db", 'd', 0, G_OPTION_ARG_FILENAME, &db_path, "Database file", "PATH"},
        {"socket", 's', 0, G_OPTION_ARG_FILENAME, &socket_path, "Daemon socket (default: <db>.sock)", "PATH"},
        {"no-daemon", 0, 0, G_OPTION_ARG_NONE, &cold, "Always load the database file instead of asking `watch`", NULL},
        {"limit", 'n', 0, G_OPTION_ARG_INT, &limit, "Maximum number of results (0 = all, default 100)", "N"},
        {"sort", 0, 0, G_OPTION_ARG_STRING, &sort, "Sort by name, path, size or mtime", "KEY"},
        {"desc", 0, 0, G_OPTION_ARG_NONE, &descending, "Sort descending", NULL},
        {"regex", 'r', 0, G_OPTION_ARG_NONE, &regex, "Treat the query as a regular expression", NULL},
        {"case", 'c', 0, G_OPTION_ARG_NONE, &match_case, "Match case", NULL},
        {"path", 'p', 0, G_OPTION_ARG_NONE, &in_path, "Match against the full path, not just the name", NULL},
        {"files", 'f', 0, G_OPTION_ARG_NONE, &files_only, "Only files", NULL},
        {"folders", 'F', 0, G_OPTION_ARG_NONE, &folders_only, "Only folders", NULL},
        {"json", 'j', 0, G_OPTION_ARG_NONE, &json, "Emit JSON lines", NULL},
        {"count", 0, 0, G_OPTION_ARG_NONE, &count, "Only print the number of matches", NULL},
        {NULL},
    };
    g_autoptr(GOptionContext) context = g_option_context_new("QUERY... - search a database");
    g_option_context_set_description(context,
                                     "QUERY uses FSearch's search syntax (wildcards, AND/OR/NOT, ext:, size:, "
                                     "path:, case:, regex: ...).\n"
                                     "Searches the running `fsearch-cli watch` daemon when one serves the database, "
                                     "otherwise loads the database file.");
    g_option_context_add_main_entries(context, entries, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        die("%s", error->message);
    }
    if (limit < 0) {
        die("search: --limit must be >= 0");
    }

    SearchRequest request = {
        .query = argc > 1 ? g_strjoinv(" ", argv + 1) : g_strdup(""),
        .limit = count ? 0 : (uint32_t)limit,
        .sort_order = parse_sort_order(sort),
        .sort_type = descending ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING,
    };
    if (request.sort_order == DATABASE_INDEX_PROPERTY_NONE) {
        die("search: unknown sort key: %s", sort);
    }
    request.flags |= regex ? QUERY_FLAG_REGEX : 0;
    request.flags |= match_case ? QUERY_FLAG_MATCH_CASE : QUERY_FLAG_AUTO_MATCH_CASE;
    request.flags |= in_path ? QUERY_FLAG_SEARCH_IN_PATH : QUERY_FLAG_AUTO_SEARCH_IN_PATH;
    request.flags |= files_only ? QUERY_FLAG_FILES_ONLY : 0;
    request.flags |= folders_only ? QUERY_FLAG_FOLDERS_ONLY : 0;

    if (!db_path) {
        db_path = default_db_path();
    }
    if (!socket_path) {
        socket_path = socket_path_for_db(db_path);
    }

    const OutputFormat format = json || count ? OUTPUT_JSON : OUTPUT_PATHS;

    if (!cold) {
        // Protocol: search <TAB> limit <TAB> flags <TAB> sort <TAB> asc|desc <TAB> format <TAB> query
        g_autofree char *line = g_strdup_printf("search\t%u\t%u\t%s\t%s\t%s\t%s",
                                                request.limit,
                                                (unsigned)request.flags,
                                                sort_order_to_string(request.sort_order),
                                                descending ? "desc" : "asc",
                                                count ? "count" : (format == OUTPUT_JSON ? "json" : "paths"),
                                                request.query);
        if (daemon_request(socket_path, line, false)) {
            search_request_clear(&request);
            return EXIT_SUCCESS;
        }
    }

    FsearchDatabase *db = open_database(db_path, true, NULL);
    FsearchFilterManager *filters = fsearch_filter_manager_new_with_defaults();
    g_autoptr(GString) out = g_string_new(NULL);
    run_search(db, filters, &request, count ? OUTPUT_COUNT : format, out);
    fwrite(out->str, 1, out->len, stdout);
    search_request_clear(&request);
    fsearch_filter_manager_unref(filters);
    g_object_unref(db);
    return EXIT_SUCCESS;
}

// endregion

// region stats

static int
cmd_stats(int argc, char **argv) {
    g_autofree char *db_path = NULL;
    g_autofree char *socket_path = NULL;
    gboolean cold = FALSE;
    GOptionEntry entries[] = {
        {"db", 'd', 0, G_OPTION_ARG_FILENAME, &db_path, "Database file", "PATH"},
        {"socket", 's', 0, G_OPTION_ARG_FILENAME, &socket_path, "Daemon socket (default: <db>.sock)", "PATH"},
        {"no-daemon", 0, 0, G_OPTION_ARG_NONE, &cold, "Always load the database file", NULL},
        {NULL},
    };
    g_autoptr(GOptionContext) context = g_option_context_new("- print database statistics as JSON");
    g_option_context_add_main_entries(context, entries, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        die("%s", error->message);
    }
    if (!db_path) {
        db_path = default_db_path();
    }
    if (!socket_path) {
        socket_path = socket_path_for_db(db_path);
    }
    if (!cold && daemon_request(socket_path, "stats", false)) {
        return EXIT_SUCCESS;
    }
    FsearchDatabase *db = open_database(db_path, true, NULL);
    g_autoptr(FsearchDatabaseInfo) info = get_database_info_blocking(db);
    g_autoptr(GString) out = g_string_new(NULL);
    append_stats_json(out, info, db_path, false);
    fwrite(out->str, 1, out->len, stdout);
    g_object_unref(db);
    return EXIT_SUCCESS;
}

// endregion

// region events

static int
cmd_events(int argc, char **argv) {
    g_autofree char *db_path = NULL;
    g_autofree char *socket_path = NULL;
    GOptionEntry entries[] = {
        {"db", 'd', 0, G_OPTION_ARG_FILENAME, &db_path, "Database file", "PATH"},
        {"socket", 's', 0, G_OPTION_ARG_FILENAME, &socket_path, "Daemon socket (default: <db>.sock)", "PATH"},
        {NULL},
    };
    g_autoptr(GOptionContext) context = g_option_context_new("- stream index change events from `watch` as JSON lines");
    g_option_context_add_main_entries(context, entries, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        die("%s", error->message);
    }
    if (!db_path) {
        db_path = default_db_path();
    }
    if (!socket_path) {
        socket_path = socket_path_for_db(db_path);
    }
    if (!daemon_request(socket_path, "subscribe", true)) {
        die("events: no `fsearch-cli watch` daemon listening on %s", socket_path);
    }
    return EXIT_SUCCESS;
}

// endregion

// region watch (daemon)

typedef struct {
    GSocketConnection *connection;
    GDataInputStream *input;
} Client;

typedef struct {
    FsearchDatabase *db;
    FsearchFilterManager *filters;
    char *db_path;
    char *socket_path;
    GSocketService *service;
    GMainLoop *loop;
    GPtrArray *subscribers; // GOutputStream*
    GQueue pending_searches; // Client* waiting for the search slot
    bool search_running;
    bool dirty;
    bool verbose;
    guint save_interval;
} Daemon;

static void
client_free(Client *client) {
    g_clear_object(&client->input);
    if (client->connection) {
        g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
    }
    g_clear_object(&client->connection);
    g_free(client);
}

static void
client_reply(Client *client, const char *text, gsize len) {
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(client->connection));
    g_output_stream_write_all(output, text, len, NULL, NULL, NULL);
}

static void
daemon_log(Daemon *daemon, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void
daemon_log(Daemon *daemon, const char *format, ...) {
    if (!daemon->verbose) {
        return;
    }
    va_list args;
    va_start(args, format);
    g_autofree char *message = g_strdup_vprintf(format, args);
    va_end(args);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *stamp = g_date_time_format(now, "%H:%M:%S");
    fprintf(stderr, "[%s] %s\n", stamp, message);
}

static void
daemon_broadcast(Daemon *daemon, const char *line) {
    for (uint32_t i = 0; i < daemon->subscribers->len;) {
        GOutputStream *output = g_ptr_array_index(daemon->subscribers, i);
        if (!g_output_stream_write_all(output, line, strlen(line), NULL, NULL, NULL)
            || !g_output_stream_flush(output, NULL, NULL)) {
            g_ptr_array_remove_index_fast(daemon->subscribers, i);
            continue;
        }
        i++;
    }
}

static void
daemon_broadcast_info(Daemon *daemon, const char *event, FsearchDatabaseInfo *info) {
    g_autoptr(GString) line = g_string_new(NULL);
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    g_string_append_printf(line,
                           "{\"event\":\"%s\",\"files\":%u,\"folders\":%u,\"time\":%lld}\n",
                           event,
                           info ? fsearch_database_info_get_num_files(info) : 0,
                           info ? fsearch_database_info_get_num_folders(info) : 0,
                           (long long)g_date_time_to_unix(now));
    daemon_broadcast(daemon, line->str);
}

static void
handle_client_line(Daemon *daemon, Client *client, char *line);

static void
daemon_run_next_search(Daemon *daemon) {
    while (!daemon->search_running && !g_queue_is_empty(&daemon->pending_searches)) {
        Client *client = g_queue_pop_head(&daemon->pending_searches);
        g_auto(GStrv) fields = g_object_steal_data(G_OBJECT(client->connection), "fsearch-cli-request");
        SearchRequest request = {
            .limit = (uint32_t)g_ascii_strtoull(fields[1], NULL, 10),
            .flags = (FsearchQueryFlags)g_ascii_strtoull(fields[2], NULL, 10),
            .sort_order = parse_sort_order(fields[3]),
            .sort_type = g_strcmp0(fields[4], "desc") == 0 ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING,
            .query = g_strdup(fields[6]),
        };
        if (request.sort_order == DATABASE_INDEX_PROPERTY_NONE) {
            request.sort_order = DATABASE_INDEX_PROPERTY_NAME;
        }
        const OutputFormat format = g_strcmp0(fields[5], "count") == 0   ? OUTPUT_COUNT
                                    : g_strcmp0(fields[5], "paths") == 0 ? OUTPUT_PATHS
                                                                         : OUTPUT_JSON;

        // run_search() pumps the main context; mark the slot busy so re-entrant requests queue up.
        daemon->search_running = true;
        g_autoptr(GTimer) timer = g_timer_new();
        g_autoptr(GString) out = g_string_new(NULL);
        run_search(daemon->db, daemon->filters, &request, format, out);
        daemon->search_running = false;
        daemon_log(daemon, "search \"%s\" answered in %.1f ms", request.query, g_timer_elapsed(timer, NULL) * 1000.0);
        client_reply(client, out->str, out->len);
        search_request_clear(&request);
        client_free(client);
    }
}

typedef struct {
    Daemon *daemon;
    Client *client;
    volatile bool done;
} DaemonCommandContext;

static void
on_save_finished_reply(FsearchDatabase *db, gpointer user_data) {
    (void)db;
    DaemonCommandContext *ctx = user_data;
    ctx->done = true;
}

static void
daemon_save(Daemon *daemon) {
    g_autoptr(FsearchDatabaseWork) work = fsearch_database_work_new_save();
    fsearch_database_queue_work(daemon->db, work);
    daemon->dirty = false;
}

static void
handle_client_line(Daemon *daemon, Client *client, char *line) {
    g_strchomp(line);
    g_auto(GStrv) fields = g_strsplit(line, "\t", 7);
    const char *command = fields[0] ? fields[0] : "";

    if (g_strcmp0(command, "search") == 0) {
        if (g_strv_length(fields) != 7) {
            const char *message = "{\"error\":\"malformed search request\"}\n";
            client_reply(client, message, strlen(message));
            client_free(client);
            return;
        }
        g_object_set_data_full(G_OBJECT(client->connection),
                               "fsearch-cli-request",
                               g_steal_pointer(&fields),
                               (GDestroyNotify)g_strfreev);
        g_queue_push_tail(&daemon->pending_searches, client);
        daemon_run_next_search(daemon);
        return;
    }
    if (g_strcmp0(command, "stats") == 0) {
        g_autoptr(FsearchDatabaseInfo) info = get_database_info_blocking(daemon->db);
        g_autoptr(GString) out = g_string_new(NULL);
        append_stats_json(out, info, daemon->db_path, true);
        client_reply(client, out->str, out->len);
        client_free(client);
        return;
    }
    if (g_strcmp0(command, "subscribe") == 0) {
        GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(client->connection));
        // Keep the connection (and with it the stream) alive for as long as the subscriber is listed.
        g_object_set_data_full(G_OBJECT(output), "fsearch-cli-connection", g_object_ref(client->connection), g_object_unref);
        g_ptr_array_add(daemon->subscribers, g_object_ref(output));
        g_autoptr(FsearchDatabaseInfo) info = get_database_info_blocking(daemon->db);
        daemon_broadcast_info(daemon, "subscribed", info);
        g_clear_object(&client->input);
        g_clear_object(&client->connection);
        g_free(client);
        return;
    }
    if (g_strcmp0(command, "save") == 0) {
        DaemonCommandContext ctx = {.daemon = daemon, .client = client};
        const gulong handler = g_signal_connect(daemon->db, "save-finished", G_CALLBACK(on_save_finished_reply), &ctx);
        daemon_save(daemon);
        wait_until(&ctx.done);
        g_signal_handler_disconnect(daemon->db, handler);
        const char *message = "{\"done\":true}\n";
        client_reply(client, message, strlen(message));
        client_free(client);
        return;
    }
    if (g_strcmp0(command, "rescan") == 0) {
        g_autoptr(FsearchDatabaseWork) work = fsearch_database_work_new_rescan();
        fsearch_database_queue_work(daemon->db, work);
        const char *message = "{\"queued\":true}\n";
        client_reply(client, message, strlen(message));
        client_free(client);
        return;
    }
    const char *message = "{\"error\":\"unknown command\"}\n";
    client_reply(client, message, strlen(message));
    client_free(client);
}

static void
on_client_line(GObject *source, GAsyncResult *result, gpointer user_data) {
    Client *client = user_data;
    Daemon *daemon = g_object_get_data(G_OBJECT(client->connection), "fsearch-cli-daemon");
    g_autofree char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), result, NULL, NULL);
    if (!line) {
        client_free(client);
        return;
    }
    handle_client_line(daemon, client, line);
}

static gboolean
on_incoming(GSocketService *service, GSocketConnection *connection, GObject *source_object, gpointer user_data) {
    (void)service;
    (void)source_object;
    Daemon *daemon = user_data;
    Client *client = g_new0(Client, 1);
    client->connection = g_object_ref(connection);
    g_object_set_data(G_OBJECT(connection), "fsearch-cli-daemon", daemon);
    client->input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_data_input_stream_read_line_async(client->input, G_PRIORITY_DEFAULT, NULL, on_client_line, client);
    return TRUE;
}

static void
on_database_changed(FsearchDatabase *db, FsearchDatabaseInfo *info, gpointer user_data) {
    (void)db;
    Daemon *daemon = user_data;
    daemon->dirty = true;
    daemon_log(daemon,
               "index changed: %u files, %u folders",
               info ? fsearch_database_info_get_num_files(info) : 0,
               info ? fsearch_database_info_get_num_folders(info) : 0);
    daemon_broadcast_info(daemon, "changed", info);
}

static void
on_scan_finished(FsearchDatabase *db, FsearchDatabaseInfo *info, gpointer user_data) {
    (void)db;
    Daemon *daemon = user_data;
    daemon_log(daemon,
               "scan finished: %u files, %u folders",
               info ? fsearch_database_info_get_num_files(info) : 0,
               info ? fsearch_database_info_get_num_folders(info) : 0);
    daemon_broadcast_info(daemon, "scan-finished", info);
    // Persist the fresh scan right away, so a later crash doesn't lose it.
    daemon_save(daemon);
}

static void
on_database_progress(FsearchDatabase *db, const char *text, gpointer user_data) {
    (void)db;
    daemon_log(user_data, "%s", text);
}

static gboolean
on_save_timer(gpointer user_data) {
    Daemon *daemon = user_data;
    if (daemon->dirty) {
        daemon_log(daemon, "saving database");
        daemon_save(daemon);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
on_quit_signal(gpointer user_data) {
    Daemon *daemon = user_data;
    daemon_log(daemon, "shutting down");
    g_main_loop_quit(daemon->loop);
    return G_SOURCE_REMOVE;
}

static int
cmd_watch(int argc, char **argv) {
    g_autofree char *db_path = NULL;
    g_autofree char *socket_path = NULL;
    gint save_interval = 60;
    gboolean quiet = FALSE;
    GOptionEntry entries[] = {
        {"db", 'd', 0, G_OPTION_ARG_FILENAME, &db_path, "Database file", "PATH"},
        {"socket", 's', 0, G_OPTION_ARG_FILENAME, &socket_path, "Socket to serve on (default: <db>.sock)", "PATH"},
        {"save-interval", 0, 0, G_OPTION_ARG_INT, &save_interval, "Save changes every N seconds (default 60)", "N"},
        {"quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Don't log activity to stderr", NULL},
        {NULL},
    };
    g_autoptr(GOptionContext) context = g_option_context_new("- keep a database live via filesystem events and serve searches");
    g_option_context_set_description(context,
                                     "Loads the database, rescans monitored folders once, then applies filesystem "
                                     "events (FSEvents on macOS, inotify/fanotify on Linux) as they happen.\n"
                                     "Clients talk to it over a Unix socket: `fsearch-cli search`, `stats` and "
                                     "`events` use it automatically.");
    g_option_context_add_main_entries(context, entries, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        die("%s", error->message);
    }
    if (save_interval < 1) {
        die("watch: --save-interval must be >= 1");
    }
    if (!db_path) {
        db_path = default_db_path();
    }
    if (!socket_path) {
        socket_path = socket_path_for_db(db_path);
    }

    // Refuse to hijack the socket of a daemon that's still alive; clean up a stale one.
    g_autoptr(GSocketConnection) existing = connect_daemon(socket_path);
    if (existing) {
        die("watch: a daemon is already listening on %s", socket_path);
    }
    g_unlink(socket_path);

    Daemon daemon = {
        .db_path = db_path,
        .socket_path = socket_path,
        .verbose = !quiet,
        .save_interval = (guint)save_interval,
        .subscribers = g_ptr_array_new_with_free_func(g_object_unref),
        .filters = fsearch_filter_manager_new_with_defaults(),
        .loop = g_main_loop_new(NULL, FALSE),
    };
    g_queue_init(&daemon.pending_searches);

    const bool monitor = true;
    daemon.db = open_database(db_path, false, &monitor);
    g_signal_connect(daemon.db, "database-changed", G_CALLBACK(on_database_changed), &daemon);
    g_signal_connect(daemon.db, "scan-finished", G_CALLBACK(on_scan_finished), &daemon);
    g_signal_connect(daemon.db, "database-progress", G_CALLBACK(on_database_progress), &daemon);

    daemon.service = g_socket_service_new();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(daemon.service),
                                       address,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL,
                                       NULL,
                                       &error)) {
        die("watch: can't listen on %s: %s", socket_path, error->message);
    }
    g_chmod(socket_path, 0600);
    g_signal_connect(daemon.service, "incoming", G_CALLBACK(on_incoming), &daemon);
    g_socket_service_start(daemon.service);

    g_unix_signal_add(SIGINT, on_quit_signal, &daemon);
    g_unix_signal_add(SIGTERM, on_quit_signal, &daemon);
    g_timeout_add_seconds(daemon.save_interval, on_save_timer, &daemon);
    signal(SIGPIPE, SIG_IGN);

    g_autoptr(FsearchDatabaseInfo) info = get_database_info_blocking(daemon.db);
    fprintf(stderr,
            "watching %s (%u files, %u folders), serving on %s\n",
            db_path,
            info ? fsearch_database_info_get_num_files(info) : 0,
            info ? fsearch_database_info_get_num_folders(info) : 0,
            socket_path);

    g_main_loop_run(daemon.loop);

    g_socket_service_stop(daemon.service);
    g_socket_listener_close(G_SOCKET_LISTENER(daemon.service));
    g_clear_object(&daemon.service);
    g_unlink(socket_path);
    g_ptr_array_unref(daemon.subscribers);
    g_queue_clear_full(&daemon.pending_searches, (GDestroyNotify)client_free);
    // Disposing saves the database file.
    g_object_unref(daemon.db);
    fsearch_filter_manager_unref(daemon.filters);
    g_main_loop_unref(daemon.loop);
    return EXIT_SUCCESS;
}

// endregion

static void
print_usage(FILE *stream) {
    fputs("Usage: fsearch-cli COMMAND [OPTION...]\n"
          "\n"
          "Commands:\n"
          "  index    Build a database from one or more folders\n"
          "  watch    Keep a database live from filesystem events and serve searches\n"
          "  search   Search a database (through `watch` when it's running)\n"
          "  stats    Print database statistics as JSON\n"
          "  events   Stream index change events from `watch` as JSON lines\n"
          "\n"
          "Run `fsearch-cli COMMAND --help` for the options of a command.\n"
          "The database defaults to $XDG_DATA_HOME/fsearch/fsearch.db (shared with the FSearch app).\n",
          stream);
}

int
main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    g_set_prgname("fsearch-cli");

    if (argc < 2) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }
    const char *command = argv[1];
    // Hand the sub-command its own argv (argv[1] becomes the program name for GOptionContext).
    argc--;
    argv++;

    if (g_strcmp0(command, "index") == 0) {
        return cmd_index(argc, argv);
    }
    if (g_strcmp0(command, "search") == 0) {
        return cmd_search(argc, argv);
    }
    if (g_strcmp0(command, "watch") == 0) {
        return cmd_watch(argc, argv);
    }
    if (g_strcmp0(command, "stats") == 0) {
        return cmd_stats(argc, argv);
    }
    if (g_strcmp0(command, "events") == 0) {
        return cmd_events(argc, argv);
    }
    if (g_strcmp0(command, "-h") == 0 || g_strcmp0(command, "--help") == 0 || g_strcmp0(command, "help") == 0) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }
    if (g_strcmp0(command, "--version") == 0) {
        printf("fsearch-cli %s\n", PACKAGE_VERSION);
        return EXIT_SUCCESS;
    }
    fprintf(stderr, "fsearch-cli: unknown command: %s\n\n", command);
    print_usage(stderr);
    return EXIT_FAILURE;
}

/*
 * End-to-end CLI test for `fsearch-cli index --exclude-from`:
 * - excludes folders and files listed one-per-line in a file (including >20,000 entries without E2BIG)
 * - supports `--exclude-from -` (stdin)
 * - ignores blank lines and CRLF endings
 * - exits non-zero with an error when the exclude file does not exist
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <stdio.h>

static const char *
cli_binary(void) {
    const char *bin = g_getenv("FSEARCH_CLI_BIN");
    g_assert_nonnull(bin);
    return bin;
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
test_exclude_from_large_file(void) {
    g_autofree char *tmp = g_dir_make_tmp("fsearch-cli-exclude-from-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *root = realpath(tmp, NULL);
    g_assert_nonnull(root);

    g_autofree char *docs = g_build_filename(root, "docs", NULL);
    g_autofree char *keep_dir = g_build_filename(docs, "keep", NULL);
    g_autofree char *skip_dir = g_build_filename(docs, "skip-tail", NULL);
    g_assert_cmpint(g_mkdir_with_parents(keep_dir, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(skip_dir, 0755), ==, 0);

    g_autofree char *keep_file = g_build_filename(keep_dir, "target-keep.txt", NULL);
    g_autofree char *skip_single_file = g_build_filename(keep_dir, "target-file-excluded.txt", NULL);
    g_autofree char *skip_nested_file = g_build_filename(skip_dir, "target-hidden.txt", NULL);
    g_assert_true(g_file_set_contents(keep_file, "ok", -1, NULL));
    g_assert_true(g_file_set_contents(skip_single_file, "skip file", -1, NULL));
    g_assert_true(g_file_set_contents(skip_nested_file, "skip dir", -1, NULL));

    // Write 20,000 synthetic paths + CRLF + blank lines + our real folder & file exclusions at the tail.
    g_autofree char *exclude_file = g_build_filename(root, "excludes.txt", NULL);
    g_autoptr(GString) content = g_string_sized_new(1024 * 1024);
    for (guint i = 0; i < 20000; i++) {
        g_string_append_printf(content, "%s/synthetic-project-%u/node_modules/pkg-%u\n", docs, i, i);
    }
    g_string_append(content, "\n\r\n");
    g_string_append_printf(content, "%s\r\n", skip_dir);
    g_string_append_printf(content, "%s\n", skip_single_file);
    g_assert_true(g_file_set_contents(exclude_file, content->str, (gssize)content->len, NULL));

    g_autofree char *db_path = g_build_filename(root, "test.db", NULL);
    char *index_argv[] = {
        (char *)cli_binary(),
        "index",
        "--db",
        db_path,
        "--include",
        docs,
        "--exclude-from",
        exclude_file,
        "--no-monitor",
        NULL,
    };
    g_autofree char *idx_out = NULL;
    g_autofree char *idx_err = NULL;
    gint idx_status = 0;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_spawn_sync(NULL, index_argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &idx_out, &idx_err, &idx_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(idx_status, ==, 0);

    char *search_argv[] = {
        (char *)cli_binary(),
        "search",
        "--db",
        db_path,
        "--no-daemon",
        "--",
        "target-",
        NULL,
    };
    g_autofree char *search_out = NULL;
    g_autofree char *search_err = NULL;
    gint search_status = 0;
    g_assert_true(
        g_spawn_sync(NULL, search_argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &search_out, &search_err, &search_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(search_status, ==, 0);
    g_assert_nonnull(strstr(search_out, "target-keep.txt"));
    g_assert_null(strstr(search_out, "target-hidden.txt"));
    g_assert_null(strstr(search_out, "target-file-excluded.txt"));

    remove_tree(root);
}

static void
test_exclude_from_missing_file_fails(void) {
    g_autofree char *tmp = g_dir_make_tmp("fsearch-cli-exclude-missing-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *db_path = g_build_filename(tmp, "test.db", NULL);
    g_autofree char *missing = g_build_filename(tmp, "does-not-exist.txt", NULL);

    char *argv[] = {
        (char *)cli_binary(),
        "index",
        "--db",
        db_path,
        "--include",
        tmp,
        "--exclude-from",
        missing,
        "--no-monitor",
        NULL,
    };
    g_autofree char *out = NULL;
    g_autofree char *err = NULL;
    gint status = 0;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, &err, &status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(status, !=, 0);
    g_assert_nonnull(strstr(err, "--exclude-from"));

    remove_tree(tmp);
}

static void
test_exclude_from_stdin(void) {
    g_autofree char *tmp = g_dir_make_tmp("fsearch-cli-exclude-stdin-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *root = realpath(tmp, NULL);
    g_assert_nonnull(root);

    g_autofree char *docs = g_build_filename(root, "docs", NULL);
    g_autofree char *keep_dir = g_build_filename(docs, "keep", NULL);
    g_autofree char *skip_dir = g_build_filename(docs, "skip-stdin", NULL);
    g_assert_cmpint(g_mkdir_with_parents(keep_dir, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(skip_dir, 0755), ==, 0);

    g_autofree char *keep_file = g_build_filename(keep_dir, "stdin-keep.txt", NULL);
    g_autofree char *skip_file = g_build_filename(skip_dir, "stdin-skip.txt", NULL);
    g_assert_true(g_file_set_contents(keep_file, "ok", -1, NULL));
    g_assert_true(g_file_set_contents(skip_file, "no", -1, NULL));

    g_autofree char *db_path = g_build_filename(root, "stdin.db", NULL);
    g_autofree char *cmd = g_strdup_printf("printf '%%s\\n' %s | %s index --db %s --include %s --exclude-from - --no-monitor",
                                           g_shell_quote(skip_dir),
                                           g_shell_quote(cli_binary()),
                                           g_shell_quote(db_path),
                                           g_shell_quote(docs));
    char *sh_argv[] = {"/bin/sh", "-c", cmd, NULL};
    g_autofree char *idx_out = NULL;
    g_autofree char *idx_err = NULL;
    gint idx_status = 0;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_spawn_sync(NULL, sh_argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &idx_out, &idx_err, &idx_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(idx_status, ==, 0);

    char *search_argv[] = {
        (char *)cli_binary(),
        "search",
        "--db",
        db_path,
        "--no-daemon",
        "--",
        "stdin-",
        NULL,
    };
    g_autofree char *search_out = NULL;
    g_autofree char *search_err = NULL;
    gint search_status = 0;
    g_assert_true(
        g_spawn_sync(NULL, search_argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &search_out, &search_err, &search_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(search_status, ==, 0);
    g_assert_nonnull(strstr(search_out, "stdin-keep.txt"));
    g_assert_null(strstr(search_out, "stdin-skip.txt"));

    remove_tree(root);
}

int
main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/cli/exclude_from_large_file", test_exclude_from_large_file);
    g_test_add_func("/FSearch/cli/exclude_from_stdin", test_exclude_from_stdin);
    g_test_add_func("/FSearch/cli/exclude_from_missing_file_fails", test_exclude_from_missing_file_fails);
    return g_test_run();
}

#include "fsearch_database_exclude_manager.h"

#include <glib.h>
#include <stdint.h>

struct _FsearchDatabaseExcludeManager {
    GObject parent_instance;

    GPtrArray *excludes;
    // O(1) deduplication set over elements owned by `excludes`.
    GHashTable *seen_excludes;
    // Fast O(1) lookup tables for active FIXED rules: pattern (borrowed from `excludes`) -> target bitmask.
    // Bit 1: files, Bit 2: folders.
    GHashTable *fixed_full_path;
    GHashTable *fixed_basename;
    // Active non-FIXED rules (wildcard / regex), usually very few; elements borrowed from `excludes`.
    GPtrArray *dynamic_excludes;

    gboolean exclude_hidden;
};

G_DEFINE_TYPE(FsearchDatabaseExcludeManager, fsearch_database_exclude_manager, G_TYPE_OBJECT)

enum {
    TARGET_MASK_FILES = 1u << 0,
    TARGET_MASK_FOLDERS = 1u << 1,
};

static guint
target_to_mask(FsearchDatabaseExcludeTarget target) {
    switch (target) {
    case FSEARCH_DATABASE_EXCLUDE_TARGET_FILES:
        return TARGET_MASK_FILES;
    case FSEARCH_DATABASE_EXCLUDE_TARGET_FOLDERS:
        return TARGET_MASK_FOLDERS;
    case FSEARCH_DATABASE_EXCLUDE_TARGET_BOTH:
        return TARGET_MASK_FILES | TARGET_MASK_FOLDERS;
    default:
        return 0;
    }
}

static guint
exclude_hash(gconstpointer key) {
    FsearchDatabaseExclude *e = (FsearchDatabaseExclude *)key;
    guint h = g_str_hash(fsearch_database_exclude_get_pattern(e));
    h = h * 31u + (guint)fsearch_database_exclude_get_active(e);
    h = h * 31u + (guint)fsearch_database_exclude_get_exclude_type(e);
    h = h * 31u + (guint)fsearch_database_exclude_get_match_scope(e);
    h = h * 31u + (guint)fsearch_database_exclude_get_target(e);
    return h;
}

static void
index_single_exclude(FsearchDatabaseExcludeManager *self, FsearchDatabaseExclude *exclude) {
    g_hash_table_add(self->seen_excludes, exclude);
    if (!fsearch_database_exclude_get_active(exclude)) {
        return;
    }
    if (fsearch_database_exclude_get_exclude_type(exclude) == FSEARCH_DATABASE_EXCLUDE_TYPE_FIXED) {
        GHashTable *table =
            fsearch_database_exclude_get_match_scope(exclude) == FSEARCH_DATABASE_EXCLUDE_MATCH_SCOPE_FULL_PATH
                ? self->fixed_full_path
                : self->fixed_basename;
        const char *pattern = fsearch_database_exclude_get_pattern(exclude);
        const guint existing = GPOINTER_TO_UINT(g_hash_table_lookup(table, pattern));
        const guint merged = existing | target_to_mask(fsearch_database_exclude_get_target(exclude));
        g_hash_table_insert(table, (gpointer)pattern, GUINT_TO_POINTER(merged));
    }
    else {
        g_ptr_array_add(self->dynamic_excludes, exclude);
    }
}

static void
rebuild_lookup_index(FsearchDatabaseExcludeManager *self) {
    g_hash_table_remove_all(self->seen_excludes);
    g_hash_table_remove_all(self->fixed_full_path);
    g_hash_table_remove_all(self->fixed_basename);
    g_ptr_array_set_size(self->dynamic_excludes, 0);
    for (guint i = 0; i < self->excludes->len; ++i) {
        index_single_exclude(self, g_ptr_array_index(self->excludes, i));
    }
}

static void
add_exclude_if_not_already_present(FsearchDatabaseExcludeManager *self, FsearchDatabaseExclude *exclude) {
    if (!g_hash_table_contains(self->seen_excludes, exclude)) {
        FsearchDatabaseExclude *owned = fsearch_database_exclude_ref(exclude);
        g_ptr_array_add(self->excludes, owned);
        index_single_exclude(self, owned);
    }
}

static void
remove_exclude(FsearchDatabaseExcludeManager *self, FsearchDatabaseExclude *exclude) {
    guint index = 0;
    if (g_ptr_array_find_with_equal_func(self->excludes, exclude, (GEqualFunc)fsearch_database_exclude_equal, &index)) {
        g_ptr_array_remove_index(self->excludes, index);
        rebuild_lookup_index(self);
    }
}

static void
fsearch_database_exclude_manager_finalize(GObject *object) {
    FsearchDatabaseExcludeManager *self = (FsearchDatabaseExcludeManager *)object;
    // Free lookup indices before `excludes` because they borrow pointers owned by `excludes`.
    g_clear_pointer(&self->seen_excludes, g_hash_table_unref);
    g_clear_pointer(&self->fixed_full_path, g_hash_table_unref);
    g_clear_pointer(&self->fixed_basename, g_hash_table_unref);
    g_clear_pointer(&self->dynamic_excludes, g_ptr_array_unref);
    g_clear_pointer(&self->excludes, g_ptr_array_unref);

    G_OBJECT_CLASS(fsearch_database_exclude_manager_parent_class)->finalize(object);
}

static void
fsearch_database_exclude_manager_class_init(FsearchDatabaseExcludeManagerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = fsearch_database_exclude_manager_finalize;
}

static void
fsearch_database_exclude_manager_init(FsearchDatabaseExcludeManager *self) {
    self->excludes = g_ptr_array_new_with_free_func((GDestroyNotify)fsearch_database_exclude_unref);
    self->seen_excludes = g_hash_table_new(exclude_hash, (GEqualFunc)fsearch_database_exclude_equal);
    self->fixed_full_path = g_hash_table_new(g_str_hash, g_str_equal);
    self->fixed_basename = g_hash_table_new(g_str_hash, g_str_equal);
    self->dynamic_excludes = g_ptr_array_new();
}

FsearchDatabaseExcludeManager *
fsearch_database_exclude_manager_new() {
    return g_object_new(FSEARCH_TYPE_DATABASE_EXCLUDE_MANAGER, NULL);
}

FsearchDatabaseExcludeManager *
fsearch_database_exclude_manager_new_with_defaults() {
    FsearchDatabaseExcludeManager *self = fsearch_database_exclude_manager_new();
    g_return_val_if_fail(self, NULL);

    const char *default_excludes[] = {
        "/.snapshots",
        "/proc",
        "/sys",
#ifdef __APPLE__
        // macOS exposes the whole root filesystem a second time below these paths (/.nofollow and
        // /.resolve are path-resolution namespaces, /System/Volumes holds the data volume whose
        // contents are already reachable through firmlinks such as /Users and /Applications, plus
        // internal system volumes). Indexing / would otherwise store most entries twice.
        "/.nofollow",
        "/.resolve",
        "/System/Volumes",
        "/dev",
#endif
    };
    for (size_t i = 0; i < G_N_ELEMENTS(default_excludes); i++) {
        g_autoptr(FsearchDatabaseExclude) exclude =
            fsearch_database_exclude_new(default_excludes[i],
                                         TRUE,
                                         FSEARCH_DATABASE_EXCLUDE_TYPE_FIXED,
                                         FSEARCH_DATABASE_EXCLUDE_MATCH_SCOPE_FULL_PATH,
                                         FSEARCH_DATABASE_EXCLUDE_TARGET_FOLDERS);
        add_exclude_if_not_already_present(self, exclude);
    }

    return self;
}

void
fsearch_database_exclude_manager_add(FsearchDatabaseExcludeManager *self, FsearchDatabaseExclude *exclude) {
    g_return_if_fail(self);
    g_return_if_fail(FSEARCH_IS_DATABASE_EXCLUDE_MANAGER(self));

    add_exclude_if_not_already_present(self, exclude);
}

void
fsearch_database_exclude_manager_set_exclude_hidden(FsearchDatabaseExcludeManager *self, gboolean exclude_hidden) {
    g_return_if_fail(self);
    g_return_if_fail(FSEARCH_IS_DATABASE_EXCLUDE_MANAGER(self));

    self->exclude_hidden = exclude_hidden;
}

void
fsearch_database_exclude_manager_remove(FsearchDatabaseExcludeManager *self, FsearchDatabaseExclude *exclude) {
    g_return_if_fail(self);
    g_return_if_fail(FSEARCH_IS_DATABASE_EXCLUDE_MANAGER(self));

    remove_exclude(self, exclude);
}

gboolean
fsearch_database_exclude_manager_excludes(FsearchDatabaseExcludeManager *self,
                                          const char *path,
                                          const char *basename,
                                          gboolean is_dir) {
    g_return_val_if_fail(self, FALSE);
    g_return_val_if_fail(FSEARCH_IS_DATABASE_EXCLUDE_MANAGER(self), FALSE);

    if (self->exclude_hidden && g_str_has_prefix(basename, ".")) {
        return TRUE;
    }

    const guint want_bit = is_dir ? TARGET_MASK_FOLDERS : TARGET_MASK_FILES;
    if (g_hash_table_size(self->fixed_full_path) > 0) {
        const guint mask = GPOINTER_TO_UINT(g_hash_table_lookup(self->fixed_full_path, path));
        if ((mask & want_bit) != 0) {
            return TRUE;
        }
    }
    if (g_hash_table_size(self->fixed_basename) > 0) {
        const guint mask = GPOINTER_TO_UINT(g_hash_table_lookup(self->fixed_basename, basename));
        if ((mask & want_bit) != 0) {
            return TRUE;
        }
    }

    for (uint32_t i = 0; i < self->dynamic_excludes->len; ++i) {
        FsearchDatabaseExclude *exclude = g_ptr_array_index(self->dynamic_excludes, i);
        if (fsearch_database_exclude_matches(exclude, path, basename, is_dir)) {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
fsearch_database_exclude_manager_equal(FsearchDatabaseExcludeManager *m1, FsearchDatabaseExcludeManager *m2) {
    g_return_val_if_fail(m1, FALSE);
    g_return_val_if_fail(m2, FALSE);
    g_return_val_if_fail(FSEARCH_IS_DATABASE_EXCLUDE_MANAGER(m1), FALSE);
    g_return_val_if_fail(FSEARCH_IS_DATABASE_EXCLUDE_MANAGER(m2), FALSE);

    if (m1->exclude_hidden != m2->exclude_hidden) {
        return FALSE;
    }

    if (m1->excludes->len != m2->excludes->len) {
        return FALSE;
    }

    for (guint i = 0; i < m1->excludes->len; ++i) {
        FsearchDatabaseExclude *e1 = g_ptr_array_index(m1->excludes, i);
        FsearchDatabaseExclude *e2 = g_ptr_array_index(m2->excludes, i);
        if (!fsearch_database_exclude_equal(e1, e2)) {
            return FALSE;
        }
    }
    return TRUE;
}

FsearchDatabaseExcludeManager *
fsearch_database_exclude_manager_copy(FsearchDatabaseExcludeManager *self) {
    g_return_val_if_fail(self, NULL);
    FsearchDatabaseExcludeManager *copy = fsearch_database_exclude_manager_new();
    copy->exclude_hidden = self->exclude_hidden;
    g_clear_pointer(&copy->excludes, g_ptr_array_unref);
    copy->excludes = g_ptr_array_copy(self->excludes, (GCopyFunc)fsearch_database_exclude_copy, NULL);
    rebuild_lookup_index(copy);

    return copy;
}

GPtrArray *
fsearch_database_exclude_manager_get_excludes(FsearchDatabaseExcludeManager *self) {
    g_return_val_if_fail(self, NULL);
    g_return_val_if_fail(FSEARCH_IS_DATABASE_EXCLUDE_MANAGER(self), NULL);
    return g_ptr_array_ref(self->excludes);
}

gboolean
fsearch_database_exclude_manager_get_exclude_hidden(FsearchDatabaseExcludeManager *self) {
    g_return_val_if_fail(self, FALSE);
    g_return_val_if_fail(FSEARCH_IS_DATABASE_EXCLUDE_MANAGER(self), FALSE);
    return self->exclude_hidden;
}

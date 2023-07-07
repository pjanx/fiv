//
// xdg.c: various *nix desktop utilities
//
// Copyright (c) 2021, Přemysl Eric Janouch <p@janouch.name>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//

#include <glib.h>

#include <stdlib.h>
#include <string.h>

/// Add `element` to the `output` set. `relation` is a map of sets of strings
/// defining is-a relations, and is traversed recursively.
static void
add_applying_transitive_closure(
	const char *element, GHashTable *relation, GHashTable *output)
{
	// Stop condition.
	if (!g_hash_table_add(output, g_strdup(element)))
		return;

	// TODO(p): Iterate over all aliases of `element` in addition to
	// any direct match (and rename this no-longer-generic function).
	GHashTable *targets = g_hash_table_lookup(relation, element);
	if (!targets)
		return;

	GHashTableIter iter;
	g_hash_table_iter_init(&iter, targets);

	gpointer key = NULL, value = NULL;
	while (g_hash_table_iter_next(&iter, &key, &value))
		add_applying_transitive_closure(key, relation, output);
}

char *
get_xdg_home_dir(const char *var, const char *default_)
{
	const char *env = getenv(var);
	if (env && g_path_is_absolute(env))
		return g_strdup(env);

#ifdef G_OS_WIN32
	return g_build_filename(g_get_home_dir(), default_, NULL);
#else
	// The specification doesn't handle a missing HOME variable explicitly.
	// Implicitly, assuming Bourne shell semantics, it simply resolves empty.
	const char *home = getenv("HOME");
	return g_build_filename(home ? home : "", default_, NULL);
#endif
}

// Reïmplemented partly due to https://gitlab.gnome.org/GNOME/glib/-/issues/2501
static gchar **
get_xdg_data_dirs(void)
{
	// GStrvBuilder is too new, it would help a little bit.
	GPtrArray *output = g_ptr_array_new_with_free_func(g_free);

#ifdef G_OS_WIN32
	g_ptr_array_add(output, g_strdup(g_get_user_data_dir()));
	for (const gchar *const *p = g_get_system_data_dirs(); *p; p++)
		g_ptr_array_add(output, g_strdup(*p));
#else
	g_ptr_array_add(output, get_xdg_home_dir("XDG_DATA_HOME", ".local/share"));

	const char *xdg_data_dirs = "";
	if (!(xdg_data_dirs = getenv("XDG_DATA_DIRS")) || !*xdg_data_dirs)
		xdg_data_dirs = "/usr/local/share/:/usr/share/";

	gchar **candidates = g_strsplit(xdg_data_dirs, G_SEARCHPATH_SEPARATOR_S, 0);
	for (gchar **p = candidates; *p; p++) {
		if (g_path_is_absolute(*p))
			g_ptr_array_add(output, *p);
		else
			g_free(*p);
	}
	g_free(candidates);
#endif
	g_ptr_array_add(output, NULL);
	return (gchar **) g_ptr_array_free(output, FALSE);
}

// --- Filtering ---------------------------------------------------------------

// Derived from shared-mime-info-spec 0.21.

static void
read_mime_subclasses(const char *path, GHashTable *subclass_sets)
{
	gchar *data = NULL;
	if (!g_file_get_contents(path, &data, NULL /* length */, NULL /* error */))
		return;

	// The format of this file is unspecified,
	// but in practice it's a list of space-separated media types.
	gchar *datasave = NULL;
	for (gchar *line = strtok_r(data, "\r\n", &datasave); line;
			line = strtok_r(NULL, "\r\n", &datasave)) {
		gchar *linesave = NULL,
			*subclass = strtok_r(line, " ", &linesave),
			*superclass = strtok_r(NULL, " ", &linesave);

		// Nothing about comments is specified, we're being nice.
		if (!subclass || *subclass == '#' || !superclass)
			continue;

		GHashTable *set = NULL;
		if (!(set = g_hash_table_lookup(subclass_sets, superclass))) {
			set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
			g_hash_table_insert(subclass_sets, g_strdup(superclass), set);
		}
		g_hash_table_add(set, g_strdup(subclass));
	}
	g_free(data);
}

static gboolean
filter_mime_globs(const char *path, guint is_globs2, GHashTable *supported_set,
	GHashTable *output_set)
{
	gchar *data = NULL;
	if (!g_file_get_contents(path, &data, NULL /* length */, NULL /* error */))
		return FALSE;

	gchar *datasave = NULL;
	for (const char *line = strtok_r(data, "\r\n", &datasave); line;
			line = strtok_r(NULL, "\r\n", &datasave)) {
		if (*line == '#')
			continue;

		// We do not support __NOGLOBS__, nor even parse out the "cs" flag.
		// The weight is irrelevant.
		gchar **f = g_strsplit(line, ":", 0);
		if (g_strv_length(f) >= is_globs2 + 2) {
			const gchar *type = f[is_globs2 + 0], *glob = f[is_globs2 + 1];
			if (g_hash_table_contains(supported_set, type))
				g_hash_table_add(output_set, g_utf8_strdown(glob, -1));
		}
		g_strfreev(f);
	}
	g_free(data);
	return TRUE;
}

char **
extract_mime_globs(const char **media_types)
{
	gchar **data_dirs = get_xdg_data_dirs();

	// The mime.cache format is inconvenient to parse,
	// we'll do it from the text files manually, and once only.
	GHashTable *subclass_sets = g_hash_table_new_full(
		g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_hash_table_destroy);
	for (gsize i = 0; data_dirs[i]; i++) {
		gchar *path =
			g_build_filename(data_dirs[i], "mime", "subclasses", NULL);
		read_mime_subclasses(path, subclass_sets);
		g_free(path);
	}

	// A hash set of all supported media types, including subclasses,
	// but not aliases.
	GHashTable *supported =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	while (*media_types) {
		add_applying_transitive_closure(
			*media_types++, subclass_sets, supported);
	}
	g_hash_table_destroy(subclass_sets);

	// We do not support the distinction of case-sensitive globs (:cs).
	GHashTable *globs =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (gsize i = 0; data_dirs[i]; i++) {
		gchar *path2 = g_build_filename(data_dirs[i], "mime", "globs2", NULL);
		gchar *path1 = g_build_filename(data_dirs[i], "mime", "globs", NULL);
		if (!filter_mime_globs(path2, TRUE, supported, globs))
			filter_mime_globs(path1, FALSE, supported, globs);
		g_free(path2);
		g_free(path1);
	}
	g_strfreev(data_dirs);
	g_hash_table_destroy(supported);

	gchar **result = (gchar **) g_hash_table_get_keys_as_array(globs, NULL);
	g_hash_table_steal_all(globs);
	g_hash_table_destroy(globs);
	return result;
}

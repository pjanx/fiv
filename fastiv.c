//
// fastiv.c: fast image viewer
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

#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <locale.h>

#include <fnmatch.h>

#include "config.h"
#include "fastiv-view.h"

// --- Utilities ---------------------------------------------------------------

static void
exit_fatal(const gchar *format, ...) G_GNUC_PRINTF(1, 2);

static void
exit_fatal(const gchar *format, ...)
{
	va_list ap;
	va_start(ap, format);

	gchar *format_nl = g_strdup_printf("%s\n", format);
	vfprintf(stderr, format_nl, ap);
	free(format_nl);

	va_end(ap);
	exit(EXIT_FAILURE);
}

/// Add `element` to the `output` set. `relation` is a map of sets of strings
/// defining is-a relations, and is traversed recursively.
static void
add_applying_transitive_closure(const gchar *element, GHashTable *relation,
	GHashTable *output)
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

// --- XDG ---------------------------------------------------------------------

gchar *
get_xdg_home_dir(const char *var, const char *default_)
{
	const char *env = getenv(var);
	if (env && *env == '/')
		return g_strdup(env);

	// The specification doesn't handle a missing HOME variable explicitly.
	// Implicitly, assuming Bourne shell semantics, it simply resolves empty.
	const char *home = getenv("HOME");
	return g_build_filename(home ? home : "", default_, NULL);
}

static gchar **
get_xdg_data_dirs(void)
{
	// GStrvBuilder is too new, it would help a little bit.
	GPtrArray *output = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(output, get_xdg_home_dir("XDG_DATA_HOME", ".local/share"));

	const gchar *xdg_data_dirs;
	if (!(xdg_data_dirs = getenv("XDG_DATA_DIRS")) || !*xdg_data_dirs)
		xdg_data_dirs = "/usr/local/share/:/usr/share/";

	gchar **candidates = g_strsplit(xdg_data_dirs, ":", 0);
	for (gchar **p = candidates; *p; p++) {
		if (**p == '/')
			g_ptr_array_add(output, *p);
		else
			g_free(*p);
	}
	g_free(candidates);
	g_ptr_array_add(output, NULL);
	return (gchar **) g_ptr_array_free(output, FALSE);
}

// --- Filtering ---------------------------------------------------------------

// Derived from shared-mime-info-spec 0.21.

// TODO(p): Move to fastiv-io.c, expose the prototype in a header file
// (perhaps finally start a new one for it).
// A subset of shared-mime-info that produces an appropriate list of
// file extensions. Chiefly motivated by the suckiness of RAW images:
// someone else will maintain the list of file extensions for us.
static const char *supported_media_types[] = {
	"image/bmp",
	"image/gif",
	"image/png",
	"image/jpeg",
#ifdef HAVE_LIBRAW
	"image/x-dcraw",
#endif  // HAVE_LIBRAW
};

static void
read_mime_subclasses(const gchar *path, GHashTable *subclass_sets)
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
filter_mime_globs(const gchar *path, guint is_globs2, GHashTable *supported_set,
	GHashTable *output_set)
{
	gchar *data = NULL;
	if (!g_file_get_contents(path, &data, NULL /* length */, NULL /* error */))
		return FALSE;

	gchar *datasave = NULL;
	for (const gchar *line = strtok_r(data, "\r\n", &datasave); line;
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

static gchar **
get_supported_globs (void)
{
	gchar **data_dirs = get_xdg_data_dirs();

	// The mime.cache format is inconvenient to parse,
	// we'll do it from the text files manually, and once only.
	GHashTable *subclass_sets = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, (GDestroyNotify) g_hash_table_destroy);
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
	for (gsize i = 0; i < G_N_ELEMENTS(supported_media_types); i++) {
		add_applying_transitive_closure(supported_media_types[i],
			subclass_sets, supported);
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

// --- Main --------------------------------------------------------------------

struct {
	gchar **supported_globs;

	gchar *directory;
	GPtrArray *files;
	gint files_index;

	gchar *basename;

	GtkWidget *window;
	GtkWidget *view;
} g;

static gboolean
is_supported(const gchar *filename)
{
	gchar *utf8 = g_filename_to_utf8(filename, -1, NULL, NULL, NULL);
	if (!utf8)
		return FALSE;

	gchar *lowercased = g_utf8_strdown(utf8, -1);
	g_free(utf8);

	// XXX: fnmatch() uses the /locale/ encoding, but who cares nowadays.
	for (gchar **p = g.supported_globs; *p; p++)
		if (!fnmatch(*p, lowercased, 0)) {
			g_free(lowercased);
			return TRUE;
		}

	g_free(lowercased);
	return FALSE;
}

static void
show_error_dialog(GError *error)
{
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g.window),
		GTK_DIALOG_MODAL,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", error->message);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	g_error_free(error);
}

static void
load_directory(const gchar *dirname)
{
	free(g.directory);
	g.directory = g_strdup(dirname);
	g_ptr_array_set_size(g.files, 0);
	g.files_index = -1;

	GError *error = NULL;
	GDir *dir = g_dir_open(dirname, 0, &error);
	if (dir) {
		for (const gchar *name = NULL; (name = g_dir_read_name(dir)); ) {
			if (!is_supported(name))
				continue;

			// XXX: We presume that this basename is from the same directory.
			if (!g_strcmp0(g.basename, name))
				g.files_index = g.files->len;

			g_ptr_array_add(g.files, g_strdup(name));
		}
		g_dir_close(dir);
	} else {
		show_error_dialog(error);
	}
	g_ptr_array_add(g.files, NULL);

	// XXX: When something outside the filtered entries is open, the index is
	// kept at -1, and browsing doesn't work. How to behave here?
	// Should we add it to the pointer array as an exception?
}

static void
open(const gchar *path)
{
	g_return_if_fail(g_path_is_absolute(path));

	GError *error = NULL;
	if (!fastiv_view_open(FASTIV_VIEW(g.view), path, &error)) {
		show_error_dialog(error);
		return;
	}

	gtk_window_set_title(GTK_WINDOW(g.window), path);

	gchar *basename = g_path_get_basename(path);
	g_free(g.basename);
	g.basename = basename;

	// So that load_directory() itself can be used for reloading.
	gchar *dirname = g_path_get_dirname(path);
	if (!g.directory || strcmp(dirname, g.directory)) {
		load_directory(dirname);
	} else {
		g.files_index = -1;
		for (guint i = 0; i + 1 < g.files->len; i++) {
			if (!g_strcmp0(g.basename, g_ptr_array_index(g.files, i)))
				g.files_index = i;
		}
	}
	g_free(dirname);

}

static void
on_open(void)
{
	// TODO(p): Populate and pass a GtkFileFilter.
	// If we want to keep this functionality, that is.
	GtkWidget *dialog = gtk_file_chooser_dialog_new("Open file",
		GTK_WINDOW(g.window), GTK_FILE_CHOOSER_ACTION_OPEN,
		"_Cancel", GTK_RESPONSE_CANCEL,
		"_Open", GTK_RESPONSE_ACCEPT, NULL);

	// The default is local-only, single item. Paths are returned absolute.
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		open(path);
		g_free(path);
	}

	gtk_widget_destroy(dialog);
}

static void
on_previous(void)
{
	if (g.files_index >= 0) {
		int previous =
			(g.files->len - 1 + g.files_index - 1) % (g.files->len - 1);
		char *absolute =
			g_canonicalize_filename(g_ptr_array_index(g.files, previous),
				g.directory);
		open(absolute);
		g_free(absolute);
	}
}

static void
on_next(void)
{
	if (g.files_index >= 0) {
		int next = (g.files_index + 1) % (g.files->len - 1);
		char *absolute =
			g_canonicalize_filename(g_ptr_array_index(g.files, next),
				g.directory);
		open(absolute);
		g_free(absolute);
	}
}

int
main(int argc, char *argv[])
{
	if (!setlocale(LC_CTYPE, ""))
		exit_fatal("cannot set locale");

	gboolean show_version = FALSE;
	gchar **path_args = NULL;
	const GOptionEntry options[] = {
		{G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &path_args,
			NULL, "[FILE | DIRECTORY]"},
		{"version", 'V', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE,
		 &show_version, "output version information and exit", NULL},
		{},
	};

	GError *error = NULL;
	if (!gtk_init_with_args(&argc, &argv, " - fast image viewer",
		options, NULL, &error))
		exit_fatal("%s", error->message);
	if (show_version) {
		printf(PROJECT_NAME " " PROJECT_VERSION "\n");
		return 0;
	}

	// NOTE: Firefox and Eye of GNOME both interpret multiple arguments
	// in a special way. This is problematic, because one-element lists
	// are unrepresentable.
	// TODO(p): Complain to the user if there's more than one argument.
	// Best show the help message, if we can figure that out.
	const gchar *path_arg = path_args ? path_args[0] : NULL;

	gtk_window_set_default_icon_name(PROJECT_NAME);

	g.view = g_object_new(FASTIV_TYPE_VIEW, NULL);
	g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(g.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_container_add(GTK_CONTAINER(g.window), g.view);

	// The references to closures are initially floating and sunk on connect.
	GtkAccelGroup *accel_group = gtk_accel_group_new();
	gtk_accel_group_connect(accel_group, GDK_KEY_Escape, 0, 0,
		g_cclosure_new(G_CALLBACK(gtk_main_quit), NULL, NULL));
	gtk_accel_group_connect(accel_group, GDK_KEY_q, 0, 0,
		g_cclosure_new(G_CALLBACK(gtk_main_quit), NULL, NULL));

	gtk_accel_group_connect(accel_group, GDK_KEY_o, 0, 0,
		g_cclosure_new(G_CALLBACK(on_open), NULL, NULL));
	gtk_accel_group_connect(accel_group, GDK_KEY_o, GDK_CONTROL_MASK, 0,
		g_cclosure_new(G_CALLBACK(on_open), NULL, NULL));

	// FIXME: The left/right arrows do not work, for whatever reason.
	gtk_accel_group_connect(accel_group, GDK_KEY_Left, 0, 0,
		g_cclosure_new(G_CALLBACK(on_previous), NULL, NULL));
	gtk_accel_group_connect(accel_group, GDK_KEY_Page_Up, 0, 0,
		g_cclosure_new(G_CALLBACK(on_previous), NULL, NULL));
	gtk_accel_group_connect(accel_group, GDK_KEY_Right, 0, 0,
		g_cclosure_new(G_CALLBACK(on_next), NULL, NULL));
	gtk_accel_group_connect(accel_group, GDK_KEY_Page_Down, 0, 0,
		g_cclosure_new(G_CALLBACK(on_next), NULL, NULL));
	gtk_accel_group_connect(accel_group, GDK_KEY_space, 0, 0,
		g_cclosure_new(G_CALLBACK(on_next), NULL, NULL));
	gtk_window_add_accel_group(GTK_WINDOW(g.window), accel_group);

	g.supported_globs = get_supported_globs();
	g.files = g_ptr_array_new_full(16, g_free);
	gchar *cwd = g_get_current_dir();

	// TODO(p): Desired behaviour:
	//  - No arguments: show directory view of the current working directory.
	//  - File argument: load its directory for browsing, open the file.
	//  - Directory argument: load its directory for browsing, show the dir.
	GStatBuf st;
	if (!path_arg) {
		load_directory(cwd);
	} else if (g_stat(path_arg, &st)) {
		show_error_dialog(g_error_new(G_FILE_ERROR,
			g_file_error_from_errno(errno),
			"%s: %s", path_arg, g_strerror(errno)));
		load_directory(cwd);
	} else {
		gchar *path_arg_absolute = g_canonicalize_filename(path_arg, cwd);
		if (S_ISDIR(st.st_mode))
			load_directory(path_arg_absolute);
		else
			open(path_arg_absolute);
		g_free(path_arg_absolute);
	}
	g_free(cwd);

	// TODO(p): When no picture is loaded, show a view of this directory
	// (we're missing a widget for that).
	gtk_widget_show_all(g.window);
	gtk_main();
	return 0;
}

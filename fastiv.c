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

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <fnmatch.h>

#include "config.h"
#include "fastiv-browser.h"
#include "fastiv-io.h"
#include "fastiv-sidebar.h"
#include "fastiv-view.h"
#include "xdg.h"

// --- Utilities ---------------------------------------------------------------

static void exit_fatal(const gchar *format, ...) G_GNUC_PRINTF(1, 2);

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

// --- Main --------------------------------------------------------------------

struct {
	gchar **supported_globs;

	gchar *directory;
	GPtrArray *files;
	gint files_index;

	gchar *basename;

	GtkWidget *window;
	GtkWidget *stack;
	GtkWidget *view;
	GtkWidget *view_scroller;
	GtkWidget *browser;
	GtkWidget *browser_scroller;
	GtkWidget *browser_paned;
	GtkWidget *browser_sidebar;
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
	GtkWidget *dialog =
		gtk_message_dialog_new(GTK_WINDOW(g.window), GTK_DIALOG_MODAL,
			GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", error->message);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	g_error_free(error);
}

static void
load_directory(const gchar *dirname)
{
	if (dirname) {
		free(g.directory);
		g.directory = g_strdup(dirname);
	}

	g_ptr_array_set_size(g.files, 0);
	g.files_index = -1;

	GFile *file = g_file_new_for_path(g.directory);
	fastiv_sidebar_set_location(FASTIV_SIDEBAR(g.browser_sidebar), file);
	g_object_unref(file);
	fastiv_browser_load(FASTIV_BROWSER(g.browser), g.directory);

	GError *error = NULL;
	GDir *dir = g_dir_open(g.directory, 0, &error);
	if (dir) {
		for (const gchar *name = NULL; (name = g_dir_read_name(dir)); ) {
			// This really wants to make you use readdir() directly.
			char *absolute = g_canonicalize_filename(name, g.directory);
			gboolean is_dir = g_file_test(absolute, G_FILE_TEST_IS_DIR);
			g_free(absolute);
			if (is_dir || !is_supported(name))
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
		char *base = g_filename_display_basename(path);
		g_prefix_error(&error, "%s: ", base);
		show_error_dialog(error);
		g_free(base);
		return;
	}

	gchar *uri = g_filename_to_uri(path, NULL, NULL);
	if (uri) {
		gtk_recent_manager_add_item(gtk_recent_manager_get_default(), uri);
		g_free(uri);
	}

	gtk_window_set_title(GTK_WINDOW(g.window), path);
	gtk_stack_set_visible_child(GTK_STACK(g.stack), g.view_scroller);

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

static GtkWidget *
create_open_dialog(void)
{
	GtkWidget *dialog = gtk_file_chooser_dialog_new("Open file",
		GTK_WINDOW(g.window), GTK_FILE_CHOOSER_ACTION_OPEN,
		"_Cancel", GTK_RESPONSE_CANCEL,
		"_Open", GTK_RESPONSE_ACCEPT, NULL);

	GtkFileFilter *filter = gtk_file_filter_new();
	for (const char **p = fastiv_io_supported_media_types; *p; p++)
		gtk_file_filter_add_mime_type(filter, *p);
#ifdef HAVE_GDKPIXBUF
	gtk_file_filter_add_pixbuf_formats(filter);
#endif  // HAVE_GDKPIXBUF
	gtk_file_filter_set_name(filter, "Supported images");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

	GtkFileFilter *all_files = gtk_file_filter_new();
	gtk_file_filter_set_name(all_files, "All files");
	gtk_file_filter_add_pattern(all_files, "*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_files);
	return dialog;
}

static void
on_open(void)
{
	static GtkWidget *dialog;
	if (!dialog)
		dialog = create_open_dialog();

	// Apparently, just keeping the dialog around doesn't mean
	// that it will remember its last location.
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), g.directory);

	// The default is local-only, single item. Paths are returned absolute.
	switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
		gchar *path;
	case GTK_RESPONSE_ACCEPT:
		path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		open(path);
		g_free(path);
		break;
	case GTK_RESPONSE_NONE:
		dialog = NULL;
		return;
	}

	gtk_widget_hide(dialog);
}

static void
on_previous(void)
{
	if (g.files_index >= 0) {
		int previous =
			(g.files->len - 1 + g.files_index - 1) % (g.files->len - 1);
		char *absolute = g_canonicalize_filename(
			g_ptr_array_index(g.files, previous), g.directory);
		open(absolute);
		g_free(absolute);
	}
}

static void
on_next(void)
{
	if (g.files_index >= 0) {
		int next = (g.files_index + 1) % (g.files->len - 1);
		char *absolute = g_canonicalize_filename(
			g_ptr_array_index(g.files, next), g.directory);
		open(absolute);
		g_free(absolute);
	}
}

static void
on_item_activated(G_GNUC_UNUSED FastivBrowser *browser, const char *path,
	G_GNUC_UNUSED gpointer data)
{
	open(path);
}

static void
spawn_path(const char *path)
{
	char *argv[] = {PROJECT_NAME, (char *) path, NULL};
	GError *error = NULL;
	g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
		NULL, &error);
	g_clear_error(&error);
}

static gboolean
open_any_path(const char *path)
{
	GStatBuf st;
	gchar *canonical = g_canonicalize_filename(path, g.directory);
	gboolean success = !g_stat(canonical, &st);
	if (!success)
		show_error_dialog(g_error_new(G_FILE_ERROR,
			g_file_error_from_errno(errno), "%s: %s", path, g_strerror(errno)));
	else if (S_ISDIR(st.st_mode))
		load_directory(canonical);
	else
		open(canonical);

	g_free(canonical);
	if (g.files_index < 0) {
		gtk_stack_set_visible_child(GTK_STACK(g.stack), g.browser_paned);
		gtk_widget_grab_focus(g.browser_scroller);
	} else {
		gtk_stack_set_visible_child(GTK_STACK(g.stack), g.view_scroller);
	}
	return success;
}

static void
on_open_location(G_GNUC_UNUSED GtkPlacesSidebar *sidebar, GFile *location,
	G_GNUC_UNUSED GtkPlacesOpenFlags flags, G_GNUC_UNUSED gpointer user_data)
{
	gchar *path = g_file_get_path(location);
	if (path) {
		if (flags & GTK_PLACES_OPEN_NEW_WINDOW)
			spawn_path(path);
		else
			open_any_path(path);
		g_free(path);
	}
}

// Cursor keys, e.g., simply cannot be bound through accelerators
// (and GtkWidget::keynav-failed would arguably be an awful solution).
//
// GtkBindingSets can be added directly through GtkStyleContext,
// but that would still require setting up action signals on the widget class,
// which is extremely cumbersome.  GtkWidget::move-focus has no return value,
// so we can't override that and abort further handling.
//
// Therefore, bind directly to keypresses.  Order can be fine-tuned with
// g_signal_connect{,after}(), or overriding the handler and either tactically
// chaining up or using gtk_window_propagate_key_event().
static gboolean
on_key_press(G_GNUC_UNUSED GtkWidget *widget, GdkEventKey *event,
	G_GNUC_UNUSED gpointer data)
{
	switch (event->state & gtk_accelerator_get_default_mod_mask()) {
	case GDK_CONTROL_MASK:
		switch (event->keyval) {
		case GDK_KEY_o:
			on_open();
			return TRUE;
		case GDK_KEY_l:
			fastiv_sidebar_show_enter_location(
				FASTIV_SIDEBAR(g.browser_sidebar));
			return TRUE;
		case GDK_KEY_n:
			spawn_path(g.directory);
			return TRUE;
		}
		break;
	case 0:
		switch (event->keyval) {
		case GDK_KEY_Escape:
		case GDK_KEY_q:
			gtk_main_quit();
			return TRUE;

		case GDK_KEY_o:
			on_open();
			return TRUE;

		case GDK_KEY_F5:
		case GDK_KEY_r:
			load_directory(NULL);
			return TRUE;

		case GDK_KEY_F9:
			if (gtk_widget_is_visible(g.browser_sidebar))
				gtk_widget_hide(g.browser_sidebar);
			else
				gtk_widget_show(g.browser_sidebar);
			return TRUE;

		case GDK_KEY_F11:
		case GDK_KEY_f:
			if (gdk_window_get_state(gtk_widget_get_window(g.window)) &
				GDK_WINDOW_STATE_FULLSCREEN)
				gtk_window_unfullscreen(GTK_WINDOW(g.window));
			else
				gtk_window_fullscreen(GTK_WINDOW(g.window));
			return TRUE;
		}
	}
	return FALSE;
}

static gboolean
on_key_press_view(G_GNUC_UNUSED GtkWidget *widget, GdkEventKey *event,
	G_GNUC_UNUSED gpointer data)
{
	switch (event->state & gtk_accelerator_get_default_mod_mask()) {
	case 0:
		switch (event->keyval) {
		case GDK_KEY_Left:
		case GDK_KEY_Up:
		case GDK_KEY_Page_Up:
			on_previous();
			return TRUE;

		case GDK_KEY_Right:
		case GDK_KEY_Down:
		case GDK_KEY_Page_Down:
		case GDK_KEY_space:
			on_next();
			return TRUE;
		}

		case GDK_KEY_Tab:
		case GDK_KEY_Return:
			gtk_stack_set_visible_child(GTK_STACK(g.stack), g.browser_paned);
			return TRUE;
	}
	return FALSE;
}

static gboolean
on_button_press_view(G_GNUC_UNUSED GtkWidget *widget, GdkEventButton *event)
{
	if ((event->state & gtk_accelerator_get_default_mod_mask()))
		return FALSE;
	switch (event->button) {
	case 8:  // back
		gtk_stack_set_visible_child(GTK_STACK(g.stack), g.browser_paned);
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean
on_button_press_browser(G_GNUC_UNUSED GtkWidget *widget, GdkEventButton *event)
{
	if ((event->state & gtk_accelerator_get_default_mod_mask()))
		return FALSE;
	switch (event->button) {
	case 9:  // forward
		gtk_stack_set_visible_child(GTK_STACK(g.stack), g.view_scroller);
		return TRUE;
	default:
		return FALSE;
	}
}

int
main(int argc, char *argv[])
{
	gboolean show_version = FALSE, show_supported_media_types = FALSE;
	gchar **path_args = NULL;
	const GOptionEntry options[] = {
		{G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &path_args,
			NULL, "[FILE | DIRECTORY]"},
		{"list-supported-media-types", 0, G_OPTION_FLAG_IN_MAIN,
			G_OPTION_ARG_NONE, &show_supported_media_types,
			"Output supported media types and exit", NULL},
		{"version", 'V', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE,
			&show_version, "Output version information and exit", NULL},
		{},
	};

	GError *error = NULL;
	gboolean initialized = gtk_init_with_args(
		&argc, &argv, " - fast image viewer", options, NULL, &error);
	if (show_version) {
		printf(PROJECT_NAME " " PROJECT_VERSION "\n");
		return 0;
	}
	if (show_supported_media_types) {
		for (char **types = fastiv_io_all_supported_media_types(); *types; )
			g_print("%s\n", *types++);
		return 0;
	}
	if (!initialized)
		exit_fatal("%s", error->message);

	// NOTE: Firefox and Eye of GNOME both interpret multiple arguments
	// in a special way. This is problematic, because one-element lists
	// are unrepresentable.
	// TODO(p): Complain to the user if there's more than one argument.
	// Best show the help message, if we can figure that out.
	const gchar *path_arg = path_args ? path_args[0] : NULL;

	gtk_window_set_default_icon_name(PROJECT_NAME);

	// This is incredibly broken https://stackoverflow.com/a/51054396/76313
	// thus resolving the problem using overlaps.
	const char *style = "@define-color fastiv-tile #3c3c3c; \
		fastiv-view, fastiv-browser { background: #222; } \
		fastiv-browser { padding: 5px; } \
		fastiv-browser.item { \
			border: 1px solid rgba(255, 255, 255, 0.5); \
			margin: 10px; color: #000; \
			background: #333; \
			background-image: \
				linear-gradient(45deg, @fastiv-tile 26%, transparent 26%), \
				linear-gradient(-45deg, @fastiv-tile 26%, transparent 26%), \
				linear-gradient(45deg, transparent 74%, @fastiv-tile 74%), \
				linear-gradient(-45deg, transparent 74%, @fastiv-tile 74%); \
			background-size: 40px 40px; \
			background-position: 0 0, 0 20px, 20px -20px, -20px 0px; \
		}";

	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider, style, strlen(style), NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	g.view_scroller = gtk_scrolled_window_new(NULL, NULL);
	g.view = g_object_new(FASTIV_TYPE_VIEW, NULL);
	gtk_widget_set_vexpand(g.view, TRUE);
	gtk_widget_set_hexpand(g.view, TRUE);
	g_signal_connect(g.view, "key-press-event",
		G_CALLBACK(on_key_press_view), NULL);
	g_signal_connect(g.view, "button-press-event",
		G_CALLBACK(on_button_press_view), NULL);
	gtk_container_add(GTK_CONTAINER(g.view_scroller), g.view);
	gtk_widget_show_all(g.view_scroller);

	// Maybe our custom widgets should derive colours from the theme instead.
	gtk_scrolled_window_set_overlay_scrolling(
		GTK_SCROLLED_WINDOW(g.view_scroller), FALSE);
	g_object_set(gtk_settings_get_default(),
		"gtk-application-prefer-dark-theme", TRUE, NULL);

	g.browser_scroller = gtk_scrolled_window_new(NULL, NULL);
	g.browser = g_object_new(FASTIV_TYPE_BROWSER, NULL);
	gtk_widget_set_vexpand(g.browser, TRUE);
	gtk_widget_set_hexpand(g.browser, TRUE);
	g_signal_connect(g.browser, "item-activated",
		G_CALLBACK(on_item_activated), NULL);
	g_signal_connect(g.browser, "button-press-event",
		G_CALLBACK(on_button_press_browser), NULL);
	gtk_container_add(GTK_CONTAINER(g.browser_scroller), g.browser);

	// TODO(p): As with GtkFileChooserWidget, bind:
	//  - C-h to filtering,
	//  - M-Up to going a level above,
	//  - mayhaps forward the rest to the sidebar, somehow.
	g.browser_sidebar = g_object_new(FASTIV_TYPE_SIDEBAR, NULL);
	g_signal_connect(g.browser_sidebar, "open-location",
		G_CALLBACK(on_open_location), NULL);

	g.browser_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_add1(GTK_PANED(g.browser_paned), g.browser_sidebar);
	gtk_paned_add2(GTK_PANED(g.browser_paned), g.browser_scroller);

	// TODO(p): Can we not do it here separately?
	gtk_widget_show_all(g.browser_paned);

	g.stack = gtk_stack_new();
	gtk_stack_set_transition_type(
		GTK_STACK(g.stack), GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_container_add(GTK_CONTAINER(g.stack), g.view_scroller);
	gtk_container_add(GTK_CONTAINER(g.stack), g.browser_paned);

	g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(g.window, "destroy",
		G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(g.window, "key-press-event",
		G_CALLBACK(on_key_press), NULL);
	gtk_container_add(GTK_CONTAINER(g.window), g.stack);

	char **types = fastiv_io_all_supported_media_types();
	g.supported_globs = extract_mime_globs((const char **) types);
	g_strfreev(types);

	g.files = g_ptr_array_new_full(16, g_free);
	g.directory = g_get_current_dir();
	if (!path_arg || !open_any_path(path_arg))
		open_any_path(g.directory);

	// Try to get half of the screen vertically, in 4:3 aspect ratio.
	//
	// We need the GdkMonitor before the GtkWindow has a GdkWindow (i.e.,
	// before it is realized). Take the smallest dimensions, out of desperation.
	GdkDisplay *display = gtk_widget_get_display(g.window);
	int unit = G_MAXINT;
	for (int i = gdk_display_get_n_monitors(display); i--; ) {
		GdkRectangle geometry = {};
		gdk_monitor_get_geometry(
			gdk_display_get_monitor(display, i), &geometry);
		unit = MIN(unit, MIN(geometry.width, geometry.height) / 6);
	}

	// Ask for at least 800x600, to cover ridiculously heterogenous setups.
	unit = MAX(200, unit);
	gtk_window_set_default_size(GTK_WINDOW(g.window), 4 * unit, 3 * unit);

	gtk_widget_show_all(g.window);
	gtk_main();
	return 0;
}

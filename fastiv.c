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

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <fnmatch.h>

#include "config.h"
#include "fiv-browser.h"
#include "fiv-io.h"
#include "fiv-sidebar.h"
#include "fiv-view.h"
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

// --- Keyboard shortcuts ------------------------------------------------------
// Fuck XML, this can be easily represented in static structures.
// Though it would be nice if the accelerators could be customized.

struct key {
	const char *accelerator;
	const char *title;
};

struct key_group {
	const char *title;
	const struct key *keys;
};

struct key_section {
	const char *title;
	const char *section_name;
	const struct key_group *groups;
};

static struct key help_keys_general[] = {
	{"F1 <control>F1", "Show this list of shortcuts"},
	{"F11 f", "Toggle fullscreen view"},
	{"<alt><shift>d", "Toggle dark theme variant"},
	{"q <control>q", "Exit the program"},
	{"Escape <control>w", "Exit the program"},
	{}
};

static struct key_group help_keys_browser[] = {
	{"General", help_keys_general},
	{"View", (struct key[]) {
		{"F9", "Toggle navigation sidebar"},
		{}
	}},
	{"Navigation", (struct key[]) {
		{"<control>l", "Open location..."},
		{"<control>n", "Open a new window"},
		{"<alt>Left", "Go back in history"},
		{"<alt>Right", "Go forward in history"},
		{"<alt>Up", "Go to parent directory"},
		{"<alt>Home", "Go home"},
		{"F5 r <control>r", "Refresh"},
		{}
	}},
	{}
};

static struct key_group help_keys_view[] = {
	{"General", help_keys_general},
	{"View", (struct key[]) {
		{"F8", "Toggle toolbar"},
		{}
	}},
	{"Navigation", (struct key[]) {
		{"<control>l", "Open location..."},
		{"<control>n", "Open a new window"},
		{"Left Up Page_Up", "Previous image"},
		{"Right Down Page_Down", "Next image"},
		{"Return <alt>Left", "Return to browser"},
		{}
	}},
	{"Zoom", (struct key[]) {
		{"<control>0", "Set zoom to 100%"},
		{"1...9", "Set zoom to N:1"},
		{"plus <control>plus", "Zoom in"},
		{"minus <control>minus", "Zoom out"},
		{"w", "Zoom to fit width if larger"},
		{"h", "Zoom to fit height if larger"},
		{}
	}},
	{"Orientation", (struct key[]) {
		{"less", "Rotate anticlockwise"},
		{"equal", "Mirror"},
		{"greater", "Rotate clockwise"},
		{}
	}},
	{"Configuration", (struct key[]) {
		{"x", "Toggle scale to fit if larger"},
		{"i", "Toggle smooth scaling"},
		{"t", "Toggle transparency highlighting"},
		{}
	}},
	{"Control", (struct key[]) {
		{"bracketleft", "Previous page"},
		{"bracketright", "Next page"},
		{"braceleft", "Previous frame"},
		{"braceright", "Next frame"},
		{"space", "Toggle playback"},
		{}
	}},
	{"Tools", (struct key[]) {
		{"<control>p", "Print..."},
		{"<control>s", "Save page as..."},
		{"<control><shift>s", "Save frame as..."},
		{"<alt>Return", "Show file information"},
		{}
	}},
	{}
};

static struct key_section help_keys[] = {
	{"Browser", "browser", help_keys_browser},
	{"View", "view", help_keys_view},
	{}
};

static GtkWidget *
make_key(const struct key *key)
{
	return gtk_widget_new(GTK_TYPE_SHORTCUTS_SHORTCUT,
		"title", key->title,
		"shortcut-type", GTK_SHORTCUT_ACCELERATOR,
		"accelerator", key->accelerator, NULL);
}

static GtkWidget *
make_key_group(const struct key_group *group)
{
	GtkWidget *widget = gtk_widget_new(
		GTK_TYPE_SHORTCUTS_GROUP, "title", group->title, NULL);
	for (const struct key *p = group->keys; p->title; p++)
		gtk_container_add(GTK_CONTAINER(widget), make_key(p));
	return widget;
}

static GtkWidget *
make_key_section(const struct key_section *section)
{
	GtkWidget *widget = gtk_widget_new(GTK_TYPE_SHORTCUTS_SECTION,
		"title", section->title, "section-name", section->section_name, NULL);
	for (const struct key_group *p = section->groups; p->title; p++)
		gtk_container_add(GTK_CONTAINER(widget), make_key_group(p));
	return widget;
}

static GtkWidget *
make_key_window(void)
{
	GtkWidget *window = gtk_widget_new(GTK_TYPE_SHORTCUTS_WINDOW, NULL);
	for (const struct key_section *p = help_keys; p->title; p++) {
		GtkWidget *section = make_key_section(p);
		gtk_widget_show_all(section);
		gtk_container_add(GTK_CONTAINER(window), section);
	}
	return window;
}

// --- Main --------------------------------------------------------------------

// TODO(p): See if it's possible to give separators room to shrink
// by some minor amount of pixels, margin-wise.
#define B make_toolbar_button
#define T make_toolbar_toggle
#define TOOLBAR(XX) \
	XX(BROWSE,        B("view-grid-symbolic", "Browse")) \
	XX(FILE_PREVIOUS, B("go-previous-symbolic", "Previous file")) \
	XX(FILE_NEXT,     B("go-next-symbolic", "Next file")) \
	XX(S1,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(PAGE_FIRST,    B("go-top-symbolic", "First page")) \
	XX(PAGE_PREVIOUS, B("go-up-symbolic", "Previous page")) \
	XX(PAGE_NEXT,     B("go-down-symbolic", "Next page")) \
	XX(PAGE_LAST,     B("go-bottom-symbolic", "Last page")) \
	XX(S2,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(SKIP_BACK,     B("media-skip-backward-symbolic", "Rewind playback")) \
	XX(SEEK_BACK,     B("media-seek-backward-symbolic", "Previous frame")) \
	XX(PLAY_PAUSE,    B("media-playback-start-symbolic", "Pause")) \
	XX(SEEK_FORWARD,  B("media-seek-forward-symbolic", "Next frame")) \
	XX(S3,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(PLUS,          B("zoom-in-symbolic", "Zoom in")) \
	XX(SCALE,         gtk_label_new("")) \
	XX(MINUS,         B("zoom-out-symbolic", "Zoom out")) \
	XX(ONE,           B("zoom-original-symbolic", "Original size")) \
	XX(FIT,           T("zoom-fit-best-symbolic", "Scale to fit")) \
	XX(S4,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	/* XX(PIN,        B("view-pin-symbolic", "Keep view configuration")) */ \
	/* Or perhaps "blur-symbolic", also in the extended set. */ \
	XX(SMOOTH,        T("blend-tool-symbolic", "Smooth scaling")) \
	XX(CHECKERBOARD,  T("checkerboard-symbolic", "Highlight transparency")) \
	/* XX(COLOR,      B("preferences-color-symbolic", "Color management")) */ \
	XX(SAVE,          B("document-save-as-symbolic", "Save as...")) \
	XX(PRINT,         B("document-print-symbolic", "Print...")) \
	XX(INFO,          B("info-symbolic", "Information")) \
	XX(S5,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(LEFT,          B("object-rotate-left-symbolic", "Rotate left")) \
	XX(MIRROR,        B("object-flip-horizontal-symbolic", "Mirror")) \
	XX(RIGHT,         B("object-rotate-right-symbolic", "Rotate right")) \
	XX(S6,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	/* We are YouTube. */ \
	XX(FULLSCREEN,    B("view-fullscreen-symbolic", "Fullscreen"))

enum {
#define XX(id, constructor) TOOLBAR_ ## id,
	TOOLBAR(XX)
#undef XX
	TOOLBAR_COUNT
};

struct {
	gchar **supported_globs;
	gboolean filtering;

	gchar *directory;          ///< Full path to the currently browsed directory
	GList *directory_back;     ///< History paths going backwards
	GList *directory_forward;  ///< History paths going forwards
	GPtrArray *files;
	gint files_index;

	gchar *path;

	GtkWidget *window;
	GtkWidget *stack;

	GtkWidget *browser_paned;
	GtkWidget *browser_sidebar;
	GtkWidget *plus;
	GtkWidget *minus;
	GtkWidget *browser_scroller;
	GtkWidget *browser;

	GtkWidget *view_box;
	GtkWidget *view_toolbar;
	GtkWidget *toolbar[TOOLBAR_COUNT];
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
	GtkWidget *dialog =
		gtk_message_dialog_new(GTK_WINDOW(g.window), GTK_DIALOG_MODAL,
			GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", error->message);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	g_error_free(error);
}

static void
switch_to_browser(void)
{
	gtk_window_set_title(GTK_WINDOW(g.window), g.directory);
	gtk_stack_set_visible_child(GTK_STACK(g.stack), g.browser_paned);
	gtk_widget_grab_focus(g.browser_scroller);
}

static void
switch_to_view(const char *path)
{
	gtk_window_set_title(GTK_WINDOW(g.window), path);
	gtk_stack_set_visible_child(GTK_STACK(g.stack), g.view_box);
	gtk_widget_grab_focus(g.view);
}

static gint
files_compare(gconstpointer a, gconstpointer b)
{
	gchar *path1 = g_canonicalize_filename(*(gchar **) a, g.directory);
	gchar *path2 = g_canonicalize_filename(*(gchar **) b, g.directory);
	GFile *location1 = g_file_new_for_path(path1);
	GFile *location2 = g_file_new_for_path(path2);
	g_free(path1);
	g_free(path2);
	gint result = fiv_io_filecmp(location1, location2);
	g_object_unref(location1);
	g_object_unref(location2);
	return result;
}

static void
update_files_index(void)
{
	g.files_index = -1;

	// FIXME: We presume that this basename is from the same directory.
	gchar *basename = g.path ? g_path_get_basename(g.path) : NULL;
	for (guint i = 0; i < g.files->len; i++) {
		if (!g_strcmp0(basename, g_ptr_array_index(g.files, i)))
			g.files_index = i;
	}
	g_free(basename);
}

static void
load_directory_without_reload(const gchar *dirname)
{
	gchar *dirname_duplicated = g_strdup(dirname);
	if (g.directory_back &&
			!strcmp(dirname, g.directory_back->data)) {
		// We're going back in history.
		if (g.directory) {
			g.directory_forward =
				g_list_prepend(g.directory_forward, g.directory);
			g.directory = NULL;
		}

		GList *link = g.directory_back;
		g.directory_back = g_list_remove_link(g.directory_back, link);
		g_list_free_full(link, g_free);
	} else if (g.directory_forward &&
			!strcmp(dirname, g.directory_forward->data)) {
		// We're going forward in history.
		if (g.directory) {
			g.directory_back =
				g_list_prepend(g.directory_back, g.directory);
			g.directory = NULL;
		}

		GList *link = g.directory_forward;
		g.directory_forward = g_list_remove_link(g.directory_forward, link);
		g_list_free_full(link, g_free);
	} else if (g.directory && strcmp(dirname, g.directory)) {
		// We're on a new subpath.
		g_list_free_full(g.directory_forward, g_free);
		g.directory_forward = NULL;

		g.directory_back = g_list_prepend(g.directory_back, g.directory);
		g.directory = NULL;
	}

	g_free(g.directory);
	g.directory = dirname_duplicated;
}

static void
load_directory(const gchar *dirname)
{
	if (dirname) {
		load_directory_without_reload(dirname);

		GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(
			GTK_SCROLLED_WINDOW(g.browser_scroller));
		gtk_adjustment_set_value(
			vadjustment, gtk_adjustment_get_lower(vadjustment));
	}

	g_ptr_array_set_size(g.files, 0);
	g.files_index = -1;

	GFile *file = g_file_new_for_path(g.directory);
	fiv_sidebar_set_location(FIV_SIDEBAR(g.browser_sidebar), file);
	g_object_unref(file);
	fiv_browser_load(
		FIV_BROWSER(g.browser), g.filtering ? is_supported : NULL, g.directory);

	GError *error = NULL;
	GDir *dir = g_dir_open(g.directory, 0, &error);
	if (dir) {
		for (const gchar *name = NULL; (name = g_dir_read_name(dir)); ) {
			// This really wants to make you use readdir() directly.
			char *absolute = g_canonicalize_filename(name, g.directory);
			gboolean is_dir = g_file_test(absolute, G_FILE_TEST_IS_DIR);
			g_free(absolute);
			if (!is_dir && is_supported(name))
				g_ptr_array_add(g.files, g_strdup(name));
		}
		g_dir_close(dir);

		g_ptr_array_sort(g.files, files_compare);
		update_files_index();
	} else {
		show_error_dialog(error);
	}

	gtk_widget_set_sensitive(
		g.toolbar[TOOLBAR_FILE_PREVIOUS], g.files->len > 1);
	gtk_widget_set_sensitive(
		g.toolbar[TOOLBAR_FILE_NEXT], g.files->len > 1);
	g_ptr_array_add(g.files, NULL);

	// XXX: When something outside the filtered entries is open, the index is
	// kept at -1, and browsing doesn't work. How to behave here?
	// Should we add it to the pointer array as an exception?
	if (dirname)
		switch_to_browser();
}

static void
on_filtering_toggled(GtkToggleButton *button, G_GNUC_UNUSED gpointer user_data)
{
	g.filtering = gtk_toggle_button_get_active(button);
	if (g.directory)
		load_directory(NULL);
}

static void
open(const gchar *path)
{
	g_return_if_fail(g_path_is_absolute(path));

	GError *error = NULL;
	if (!fiv_view_open(FIV_VIEW(g.view), path, &error)) {
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

	g_list_free_full(g.directory_forward, g_free);
	g.directory_forward = NULL;

	g_free(g.path);
	g.path = g_strdup(path);

	// So that load_directory() itself can be used for reloading.
	gchar *dirname = g_path_get_dirname(path);
	if (!g.files->len /* hack to always load the directory after launch */ ||
		!g.directory || strcmp(dirname, g.directory))
		load_directory(dirname);
	else
		update_files_index();
	g_free(dirname);

	switch_to_view(path);
}

static GtkWidget *
create_open_dialog(void)
{
	GtkWidget *dialog = gtk_file_chooser_dialog_new("Open file",
		GTK_WINDOW(g.window), GTK_FILE_CHOOSER_ACTION_OPEN,
		"_Cancel", GTK_RESPONSE_CANCEL,
		"_Open", GTK_RESPONSE_ACCEPT, NULL);

	GtkFileFilter *filter = gtk_file_filter_new();
	for (const char **p = fiv_io_supported_media_types; *p; p++)
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
spawn_path(const char *path)
{
	char *argv[] = {PROJECT_NAME, (char *) path, NULL};
	GError *error = NULL;
	g_spawn_async(
		NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
	g_clear_error(&error);
}

static void
on_item_activated(G_GNUC_UNUSED FivBrowser *browser, GFile *location,
	GtkPlacesOpenFlags flags, G_GNUC_UNUSED gpointer data)
{
	gchar *path = g_file_get_path(location);
	if (path) {
		if (flags == GTK_PLACES_OPEN_NEW_WINDOW)
			spawn_path(path);
		else
			open(path);
		g_free(path);
	}
}

static gboolean
open_any_path(const char *path, gboolean force_browser)
{
	GStatBuf st;
	gchar *canonical = g_canonicalize_filename(path, g.directory);
	gboolean success = !g_stat(canonical, &st);
	if (!success) {
		show_error_dialog(g_error_new(G_FILE_ERROR,
			g_file_error_from_errno(errno), "%s: %s", path, g_strerror(errno)));
	} else if (S_ISDIR(st.st_mode)) {
		load_directory(canonical);
	} else if (force_browser) {
		// GNOME, e.g., invokes this as a hint to focus the particular file,
		// which we can't currently do yet.
		gchar *directory = g_path_get_dirname(canonical);
		load_directory(directory);
		g_free(directory);
	} else {
		open(canonical);
	}
	g_free(canonical);
	return success;
}

static void
on_open_location(G_GNUC_UNUSED GtkPlacesSidebar *sidebar, GFile *location,
	GtkPlacesOpenFlags flags, G_GNUC_UNUSED gpointer user_data)
{
	gchar *path = g_file_get_path(location);
	if (path) {
		if (flags & GTK_PLACES_OPEN_NEW_WINDOW)
			spawn_path(path);
		else
			open_any_path(path, FALSE);
		g_free(path);
	}
}

static void
on_toolbar_zoom(G_GNUC_UNUSED GtkButton *button, gpointer user_data)
{
	FivIoThumbnailSize size = FIV_IO_THUMBNAIL_SIZE_COUNT;
	g_object_get(g.browser, "thumbnail-size", &size, NULL);

	size += (gintptr) user_data;
	g_return_if_fail(size >= FIV_IO_THUMBNAIL_SIZE_MIN &&
		size <= FIV_IO_THUMBNAIL_SIZE_MAX);

	g_object_set(g.browser, "thumbnail-size", size, NULL);
}

static void
on_notify_thumbnail_size(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	FivIoThumbnailSize size = 0;
	g_object_get(object, g_param_spec_get_name(param_spec), &size, NULL);
	gtk_widget_set_sensitive(g.plus, size < FIV_IO_THUMBNAIL_SIZE_MAX);
	gtk_widget_set_sensitive(g.minus, size > FIV_IO_THUMBNAIL_SIZE_MIN);
}

static void
toggle_fullscreen(void)
{
	if (gdk_window_get_state(gtk_widget_get_window(g.window)) &
		GDK_WINDOW_STATE_FULLSCREEN)
		gtk_window_unfullscreen(GTK_WINDOW(g.window));
	else
		gtk_window_fullscreen(GTK_WINDOW(g.window));
}

static void
on_window_state_event(G_GNUC_UNUSED GtkWidget *widget,
	GdkEventWindowState *event, G_GNUC_UNUSED gpointer user_data)
{
	if (!(event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN))
		return;

	const char *name = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN)
		? "view-restore-symbolic"
		: "view-fullscreen-symbolic";

	GtkButton *button = GTK_BUTTON(g.toolbar[TOOLBAR_FULLSCREEN]);
	GtkImage *image = GTK_IMAGE(gtk_button_get_image(button));
	gtk_image_set_from_icon_name(image, name, GTK_ICON_SIZE_BUTTON);
}

static void
on_help_destroyed(GtkWidget *window, GtkWidget **storage)
{
	g_return_if_fail(*storage == window);
	*storage = NULL;
}

static void
show_help_shortcuts(void)
{
	static GtkWidget *window;
	if (!window) {
		window = make_key_window();
		g_signal_connect(
			window, "destroy", G_CALLBACK(on_help_destroyed), &window);
	}

	g_object_set(window, "section-name",
		gtk_stack_get_visible_child(GTK_STACK(g.stack)) == g.view_box
			? "view"
			: "browser",
		NULL);
	gtk_widget_show(window);
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
	case GDK_MOD1_MASK | GDK_SHIFT_MASK:
		if (event->keyval == GDK_KEY_D) {
			GtkSettings *settings = gtk_settings_get_default();
			const char *property = "gtk-application-prefer-dark-theme";
			gboolean set = FALSE;
			g_object_get(settings, property, &set, NULL);
			g_object_set(settings, property, !set, NULL);
		}
		break;
	case GDK_CONTROL_MASK:
		switch (event->keyval) {
		case GDK_KEY_o:
			on_open();
			return TRUE;
		case GDK_KEY_l:
			fiv_sidebar_show_enter_location(FIV_SIDEBAR(g.browser_sidebar));
			return TRUE;
		case GDK_KEY_n:
			spawn_path(g.directory);
			return TRUE;
		case GDK_KEY_r:
			// TODO(p): Reload the image instead, if it's currently visible.
			load_directory(NULL);
			return TRUE;
		case GDK_KEY_q:
		case GDK_KEY_w:
			gtk_widget_destroy(g.window);
			return TRUE;

		case GDK_KEY_F1:
			show_help_shortcuts();
			return TRUE;
		}
		break;
	case GDK_MOD1_MASK:
		switch (event->keyval) {
		case GDK_KEY_Left:
			if (gtk_stack_get_visible_child(GTK_STACK(g.stack)) == g.view_box)
				switch_to_browser();
			else if (g.directory_back)
				load_directory(g.directory_back->data);
			return TRUE;
		case GDK_KEY_Right:
			if (g.directory_forward)
				load_directory(g.directory_forward->data);
			else
				switch_to_view(g.path);
			return TRUE;
		case GDK_KEY_Up:
			if (gtk_stack_get_visible_child(GTK_STACK(g.stack)) != g.view_box) {
				// This isn't exact, trailing slashes should be ignored.
				gchar *parent = g_path_get_dirname(g.directory);
				load_directory(parent);
				g_free(parent);
			}
			return TRUE;
		case GDK_KEY_Home:
			load_directory(g_get_home_dir());
			return TRUE;
		}
		break;
	case 0:
		switch (event->keyval) {
		case GDK_KEY_Escape:
		case GDK_KEY_q:
			gtk_widget_destroy(g.window);
			return TRUE;

		case GDK_KEY_o:
			on_open();
			return TRUE;
		case GDK_KEY_F5:
		case GDK_KEY_r:
			// TODO(p): See the comment for C-r above.
			load_directory(NULL);
			return TRUE;

		case GDK_KEY_F1:
			show_help_shortcuts();
			return TRUE;
		case GDK_KEY_F9:
			gtk_widget_set_visible(g.browser_sidebar,
				!gtk_widget_is_visible(g.browser_sidebar));
			return TRUE;

		case GDK_KEY_F11:
		case GDK_KEY_f:
			toggle_fullscreen();
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
		case GDK_KEY_F8:
			gtk_widget_set_visible(g.view_toolbar,
				!gtk_widget_is_visible(g.view_toolbar));
			return TRUE;

		case GDK_KEY_Left:
		case GDK_KEY_Up:
		case GDK_KEY_Page_Up:
			on_previous();
			return TRUE;

		case GDK_KEY_Right:
		case GDK_KEY_Down:
		case GDK_KEY_Page_Down:
			on_next();
			return TRUE;

		case GDK_KEY_Return:
			switch_to_browser();
			return TRUE;
		}
		break;
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
		switch_to_browser();
		return TRUE;
	case GDK_BUTTON_PRIMARY:
		if (event->type == GDK_2BUTTON_PRESS) {
			toggle_fullscreen();
			return TRUE;
		}
		return FALSE;
	default:
		return FALSE;
	}
}

static gboolean
on_button_press_browser_paned(
	G_GNUC_UNUSED GtkWidget *widget, GdkEventButton *event)
{
	if ((event->state & gtk_accelerator_get_default_mod_mask()))
		return FALSE;
	switch (event->button) {
	case 8:  // back
		if (g.directory_back)
			load_directory(g.directory_back->data);
		return TRUE;
	case 9:  // forward
		// FIXME: It may be inappropriate to go to the picture,
		// which may be left over from a different directory.
		if (g.directory_forward)
			load_directory(g.directory_forward->data);
		else
			switch_to_view(g.path);
		return TRUE;
	default:
		return FALSE;
	}
}

static GtkWidget *
make_toolbar_button(const gchar *symbolic, const gchar *tooltip)
{
	GtkWidget *button =
		gtk_button_new_from_icon_name(symbolic, GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_set_focus_on_click(button, FALSE);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(button), GTK_STYLE_CLASS_FLAT);
	return button;
}

static GtkWidget *
make_toolbar_toggle(const gchar *symbolic, const gchar *tooltip)
{
	GtkWidget *button = gtk_toggle_button_new();
	gtk_button_set_image(GTK_BUTTON(button),
		gtk_image_new_from_icon_name(symbolic, GTK_ICON_SIZE_BUTTON));
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_set_focus_on_click(button, FALSE);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(button), GTK_STYLE_CLASS_FLAT);
	return button;
}

static void
on_view_actions_changed(void)
{
	gboolean has_image = FALSE, can_animate = FALSE;
	gboolean has_previous = FALSE, has_next = FALSE;
	g_object_get(g.view, "has-image", &has_image, "can-animate", &can_animate,
		"has-previous-page", &has_previous, "has-next-page", &has_next, NULL);

	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_PAGE_FIRST], has_previous);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_PAGE_PREVIOUS], has_previous);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_PAGE_NEXT], has_next);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_PAGE_LAST], has_next);

	// We don't want these to flash during playback.
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_SKIP_BACK], can_animate);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_SEEK_BACK], can_animate);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_PLAY_PAUSE], can_animate);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_SEEK_FORWARD], can_animate);

	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_PLUS], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_SCALE], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_MINUS], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_ONE], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_FIT], has_image);

	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_SMOOTH], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_CHECKERBOARD], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_SAVE], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_PRINT], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_INFO], has_image);

	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_LEFT], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_MIRROR], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_RIGHT], has_image);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
on_notify_view_scale(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	double scale = 0;
	g_object_get(object, g_param_spec_get_name(param_spec), &scale, NULL);

	gchar *scale_str = g_strdup_printf("%.0f%%", round(scale * 100));
	gtk_label_set_text(GTK_LABEL(g.toolbar[TOOLBAR_SCALE]), scale_str);
	g_free(scale_str);

	// FIXME: The label doesn't immediately assume its new width.
}

static void
on_notify_view_playing(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	gboolean b = FALSE;
	g_object_get(object, g_param_spec_get_name(param_spec), &b, NULL);
	const char *name = b
		? "media-playback-pause-symbolic"
		: "media-playback-start-symbolic";

	GtkButton *button = GTK_BUTTON(g.toolbar[TOOLBAR_PLAY_PAUSE]);
	GtkImage *image = GTK_IMAGE(gtk_button_get_image(button));
	gtk_image_set_from_icon_name(image, name, GTK_ICON_SIZE_BUTTON);
}

static void
on_notify_view_boolean(
	GObject *object, GParamSpec *param_spec, gpointer user_data)
{
	gboolean b = FALSE;
	g_object_get(object, g_param_spec_get_name(param_spec), &b, NULL);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(user_data), b);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
on_toolbar_view_toggle(GtkToggleButton *button, const char *property)
{
	g_object_set(g.view, property, gtk_toggle_button_get_active(button), NULL);
}

static void
toolbar_toggler(int index, const char *property)
{
	g_signal_connect(g.toolbar[index], "toggled",
		G_CALLBACK(on_toolbar_view_toggle), (gpointer) property);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
on_toolbar_view_command(intptr_t command)
{
	fiv_view_command(FIV_VIEW(g.view), command);
}

static void
toolbar_command(int index, FivViewCommand command)
{
	g_signal_connect_swapped(g.toolbar[index], "clicked",
		G_CALLBACK(on_toolbar_view_command), (void *) (intptr_t) command);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
toolbar_connect(int index, GCallback callback)
{
	g_signal_connect_swapped(g.toolbar[index], "clicked", callback, NULL);
}

// TODO(p): The text and icons should be faded, unless the mouse cursor is
// on the toolbar. However, GtkEventBox is of no use, because either buttons
// steal our {enter,leave}-notify-events, or we steal all their input.
// Not even connecting to these signals on children works, insensitive buttons
// will not trigger anything.
// TODO(p): The toolbar should not be visible in fullscreen, or should show up
// only when the cursor reaches the top of the screen. Translucency sounds
// like a good mechanism here. Presumably, GtkOverlay could be used for this,
// but it faces the same problem as above--the input model sucks.
// TODO(p): Simply hide it in fullscreen and add a replacement context menu.
static GtkWidget *
make_view_toolbar(void)
{
#define XX(id, constructor) g.toolbar[TOOLBAR_ ## id] = constructor;
	TOOLBAR(XX)
#undef XX

	gtk_widget_set_margin_start(g.toolbar[TOOLBAR_SCALE], 5);
	gtk_widget_set_margin_end(g.toolbar[TOOLBAR_SCALE], 5);

	// So that the width doesn't jump around in the usual zoom range.
	// Ideally, we'd measure the widest digit and use width(NNN%).
	gtk_label_set_width_chars(GTK_LABEL(g.toolbar[TOOLBAR_SCALE]), 5);
	gtk_widget_set_halign(g.toolbar[TOOLBAR_SCALE], GTK_ALIGN_CENTER);

	// GtkStatusBar solves a problem we do not have here.
	GtkWidget *view_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_name(view_toolbar, "toolbar");
	GtkBox *box = GTK_BOX(view_toolbar);

	// Exploring different versions of awkward layouts.
	for (int i = 0; i <= TOOLBAR_S1; i++)
		gtk_box_pack_start(box, g.toolbar[i], FALSE, FALSE, 0);
	for (int i = TOOLBAR_COUNT; --i >= TOOLBAR_S6; )
		gtk_box_pack_end(box, g.toolbar[i], FALSE, FALSE, 0);

	GtkWidget *center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	for (int i = TOOLBAR_S1; ++i < TOOLBAR_S6; )
		gtk_box_pack_start(GTK_BOX(center), g.toolbar[i], FALSE, FALSE, 0);
	gtk_box_set_center_widget(box, center);

	toolbar_connect(TOOLBAR_BROWSE,        G_CALLBACK(switch_to_browser));
	toolbar_connect(TOOLBAR_FILE_PREVIOUS, G_CALLBACK(on_previous));
	toolbar_connect(TOOLBAR_FILE_NEXT,     G_CALLBACK(on_next));
	toolbar_command(TOOLBAR_PAGE_FIRST,    FIV_VIEW_COMMAND_PAGE_FIRST);
	toolbar_command(TOOLBAR_PAGE_PREVIOUS, FIV_VIEW_COMMAND_PAGE_PREVIOUS);
	toolbar_command(TOOLBAR_PAGE_NEXT,     FIV_VIEW_COMMAND_PAGE_NEXT);
	toolbar_command(TOOLBAR_PAGE_LAST,     FIV_VIEW_COMMAND_PAGE_LAST);
	toolbar_command(TOOLBAR_SKIP_BACK,     FIV_VIEW_COMMAND_FRAME_FIRST);
	toolbar_command(TOOLBAR_SEEK_BACK,     FIV_VIEW_COMMAND_FRAME_PREVIOUS);
	toolbar_command(TOOLBAR_PLAY_PAUSE,    FIV_VIEW_COMMAND_TOGGLE_PLAYBACK);
	toolbar_command(TOOLBAR_SEEK_FORWARD,  FIV_VIEW_COMMAND_FRAME_NEXT);
	toolbar_command(TOOLBAR_PLUS,          FIV_VIEW_COMMAND_ZOOM_IN);
	toolbar_command(TOOLBAR_MINUS,         FIV_VIEW_COMMAND_ZOOM_OUT);
	toolbar_command(TOOLBAR_ONE,           FIV_VIEW_COMMAND_ZOOM_1);
	toolbar_toggler(TOOLBAR_FIT,           "scale-to-fit");
	toolbar_toggler(TOOLBAR_SMOOTH,        "filter");
	toolbar_toggler(TOOLBAR_CHECKERBOARD,  "checkerboard");
	toolbar_command(TOOLBAR_PRINT,         FIV_VIEW_COMMAND_PRINT);
	toolbar_command(TOOLBAR_SAVE,          FIV_VIEW_COMMAND_SAVE_PAGE);
	toolbar_command(TOOLBAR_INFO,          FIV_VIEW_COMMAND_INFO);
	toolbar_command(TOOLBAR_LEFT,          FIV_VIEW_COMMAND_ROTATE_LEFT);
	toolbar_command(TOOLBAR_MIRROR,        FIV_VIEW_COMMAND_MIRROR);
	toolbar_command(TOOLBAR_RIGHT,         FIV_VIEW_COMMAND_ROTATE_RIGHT);
	toolbar_connect(TOOLBAR_FULLSCREEN,    G_CALLBACK(toggle_fullscreen));

	g_signal_connect(g.view, "notify::scale",
		G_CALLBACK(on_notify_view_scale), NULL);
	g_signal_connect(g.view, "notify::playing",
		G_CALLBACK(on_notify_view_playing), NULL);
	g_signal_connect(g.view, "notify::scale-to-fit",
		G_CALLBACK(on_notify_view_boolean), g.toolbar[TOOLBAR_FIT]);
	g_signal_connect(g.view, "notify::filter",
		G_CALLBACK(on_notify_view_boolean), g.toolbar[TOOLBAR_SMOOTH]);
	g_signal_connect(g.view, "notify::checkerboard",
		G_CALLBACK(on_notify_view_boolean), g.toolbar[TOOLBAR_CHECKERBOARD]);

	g_object_notify(G_OBJECT(g.view), "scale");
	g_object_notify(G_OBJECT(g.view), "playing");
	g_object_notify(G_OBJECT(g.view), "scale-to-fit");
	g_object_notify(G_OBJECT(g.view), "filter");
	g_object_notify(G_OBJECT(g.view), "checkerboard");

	GCallback callback = G_CALLBACK(on_view_actions_changed);
	g_signal_connect(g.view, "notify::has-image", callback, NULL);
	g_signal_connect(g.view, "notify::can-animate", callback, NULL);
	g_signal_connect(g.view, "notify::has-previous-page", callback, NULL);
	g_signal_connect(g.view, "notify::has-next-page", callback, NULL);
	callback();
	return view_toolbar;
}

// This is incredibly broken https://stackoverflow.com/a/51054396/76313
// thus resolving the problem using overlaps.
// We're trying to be universal for light and dark themes both. It's hard.
static const char stylesheet[] = "@define-color fiv-tile @content_view_bg; \
	fiv-view, fiv-browser { background: @content_view_bg; } \
	placessidebar.fiv .toolbar { padding: 2px 6px; } \
	placessidebar.fiv box > separator { margin: 4px 0; } \
	#toolbar button { padding-left: 0; padding-right: 0; } \
	#toolbar > button:first-child { padding-left: 4px; } \
	#toolbar > button:last-child { padding-right: 4px; } \
	#toolbar separator { \
		background: mix(@insensitive_fg_color, \
			@insensitive_bg_color, 0.4); margin: 6px 10px; \
	} \
	fiv-browser { padding: 5px; } \
	fiv-browser.item { \
		color: mix(#000, @content_view_bg, 0.625); margin: 8px; \
		border: 2px solid #fff; \
	} \
	fiv-browser.item, fiv-view.checkerboard { \
		background: @theme_bg_color; background-image: \
			linear-gradient(45deg, @fiv-tile 26%, transparent 26%), \
			linear-gradient(-45deg, @fiv-tile 26%, transparent 26%), \
			linear-gradient(45deg, transparent 74%, @fiv-tile 74%), \
			linear-gradient(-45deg, transparent 74%, @fiv-tile 74%); \
		background-size: 40px 40px; \
		background-position: 0 0, 0 20px, 20px -20px, -20px 0px; \
	} \
	fiv-browser.item:backdrop { \
		color: mix(#000, @content_view_bg, 0.875); \
		border-color: mix(#fff, @content_view_bg, 0.5); \
	} \
	fiv-browser.item.symbolic { \
		border-color: transparent; color: shade(@theme_bg_color, 0.875); \
		background: @theme_bg_color; background-image: none; \
	}";

int
main(int argc, char *argv[])
{
	gboolean show_version = FALSE, show_supported_media_types = FALSE,
		browse = FALSE;
	gchar **path_args = NULL;
	const GOptionEntry options[] = {
		{G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &path_args,
			NULL, "[FILE | DIRECTORY]"},
		{"list-supported-media-types", 0, G_OPTION_FLAG_IN_MAIN,
			G_OPTION_ARG_NONE, &show_supported_media_types,
			"Output supported media types and exit", NULL},
		{"browse", 0, G_OPTION_FLAG_IN_MAIN,
			G_OPTION_ARG_NONE, &browse,
			"Start in filesystem browsing mode", NULL},
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
		for (char **types = fiv_io_all_supported_media_types(); *types; )
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
	gtk_icon_theme_add_resource_path(
		gtk_icon_theme_get_default(), "/org/gnome/design/IconLibrary/");

	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(
		provider, stylesheet, sizeof stylesheet - 1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);

	GtkWidget *view_scroller = gtk_scrolled_window_new(NULL, NULL);
	g.view = g_object_new(FIV_TYPE_VIEW, NULL);
	g_signal_connect(g.view, "key-press-event",
		G_CALLBACK(on_key_press_view), NULL);
	g_signal_connect(g.view, "button-press-event",
		G_CALLBACK(on_button_press_view), NULL);
	gtk_container_add(GTK_CONTAINER(view_scroller), g.view);

	// Need to put the toolbar at the top, because of the horizontal scrollbar.
	g.view_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	g.view_toolbar = make_view_toolbar();
	gtk_box_pack_start(GTK_BOX(g.view_box), g.view_toolbar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(g.view_box),
		gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(g.view_box), view_scroller, TRUE, TRUE, 0);
	gtk_widget_show_all(g.view_box);

	g.browser_scroller = gtk_scrolled_window_new(NULL, NULL);
	g.browser = g_object_new(FIV_TYPE_BROWSER, NULL);
	gtk_widget_set_vexpand(g.browser, TRUE);
	gtk_widget_set_hexpand(g.browser, TRUE);
	g_signal_connect(g.browser, "item-activated",
		G_CALLBACK(on_item_activated), NULL);
	gtk_container_add(GTK_CONTAINER(g.browser_scroller), g.browser);

	// Christ, no, do not scroll all the way to the top on focus.
	GtkWidget *browser_port = gtk_bin_get_child(GTK_BIN(g.browser_scroller));
	gtk_container_set_focus_hadjustment(GTK_CONTAINER(browser_port), NULL);
	gtk_container_set_focus_vadjustment(GTK_CONTAINER(browser_port), NULL);

	// TODO(p): As with GtkFileChooserWidget, bind C-h to filtering,
	// and mayhaps forward the rest to the sidebar, somehow.
	g.browser_sidebar = g_object_new(FIV_TYPE_SIDEBAR, NULL);
	g_signal_connect(g.browser_sidebar, "open-location",
		G_CALLBACK(on_open_location), NULL);

	// The opposite case, and it doesn't work from the init function.
	GtkWidget *sidebar_port = gtk_bin_get_child(GTK_BIN(g.browser_sidebar));
	gtk_container_set_focus_hadjustment(GTK_CONTAINER(sidebar_port),
		gtk_scrolled_window_get_hadjustment(
			GTK_SCROLLED_WINDOW(g.browser_sidebar)));
	gtk_container_set_focus_vadjustment(GTK_CONTAINER(sidebar_port),
		gtk_scrolled_window_get_vadjustment(
			GTK_SCROLLED_WINDOW(g.browser_sidebar)));

	g.plus = gtk_button_new_from_icon_name("zoom-in-symbolic",
		GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(g.plus, "Larger thumbnails");
	g_signal_connect(g.plus, "clicked",
		G_CALLBACK(on_toolbar_zoom), (gpointer) +1);

	g.minus = gtk_button_new_from_icon_name("zoom-out-symbolic",
		GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(g.minus, "Smaller thumbnails");
	g_signal_connect(g.minus, "clicked",
		G_CALLBACK(on_toolbar_zoom), (gpointer) -1);

	GtkWidget *zoom_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(zoom_group), GTK_STYLE_CLASS_LINKED);
	gtk_box_pack_start(GTK_BOX(zoom_group), g.plus, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(zoom_group), g.minus, FALSE, FALSE, 0);

	GtkWidget *funnel = gtk_toggle_button_new();
	gtk_container_add(GTK_CONTAINER(funnel),
		gtk_image_new_from_icon_name("funnel-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_widget_set_tooltip_text(funnel, "Hide unsupported files");
	g_signal_connect(funnel, "toggled",
		G_CALLBACK(on_filtering_toggled), NULL);

	GtkBox *toolbar = fiv_sidebar_get_toolbar(FIV_SIDEBAR(g.browser_sidebar));
	gtk_box_pack_start(toolbar, zoom_group, FALSE, FALSE, 0);
	gtk_box_pack_start(toolbar, funnel, FALSE, FALSE, 0);
	gtk_widget_set_halign(GTK_WIDGET(toolbar), GTK_ALIGN_CENTER);

	g.browser_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_add1(GTK_PANED(g.browser_paned), g.browser_sidebar);
	gtk_paned_add2(GTK_PANED(g.browser_paned), g.browser_scroller);
	g_signal_connect(g.browser_paned, "button-press-event",
		G_CALLBACK(on_button_press_browser_paned), NULL);

	// TODO(p): Can we not do it here separately?
	gtk_widget_show_all(g.browser_paned);

	g.stack = gtk_stack_new();
	gtk_stack_set_transition_type(
		GTK_STACK(g.stack), GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_container_add(GTK_CONTAINER(g.stack), g.view_box);
	gtk_container_add(GTK_CONTAINER(g.stack), g.browser_paned);

	g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(g.window, "destroy",
		G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(g.window, "key-press-event",
		G_CALLBACK(on_key_press), NULL);
	g_signal_connect(g.window, "window-state-event",
		G_CALLBACK(on_window_state_event), NULL);
	gtk_container_add(GTK_CONTAINER(g.window), g.stack);

	char **types = fiv_io_all_supported_media_types();
	g.supported_globs = extract_mime_globs((const char **) types);
	g_strfreev(types);

	g_signal_connect(g.browser, "notify::thumbnail-size",
		G_CALLBACK(on_notify_thumbnail_size), NULL);
	on_toolbar_zoom(NULL, (gpointer) 0);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(funnel), TRUE);

	g.files = g_ptr_array_new_full(16, g_free);
	g.directory = g_get_current_dir();
	if (!path_arg || !open_any_path(path_arg, browse))
		open_any_path(g.directory, FALSE);

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

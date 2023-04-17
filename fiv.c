//
// fiv.c: fuck-if-I-know-how-to-name-it image browser and viewer
//
// Copyright (c) 2021 - 2023, Přemysl Eric Janouch <p@janouch.name>
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
#ifdef GDK_WINDOWING_QUARTZ
#include <gdk/gdkquartz.h>
#endif  // GDK_WINDOWING_QUARTZ

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef G_OS_WIN32
#include <io.h>
#include <fcntl.h>
#endif  // G_OS_WIN32

#include "config.h"

#include "fiv-browser.h"
#include "fiv-collection.h"
#include "fiv-io.h"
#include "fiv-sidebar.h"
#include "fiv-thumbnail.h"
#include "fiv-view.h"

// --- Utilities ---------------------------------------------------------------

static void exit_fatal(const char *format, ...) G_GNUC_PRINTF(1, 2);

static void
exit_fatal(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	gchar *format_nl = g_strdup_printf("%s\n", format);
	vfprintf(stderr, format_nl, ap);
	free(format_nl);

	va_end(ap);
	exit(EXIT_FAILURE);
}

static gchar **
slist_to_strv(GSList *slist)
{
	gchar **strv = g_malloc0_n(g_slist_length(slist) + 1, sizeof *strv),
		**p = strv;
	for (GSList *link = slist; link; link = link->next)
		*p++ = link->data;
	g_slist_free(slist);
	return strv;
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
	{"F1", "Show help"},
	{"F10", "Open menu"},
	{"<Control>comma", "Preferences"},
	{"<Control>question", "Keyboard shortcuts"},
	{"q <Control>q", "Quit"},
	{"<Control>w", "Quit"},
	{}
};

static struct key help_keys_navigation[] = {
	{"<Control>l", "Open location..."},
	{"<Control>n", "Open a new window"},
	{"<Alt>Left", "Go back in history"},
	{"<Alt>Right", "Go forward in history"},
	{}
};

static struct key help_keys_view[] = {
	{"F11 f", "Toggle fullscreen"},
	{"<Alt><Shift>d", "Toggle dark theme variant"},
	{}
};

static struct key_group help_keys_browser[] = {
	{"General", help_keys_general},
	{"General: Navigation", help_keys_navigation},
	{"General: View", help_keys_view},
	{"Navigation", (struct key[]) {
		{"<Alt>Home", "Go home"},
		{"<Alt>Up", "Go to parent directory"},
		{"bracketleft", "Go to previous directory in tree"},
		{"bracketright", "Go to next directory in tree"},
		{"Return", "Open selected item"},
		{"<Alt>Return", "Show file information"},
		{}
	}},
	{"View", (struct key[]) {
		{"F7", "Toggle toolbar"},
		{"F9", "Toggle navigation sidebar"},
		{"F5 r <Control>r", "Reload"},
		{"h <Control>h", "Toggle hiding unsupported files"},
		{"t <Control>t", "Toggle showing filenames"},
		{"<Control>plus", "Larger thumbnails"},
		{"<Control>minus", "Smaller thumbnails"},
		{}
	}},
	{}
};

static struct key_group help_keys_viewer[] = {
	{"General", help_keys_general},
	{"General: Navigation", help_keys_navigation},
	{"General: View", help_keys_view},
	{"Navigation", (struct key[]) {
		{"Left Up Page_Up", "Previous image"},
		{"Right Down Page_Down", "Next image"},
		{"Escape Return", "Return to browser"},
		{}
	}},
	{"View", (struct key[]) {
		{"F7", "Toggle toolbar"},
		{"F5 r <Primary>r", "Reload"},
		{}
	}},
	{"Zoom", (struct key[]) {
		{"<Primary>0", "Set zoom to 100%"},
		{"1...9", "Set zoom to N:1"},
		{"plus <Primary>plus", "Zoom in"},
		{"minus <Primary>minus", "Zoom out"},
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
		{"k", "Toggle keeping zoom and position"},
		{"x", "Toggle scale to fit if larger"},
#ifdef HAVE_LCMS2
		{"c", "Toggle color management"},
#endif
		{"i", "Toggle smooth scaling"},
		{"t", "Toggle transparency highlighting"},
#ifdef HAVE_JPEG_QS
		{"e", "Toggle low-quality JPEG enhancement"},
#endif
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
		{"<Primary>c", "Copy frame to clipboard"},
		{"<Primary>p", "Print..."},
		{"<Primary>s", "Save page as..."},
		{"<Primary><Shift>s", "Save frame as..."},
		{"<Alt>Return", "Show file information"},
		{}
	}},
	{}
};

static struct key_section help_keys[] = {
	{"Browser", "browser", help_keys_browser},
	{"Viewer", "viewer", help_keys_viewer},
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

// --- About -------------------------------------------------------------------

typedef struct {
	gint cx, cy;
	cairo_pattern_t *v_pattern;
} AboutContext;

static void
on_about_map(GtkWidget *widget, G_GNUC_UNUSED gpointer user_data)
{
	GdkFrameClock *clock = gtk_widget_get_frame_clock(widget);
	(void) g_signal_connect_swapped(
		clock, "update", G_CALLBACK(gtk_widget_queue_draw), widget);
	gdk_frame_clock_begin_updating(clock);
}

static void
on_about_unmap(GtkWidget *widget, G_GNUC_UNUSED gpointer user_data)
{
	GdkFrameClock *clock = gtk_widget_get_frame_clock(widget);
	gdk_frame_clock_end_updating(clock);
}

static gboolean
on_about_motion(
	G_GNUC_UNUSED GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
	AboutContext *ctx = user_data;
	ctx->cx = event->x;
	ctx->cy = event->y;
	return FALSE;
}

static gboolean
on_about_leave(G_GNUC_UNUSED GtkWidget *widget,
	G_GNUC_UNUSED GdkEventCrossing *event, gpointer user_data)
{
	AboutContext *ctx = user_data;
	ctx->cx = -1;
	ctx->cy = -1;
	return FALSE;
}

enum { ABOUT_SIZE = 48, ABOUT_SCALE = 3, ABOUT_HEIGHT = ABOUT_SIZE * 4 / 3 };

// The mismatching resolution is incidental, and kept for interesting looks.
static cairo_pattern_t *
make_infinite_v_pattern(void)
{
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ABOUT_SIZE, ABOUT_SIZE);
	cairo_t *cr = cairo_create(surface);
	cairo_move_to(cr, 2, 7);
	cairo_rel_line_to(cr, 44, 0);
	cairo_rel_line_to(cr, -17, 39);
	cairo_rel_line_to(cr, -10, 0);
	cairo_close_path(cr);

	cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 7, 0, 46);
	cairo_pattern_add_color_stop_rgb(gradient, 1, 1, 0x66 / 255., 0);
	cairo_pattern_add_color_stop_rgb(gradient, 0, 1, 0xaa / 255., 0);
	cairo_set_source(cr, gradient);
	cairo_pattern_destroy(gradient);
	cairo_fill(cr);

	cairo_destroy(cr);
	cairo_pattern_t *pattern = cairo_pattern_create_for_surface(surface);
	cairo_surface_destroy(surface);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
	cairo_matrix_t matrix = {};
	cairo_matrix_init_translate(&matrix, ABOUT_SIZE / 2, 0);
	cairo_pattern_set_matrix(pattern, &matrix);
	return pattern;
}

static void
draw_ligature(cairo_t *cr)
{
	// Transcribed fiv.svg--it would arguably make more sense to generate
	// that file and these Cairo instructions from the same source.
	cairo_move_to(cr, 12.5, 20.5);
	cairo_rel_line_to(cr, -4, 0);
	cairo_rel_line_to(cr, 0, +5);
	cairo_rel_line_to(cr, +4, 0);
	cairo_rel_line_to(cr, 0, +15);
	cairo_rel_line_to(cr, 9, 0);
	cairo_rel_line_to(cr, 0, -15);
	cairo_rel_line_to(cr, +2, 0);
	cairo_rel_line_to(cr, 0, -5);
	cairo_rel_line_to(cr, -2, 0);
	cairo_rel_line_to(cr, 0, -8);
	cairo_rel_curve_to(cr, 0, -4, 5, -4, 5, 0);
	cairo_rel_curve_to(cr, 0, 6, 9, 6, 9, 0);
	cairo_rel_curve_to(cr, 0, -12, -23, -12, -23, 0);
	cairo_close_path(cr);

	cairo_move_to(cr, 26.5, 20.5);
	cairo_rel_line_to(cr, 9, 0);
	cairo_rel_line_to(cr, 0, 20);
	cairo_rel_line_to(cr, -9, 0);
	cairo_close_path(cr);

	cairo_path_t *ligature = cairo_copy_path(cr);
	cairo_save(cr);
	cairo_clip(cr);

	// Shadow approximation, given the lack of filters in Cairo.
	enum { STEPS = 5 };
	for (int i = 0; i <= STEPS; i++) {
		cairo_save(cr);
		double o = 1. / ABOUT_SCALE + 1. / ABOUT_SCALE * i;
		cairo_translate(cr, o, o);
		cairo_append_path(cr, ligature);
		double v = 1 - (STEPS - i) * 0.075;
		cairo_set_source_rgb(cr, v, v, v);
		cairo_fill(cr);
		cairo_restore(cr);
	}

	cairo_restore(cr);
	cairo_append_path(cr, ligature);
	cairo_path_destroy(ligature);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

static gboolean
on_about_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
	AboutContext *ctx = user_data;
	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);
	GtkStyleContext *style = gtk_widget_get_style_context(widget);
	gtk_render_background(style, cr, 0, 0, allocation.width, allocation.height);

	// The transformation matrix turns out/is applied wrongly on Quartz.
	gboolean broken_backend = cairo_surface_get_type(cairo_get_target(cr)) ==
		CAIRO_SURFACE_TYPE_QUARTZ;
	if (broken_backend)
		cairo_push_group(cr);

	cairo_translate(cr, (allocation.width - ABOUT_SIZE * ABOUT_SCALE) / 2,
		ABOUT_SIZE * ABOUT_SCALE / 4);
	cairo_scale(cr, ABOUT_SCALE, ABOUT_SCALE);

	cairo_save(cr);
	cairo_translate(cr, ABOUT_SIZE / 2, ABOUT_SIZE / 2);
	if (ctx->cx >= 0 && ctx->cy >= 0) {
		gint dx = ctx->cx - allocation.width / 2;
		gint dy = ctx->cy - ABOUT_SIZE * ABOUT_SCALE * 3 / 4;
		cairo_rotate(cr, atan2(dy, dx) - M_PI_2);
	}

	GdkFrameClock *clock = gtk_widget_get_frame_clock(widget);
	gint64 t = gdk_frame_clock_get_frame_time(clock);
	cairo_translate(cr, 0, (gint64) (t / 4e4) % ABOUT_SIZE);

	cairo_set_source(cr, ctx->v_pattern);
	cairo_paint(cr);
	cairo_translate(cr, ABOUT_SIZE / 2, 14 /* Through trial and error. */);
	cairo_scale(cr, 1, -1);
	cairo_set_source(cr, ctx->v_pattern);
	cairo_paint(cr);

	cairo_restore(cr);
	draw_ligature(cr);

	if (broken_backend) {
		cairo_pop_group_to_source(cr);
		cairo_paint(cr);
	}
	return TRUE;
}

static void
show_about_dialog(GtkWidget *parent)
{
	GtkWidget *dialog = gtk_widget_new(GTK_TYPE_DIALOG, "use-header-bar", TRUE,
		"title", "About", "transient-for", parent, "destroy-with-parent", TRUE,
		NULL);

	GtkWidget *area = gtk_drawing_area_new();
	gtk_style_context_add_class(gtk_widget_get_style_context(area), "view");
	gtk_widget_set_size_request(
		area, ABOUT_SIZE * ABOUT_SCALE * 2, ABOUT_HEIGHT * ABOUT_SCALE);

	AboutContext ctx = {
		.cx = -1, .cy = -1, .v_pattern = make_infinite_v_pattern()};
	gtk_widget_add_events(
		area, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
	g_signal_connect(
		area, "motion-notify-event", G_CALLBACK(on_about_motion), &ctx);
	g_signal_connect(
		area, "leave-notify-event", G_CALLBACK(on_about_leave), &ctx);

	g_signal_connect(area, "draw", G_CALLBACK(on_about_draw), &ctx);
	g_signal_connect(area, "map", G_CALLBACK(on_about_map), &ctx);
	g_signal_connect(area, "unmap", G_CALLBACK(on_about_unmap), &ctx);

	// The rest is approximately copying GTK+'s own gtkaboutdialog.ui.
	GtkWidget *name = gtk_label_new(NULL);
	gtk_label_set_selectable(GTK_LABEL(name), TRUE);

	const char *version = PROJECT_VERSION;
	gchar *markup = g_strdup_printf(
		"<b>%s</b> %s", PROJECT_NAME, &version[*version == 'v']);
	gtk_label_set_markup(GTK_LABEL(name), markup);
	g_free(markup);

	GtkWidget *website = gtk_label_new(NULL);
	gtk_label_set_selectable(GTK_LABEL(website), TRUE);
	const char *url = "https://git.janouch.name/p/" PROJECT_NAME;
	gchar *link = g_strdup_printf("<a href='%s'>%s</a>", url, url);
	gtk_label_set_markup(GTK_LABEL(website), link);
	g_free(link);

	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(
		GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);

	GtkWidget *viewer = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(viewer), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(viewer), FALSE);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(viewer), 8);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(viewer), 8);
	gtk_container_add(GTK_CONTAINER(sw), viewer);

	GBytes *license =
		g_resources_lookup_data("/LICENSE", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	g_return_if_fail(license != NULL);
	gchar *escaped = g_markup_escape_text(g_bytes_get_data(license, NULL), -1);
	g_bytes_unref(license);

	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer));
	gchar *formatted = g_strdup_printf("<small>%s</small>", escaped);
	g_free(escaped);
	GtkTextIter iter = {};
	gtk_text_buffer_get_start_iter(buffer, &iter);
	gtk_text_buffer_insert_markup(buffer, &iter, formatted, -1);
	g_free(formatted);

	enum { SUBBOX_MARGIN = 12 /* GNOME HIG 1.0 */ };
	GtkWidget *subbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(subbox, SUBBOX_MARGIN);
	gtk_widget_set_margin_end(subbox, SUBBOX_MARGIN);
	gtk_box_pack_start(GTK_BOX(subbox), name, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(subbox), website, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(subbox), sw, TRUE, TRUE, 0);

	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_box_pack_start(GTK_BOX(box), area, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(box), subbox, TRUE, TRUE, SUBBOX_MARGIN);

	gtk_window_set_default_size(GTK_WINDOW(dialog), -1, 480);
	gtk_window_set_geometry_hints(GTK_WINDOW(dialog), NULL,
		&(GdkGeometry) {.max_width = -1, .max_height = G_MAXSHORT},
		GDK_HINT_MAX_SIZE);

	gtk_widget_grab_focus(viewer);
	gtk_widget_show_all(dialog);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	cairo_pattern_destroy(ctx.v_pattern);
}

// --- Main --------------------------------------------------------------------

// TODO(p): See if it's possible to give separators room to shrink
// by some minor amount of pixels, margin-wise.
#define B make_toolbar_button
#define T make_toolbar_toggle
#define R make_toolbar_radio
#define BROWSEBAR(XX) \
	XX(SIDEBAR,       T("sidebar-show-symbolic", "Show sidebar")) \
	XX(S1,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(DIR_PREVIOUS,  B("go-previous-symbolic", "Previous directory")) \
	XX(DIR_NEXT,      B("go-next-symbolic", "Next directory")) \
	XX(S2,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(PLUS,          B("zoom-in-symbolic", "Larger thumbnails")) \
	XX(MINUS,         B("zoom-out-symbolic", "Smaller thumbnails")) \
	XX(S3,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(FILENAMES,     T("text-symbolic", "Show filenames")) \
	XX(FILTER,        T("funnel-symbolic", "Hide unsupported files")) \
	XX(S4,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(SORT_DIR,      B("view-sort-ascending-symbolic", "Sort ascending")) \
	XX(SORT_NAME,     R("Name", "Sort by filename")) \
	XX(SORT_TIME,     R("Time", "Sort by time of last modification")) \
	XX(S5,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	/* We are YouTube. */ \
	XX(FULLSCREEN,    B("view-fullscreen-symbolic", "Fullscreen"))

enum {
#define XX(id, constructor) BROWSEBAR_ ## id,
	BROWSEBAR(XX)
#undef XX
	BROWSEBAR_COUNT
};

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
	XX(FIXATE,        T("pin2-symbolic", "Keep zoom and position")) \
	XX(MINUS,         B("zoom-out-symbolic", "Zoom out")) \
	XX(SCALE,         gtk_label_new("")) \
	XX(PLUS,          B("zoom-in-symbolic", "Zoom in")) \
	XX(ONE,           B("zoom-original-symbolic", "Original size")) \
	XX(FIT,           T("zoom-fit-best-symbolic", "Scale to fit")) \
	XX(S4,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	/* Or perhaps "blur-symbolic", also in the extended set. */ \
	XX(COLOR,         T("preferences-color-symbolic", "Color management")) \
	XX(SMOOTH,        T("blend-tool-symbolic", "Smooth scaling")) \
	XX(CHECKERBOARD,  T("checkerboard-symbolic", "Highlight transparency")) \
	XX(ENHANCE,       T("heal-symbolic", "Enhance low-quality JPEG")) \
	XX(S5,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(SAVE,          B("document-save-as-symbolic", "Save as...")) \
	XX(PRINT,         B("document-print-symbolic", "Print...")) \
	XX(INFO,          B("info-symbolic", "Information")) \
	XX(S6,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	XX(LEFT,          B("object-rotate-left-symbolic", "Rotate left")) \
	XX(MIRROR,        B("object-flip-horizontal-symbolic", "Mirror")) \
	XX(RIGHT,         B("object-rotate-right-symbolic", "Rotate right")) \
	XX(S7,            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)) \
	/* We are YouTube. */ \
	XX(FULLSCREEN,    B("view-fullscreen-symbolic", "Fullscreen"))

enum {
#define XX(id, constructor) TOOLBAR_ ## id,
	TOOLBAR(XX)
#undef XX
	TOOLBAR_COUNT
};

struct {
	FivIoModel *model;         ///< "directory" contents
	gchar *directory;          ///< URI of the currently browsed directory
	GList *directory_back;     ///< History paths as URIs going backwards
	GList *directory_forward;  ///< History paths as URIs going forwards

	gchar *uri;                ///< Current image URI, if any
	gint files_index;          ///< Where "uri" is within the model's files

	GtkWidget *window;
	GtkWidget *menu;
	GtkWidget *stack;

	GtkWidget *browser_paned;
	GtkWidget *browser_sidebar;
	GtkWidget *browser_toolbar;
	GtkWidget *browsebar[BROWSEBAR_COUNT];
	GtkWidget *browser_scroller;
	GtkWidget *browser;

	GtkWidget *view_box;
	GtkWidget *view_toolbar;
	GtkWidget *view_info;
	GtkWidget *view_info_label;
	GtkWidget *toolbar[TOOLBAR_COUNT];
	GtkWidget *view;
} g;

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
set_window_title(const char *uri)
{
	GFile *file = g_file_new_for_uri(uri);
	gchar *name = g_file_get_parse_name(file);
	gtk_window_set_title(GTK_WINDOW(g.window), name);
	g_free(name);
	g_object_unref(file);
}

static void
switch_to_browser_noselect(void)
{
	set_window_title(g.directory);
	gtk_stack_set_visible_child(GTK_STACK(g.stack), g.browser_paned);
	gtk_widget_grab_focus(g.browser);
}

static void
switch_to_browser(void)
{
	// XXX: This distinction is weird, it might make sense to make
	// an end-user option for the behaviour.
	switch_to_browser_noselect();
	fiv_browser_select(FIV_BROWSER(g.browser), g.uri);
}

static void
switch_to_view(void)
{
	g_return_if_fail(g.uri != NULL);

	set_window_title(g.uri);
	gtk_stack_set_visible_child(GTK_STACK(g.stack), g.view_box);
	gtk_widget_grab_focus(g.view);
}

static gchar *
parent_uri(GFile *child_file)
{
	// The empty URI results in a convenient dummy GFile implementation.
	GFile *parent = g_file_get_parent(child_file);
	if (!parent)
		return g_strdup("");

	gchar *parent_uri = g_file_get_uri(parent);
	g_object_unref(parent);
	return parent_uri;
}

static void
update_files_index(void)
{
	gsize files_len = 0;
	FivIoModelEntry *const *files = fiv_io_model_get_files(g.model, &files_len);

	g.files_index = -1;
	for (guint i = 0; i < files_len; i++)
		if (!g_strcmp0(g.uri, files[i]->uri))
			g.files_index = i;
}

static void
change_directory_without_reload(const char *uri)
{
	if (g.directory) {
		// Note that this function can be passed g.directory directly.
		if (!strcmp(uri, g.directory))
			return;

		// We're on a new subpath.
		g_list_free_full(g.directory_forward, g_free);
		g.directory_forward = NULL;

		g.directory_back = g_list_prepend(g.directory_back, g.directory);
	}

	g.directory = g_strdup(uri);
}

static void
load_directory_without_switching(const char *uri)
{
	if (uri) {
		change_directory_without_reload(uri);

		GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(
			GTK_SCROLLED_WINDOW(g.browser_scroller));
		gtk_adjustment_set_value(
			vadjustment, gtk_adjustment_get_lower(vadjustment));
	}

	GError *error = NULL;
	GFile *file = g_file_new_for_uri(g.directory);
	if (fiv_io_model_open(g.model, file, &error)) {
		// This is handled by our ::files-changed callback.
	} else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
		g_error_free(error);
	} else {
		show_error_dialog(error);
	}

	g_object_unref(file);
}

static void
load_directory(const char *uri)
{
	load_directory_without_switching(uri);

	if (uri) {
		switch_to_browser_noselect();

		// TODO(p): Rather place it in history.
		g_clear_pointer(&g.uri, g_free);
	}
}

static void
go_back(void)
{
	if (gtk_stack_get_visible_child(GTK_STACK(g.stack)) == g.view_box) {
		switch_to_browser_noselect();
	} else if (g.directory_back) {
		if (g.directory)
			g.directory_forward =
				g_list_prepend(g.directory_forward, g.directory);

		const gchar *uri = g.directory = g.directory_back->data;

		GList *link = g.directory_back;
		g.directory_back = g_list_remove_link(g.directory_back, link);
		g_list_free(link);

		load_directory(uri);
	}
}

static void
go_forward(void)
{
	if (g.directory_forward) {
		if (g.directory)
			g.directory_back =
				g_list_prepend(g.directory_back, g.directory);

		const gchar *uri = g.directory = g.directory_forward->data;

		GList *link = g.directory_forward;
		g.directory_forward = g_list_remove_link(g.directory_forward, link);
		g_list_free(link);

		load_directory(uri);
	} else if (g.uri) {
		switch_to_view();
	}
}

static void
on_model_files_changed(FivIoModel *model, G_GNUC_UNUSED gpointer user_data)
{
	g_return_if_fail(model == g.model);

	gsize files_len = 0;
	(void) fiv_io_model_get_files(g.model, &files_len);

	update_files_index();

	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_FILE_PREVIOUS], files_len > 1);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_FILE_NEXT], files_len > 1);
}

static void
on_sidebar_toggled(GtkToggleButton *button, G_GNUC_UNUSED gpointer user_data)
{
	gboolean active = gtk_toggle_button_get_active(button);
	gtk_widget_set_visible(g.browser_sidebar, active);
}

static void
on_filtering_toggled(GtkToggleButton *button, G_GNUC_UNUSED gpointer user_data)
{
	gboolean active = gtk_toggle_button_get_active(button);
	g_object_set(g.model, "filtering", active, NULL);
}

static void
on_filenames_toggled(GtkToggleButton *button, G_GNUC_UNUSED gpointer user_data)
{
	gboolean active = gtk_toggle_button_get_active(button);
	g_object_set(g.browser, "show-labels", active, NULL);
}

static void
on_sort_field(G_GNUC_UNUSED GtkToggleButton *button, gpointer data)
{
	gboolean active = gtk_toggle_button_get_active(button);
	if (!active)
		return;

	int old = -1, new = (int) (intptr_t) data;
	g_object_get(g.model, "sort-field", &old, NULL);
	if (old != new)
		g_object_set(g.model, "sort-field", new, NULL);
}

static void
on_sort_direction(G_GNUC_UNUSED GtkToggleButton *button,
	G_GNUC_UNUSED gpointer data)
{
	gboolean old = FALSE;
	g_object_get(g.model, "sort-descending", &old, NULL);
	g_object_set(g.model, "sort-descending", !old, NULL);
}

static void
on_notify_view_messages(FivView *view, G_GNUC_UNUSED GParamSpec *param_spec,
	G_GNUC_UNUSED gpointer user_data)
{
	gchar *messages = NULL;
	g_object_get(view, "messages", &messages, NULL);
	if (messages) {
		gchar *escaped = g_markup_escape_text(messages, -1);
		g_free(messages);
		gchar *message = g_strdup_printf("<b>Error:</b> %s", escaped);
		g_free(escaped);
		gtk_label_set_markup(GTK_LABEL(g.view_info_label), message);
		g_free(message);
		gtk_widget_show(g.view_info);
	} else {
		gtk_widget_hide(g.view_info);
	}
}

static void
open_image(const char *uri)
{
	GFile *file = g_file_new_for_uri(uri);
	if (fiv_view_set_uri(FIV_VIEW(g.view), uri))
		gtk_recent_manager_add_item(gtk_recent_manager_get_default(), uri);

	g_list_free_full(g.directory_forward, g_free);
	g.directory_forward = NULL;
	g_free(g.uri);
	g.uri = g_strdup(uri);

	// So that load_directory() itself can be used for reloading.
	gchar *parent = parent_uri(file);
	g_object_unref(file);
	if (!fiv_io_model_get_location(g.model) || !g.directory ||
		strcmp(parent, g.directory))
		load_directory_without_switching(parent);
	else
		update_files_index();
	g_free(parent);

	// XXX: When something outside currently filtered entries is open,
	// g.files_index is kept at -1, and browsing doesn't work.
	// How to behave here?

	switch_to_view();
}

static GtkWidget *
create_open_dialog(void)
{
	GtkWidget *dialog = gtk_file_chooser_dialog_new("Open file",
		GTK_WINDOW(g.window), GTK_FILE_CHOOSER_ACTION_OPEN,
		"_Cancel", GTK_RESPONSE_CANCEL,
		"_Open", GTK_RESPONSE_ACCEPT, NULL);
	gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), FALSE);
	gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

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
	(void) gtk_file_chooser_set_current_folder_uri(
		GTK_FILE_CHOOSER(dialog), g.directory);

	switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
		GSList *uri_list;
	case GTK_RESPONSE_ACCEPT:
		if (!(uri_list = gtk_file_chooser_get_uris(GTK_FILE_CHOOSER(dialog))))
			break;

		gchar **uris = slist_to_strv(uri_list);
		if (g_strv_length(uris) == 1) {
			open_image(uris[0]);
		} else {
			fiv_collection_reload(uris);
			load_directory(FIV_COLLECTION_SCHEME ":/");
		}
		g_strfreev(uris);
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
	gsize files_len = 0;
	FivIoModelEntry *const *files = fiv_io_model_get_files(g.model, &files_len);
	if (g.files_index >= 0) {
		int previous = (files_len + g.files_index - 1) % files_len;
		open_image(files[previous]->uri);
	}
}

static void
on_next(void)
{
	gsize files_len = 0;
	FivIoModelEntry *const *files = fiv_io_model_get_files(g.model, &files_len);
	if (g.files_index >= 0) {
		int next = (g.files_index + 1) % files_len;
		open_image(files[next]->uri);
	}
}

static gchar **
build_spawn_argv(const char *uri)
{
	// Because we only pass URIs, there is no need to prepend "--" here.
	GPtrArray *a = g_ptr_array_new();
	g_ptr_array_add(a, g_strdup(PROJECT_NAME));

	// Process-local VFS URIs need to be resolved to globally accessible URIs.
	// It doesn't seem possible to reliably tell if a GFile is process-local,
	// but our collection VFS is the only one to realistically cause problems.
	if (!fiv_collection_uri_matches(uri)) {
		g_ptr_array_add(a, g_strdup(uri));
		goto out;
	}

	GFile *file = g_file_new_for_uri(uri);
	GError *error = NULL;
	GFileInfo *info =
		g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
			G_FILE_QUERY_INFO_NONE, NULL, &error);
	g_object_unref(file);
	if (!info) {
		g_warning("%s", error->message);
		g_error_free(error);
		goto out;
	}

	const char *target_uri = g_file_info_get_attribute_string(
		info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
	if (target_uri) {
		g_ptr_array_add(a, g_strdup(target_uri));
	} else {
		gsize len = 0;
		GFile **files = fiv_collection_get_contents(&len);
		for (gsize i = 0; i < len; i++)
			g_ptr_array_add(a, g_file_get_uri(files[i]));
	}
	g_object_unref(info);

out:
	g_ptr_array_add(a, NULL);
	return (gchar **) g_ptr_array_free(a, FALSE);
}

static void
spawn_uri(const char *uri)
{
	gchar **argv = build_spawn_argv(uri);
	GError *error = NULL;
	if (!g_spawn_async(
		NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
		g_warning("%s", error->message);
		g_error_free(error);
	}
	g_strfreev(argv);
}

static void
on_item_activated(G_GNUC_UNUSED FivBrowser *browser, GFile *location,
	GtkPlacesOpenFlags flags, G_GNUC_UNUSED gpointer data)
{
	gchar *uri = g_file_get_uri(location);
	if (flags == GTK_PLACES_OPEN_NEW_WINDOW)
		spawn_uri(uri);
	else
		open_image(uri);
	g_free(uri);
}

static void open_any_file(GFile *file, gboolean force_browser);

static void
on_mounted_enclosing(
	GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GFile *file = G_FILE(source_object);
	GError *error = NULL;
	if (g_file_mount_enclosing_volume_finish(file, res, &error))
		goto retry;

	if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
		show_error_dialog(error);
		return;
	}

	// The "http" scheme doesn't behave nicely, make a leap of faith.
	g_error_free(error);
	if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) ==
		G_FILE_TYPE_UNKNOWN) {
		gchar *uri = g_file_get_uri(file);
		open_image(uri);
		g_free(uri);
		return;
	}

retry:
	open_any_file(file, (gboolean) (gintptr) user_data);
}

static void
open_any_file(GFile *file, gboolean force_browser)
{
	// Various GVfs schemes may need mounting.
	GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);
	if (type == G_FILE_TYPE_UNKNOWN &&
		G_FILE_GET_IFACE(file)->mount_enclosing_volume) {
		// TODO(p): At least provide some kind of indication.
		GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(g.window));
		g_file_mount_enclosing_volume(file, G_MOUNT_MOUNT_NONE, op, NULL,
			on_mounted_enclosing, (gpointer) (gintptr) force_browser);
		g_object_unref(op);
		return;
	}

	gchar *uri = g_file_get_uri(file);
	if (type == G_FILE_TYPE_UNKNOWN) {
		errno = ENOENT;
		show_error_dialog(g_error_new(G_FILE_ERROR,
			g_file_error_from_errno(errno), "%s: %s", uri, g_strerror(errno)));
	} else if (type == G_FILE_TYPE_DIRECTORY) {
		load_directory(uri);
	} else if (force_browser) {
		// GNOME, e.g., invokes this as a hint to focus the particular file.
		gchar *parent = parent_uri(file);
		load_directory(parent);
		g_free(parent);

		fiv_browser_select(FIV_BROWSER(g.browser), uri);
	} else {
		open_image(uri);
	}
	g_free(uri);
}

static void
on_open_location(G_GNUC_UNUSED GtkPlacesSidebar *sidebar, GFile *location,
	GtkPlacesOpenFlags flags, G_GNUC_UNUSED gpointer user_data)
{
	gchar *uri = g_file_get_uri(location);
	if (flags & GTK_PLACES_OPEN_NEW_WINDOW)
		spawn_uri(uri);
	else
		open_any_file(location, FALSE);
	g_free(uri);
}

static void
on_view_drag_data_received(G_GNUC_UNUSED GtkWidget *widget,
	GdkDragContext *context, G_GNUC_UNUSED gint x, G_GNUC_UNUSED gint y,
	GtkSelectionData *data, G_GNUC_UNUSED guint info, guint time,
	G_GNUC_UNUSED gpointer user_data)
{
	gchar **uris = gtk_selection_data_get_uris(data);
	if (!uris) {
		gtk_drag_finish(context, FALSE, FALSE, time);
		return;
	}

	GFile *file = NULL;
	if (g_strv_length(uris) == 1) {
		file = g_file_new_for_uri(uris[0]);
	} else {
		fiv_collection_reload(uris);
		file = g_file_new_for_uri(FIV_COLLECTION_SCHEME ":/");
	}
	open_any_file(file, FALSE);
	g_object_unref(file);
	gtk_drag_finish(context, TRUE, FALSE, time);
	g_strfreev(uris);
}

static void
on_notify_sidebar_visible(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	gboolean b = FALSE;
	g_object_get(object, g_param_spec_get_name(param_spec), &b, NULL);
	gtk_toggle_button_set_active(
		GTK_TOGGLE_BUTTON(g.browsebar[BROWSEBAR_SIDEBAR]), b);
}

static void
on_dir_previous(void)
{
	GFile *directory = fiv_io_model_get_previous_directory(g.model);
	if (directory) {
		gchar *uri = g_file_get_uri(directory);
		g_object_unref(directory);
		load_directory(uri);
		g_free(uri);
	}
}

static void
on_dir_next(void)
{
	GFile *directory = fiv_io_model_get_next_directory(g.model);
	if (directory) {
		gchar *uri = g_file_get_uri(directory);
		g_object_unref(directory);
		load_directory(uri);
		g_free(uri);
	}
}

static void
on_toolbar_zoom(G_GNUC_UNUSED GtkButton *button, gpointer user_data)
{
	FivThumbnailSize size = FIV_THUMBNAIL_SIZE_COUNT;
	g_object_get(g.browser, "thumbnail-size", &size, NULL);

	size += (gintptr) user_data;
	g_return_if_fail(size >= FIV_THUMBNAIL_SIZE_MIN &&
		size <= FIV_THUMBNAIL_SIZE_MAX);

	g_object_set(g.browser, "thumbnail-size", size, NULL);
}

static void
on_notify_thumbnail_size(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	FivThumbnailSize size = 0;
	g_object_get(object, g_param_spec_get_name(param_spec), &size, NULL);
	gtk_widget_set_sensitive(
		g.browsebar[BROWSEBAR_PLUS], size < FIV_THUMBNAIL_SIZE_MAX);
	gtk_widget_set_sensitive(
		g.browsebar[BROWSEBAR_MINUS], size > FIV_THUMBNAIL_SIZE_MIN);
}

static void
on_notify_show_labels(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	gboolean show_labels = 0;
	g_object_get(object, g_param_spec_get_name(param_spec), &show_labels, NULL);
	gtk_toggle_button_set_active(
		GTK_TOGGLE_BUTTON(g.browsebar[BROWSEBAR_FILENAMES]), show_labels);
}

static void
on_notify_filtering(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	gboolean b = FALSE;
	g_object_get(object, g_param_spec_get_name(param_spec), &b, NULL);
	gtk_toggle_button_set_active(
		GTK_TOGGLE_BUTTON(g.browsebar[BROWSEBAR_FILTER]), b);
}

static void
on_notify_sort_field(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	gint field = -1;
	g_object_get(object, g_param_spec_get_name(param_spec), &field, NULL);
	gtk_toggle_button_set_active(
		GTK_TOGGLE_BUTTON(g.browsebar[BROWSEBAR_SORT_NAME + field]), TRUE);
}

static void
on_notify_sort_descending(
	GObject *object, GParamSpec *param_spec, G_GNUC_UNUSED gpointer user_data)
{
	gboolean b = FALSE;
	g_object_get(object, g_param_spec_get_name(param_spec), &b, NULL);

	const char *title = b
		? "Sort ascending"
		: "Sort descending";
	const char *name = b
		? "view-sort-ascending-symbolic"
		: "view-sort-descending-symbolic";

	GtkButton *button = GTK_BUTTON(g.browsebar[BROWSEBAR_SORT_DIR]);
	GtkImage *image = GTK_IMAGE(gtk_button_get_image(button));
	gtk_widget_set_tooltip_text(GTK_WIDGET(button), title);
	gtk_image_set_from_icon_name(image, name, GTK_ICON_SIZE_BUTTON);
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

	gtk_image_set_from_icon_name(
		GTK_IMAGE(gtk_button_get_image(
			GTK_BUTTON(g.toolbar[TOOLBAR_FULLSCREEN]))),
		name, GTK_ICON_SIZE_BUTTON);
	gtk_image_set_from_icon_name(
		GTK_IMAGE(gtk_button_get_image(
			GTK_BUTTON(g.browsebar[BROWSEBAR_FULLSCREEN]))),
		name, GTK_ICON_SIZE_BUTTON);
}

static void
show_help_contents(void)
{
	gchar *filename = g_strdup_printf("%s.html", PROJECT_NAME);
#ifdef G_OS_WIN32
	gchar *prefix = g_win32_get_package_installation_directory_of_module(NULL);
	gchar *path = g_build_filename(prefix, PROJECT_DOCDIR, filename, NULL);
	g_free(prefix);
#else
	gchar *path = g_build_filename(PROJECT_DOCDIR, filename, NULL);
#endif
	g_free(filename);
	GError *error = NULL;
	gchar *uri = g_filename_to_uri(path, NULL, &error);
	g_free(path);

	// For some reason, it doesn't work with a parent window.
	if (!uri || !gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, &error))
		show_error_dialog(error);
	g_free(uri);
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
			? "viewer"
			: "browser",
		NULL);
	gtk_widget_show(window);
}

static void
show_preferences(void)
{
	char *argv[] = {"dconf-editor", PROJECT_NS PROJECT_NAME, NULL};
	GError *error = NULL;
	if (!g_spawn_async(
		NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
		if (g_error_matches(error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
			g_prefix_error_literal(&error,
				"Please install dconf-editor, or use the gsettings utility.\n");
		show_error_dialog(error);
	}
}

static void
toggle_sunlight(void)
{
	GtkSettings *settings = gtk_settings_get_default();
	const char *property = "gtk-application-prefer-dark-theme";
	gboolean set = FALSE;
	g_object_get(settings, property, &set, NULL);
	g_object_set(settings, property, !set, NULL);
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
		if (event->keyval == GDK_KEY_D)
			toggle_sunlight();
		break;
	case GDK_CONTROL_MASK:
	case GDK_CONTROL_MASK | GDK_SHIFT_MASK:
		switch (event->keyval) {
		case GDK_KEY_h:
			gtk_button_clicked(GTK_BUTTON(g.browsebar[BROWSEBAR_FILTER]));
			return TRUE;
		case GDK_KEY_l:
			fiv_sidebar_show_enter_location(FIV_SIDEBAR(g.browser_sidebar));
			return TRUE;
		case GDK_KEY_n:
			if (gtk_stack_get_visible_child(GTK_STACK(g.stack)) == g.view_box)
				spawn_uri(g.uri);
			else
				spawn_uri(g.directory);
			return TRUE;
		case GDK_KEY_o:
			on_open();
			return TRUE;
		case GDK_KEY_q:
		case GDK_KEY_w:
			gtk_widget_destroy(g.window);
			return TRUE;

		case GDK_KEY_question:
			show_help_shortcuts();
			return TRUE;
		case GDK_KEY_comma:
			show_preferences();
			return TRUE;
		}
		break;
	case GDK_MOD1_MASK:
		switch (event->keyval) {
		case GDK_KEY_Left:
			go_back();
			return TRUE;
		case GDK_KEY_Right:
			go_forward();
			return TRUE;
		}
		break;
	case GDK_SHIFT_MASK:
		switch (event->keyval) {
		case GDK_KEY_F1:
			show_about_dialog(g.window);
			return TRUE;
		}
		break;
	case 0:
		switch (event->keyval) {
		case GDK_KEY_q:
			gtk_widget_destroy(g.window);
			return TRUE;
		case GDK_KEY_o:
			on_open();
			return TRUE;
		case GDK_KEY_F1:
			show_help_contents();
			return TRUE;
		case GDK_KEY_F11:
		case GDK_KEY_f:
			toggle_fullscreen();
			return TRUE;
		}
	}

	gchar *accelerator = NULL;
	g_object_get(gtk_widget_get_settings(g.window), "gtk-menu-bar-accel",
		&accelerator, NULL);
	if (!accelerator)
		return FALSE;

	guint key = 0;
	GdkModifierType mods = 0;
	gtk_accelerator_parse(accelerator, &key, &mods);
	g_free(accelerator);

	guint mask = gtk_accelerator_get_default_mod_mask();
	if (key && event->keyval == key && (event->state & mask) == mods) {
		gtk_widget_show(g.menu);

		// _gtk_menu_shell_set_keyboard_mode() is private.
		// We've added a viewable menu bar, so calling this again will work.
		return gtk_window_activate_key(GTK_WINDOW(g.window), event);
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
		case GDK_KEY_F7:
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

		case GDK_KEY_Escape:
		case GDK_KEY_Return:
			switch_to_browser();
			return TRUE;
		}
	}
	return FALSE;
}

static gboolean
on_key_press_browser_paned(G_GNUC_UNUSED GtkWidget *widget, GdkEventKey *event,
	G_GNUC_UNUSED gpointer data)
{
	// TODO(p): Consider replicating more GtkFileChooserWidget bindings.
	switch (event->state & gtk_accelerator_get_default_mod_mask()) {
	case GDK_CONTROL_MASK:
		switch (event->keyval) {
		case GDK_KEY_r:
			load_directory(NULL);
			return TRUE;
		case GDK_KEY_t:
			gtk_button_clicked(GTK_BUTTON(g.browsebar[BROWSEBAR_FILENAMES]));
			return TRUE;
		}
		break;
	case GDK_MOD1_MASK:
		switch (event->keyval) {
		case GDK_KEY_Up: {
			GFile *directory = g_file_new_for_uri(g.directory);
			gchar *parent = parent_uri(directory);
			g_object_unref(directory);
			load_directory(parent);
			g_free(parent);
			return TRUE;
		}
		case GDK_KEY_Home: {
			gchar *uri = g_filename_to_uri(g_get_home_dir(), NULL, NULL);
			load_directory(uri);
			g_free(uri);
			return TRUE;
		}
		}
		break;
	case 0:
		switch (event->keyval) {
		case GDK_KEY_F7:
			gtk_widget_set_visible(g.browser_toolbar,
				!gtk_widget_is_visible(g.browser_toolbar));
			return TRUE;
		case GDK_KEY_F9:
			gtk_widget_set_visible(g.browser_sidebar,
				!gtk_widget_is_visible(g.browser_sidebar));
			return TRUE;

		case GDK_KEY_bracketleft:
			on_dir_previous();
			return TRUE;
		case GDK_KEY_bracketright:
			on_dir_next();
			return TRUE;

		case GDK_KEY_Escape:
			fiv_browser_select(FIV_BROWSER(g.browser), NULL);
			return TRUE;
		case GDK_KEY_h:
			gtk_button_clicked(GTK_BUTTON(g.browsebar[BROWSEBAR_FILTER]));
			return TRUE;
		case GDK_KEY_F5:
		case GDK_KEY_r:
			load_directory(NULL);
			return TRUE;
		case GDK_KEY_t:
			gtk_button_clicked(GTK_BUTTON(g.browsebar[BROWSEBAR_FILENAMES]));
			return TRUE;
		}
	}
	return FALSE;
}

static gboolean
on_button_press_view(G_GNUC_UNUSED GtkWidget *widget, GdkEventButton *event)
{
	if ((event->state & gtk_accelerator_get_default_mod_mask()))
		return FALSE;
	switch (event->button) {
	case 4:  // back (GdkWin32, GdkQuartz)
	case 8:  // back
		go_back();
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
	case 4:  // back (GdkWin32, GdkQuartz)
	case 8:  // back
		go_back();
		return TRUE;
	case 5:  // forward (GdkWin32, GdkQuartz)
	case 9:  // forward
		go_forward();
		return TRUE;
	default:
		return FALSE;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static GtkWidget *
make_toolbar_button(const char *symbolic, const char *tooltip)
{
	GtkWidget *button = gtk_button_new();
	gtk_button_set_image(GTK_BUTTON(button),
		gtk_image_new_from_icon_name(symbolic, GTK_ICON_SIZE_BUTTON));
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_set_focus_on_click(button, FALSE);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(button), GTK_STYLE_CLASS_FLAT);
	return button;
}

static GtkWidget *
make_toolbar_toggle(const char *symbolic, const char *tooltip)
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

static GtkWidget *
make_toolbar_radio(const char *label, const char *tooltip)
{
	GtkWidget *button = gtk_radio_button_new_with_label(NULL, label);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_set_focus_on_click(button, FALSE);
	return button;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
browsebar_connect(int index, GCallback callback)
{
	g_signal_connect_swapped(g.browsebar[index], "clicked", callback, NULL);
}

static GtkWidget *
make_browser_toolbar(void)
{
#define XX(id, constructor) g.browsebar[BROWSEBAR_ ## id] = constructor;
	BROWSEBAR(XX)
#undef XX

	// GtkStatusBar solves a problem we do not have here.
	GtkWidget *browser_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(browser_toolbar), "fiv-toolbar");
	GtkBox *box = GTK_BOX(browser_toolbar);

	// Exploring different versions of awkward layouts.
	for (int i = 0; i <= BROWSEBAR_S2; i++)
		gtk_box_pack_start(box, g.browsebar[i], FALSE, FALSE, 0);
	for (int i = BROWSEBAR_COUNT; --i >= BROWSEBAR_S5; )
		gtk_box_pack_end(box, g.browsebar[i], FALSE, FALSE, 0);

	GtkWidget *center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	for (int i = BROWSEBAR_S2; ++i < BROWSEBAR_S5; )
		gtk_box_pack_start(GTK_BOX(center), g.browsebar[i], FALSE, FALSE, 0);
	gtk_box_set_center_widget(box, center);

	g_signal_connect(g.browsebar[BROWSEBAR_SIDEBAR], "toggled",
		G_CALLBACK(on_sidebar_toggled), NULL);

	browsebar_connect(BROWSEBAR_DIR_PREVIOUS, G_CALLBACK(on_dir_previous));
	browsebar_connect(BROWSEBAR_DIR_NEXT,     G_CALLBACK(on_dir_next));
	browsebar_connect(BROWSEBAR_SORT_DIR,     G_CALLBACK(on_sort_direction));
	browsebar_connect(BROWSEBAR_FULLSCREEN,   G_CALLBACK(toggle_fullscreen));

	g_signal_connect(g.browsebar[BROWSEBAR_PLUS], "clicked",
		G_CALLBACK(on_toolbar_zoom), (gpointer) +1);
	g_signal_connect(g.browsebar[BROWSEBAR_MINUS], "clicked",
		G_CALLBACK(on_toolbar_zoom), (gpointer) -1);

	g_signal_connect(g.browsebar[BROWSEBAR_FILTER], "toggled",
		G_CALLBACK(on_filtering_toggled), NULL);
	g_signal_connect(g.browsebar[BROWSEBAR_FILENAMES], "toggled",
		G_CALLBACK(on_filenames_toggled), NULL);

	GtkRadioButton *last = GTK_RADIO_BUTTON(g.browsebar[BROWSEBAR_SORT_NAME]);
	for (int i = BROWSEBAR_SORT_NAME; i <= BROWSEBAR_SORT_TIME; i++) {
		GtkRadioButton *radio = GTK_RADIO_BUTTON(g.browsebar[i]);
		g_signal_connect(radio, "toggled", G_CALLBACK(on_sort_field),
			(gpointer) (gintptr) i - BROWSEBAR_SORT_NAME);
		gtk_radio_button_join_group(radio, last);
		last = radio;
	}

	return browser_toolbar;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

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

	// Note that none of the following should be visible with no image.
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_MINUS], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_SCALE], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_PLUS], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_ONE], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_FIT], has_image);

	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_COLOR], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_SMOOTH], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_CHECKERBOARD], has_image);
	gtk_widget_set_sensitive(g.toolbar[TOOLBAR_ENHANCE], has_image);

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
	gtk_style_context_add_class(
		gtk_widget_get_style_context(view_toolbar), "fiv-toolbar");
	GtkBox *box = GTK_BOX(view_toolbar);

	// Exploring different versions of awkward layouts.
	for (int i = 0; i <= TOOLBAR_S1; i++)
		gtk_box_pack_start(box, g.toolbar[i], FALSE, FALSE, 0);
	for (int i = TOOLBAR_COUNT; --i >= TOOLBAR_S7; )
		gtk_box_pack_end(box, g.toolbar[i], FALSE, FALSE, 0);

	GtkWidget *center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	for (int i = TOOLBAR_S1; ++i < TOOLBAR_S7; )
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
	toolbar_command(TOOLBAR_MINUS,         FIV_VIEW_COMMAND_ZOOM_OUT);
	toolbar_command(TOOLBAR_PLUS,          FIV_VIEW_COMMAND_ZOOM_IN);
	toolbar_command(TOOLBAR_ONE,           FIV_VIEW_COMMAND_ZOOM_1);
	toolbar_toggler(TOOLBAR_FIT,           "scale-to-fit");
	toolbar_toggler(TOOLBAR_FIXATE,        "fixate");
	toolbar_toggler(TOOLBAR_COLOR,         "enable-cms");
	toolbar_toggler(TOOLBAR_SMOOTH,        "filter");
	toolbar_toggler(TOOLBAR_CHECKERBOARD,  "checkerboard");
	toolbar_toggler(TOOLBAR_ENHANCE,       "enhance");
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
	g_signal_connect(g.view, "notify::fixate",
		G_CALLBACK(on_notify_view_boolean), g.toolbar[TOOLBAR_FIXATE]);
	g_signal_connect(g.view, "notify::enable-cms",
		G_CALLBACK(on_notify_view_boolean), g.toolbar[TOOLBAR_COLOR]);
	g_signal_connect(g.view, "notify::filter",
		G_CALLBACK(on_notify_view_boolean), g.toolbar[TOOLBAR_SMOOTH]);
	g_signal_connect(g.view, "notify::checkerboard",
		G_CALLBACK(on_notify_view_boolean), g.toolbar[TOOLBAR_CHECKERBOARD]);
	g_signal_connect(g.view, "notify::enhance",
		G_CALLBACK(on_notify_view_boolean), g.toolbar[TOOLBAR_ENHANCE]);

	g_object_notify(G_OBJECT(g.view), "scale");
	g_object_notify(G_OBJECT(g.view), "playing");
	g_object_notify(G_OBJECT(g.view), "scale-to-fit");
	g_object_notify(G_OBJECT(g.view), "fixate");
	g_object_notify(G_OBJECT(g.view), "enable-cms");
	g_object_notify(G_OBJECT(g.view), "filter");
	g_object_notify(G_OBJECT(g.view), "checkerboard");
	g_object_notify(G_OBJECT(g.view), "enhance");

#ifndef HAVE_LCMS2
	gtk_widget_set_no_show_all(g.toolbar[TOOLBAR_COLOR], TRUE);
#endif
#ifndef HAVE_JPEG_QS
	gtk_widget_set_no_show_all(g.toolbar[TOOLBAR_ENHANCE], TRUE);
#endif

	GCallback callback = G_CALLBACK(on_view_actions_changed);
	g_signal_connect(g.view, "notify::has-image", callback, NULL);
	g_signal_connect(g.view, "notify::can-animate", callback, NULL);
	g_signal_connect(g.view, "notify::has-previous-page", callback, NULL);
	g_signal_connect(g.view, "notify::has-next-page", callback, NULL);
	callback();
	return view_toolbar;
}

static GtkWidget *
make_browser_sidebar(FivIoModel *model)
{
	GtkWidget *sidebar = fiv_sidebar_new(model);
	g_signal_connect(sidebar, "open-location",
		G_CALLBACK(on_open_location), NULL);

	g_signal_connect(sidebar, "notify::visible",
		G_CALLBACK(on_notify_sidebar_visible), NULL);

	g_object_notify(G_OBJECT(sidebar), "visible");

	g_signal_connect(g.browser, "notify::thumbnail-size",
		G_CALLBACK(on_notify_thumbnail_size), NULL);
	g_signal_connect(g.browser, "notify::show-labels",
		G_CALLBACK(on_notify_show_labels), NULL);
	g_signal_connect(model, "notify::filtering",
		G_CALLBACK(on_notify_filtering), NULL);
	g_signal_connect(model, "notify::sort-field",
		G_CALLBACK(on_notify_sort_field), NULL);
	g_signal_connect(model, "notify::sort-descending",
		G_CALLBACK(on_notify_sort_descending), NULL);

	on_toolbar_zoom(NULL, (gpointer) 0);

	g_object_notify(G_OBJECT(g.model), "filtering");
	g_object_notify(G_OBJECT(g.model), "sort-field");
	g_object_notify(G_OBJECT(g.model), "sort-descending");
	return sidebar;
}

static GtkWidget *
make_menu_bar(void)
{
	g.menu = gtk_menu_bar_new();

	GtkWidget *item_quit = gtk_menu_item_new_with_mnemonic("_Quit");
	g_signal_connect_swapped(item_quit, "activate",
		G_CALLBACK(gtk_widget_destroy), g.window);

	GtkWidget *menu_file = gtk_menu_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_file), item_quit);
	GtkWidget *item_file = gtk_menu_item_new_with_mnemonic("_File");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_file), menu_file);
	gtk_menu_shell_append(GTK_MENU_SHELL(g.menu), item_file);

	GtkWidget *item_contents = gtk_menu_item_new_with_mnemonic("_Contents");
	g_signal_connect_swapped(item_contents, "activate",
		G_CALLBACK(show_help_contents), NULL);
	GtkWidget *item_shortcuts =
		gtk_menu_item_new_with_mnemonic("_Keyboard Shortcuts");
	g_signal_connect_swapped(item_shortcuts, "activate",
		G_CALLBACK(show_help_shortcuts), NULL);
	GtkWidget *item_about = gtk_menu_item_new_with_mnemonic("_About");
	g_signal_connect_swapped(item_about, "activate",
		G_CALLBACK(show_about_dialog), g.window);

	GtkWidget *menu_help = gtk_menu_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_help), item_contents);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_help), item_shortcuts);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_help), item_about);
	GtkWidget *item_help = gtk_menu_item_new_with_mnemonic("_Help");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_help), menu_help);
	gtk_menu_shell_append(GTK_MENU_SHELL(g.menu), item_help);

	// Don't let it take up space by default. Firefox sets a precedent here.
	gtk_widget_show_all(g.menu);
	gtk_widget_set_no_show_all(g.menu, TRUE);
	gtk_widget_hide(g.menu);
	g_signal_connect(g.menu, "deactivate", G_CALLBACK(gtk_widget_hide), NULL);
	return g.menu;
}

// This is incredibly broken https://stackoverflow.com/a/51054396/76313
// thus resolving the problem using overlaps.
// We're trying to be universal for light and dark themes both. It's hard.
static const char stylesheet[] = "@define-color fiv-tile @content_view_bg; \
	@define-color fiv-semiselected \
		mix(@theme_selected_bg_color, @content_view_bg, 0.5); \
	fiv-view, fiv-browser { background: @content_view_bg; } \
	placessidebar.fiv box > separator { margin: 4px 0; } \
	.fiv-toolbar button { padding-left: 0; padding-right: 0; } \
	.fiv-toolbar > button:first-child { padding-left: 4px; } \
	.fiv-toolbar > button:last-child { padding-right: 4px; } \
	.fiv-toolbar separator { \
		background: mix(@insensitive_fg_color, \
			@insensitive_bg_color, 0.4); margin: 6px 8px; \
	} \
	fiv-browser { padding: 5px; } \
	fiv-browser.item { \
		/* For non-symbolic, color is applied to the glowing margin. */ \
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
	fiv-browser.item.label, fiv-browser.item.symbolic.label { \
		color: @theme_fg_color; \
	} \
	fiv-browser.item.label:backdrop:not(:selected) { \
		color: @theme_unfocused_fg_color; \
	} \
	fiv-browser.item:selected { \
		color: @theme_selected_bg_color; \
		border-color: @theme_selected_bg_color; \
	} \
	fiv-browser.item:selected:not(:focus) { \
		color: @fiv-semiselected; \
		border-color: @fiv-semiselected; \
	} \
	fiv-browser.item:backdrop:not(:selected) { \
		color: mix(#000, @content_view_bg, 0.875); \
		border-color: mix(#fff, @content_view_bg, 0.5); \
	} \
	fiv-browser.item.symbolic, \
	fiv-browser.item.symbolic:selected, \
	fiv-browser.item.symbolic:backdrop { \
		color: shade(@theme_bg_color, 0.875); \
		border-color: transparent; \
	} \
	fiv-browser.item.symbolic { \
		background-blend-mode: color; \
		background: @theme_bg_color; background-image: none; \
	} \
	fiv-browser.item.symbolic:selected { \
		color: @theme_selected_bg_color; background-image: linear-gradient(0, \
			@theme_selected_bg_color, @theme_selected_bg_color); \
	} \
	fiv-browser.item.symbolic:selected:not(:focus) { \
		color: @fiv-semiselected; background-image: linear-gradient(0, \
			@fiv-semiselected, @fiv-semiselected); \
	} \
	.fiv-information label { padding: 0 4px; }";

static FivThumbnailSize
output_thumbnail_prologue(gchar **uris, const char *size_arg)
{
	if (!uris)
		exit_fatal("No path given");
	if (uris[1])
		exit_fatal("Only one thumbnail at a time may be produced");

	FivThumbnailSize size = FIV_THUMBNAIL_SIZE_COUNT;
	if (size_arg) {
		for (size = 0; size < FIV_THUMBNAIL_SIZE_COUNT; size++) {
			if (!strcmp(
					fiv_thumbnail_sizes[size].thumbnail_spec_name, size_arg))
				break;
		}
		if (size >= FIV_THUMBNAIL_SIZE_COUNT)
			exit_fatal("unknown thumbnail size: %s", size_arg);
	}

#ifdef G_OS_WIN32
	_setmode(fileno(stdout), _O_BINARY);
#endif
	return size;
}

static void
output_thumbnail_for_search(gchar **uris, const char *size_arg)
{
	FivThumbnailSize size = output_thumbnail_prologue(uris, size_arg);

	GError *error = NULL;
	GFile *file = g_file_new_for_uri(uris[0]);
	cairo_surface_t *surface = NULL;
	GBytes *bytes = NULL;
	if ((surface = fiv_thumbnail_produce(file, size, &error)) &&
		(bytes = fiv_io_serialize_for_search(surface, &error))) {
		fwrite(
			g_bytes_get_data(bytes, NULL), 1, g_bytes_get_size(bytes), stdout);
		g_bytes_unref(bytes);
	} else {
		g_assert(error != NULL);
	}

	g_object_unref(file);
	if (error)
		exit_fatal("%s", error->message);

	cairo_surface_destroy(surface);
}

static void
output_thumbnail(gchar **uris, gboolean extract, const char *size_arg)
{
	FivThumbnailSize size = output_thumbnail_prologue(uris, size_arg);

	GError *error = NULL;
	GFile *file = g_file_new_for_uri(uris[0]);
	cairo_surface_t *surface = NULL;
	if (extract && (surface = fiv_thumbnail_extract(file, size, &error)))
		fiv_io_serialize_to_stdout(surface, FIV_IO_SERIALIZE_LOW_QUALITY);
	else if (size_arg &&
		(g_clear_error(&error),
			(surface = fiv_thumbnail_produce(file, size, &error))))
		fiv_io_serialize_to_stdout(surface, 0);
	else
		g_assert(error != NULL);

	g_object_unref(file);
	if (error)
		exit_fatal("%s", error->message);

	cairo_surface_destroy(surface);
}

int
main(int argc, char *argv[])
{
	gboolean show_version = FALSE, show_supported_media_types = FALSE,
		invalidate_cache = FALSE, browse = FALSE, extract_thumbnail = FALSE;
	gchar **args = NULL, *thumbnail_size = NULL, *thumbnail_size_search = NULL;
	const GOptionEntry options[] = {
		{G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &args,
			NULL, "[PATH | URI]..."},
		{"browse", 0, G_OPTION_FLAG_IN_MAIN,
			G_OPTION_ARG_NONE, &browse,
			"Start in filesystem browsing mode", NULL},
		{"invalidate-cache", 0, G_OPTION_FLAG_IN_MAIN,
			G_OPTION_ARG_NONE, &invalidate_cache,
			"Invalidate the wide thumbnail cache", NULL},
		{"list-supported-media-types", 0, G_OPTION_FLAG_IN_MAIN,
			G_OPTION_ARG_NONE, &show_supported_media_types,
			"Output supported media types and exit", NULL},
		{"version", 'V', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE,
			&show_version, "Output version information and exit", NULL},
		{},
	};
	const GOptionEntry options_internal[] = {
		{"extract-thumbnail", 0, 0,
			G_OPTION_ARG_NONE, &extract_thumbnail,
			"Output any embedded thumbnail (superseding --thumbnail)", NULL},
		{"thumbnail", 0, 0,
			G_OPTION_ARG_STRING, &thumbnail_size,
			"Generate thumbnails, up to SIZE, and output that size", "SIZE"},
		{"thumbnail-for-search", 0, 0,
			G_OPTION_ARG_STRING, &thumbnail_size_search,
			"Output an image file suitable for searching by content", "SIZE"},
		{},
	};

	GOptionContext *context =
		g_option_context_new(" - Image browser and viewer");
	g_option_context_add_group(context, gtk_get_option_group(TRUE));
	g_option_context_add_main_entries(context, options, NULL);

	GOptionGroup *internals = g_option_group_new(
		"internal", "Internal Options:", "Show internal options", NULL, NULL);
	g_option_group_add_entries(internals, options_internal);
	g_option_context_add_group(context, internals);

	GError *error = NULL;
	gboolean initialized =
		g_option_context_parse(context, &argc, &argv, &error);
	g_option_context_free(context);
	if (show_version) {
		const char *version = PROJECT_VERSION;
		printf("%s %s\n", PROJECT_NAME, &version[*version == 'v']);
		return 0;
	}
	if (show_supported_media_types) {
		char **types = fiv_io_all_supported_media_types();
		for (char **p = types; *p; p++)
			g_print("%s\n", *p);
		g_strfreev(types);
		return 0;
	}
	if (invalidate_cache) {
		fiv_thumbnail_invalidate();
		return 0;
	}
	if (!initialized)
		exit_fatal("%s", error->message);

	// Normalize all arguments to URIs.
	for (gsize i = 0; args && args[i]; i++) {
		GFile *resolved = g_file_new_for_commandline_arg(args[i]);
		g_free(args[i]);
		args[i] = g_file_get_uri(resolved);
		g_object_unref(resolved);
	}
	if (thumbnail_size_search) {
		output_thumbnail_for_search(args, thumbnail_size_search);
		return 0;
	}
	if (extract_thumbnail || thumbnail_size) {
		output_thumbnail(args, extract_thumbnail, thumbnail_size);
		return 0;
	}

	// It doesn't make much sense to have command line arguments able to
	// resolve to the VFS they may end up being contained within.
	fiv_collection_register();

	g.model = g_object_new(FIV_TYPE_IO_MODEL, NULL);
	g_signal_connect(g.model, "files-changed",
		G_CALLBACK(on_model_files_changed), NULL);

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
	gtk_drag_dest_set(g.view, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
	gtk_drag_dest_add_uri_targets(g.view);
	g_signal_connect(g.view, "key-press-event",
		G_CALLBACK(on_key_press_view), NULL);
	g_signal_connect(g.view, "button-press-event",
		G_CALLBACK(on_button_press_view), NULL);
	g_signal_connect(g.view, "drag-data-received",
		G_CALLBACK(on_view_drag_data_received), NULL);
	gtk_container_add(GTK_CONTAINER(view_scroller), g.view);

	// We need to hide it together with its separator.
	g.view_toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(g.view_toolbar),
		make_view_toolbar(), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(g.view_toolbar),
		gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

	g.view_info = gtk_info_bar_new();
	// The button cannot be made flat or reflect the message type under Adwaita.
	gtk_info_bar_set_show_close_button(GTK_INFO_BAR(g.view_info), TRUE);
	// Do not use gtk_info_bar_set_revealed(), as it animates.
	gtk_info_bar_set_message_type(GTK_INFO_BAR(g.view_info), GTK_MESSAGE_ERROR);
	g_signal_connect(g.view_info, "response",
		G_CALLBACK(gtk_widget_hide), NULL);

	g.view_info_label = gtk_label_new(NULL);
	gtk_label_set_line_wrap(GTK_LABEL(g.view_info_label), TRUE);
	gtk_container_add(
		GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(g.view_info))),
		g.view_info_label);
	g_signal_connect(g.view, "notify::messages",
		G_CALLBACK(on_notify_view_messages), NULL);
	gtk_widget_show_all(g.view_info);
	gtk_widget_set_no_show_all(g.view_info, TRUE);
	gtk_widget_hide(g.view_info);

	// Need to put the toolbar at the top, because of the horizontal scrollbar.
	g.view_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(g.view_box), g.view_toolbar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(g.view_box), g.view_info, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(g.view_box), view_scroller, TRUE, TRUE, 0);

	g.browser_scroller = gtk_scrolled_window_new(NULL, NULL);
	g.browser = fiv_browser_new(g.model);
	gtk_widget_set_vexpand(g.browser, TRUE);
	gtk_widget_set_hexpand(g.browser, TRUE);
	g_signal_connect(g.browser, "item-activated",
		G_CALLBACK(on_item_activated), NULL);
	gtk_container_add(GTK_CONTAINER(g.browser_scroller), g.browser);

	// We need to hide it together with its separator.
	g.browser_toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(g.browser_toolbar),
		make_browser_toolbar(), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(g.browser_toolbar),
		gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

	GtkWidget *browser_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(browser_right),
		g.browser_toolbar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(browser_right),
		g.browser_scroller, TRUE, TRUE, 0);

	g.browser_sidebar = make_browser_sidebar(g.model);
	g.browser_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_add1(GTK_PANED(g.browser_paned), g.browser_sidebar);
	gtk_paned_add2(GTK_PANED(g.browser_paned), browser_right);
	g_signal_connect(g.browser_paned, "key-press-event",
		G_CALLBACK(on_key_press_browser_paned), NULL);
	g_signal_connect(g.browser_paned, "button-press-event",
		G_CALLBACK(on_button_press_browser_paned), NULL);

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

	GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(menu_box), make_menu_bar());
	gtk_container_add(GTK_CONTAINER(menu_box), g.stack);
	gtk_container_add(GTK_CONTAINER(g.window), menu_box);

	GSettings *settings = g_settings_new(PROJECT_NS PROJECT_NAME);
	if (g_settings_get_boolean(settings, "dark-theme"))
		toggle_sunlight();
	g_object_set(g.browser, "thumbnail-size",
		g_settings_get_enum(settings, "thumbnail-size"), NULL);

	gtk_widget_show_all(menu_box);
	gtk_widget_set_visible(g.browser_sidebar,
		g_settings_get_boolean(settings, "show-browser-sidebar"));
	gtk_widget_set_visible(g.browser_toolbar,
		g_settings_get_boolean(settings, "show-browser-toolbar"));
	gtk_widget_set_visible(g.view_toolbar,
		g_settings_get_boolean(settings, "show-view-toolbar"));

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

#ifdef GDK_WINDOWING_QUARTZ
	// Otherwise the window simply opens at (0, 0),
	// while other macOS applications are more likely to start centered.
	if (GDK_IS_QUARTZ_DISPLAY(display))
		gtk_window_set_position(GTK_WINDOW(g.window), GTK_WIN_POS_CENTER);
#endif  // GDK_WINDOWING_QUARTZ

	// XXX: The widget wants to read the display's profile. The realize is ugly.
	gtk_widget_realize(g.view);

	// XXX: We follow the behaviour of Firefox and Eye of GNOME, which both
	// interpret multiple command line arguments differently, as a collection.
	// However, single-element collections are unrepresentable this way.
	// Should we allow multiple targets only in a special new mode?
	g.files_index = -1;
	if (args) {
		const gchar *target = *args;
		if (args[1]) {
			fiv_collection_reload(args);
			target = FIV_COLLECTION_SCHEME ":/";
		}

		GFile *file = g_file_new_for_uri(target);
		open_any_file(file, browse);
		g_object_unref(file);
		g_strfreev(args);
	}
	if (!g.directory) {
		GFile *file = g_file_new_for_path(".");
		open_any_file(file, FALSE);
		g_object_unref(file);
	}

	gtk_widget_show(g.window);
	gtk_main();
	return 0;
}

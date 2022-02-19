//
// fiv-jpegcrop.c: lossless JPEG cropper
//
// Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
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
#include <turbojpeg.h>

#include "config.h"

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
	GFile *location;
	gchar *data;
	gsize len;
	int width, height, subsampling, colorspace;
	int mcu_width, mcu_height;
	cairo_surface_t *surface;

	int top, left, right, bottom;

	GtkWidget *label;
	GtkWidget *window;
	GtkWidget *scrolled;
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

static gboolean
on_draw(G_GNUC_UNUSED GtkWidget *widget, cairo_t *cr,
	G_GNUC_UNUSED gpointer user_data)
{
	cairo_set_source_surface(cr, g.surface, 1, 1);
	cairo_paint(cr);

	cairo_rectangle(cr,
		1 + g.left - 0.5,
		1 + g.top - 0.5,
		g.right - g.left + 1,
		g.bottom - g.top + 1);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 1);
	cairo_set_operator(cr, CAIRO_OPERATOR_DIFFERENCE);
	cairo_stroke(cr);

	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	cairo_rectangle(cr, 1, 1, g.width, g.height);
	cairo_rectangle(
		cr, g.left, g.top, g.right - g.left + 2, g.bottom - g.top + 2);
	cairo_clip(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_paint(cr);
	return TRUE;
}

static GFile *
choose_filename(void)
{
	GtkWidget *dialog = gtk_file_chooser_dialog_new("Saved cropped image as",
		GTK_WINDOW(g.window), GTK_FILE_CHOOSER_ACTION_SAVE,
		"_Cancel", GTK_RESPONSE_CANCEL,
		"_Save", GTK_RESPONSE_ACCEPT, NULL);
	GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
	gtk_file_chooser_set_local_only(chooser, FALSE);
	gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
	(void) gtk_file_chooser_set_file(chooser, g.location, NULL);

	GtkFileFilter *jpeg = gtk_file_filter_new();
	gtk_file_filter_add_mime_type(jpeg, "image/jpeg");
	gtk_file_filter_add_pattern(jpeg, "*.jpg");
	gtk_file_filter_add_pattern(jpeg, "*.jpeg");
	gtk_file_filter_add_pattern(jpeg, "*.jpe");
	gtk_file_filter_set_name(jpeg, "JPEG");
	gtk_file_chooser_add_filter(chooser, jpeg);

	GtkFileFilter *all = gtk_file_filter_new();
	gtk_file_filter_add_pattern(all, "*");
	gtk_file_filter_set_name(all, "All files");
	gtk_file_chooser_add_filter(chooser, all);

	GFile *file = NULL;
	switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
	default:
		gtk_widget_destroy(dialog);
		// Fall-through.
	case GTK_RESPONSE_NONE:
		return file;
	case GTK_RESPONSE_ACCEPT:
		file = gtk_file_chooser_get_file(chooser);
		gtk_widget_destroy(dialog);
		return file;
	}
}

static void
on_save_as(G_GNUC_UNUSED GtkButton *button, G_GNUC_UNUSED gpointer user_data)
{
	tjhandle h = tjInitTransform();
	if (!h) {
		show_error_dialog(g_error_new_literal(
			G_IO_ERROR, G_IO_ERROR_FAILED, tjGetErrorStr2(h)));
		return;
	}

	// Convert up front, because the target is in memory.
	tjtransform t = {
		.r.x = g.left,
		.r.y = g.top,
		.r.w = g.right - g.left,
		.r.h = g.bottom - g.top,
		.op = TJXOP_NONE,
		.options = TJXOPT_CROP | TJXOPT_PROGRESSIVE | TJXOPT_PERFECT,
	};

	guchar *data = NULL;
	gulong len = 0;
	if (tjTransform(h, (const guchar *) g.data, g.len, 1, &data, &len, &t, 0)) {
		show_error_dialog(g_error_new_literal(
			G_IO_ERROR, G_IO_ERROR_FAILED, tjGetErrorStr2(h)));
		goto out;
	}

	GFile *file = choose_filename();
	GError *error = NULL;
	if (file &&
		!g_file_replace_contents(file, (const char *) data, len, NULL, FALSE,
			G_FILE_CREATE_NONE, NULL, NULL, &error)) {
		show_error_dialog(error);
		goto out;
	}

	g_clear_object(&file);
	tjFree(data);
out:
	if (tjDestroy(h)) {
		show_error_dialog(g_error_new_literal(
			G_IO_ERROR, G_IO_ERROR_FAILED, tjGetErrorStr2(h)));
	}
}

static void
update_label(void)
{
	gchar *text = g_strdup_printf("(%d, %d) × (%d, %d)", g.left, g.top,
		g.right - g.left, g.bottom - g.top);
	gtk_label_set_label(GTK_LABEL(g.label), text);
	g_free(text);
}

static void
update(void)
{
	update_label();
	gtk_widget_queue_draw(g.view);
}

static void
on_reset(G_GNUC_UNUSED GtkButton *button, G_GNUC_UNUSED gpointer user_data)
{
	g.top = 0;
	g.left = 0;
	g.right = g.width;
	g.bottom = g.height;
	update();
}

static gboolean
on_mouse(guint state, guint button, gdouble x, gdouble y)
{
	if (state != 0)
		return FALSE;

	switch (button) {
	case GDK_BUTTON_PRIMARY:
		g.left = CLAMP((int) (x - 1), 0, g.right) / g.mcu_width * g.mcu_width;
		g.top = CLAMP((int) (y - 1), 0, g.bottom) / g.mcu_height * g.mcu_height;
		update();
		return TRUE;
	case GDK_BUTTON_SECONDARY:
		// Inclusive of pointer position.
		g.right = CLAMP(x, g.left, g.width);
		g.bottom = CLAMP(y, g.top, g.height);
		update();
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean
on_press(G_GNUC_UNUSED GtkWidget *self, GdkEventButton *event,
	G_GNUC_UNUSED gpointer user_data)
{
	return on_mouse(event->state, event->button, event->x, event->y);
}

static gboolean
on_motion(G_GNUC_UNUSED GtkWidget *self, GdkEventMotion *event,
	G_GNUC_UNUSED gpointer user_data)
{
	switch (event->state) {
	case GDK_BUTTON1_MASK:
		return on_mouse(0, GDK_BUTTON_PRIMARY, event->x, event->y);
	case GDK_BUTTON3_MASK:
		return on_mouse(0, GDK_BUTTON_SECONDARY, event->x, event->y);
	}
	return FALSE;
}

static gboolean
open_jpeg(const gchar *data, gsize len, GError **error)
{
	tjhandle h = tjInitDecompress();
	if (!h) {
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_FAILED, tjGetErrorStr2(h));
		return FALSE;
	}

	if (tjDecompressHeader3(h, (const guint8 *) data, len, &g.width, &g.height,
			&g.subsampling, &g.colorspace)) {
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_FAILED, tjGetErrorStr2(h));
		tjDestroy(h);
		return FALSE;
	}

	g.top = 0;
	g.left = 0;
	g.right = g.width;
	g.bottom = g.height;

	g.mcu_width = tjMCUWidth[g.subsampling];
	g.mcu_height = tjMCUHeight[g.subsampling];

	if (tjDestroy(h)) {
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_FAILED, tjGetErrorStr2(h));
		return FALSE;
	}

	// TODO(p): Eventually, convert to using fiv-io.c directly,
	// which will pull in most of fiv's dependencies,
	// but also enable correct color management, even for CMYK.
	// NOTE: It's possible to include this as a mode of the main binary.
	GInputStream *is = g_memory_input_stream_new_from_data(data, len, NULL);
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(is, NULL, error);
	g_object_unref(is);
	if (!pixbuf)
		return FALSE;

	const char *orientation = gdk_pixbuf_get_option(pixbuf, "orientation");
	if (orientation && strlen(orientation) == 1) {
		int n = *orientation - '0';
		if (n >= 1 && n <= 8) {
			// TODO(p): Apply this to the view, somehow.
		}
	}

	g.surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, NULL);
	cairo_status_t surface_status = cairo_surface_status(g.surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			cairo_status_to_string(surface_status));
		g_clear_pointer(&g.surface, cairo_surface_destroy);
		g_object_unref(pixbuf);
		return FALSE;
	}
	return TRUE;
}

int
main(int argc, char *argv[])
{
	gboolean show_version = FALSE;
	gchar **path_args = NULL;

	const GOptionEntry options[] = {
		{G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &path_args,
			NULL, "[FILE | URI]"},
		{"version", 'V', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE,
			&show_version, "Output version information and exit", NULL},
		{},
	};

	GError *error = NULL;
	gboolean initialized = gtk_init_with_args(
		&argc, &argv, " - Lossless JPEG cropper", options, NULL, &error);
	if (show_version) {
		printf("fiv-jpegcrop " PROJECT_VERSION "\n");
		return 0;
	}
	if (!initialized)
		exit_fatal("%s", error->message);

	// TODO(p): Rather use G_OPTION_ARG_CALLBACK with G_OPTION_FLAG_FILENAME.
	// Alternatively, GOptionContext with gtk_get_option_group(TRUE).
	// Then we can show the help string here instead (in fiv as well).
	if (!path_args || !path_args[0] || path_args[1])
		exit_fatal("invalid arguments");

	gtk_window_set_default_icon_name(PROJECT_NAME);

	g.location = g_file_new_for_commandline_arg(path_args[0]);
	g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(g.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	GFileInfo *info = g_file_query_info(g.location,
		G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
		G_FILE_QUERY_INFO_NONE, NULL, &error);
	if (!info ||
		!g_file_load_contents(
			g.location, NULL, &g.data, &g.len, NULL, &error) ||
		!open_jpeg(g.data, g.len, &error)) {
		show_error_dialog(error);
		exit(EXIT_FAILURE);
	}

	GtkWidget *header = gtk_header_bar_new();
	gtk_window_set_titlebar(GTK_WINDOW(g.window), header);
	gtk_header_bar_set_title(
		GTK_HEADER_BAR(header), g_file_info_get_display_name(info));
	gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header),
		"Use L/R mouse buttons to adjust the crop region.");
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);

	g.label = gtk_label_new(NULL);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), g.label);
	update_label();

	GtkWidget *save = gtk_button_new_from_icon_name(
		"document-save-as-symbolic", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(save, "Save as...");
	g_signal_connect(save, "clicked", G_CALLBACK(on_save_as), NULL);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), save);

	GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset");
	gtk_widget_set_tooltip_text(reset, "Reset the crop region");
	g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), NULL);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), reset);

	g.view = gtk_drawing_area_new();
	gtk_widget_set_size_request(g.view, g.width + 2, g.height + 2);
	gtk_widget_add_events(
		g.view, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
	g_signal_connect(g.view, "draw", G_CALLBACK(on_draw), NULL);
	g_signal_connect(g.view, "button-press-event",
		G_CALLBACK(on_press), NULL);
	g_signal_connect(g.view, "motion-notify-event",
		G_CALLBACK(on_motion), NULL);

	// TODO(p): Track middle mouse button drags, adjust the adjustments.
	g.scrolled = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_overlay_scrolling(
		GTK_SCROLLED_WINDOW(g.scrolled), FALSE);
	gtk_scrolled_window_set_propagate_natural_width(
		GTK_SCROLLED_WINDOW(g.scrolled), TRUE);
	gtk_scrolled_window_set_propagate_natural_height(
		GTK_SCROLLED_WINDOW(g.scrolled), TRUE);

	gtk_container_add(GTK_CONTAINER(g.scrolled), g.view);
	gtk_container_add(GTK_CONTAINER(g.window), g.scrolled);
	gtk_window_set_default_size(GTK_WINDOW(g.window), 800, 600);
	gtk_widget_show_all(g.window);

	// It probably needs to be realized.
	GdkWindow *window = gtk_widget_get_window(g.view);
	GdkCursor *cursor =
		gdk_cursor_new_from_name(gdk_window_get_display(window), "crosshair");
	gdk_window_set_cursor(window, cursor);
	g_object_unref(cursor);

	gtk_main();

	g_free(g.data);
	g_object_unref(g.location);
	g_object_unref(info);
	return 0;
}

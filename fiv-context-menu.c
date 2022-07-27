//
// fiv-context-menu.c: popup menu
//
// Copyright (c) 2021 - 2022, Přemysl Eric Janouch <p@janouch.name>
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

#include "config.h"

#include "fiv-collection.h"
#include "fiv-context-menu.h"

G_DEFINE_QUARK(fiv-context-menu-cancellable-quark, fiv_context_menu_cancellable)

static GtkWidget *
info_start_group(GtkWidget *vbox, const char *group)
{
	GtkWidget *label = gtk_label_new(group);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_widget_set_halign(label, GTK_ALIGN_FILL);
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
	gtk_label_set_attributes(GTK_LABEL(label), attrs);
	pango_attr_list_unref(attrs);

	GtkWidget *grid = gtk_grid_new();
	GtkWidget *expander = gtk_expander_new(NULL);
	gtk_expander_set_label_widget(GTK_EXPANDER(expander), label);
	gtk_expander_set_expanded(GTK_EXPANDER(expander), TRUE);
	gtk_container_add(GTK_CONTAINER(expander), grid);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_box_pack_start(GTK_BOX(vbox), expander, FALSE, FALSE, 0);
	return grid;
}

static GtkWidget *
info_parse(char *tsv)
{
	GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

	const char *last_group = NULL;
	GtkWidget *grid = NULL;
	int line = 1, row = 0;
	for (char *nl; (nl = strchr(tsv, '\n')); line++, tsv = ++nl) {
		*nl = 0;
		if (nl > tsv && nl[-1] == '\r')
			nl[-1] = 0;

		char *group = tsv, *tag = strchr(group, '\t');
		if (!tag) {
			g_warning("ExifTool parse error on line %d", line);
			continue;
		}

		*tag++ = 0;
		for (char *p = group; *p; p++)
			if (*p == '_')
				*p = ' ';

		char *value = strchr(tag, '\t');
		if (!value) {
			g_warning("ExifTool parse error on line %d", line);
			continue;
		}

		*value++ = 0;
		if (!last_group || strcmp(last_group, group)) {
			grid = info_start_group(vbox, (last_group = group));
			row = 0;
		}

		GtkWidget *a = gtk_label_new(tag);
		gtk_size_group_add_widget(sg, a);
		gtk_label_set_selectable(GTK_LABEL(a), TRUE);
		gtk_label_set_xalign(GTK_LABEL(a), 0.);
		gtk_grid_attach(GTK_GRID(grid), a, 0, row, 1, 1);

		GtkWidget *b = gtk_label_new(value);
		gtk_label_set_selectable(GTK_LABEL(b), TRUE);
		gtk_label_set_xalign(GTK_LABEL(b), 0.);
		gtk_label_set_line_wrap(GTK_LABEL(b), TRUE);
		gtk_widget_set_hexpand(b, TRUE);
		gtk_grid_attach(GTK_GRID(grid), b, 1, row, 1, 1);
		row++;
	}
	g_object_unref(sg);
	return vbox;
}

static GtkWidget *
info_make_bar(const char *message)
{
	GtkWidget *info = gtk_info_bar_new();
	gtk_info_bar_set_message_type(GTK_INFO_BAR(info), GTK_MESSAGE_WARNING);
	GtkWidget *info_area = gtk_info_bar_get_content_area(GTK_INFO_BAR(info));
	// When the label is made selectable, Escape doesn't work when it has focus.
	gtk_container_add(GTK_CONTAINER(info_area), gtk_label_new(message));
	return info;
}

static void
info_redirect_error(gpointer dialog, GError *error)
{
	// The dialog has been closed and destroyed.
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_error_free(error);
		return;
	}

	GtkContainer *content_area =
		GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
	gtk_container_foreach(content_area, (GtkCallback) gtk_widget_destroy, NULL);
	gtk_container_add(content_area, info_make_bar(error->message));
	if (g_error_matches(error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT)) {
		gtk_box_pack_start(GTK_BOX(content_area),
			gtk_label_new("Please install ExifTool."), TRUE, FALSE, 12);
	}

	g_error_free(error);
	gtk_widget_show_all(GTK_WIDGET(dialog));
}

static gchar *
bytes_to_utf8(GBytes *bytes)
{
	gsize length = 0;
	gconstpointer data = g_bytes_get_data(bytes, &length);
	gchar *utf8 = data ? g_utf8_make_valid(data, length) : g_strdup("");
	g_bytes_unref(bytes);
	return utf8;
}

static void
on_info_finished(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GError *error = NULL;
	GBytes *bytes_out = NULL, *bytes_err = NULL;
	if (!g_subprocess_communicate_finish(
			G_SUBPROCESS(source_object), res, &bytes_out, &bytes_err, &error)) {
		info_redirect_error(user_data, error);
		return;
	}

	gchar *out = bytes_to_utf8(bytes_out);
	gchar *err = bytes_to_utf8(bytes_err);

	GtkWidget *dialog = GTK_WIDGET(user_data);
	GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_foreach(
		GTK_CONTAINER(content_area), (GtkCallback) gtk_widget_destroy, NULL);

	GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_box_pack_start(GTK_BOX(content_area), scroller, TRUE, TRUE, 0);
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(scroller), vbox);
	if (*err)
		gtk_container_add(GTK_CONTAINER(vbox), info_make_bar(g_strstrip(err)));

	GtkWidget *info = info_parse(out);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(info), "fiv-information");
	gtk_box_pack_start(GTK_BOX(vbox), info, TRUE, TRUE, 0);

	g_free(out);
	g_free(err);
	gtk_widget_show_all(dialog);
	gtk_widget_grab_focus(scroller);
}

static void
info_spawn(GtkWidget *dialog, const char *path, GBytes *bytes_in)
{
	int flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
	if (bytes_in)
		flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;

	// TODO(p): Add a fallback to internal capabilities.
	// The simplest is to specify the filename and the resolution.
	GError *error = NULL;
	GSubprocess *subprocess = g_subprocess_new(flags, &error, "exiftool",
		"-tab", "-groupNames", "-duplicates", "-extractEmbedded", "--binary",
		"-quiet", "--", path, NULL);
	if (error) {
		info_redirect_error(dialog, error);
		return;
	}

	GCancellable *cancellable = g_object_get_qdata(
		G_OBJECT(dialog), fiv_context_menu_cancellable_quark());
	g_subprocess_communicate_async(
		subprocess, bytes_in, cancellable, on_info_finished, dialog);
	g_object_unref(subprocess);
}

static void
on_info_loaded(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	gchar *file_data = NULL;
	gsize file_len = 0;
	GError *error = NULL;
	if (!g_file_load_contents_finish(
			G_FILE(source_object), res, &file_data, &file_len, NULL, &error)) {
		info_redirect_error(user_data, error);
		return;
	}

	GtkWidget *dialog = GTK_WIDGET(user_data);
	GBytes *bytes_in = g_bytes_new_take(file_data, file_len);
	info_spawn(dialog, "-", bytes_in);
	g_bytes_unref(bytes_in);
}

static void
on_info_queried(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GFile *file = G_FILE(source_object);
	GError *error = NULL;
	GFileInfo *info = g_file_query_info_finish(file, res, &error);
	gboolean cancelled =
		error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error(&error);
	if (cancelled)
		return;

	gchar *path = NULL;
	const char *target_uri = g_file_info_get_attribute_string(
		info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
	if (target_uri) {
		GFile *target = g_file_new_for_uri(target_uri);
		path = g_file_get_path(target);
		g_object_unref(target);
	}
	g_object_unref(info);

	GtkWidget *dialog = GTK_WIDGET(user_data);
	GCancellable *cancellable = g_object_get_qdata(
		G_OBJECT(dialog), fiv_context_menu_cancellable_quark());
	if (path) {
		info_spawn(dialog, path, NULL);
		g_free(path);
	} else {
		g_file_load_contents_async(file, cancellable, on_info_loaded, dialog);
	}
}

void
fiv_context_menu_information(GtkWindow *parent, const char *uri)
{
	GtkWidget *dialog = gtk_widget_new(GTK_TYPE_DIALOG,
		"use-header-bar", TRUE,
		"title", "Information",
		"transient-for", parent,
		"destroy-with-parent", TRUE, NULL);

	// When the window closes, we cancel all asynchronous calls.
	GCancellable *cancellable = g_cancellable_new();
	g_object_set_qdata_full(G_OBJECT(dialog),
		fiv_context_menu_cancellable_quark(), cancellable, g_object_unref);
	g_signal_connect_swapped(
		dialog, "destroy", G_CALLBACK(g_cancellable_cancel), cancellable);

	GtkWidget *spinner = gtk_spinner_new();
	gtk_spinner_start(GTK_SPINNER(spinner));
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
		spinner, TRUE, TRUE, 12);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 800);
	gtk_widget_show_all(dialog);

	// Mostly to identify URIs with no local path--we pipe these into ExifTool.
	GFile *file = g_file_new_for_uri(uri);
	gchar *parse_name = g_file_get_parse_name(file);
	gtk_header_bar_set_subtitle(
		GTK_HEADER_BAR(gtk_dialog_get_header_bar(GTK_DIALOG(dialog))),
		parse_name);
	g_free(parse_name);

	gchar *path = g_file_get_path(file);
	if (path) {
		info_spawn(dialog, path, NULL);
		g_free(path);
	} else {
		// Several GVfs schemes contain pseudo-symlinks
		// that don't give out filesystem paths directly.
		g_file_query_info_async(file, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
			G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, cancellable,
			on_info_queried, dialog);
	}
	g_object_unref(file);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

typedef struct _OpenContext {
	GWeakRef window;                    ///< Parent window for any dialogs
	GFile *file;                        ///< The file in question
	gchar *content_type;
	GAppInfo *app_info;
} OpenContext;

static void
open_context_finalize(gpointer data)
{
	OpenContext *self = data;
	g_weak_ref_clear(&self->window);
	g_clear_object(&self->app_info);
	g_clear_object(&self->file);
	g_free(self->content_type);
}

static void
open_context_unref(gpointer data, G_GNUC_UNUSED GClosure *closure)
{
	g_rc_box_release_full(data, open_context_finalize);
}

static void
open_context_launch(GtkWidget *widget, OpenContext *self)
{
	GdkAppLaunchContext *context =
		gdk_display_get_app_launch_context(gtk_widget_get_display(widget));
	gdk_app_launch_context_set_screen(context, gtk_widget_get_screen(widget));
	gdk_app_launch_context_set_timestamp(context, gtk_get_current_event_time());

	GList *files = g_list_append(NULL, self->file);
	GError *error = NULL;
	if (g_app_info_launch(
			self->app_info, files, G_APP_LAUNCH_CONTEXT(context), &error)) {
		(void) g_app_info_set_as_last_used_for_type(
			self->app_info, self->content_type, NULL);
	} else {
		g_warning("%s", error->message);
		g_error_free(error);
	}
	g_list_free(files);
	g_object_unref(context);
}

static void
append_opener(GtkWidget *menu, GAppInfo *opener, const OpenContext *template)
{
	OpenContext *ctx = g_rc_box_alloc0(sizeof *ctx);
	g_weak_ref_init(&ctx->window, NULL);
	ctx->file = g_object_ref(template->file);
	ctx->content_type = g_strdup(template->content_type);
	ctx->app_info = opener;

	// On Linux, this prefers the obsoleted X-GNOME-FullName.
	gchar *name =
		g_strdup_printf("Open With %s", g_app_info_get_display_name(opener));

	// It's documented that we can touch the child, if we want to use markup.
#if 0
	GtkWidget *item = gtk_menu_item_new_with_label(name);
#else
	// GtkImageMenuItem overrides the toggle_size_request class method
	// to get the image shown in the "margin"--too much work to duplicate.
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;

	GtkWidget *item = gtk_image_menu_item_new_with_label(name);
	GIcon *icon = g_app_info_get_icon(opener);
	if (icon) {
		GtkWidget *image = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), image);
		gtk_image_menu_item_set_always_show_image(
			GTK_IMAGE_MENU_ITEM(item), TRUE);
	}

	G_GNUC_END_IGNORE_DEPRECATIONS;
#endif

	g_free(name);
	g_signal_connect_data(item, "activate", G_CALLBACK(open_context_launch),
		ctx, open_context_unref, 0);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void
on_chooser_activate(GtkMenuItem *item, gpointer user_data)
{
	OpenContext *ctx = user_data;
	GtkWindow *window = g_weak_ref_get(&ctx->window);
	GtkWidget *dialog = gtk_app_chooser_dialog_new_for_content_type(window,
		GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, ctx->content_type);
	g_clear_object(&window);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
		ctx->app_info = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dialog));
		open_context_launch(GTK_WIDGET(item), ctx);
	}
	gtk_widget_destroy(dialog);
}

static void
on_info_activate(G_GNUC_UNUSED GtkMenuItem *item, gpointer user_data)
{
	OpenContext *ctx = user_data;
	GtkWindow *window = g_weak_ref_get(&ctx->window);
	gchar *uri = g_file_get_uri(ctx->file);
	fiv_context_menu_information(window, uri);
	g_clear_object(&window);
	g_free(uri);
}

static gboolean
destroy_widget_idle_source_func(GtkWidget *widget)
{
	// The whole menu is deactivated /before/ any item is activated,
	// and a destroyed child item will not activate.
	gtk_widget_destroy(widget);
	return FALSE;
}

GtkMenu *
fiv_context_menu_new(GtkWidget *widget, GFile *file)
{
	GFileInfo *info = g_file_query_info(file,
		G_FILE_ATTRIBUTE_STANDARD_TYPE
		"," G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
		"," G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!info)
		return NULL;

	GtkWindow *window = NULL;
	if (widget && GTK_IS_WINDOW((widget = gtk_widget_get_toplevel(widget))))
		window = GTK_WINDOW(widget);

	// This will have no application pre-assigned, for use with GTK+'s dialog.
	OpenContext *ctx = g_rc_box_alloc0(sizeof *ctx);
	g_weak_ref_init(&ctx->window, window);
	if (!(ctx->content_type = g_strdup(g_file_info_get_content_type(info))))
		ctx->content_type = g_content_type_guess(NULL, NULL, 0, NULL);

	GFileType type = g_file_info_get_file_type(info);
	const char *target_uri = g_file_info_get_attribute_string(
		info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
	ctx->file = target_uri && g_file_has_uri_scheme(file, FIV_COLLECTION_SCHEME)
		? g_file_new_for_uri(target_uri)
		: g_object_ref(file);
	g_object_unref(info);

	GAppInfo *default_ =
		g_app_info_get_default_for_type(ctx->content_type, FALSE);
	GList *recommended = g_app_info_get_recommended_for_type(ctx->content_type);
	GList *fallback = g_app_info_get_fallback_for_type(ctx->content_type);

	GtkWidget *menu = gtk_menu_new();
	if (default_) {
		append_opener(menu, default_, ctx);
		gtk_menu_shell_append(
			GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	}

	for (GList *iter = recommended; iter; iter = iter->next) {
		if (!default_ || !g_app_info_equal(iter->data, default_))
			append_opener(menu, iter->data, ctx);
		else
			g_object_unref(iter->data);
	}
	if (recommended) {
		g_list_free(recommended);
		gtk_menu_shell_append(
			GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	}

	for (GList *iter = fallback; iter; iter = iter->next) {
		if (!default_ || !g_app_info_equal(iter->data, default_))
			append_opener(menu, iter->data, ctx);
		else
			g_object_unref(iter->data);
	}
	if (fallback) {
		g_list_free(fallback);
		gtk_menu_shell_append(
			GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	}

	GtkWidget *item = gtk_menu_item_new_with_label("Open With...");
	g_signal_connect_data(item, "activate", G_CALLBACK(on_chooser_activate),
		ctx, open_context_unref, 0);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	if (type == G_FILE_TYPE_REGULAR) {
		gtk_menu_shell_append(
			GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

		item = gtk_menu_item_new_with_mnemonic("_Information...");
		g_signal_connect_data(item, "activate", G_CALLBACK(on_info_activate),
			g_rc_box_acquire(ctx), open_context_unref, 0);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}

	// As per GTK+ 3 Common Questions, 1.5.
	g_object_ref_sink(menu);
	g_signal_connect_swapped(menu, "deactivate",
		G_CALLBACK(g_idle_add), destroy_widget_idle_source_func);
	g_signal_connect(menu, "destroy", G_CALLBACK(g_object_unref), NULL);

	gtk_widget_show_all(menu);
	return GTK_MENU(menu);
}

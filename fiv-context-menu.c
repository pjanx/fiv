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

#include "fiv-context-menu.h"

typedef struct _OpenContext {
	GWeakRef widget;
	GFile *file;
	gchar *content_type;
	GAppInfo *app_info;
} OpenContext;

static void
open_context_notify(gpointer data, G_GNUC_UNUSED GClosure *closure)
{
	OpenContext *self = data;
	g_weak_ref_clear(&self->widget);
	g_clear_object(&self->app_info);
	g_clear_object(&self->file);
	g_free(self->content_type);
	g_free(self);
}

static void
open_context_launch(GtkWidget *widget, OpenContext *self)
{
	GdkAppLaunchContext *context =
		gdk_display_get_app_launch_context(gtk_widget_get_display(widget));
	gdk_app_launch_context_set_screen(context, gtk_widget_get_screen(widget));
	gdk_app_launch_context_set_timestamp(context, gtk_get_current_event_time());

	// TODO(p): Display errors.
	GList *files = g_list_append(NULL, self->file);
	if (g_app_info_launch(
			self->app_info, files, G_APP_LAUNCH_CONTEXT(context), NULL)) {
		g_app_info_set_as_last_used_for_type(
			self->app_info, self->content_type, NULL);
	}
	g_list_free(files);
	g_object_unref(context);
}

static void
append_opener(GtkWidget *menu, GAppInfo *opener, const OpenContext *template)
{
	OpenContext *ctx = g_malloc0(sizeof *ctx);
	g_weak_ref_init(&ctx->widget, NULL);
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
		ctx, open_context_notify, 0);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void
on_chooser_activate(GtkMenuItem *item, gpointer user_data)
{
	OpenContext *ctx = user_data;
	GtkWindow *window = NULL;
	GtkWidget *widget = g_weak_ref_get(&ctx->widget);
	if (widget) {
		if (GTK_IS_WINDOW((widget = gtk_widget_get_toplevel(widget))))
			window = GTK_WINDOW(widget);
	}

	GtkWidget *dialog = gtk_app_chooser_dialog_new_for_content_type(window,
		GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, ctx->content_type);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
		ctx->app_info = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dialog));
		open_context_launch(GTK_WIDGET(item), ctx);
	}
	gtk_widget_destroy(dialog);
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
		G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!info)
		return NULL;

	// This will have no application pre-assigned, for use with GTK+'s dialog.
	OpenContext *ctx = g_malloc0(sizeof *ctx);
	g_weak_ref_init(&ctx->widget, widget);
	ctx->file = g_object_ref(file);
	ctx->content_type = g_strdup(g_file_info_get_content_type(info));
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
		ctx, open_context_notify, 0);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	// As per GTK+ 3 Common Questions, 1.5.
	g_object_ref_sink(menu);
	g_signal_connect_swapped(menu, "deactivate",
		G_CALLBACK(g_idle_add), destroy_widget_idle_source_func);
	g_signal_connect(menu, "destroy", G_CALLBACK(g_object_unref), NULL);

	gtk_widget_show_all(menu);
	return GTK_MENU(menu);
}

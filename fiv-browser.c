//
// fiv-browser.c: filesystem browsing widget
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

#include <math.h>
#include <pixman.h>

#include "config.h"

#include "fiv-browser.h"
#include "fiv-io.h"
#include "fiv-thumbnail.h"
#include "fiv-view.h"

// --- Widget ------------------------------------------------------------------
//                     _________________________________
//                    │    p   a   d   d   i   n   g
//                    │ p ╭───────────────────╮ s ╭┄┄┄┄┄
//                    │ a │ glow border   ┊   │ p ┊
//                    │ d │ ┄ ╔═══════════╗ ┄ │ a ┊
//                    │ d │   ║ thumbnail ║   │ c ┊ ...
//                    │ i │ ┄ ╚═══════════╝ ┄ │ i ┊
//                    │ n │   ┊   glow border │ n ┊
//                    │ g ╰───────────────────╯ g ╰┄┄┄┄┄
//                    │    s  p  a  c  i  n  g
//                    │   ╭┄┄┄┄┄┄┄┄┄┄┄┄╮   ╭┄┄┄┄┄┄┄┄┄┄┄┄
//
// The glow is actually a glowing margin, the border is rendered in two parts.
//

struct _FivBrowser {
	GtkWidget parent_instance;

	FivThumbnailSize item_size;         ///< Thumbnail size
	int item_height;                    ///< Thumbnail height in pixels
	int item_spacing;                   ///< Space between items in pixels

	FivIoModel *model;                  ///< Filesystem model
	GArray *entries;                    ///< []Entry
	GArray *layouted_rows;              ///< []Row
	int selected;

	GList *thumbnail_queue;             ///< Entry pointers
	GSubprocess *thumbnailer;           ///< A slave for the current queue head
	GCancellable *thumbnail_cancel;     ///< Cancellable handle

	GdkCursor *pointer;                 ///< Cached pointer cursor
	cairo_surface_t *glow;              ///< CAIRO_FORMAT_A8 mask
	int item_border_x;                  ///< L/R .item margin + border
	int item_border_y;                  ///< T/B .item margin + border
};

typedef struct entry Entry;
typedef struct item Item;
typedef struct row Row;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct entry {
	char *uri;                          ///< GIO URI
	cairo_surface_t *thumbnail;         ///< Prescaled thumbnail
	GIcon *icon;                        ///< If no thumbnail, use this icon
};

static void
entry_free(Entry *self)
{
	g_free(self->uri);
	g_clear_pointer(&self->thumbnail, cairo_surface_destroy);
	g_clear_object(&self->icon);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct item {
	const Entry *entry;
	int x_offset;                       ///< Offset within the row
};

struct row {
	Item *items;                        ///< Ends with a NULL entry
	int x_offset;                       ///< Start position outside borders
	int y_offset;                       ///< Start position inside borders
};

static void
row_free(Row *self)
{
	g_free(self->items);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
append_row(FivBrowser *self, int *y, int x, GArray *items_array)
{
	if (self->layouted_rows->len)
		*y += self->item_spacing;

	*y += self->item_border_y;
	g_array_append_val(self->layouted_rows, ((Row) {
		.items = g_array_steal(items_array, NULL),
		.x_offset = x,
		.y_offset = *y,
	}));

	// Not trying to pack them vertically, but this would be the place to do it.
	*y += self->item_height;
	*y += self->item_border_y;
}

static int
relayout(FivBrowser *self, int width)
{
	GtkWidget *widget = GTK_WIDGET(self);
	GtkStyleContext *style = gtk_widget_get_style_context(widget);

	GtkBorder padding = {};
	gtk_style_context_get_padding(style, GTK_STATE_FLAG_NORMAL, &padding);
	int available_width = width - padding.left - padding.right;

	g_array_set_size(self->layouted_rows, 0);

	GArray *items = g_array_new(TRUE, TRUE, sizeof(Item));
	int x = 0, y = padding.top;
	for (guint i = 0; i < self->entries->len; i++) {
		const Entry *entry = &g_array_index(self->entries, Entry, i);
		if (!entry->thumbnail)
			continue;

		int width = cairo_image_surface_get_width(entry->thumbnail) +
			2 * self->item_border_x;
		if (!items->len) {
			// Just insert it, whether or not there's any space.
		} else if (x + self->item_spacing + width <= available_width) {
			x += self->item_spacing;
		} else {
			append_row(self, &y,
				padding.left + MAX(0, available_width - x) / 2, items);
			x = 0;
		}

		g_array_append_val(items,
			((Item) {.entry = entry, .x_offset = x + self->item_border_x}));
		x += width;
	}
	if (items->len) {
		append_row(self, &y,
			padding.left + MAX(0, available_width - x) / 2, items);
	}

	g_array_free(items, TRUE);
	return y + padding.bottom;
}

static void
draw_outer_border(FivBrowser *self, cairo_t *cr, int width, int height)
{
	int offset_x = cairo_image_surface_get_width(self->glow);
	int offset_y = cairo_image_surface_get_height(self->glow);
	cairo_pattern_t *mask = cairo_pattern_create_for_surface(self->glow);
	cairo_matrix_t matrix;

	cairo_pattern_set_extend(mask, CAIRO_EXTEND_PAD);
	cairo_save(cr);
	cairo_translate(cr, -offset_x, -offset_y);
	cairo_rectangle(cr, 0, 0, offset_x + width, offset_y + height);
	cairo_clip(cr);
	cairo_mask(cr, mask);
	cairo_restore(cr);
	cairo_save(cr);
	cairo_translate(cr, width + offset_x, height + offset_y);
	cairo_rectangle(cr, 0, 0, -offset_x - width, -offset_y - height);
	cairo_clip(cr);
	cairo_scale(cr, -1, -1);
	cairo_mask(cr, mask);
	cairo_restore(cr);

	cairo_pattern_set_extend(mask, CAIRO_EXTEND_NONE);
	cairo_matrix_init_scale(&matrix, -1, 1);
	cairo_matrix_translate(&matrix, -width - offset_x, offset_y);
	cairo_pattern_set_matrix(mask, &matrix);
	cairo_mask(cr, mask);
	cairo_matrix_init_scale(&matrix, 1, -1);
	cairo_matrix_translate(&matrix, offset_x, -height - offset_y);
	cairo_pattern_set_matrix(mask, &matrix);
	cairo_mask(cr, mask);

	cairo_pattern_destroy(mask);
}

static GdkRectangle
item_extents(FivBrowser *self, const Item *item, const Row *row)
{
	int width = cairo_image_surface_get_width(item->entry->thumbnail);
	int height = cairo_image_surface_get_height(item->entry->thumbnail);
	return (GdkRectangle) {
		.x = row->x_offset + item->x_offset,
		.y = row->y_offset + self->item_height - height,
		.width = width,
		.height = height,
	};
}

static const Entry *
entry_at(FivBrowser *self, int x, int y)
{
	for (guint i = 0; i < self->layouted_rows->len; i++) {
		const Row *row = &g_array_index(self->layouted_rows, Row, i);
		for (Item *item = row->items; item->entry; item++) {
			GdkRectangle extents = item_extents(self, item, row);
			if (x >= extents.x &&
				y >= extents.y &&
				x <= extents.x + extents.width &&
				y <= extents.y + extents.height)
				return item->entry;
		}
	}
	return NULL;
}

static void
draw_row(FivBrowser *self, cairo_t *cr, const Row *row)
{
	GtkStyleContext *style = gtk_widget_get_style_context(GTK_WIDGET(self));
	gtk_style_context_save(style);
	gtk_style_context_add_class(style, "item");

	GdkRGBA glow_color = {};
	GtkStateFlags state = gtk_style_context_get_state (style);
	gtk_style_context_get_color(style, state, &glow_color);

	GtkBorder border;
	gtk_style_context_get_border(style, state, &border);
	for (Item *item = row->items; item->entry; item++) {
		cairo_save(cr);
		GdkRectangle extents = item_extents(self, item, row);
		cairo_translate(cr, extents.x - border.left, extents.y - border.top);

		gtk_style_context_save(style);
		if (item->entry->icon) {
			gtk_style_context_add_class(style, "symbolic");
		} else {
			gdk_cairo_set_source_rgba(cr, &glow_color);
			draw_outer_border(self, cr,
				border.left + extents.width + border.right,
				border.top + extents.height + border.bottom);
		}

		// Performance optimization--specifically targeting the checkerboard.
		if (cairo_image_surface_get_format(item->entry->thumbnail) !=
				CAIRO_FORMAT_RGB24) {
			gtk_render_background(style, cr, border.left, border.top,
				extents.width, extents.height);
		}

		gtk_render_frame(style, cr, 0, 0,
			border.left + extents.width + border.right,
			border.top + extents.height + border.bottom);

		if (item->entry->icon) {
			GdkRGBA color = {};
			gtk_style_context_get_color(style, state, &color);
			gdk_cairo_set_source_rgba(cr, &color);
			cairo_mask_surface(
				cr, item->entry->thumbnail, border.left, border.top);
		} else {
			cairo_set_source_surface(
				cr, item->entry->thumbnail, border.left, border.top);
			cairo_paint(cr);
		}

		cairo_restore(cr);
		gtk_style_context_restore(style);
	}
	gtk_style_context_restore(style);
}

// --- Thumbnails --------------------------------------------------------------

// NOTE: "It is important to note that when an image with an alpha channel is
// scaled, linear encoded, pre-multiplied component values must be used!"
static cairo_surface_t *
rescale_thumbnail(cairo_surface_t *thumbnail, double row_height)
{
	if (!thumbnail)
		return thumbnail;

	int width = cairo_image_surface_get_width(thumbnail);
	int height = cairo_image_surface_get_height(thumbnail);

	double scale_x = 1;
	double scale_y = 1;
	if (width > FIV_THUMBNAIL_WIDE_COEFFICIENT * height) {
		scale_x = FIV_THUMBNAIL_WIDE_COEFFICIENT * row_height / width;
		scale_y = round(scale_x * height) / height;
	} else {
		scale_y = row_height / height;
		scale_x = round(scale_y * width) / width;
	}
	if (scale_x == 1 && scale_y == 1)
		return thumbnail;

	int projected_width = round(scale_x * width);
	int projected_height = round(scale_y * height);
	cairo_format_t cairo_format = cairo_image_surface_get_format(thumbnail);
	cairo_surface_t *scaled = cairo_image_surface_create(
		cairo_format, projected_width, projected_height);

	// pixman can take gamma into account when scaling, unlike Cairo.
	struct pixman_f_transform xform_floating;
	struct pixman_transform xform;

	// PIXMAN_a8r8g8b8_sRGB can be used for gamma-correct results,
	// but it's an incredibly slow transformation
	pixman_format_code_t format =
		cairo_format == CAIRO_FORMAT_RGB24 ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;

	pixman_image_t *src = pixman_image_create_bits(format, width, height,
		(uint32_t *) cairo_image_surface_get_data(thumbnail),
		cairo_image_surface_get_stride(thumbnail));
	pixman_image_t *dest = pixman_image_create_bits(format,
		cairo_image_surface_get_width(scaled),
		cairo_image_surface_get_height(scaled),
		(uint32_t *) cairo_image_surface_get_data(scaled),
		cairo_image_surface_get_stride(scaled));

	pixman_f_transform_init_scale(&xform_floating, scale_x, scale_y);
	pixman_f_transform_invert(&xform_floating, &xform_floating);
	pixman_transform_from_pixman_f_transform(&xform, &xform_floating);
	pixman_image_set_transform(src, &xform);
	pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);
	pixman_image_set_repeat(src, PIXMAN_REPEAT_PAD);

	pixman_image_composite(PIXMAN_OP_SRC, src, NULL, dest, 0, 0, 0, 0, 0, 0,
		projected_width, projected_height);
	pixman_image_unref(src);
	pixman_image_unref(dest);

	cairo_surface_set_user_data(
		scaled, &fiv_thumbnail_key_lq, (void *) (intptr_t) 1, NULL);
	cairo_surface_destroy(thumbnail);
	cairo_surface_mark_dirty(scaled);
	return scaled;
}

static void
entry_add_thumbnail(gpointer data, gpointer user_data)
{
	Entry *self = data;
	g_clear_object(&self->icon);
	g_clear_pointer(&self->thumbnail, cairo_surface_destroy);

	FivBrowser *browser = FIV_BROWSER(user_data);
	GFile *file = g_file_new_for_uri(self->uri);
	self->thumbnail = rescale_thumbnail(
		fiv_thumbnail_lookup(file, browser->item_size), browser->item_height);
	if (self->thumbnail)
		goto out;

	// Fall back to symbolic icons, though there's only so much we can do
	// in parallel--GTK+ isn't thread-safe.
	GFileInfo *info = g_file_query_info(file,
		G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (info) {
		GIcon *icon = g_file_info_get_symbolic_icon(info);
		if (icon)
			self->icon = g_object_ref(icon);
		g_object_unref(info);
	}
out:
	g_object_unref(file);
}

static void
materialize_icon(FivBrowser *self, Entry *entry)
{
	if (!entry->icon)
		return;

	// Fucker will still give us non-symbolic icons, no more playing nice.
	// TODO(p): Investigate a bit closer. We may want to abandon the idea
	// of using GLib to look up icons for us, derive a list from a guessed
	// MIME type, with "-symbolic" prefixes and fallbacks,
	// and use gtk_icon_theme_choose_icon() instead.
	// TODO(p): Make sure we have /some/ icon for every entry.
	// TODO(p): We might want to populate these on an as-needed basis.
	GtkIconInfo *icon_info = gtk_icon_theme_lookup_by_gicon(
		gtk_icon_theme_get_default(), entry->icon, self->item_height / 2,
		GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
	if (!icon_info)
		return;

	// Bílá, bílá, bílá, bílá... komu by se nelíbí-lá...
	// We do not want any highlights, nor do we want to remember the style.
	const GdkRGBA white = {1, 1, 1, 1};
	GdkPixbuf *pixbuf = gtk_icon_info_load_symbolic(
		icon_info, &white, &white, &white, &white, NULL, NULL);
	if (pixbuf) {
		int outer_size = self->item_height;
		entry->thumbnail =
			cairo_image_surface_create(CAIRO_FORMAT_A8, outer_size, outer_size);

		// "Note that the resulting pixbuf may not be exactly this size;"
		// though GTK_ICON_LOOKUP_FORCE_SIZE is also an option.
		int x = (outer_size - gdk_pixbuf_get_width(pixbuf)) / 2;
		int y = (outer_size - gdk_pixbuf_get_height(pixbuf)) / 2;

		cairo_t *cr = cairo_create(entry->thumbnail);
		gdk_cairo_set_source_pixbuf(cr, pixbuf, x, y);
		cairo_paint(cr);
		cairo_destroy(cr);

		g_object_unref(pixbuf);
	}
	g_object_unref(icon_info);
}

static void
reload_thumbnails(FivBrowser *self)
{
	GThreadPool *pool = g_thread_pool_new(
		entry_add_thumbnail, self, g_get_num_processors(), FALSE, NULL);
	for (guint i = 0; i < self->entries->len; i++)
		g_thread_pool_push(pool, &g_array_index(self->entries, Entry, i), NULL);
	g_thread_pool_free(pool, FALSE, TRUE);

	for (guint i = 0; i < self->entries->len; i++)
		materialize_icon(self, &g_array_index(self->entries, Entry, i));

	gtk_widget_queue_resize(GTK_WIDGET(self));
}

// --- Slave management --------------------------------------------------------

static void thumbnailer_next(FivBrowser *self);

static void
thumbnailer_reprocess_entry(FivBrowser *self, Entry *entry)
{
	entry_add_thumbnail(entry, self);
	materialize_icon(self, entry);
	gtk_widget_queue_resize(GTK_WIDGET(self));
}

static void
on_thumbnailer_ready(GObject *object, GAsyncResult *res, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS(object);
	FivBrowser *self = FIV_BROWSER(user_data);
	GError *error = NULL;
	if (!g_subprocess_wait_check_finish(subprocess, res, &error)) {
		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free(error);
			return;
		}
		if (!g_subprocess_get_if_exited(subprocess) ||
			g_subprocess_get_exit_status(subprocess) != EXIT_FAILURE)
			g_warning("%s", error->message);
		g_error_free(error);
	}

	gboolean succeeded = g_subprocess_get_if_exited(subprocess) &&
		g_subprocess_get_exit_status(subprocess) == EXIT_SUCCESS;
	g_clear_object(&self->thumbnailer);
	if (!self->thumbnail_queue) {
		g_warning("finished thumbnailing an unknown image");
		return;
	}

	Entry *entry = self->thumbnail_queue->data;
	self->thumbnail_queue =
		g_list_delete_link(self->thumbnail_queue, self->thumbnail_queue);
	if (succeeded)
		thumbnailer_reprocess_entry(self, entry);

	thumbnailer_next(self);
}

static void
thumbnailer_next(FivBrowser *self)
{
	// TODO(p): At least launch multiple thumbnailers in parallel.
	// Ideally, try to keep them alive.
	GList *link = self->thumbnail_queue;
	if (!link)
		return;

	const Entry *entry = link->data;
	GError *error = NULL;
	self->thumbnailer = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
		PROJECT_NAME, "--thumbnail",
		fiv_thumbnail_sizes[self->item_size].thumbnail_spec_name, "--",
		entry->uri, NULL);
	if (error) {
		g_warning("%s", error->message);
		g_error_free(error);
		return;
	}

	self->thumbnail_cancel = g_cancellable_new();
	g_subprocess_wait_check_async(
		self->thumbnailer, self->thumbnail_cancel, on_thumbnailer_ready, self);
}

static void
thumbnailer_abort(FivBrowser *self)
{
	if (self->thumbnail_cancel) {
		g_cancellable_cancel(self->thumbnail_cancel);
		g_clear_object(&self->thumbnail_cancel);
	}

	// Just let it exit on its own.
	g_clear_object(&self->thumbnailer);
	g_list_free(self->thumbnail_queue);
	self->thumbnail_queue = NULL;
}

static void
thumbnailer_start(FivBrowser *self)
{
	thumbnailer_abort(self);

	// TODO(p): Leave out all paths containing .cache/thumbnails altogether.
	gchar *thumbnails_dir = fiv_thumbnail_get_root();
	GFile *thumbnails = g_file_new_for_path(thumbnails_dir);
	g_free(thumbnails_dir);

	GFile *current = fiv_io_model_get_location(self->model);
	gboolean is_a_thumbnail = current && g_file_has_prefix(current, thumbnails);
	g_object_unref(thumbnails);
	if (is_a_thumbnail)
		return;

	GList *missing = NULL, *lq = NULL;
	for (guint i = self->entries->len; i--; ) {
		Entry *entry = &g_array_index(self->entries, Entry, i);
		if (entry->icon)
			missing = g_list_prepend(missing, entry);
		else if (cairo_surface_get_user_data(
			entry->thumbnail, &fiv_thumbnail_key_lq))
			lq = g_list_prepend(lq, entry);
	}

	self->thumbnail_queue = g_list_concat(missing, lq);
	thumbnailer_next(self);
}

// --- Context menu-------------------------------------------------------------

typedef struct _OpenContext {
	GWeakRef widget;
	GFile *file;
	char *content_type;
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

	// It's documented that we can touch the child, if we want formatting:
	// https://docs.gtk.org/gtk3/class.MenuItem.html
	// XXX: Would g_app_info_get_display_name() be any better?
	gchar *name = g_strdup_printf("Open With %s", g_app_info_get_name(opener));
	GtkWidget *item = gtk_menu_item_new_with_label(name);
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

static void
show_context_menu(GtkWidget *widget, GFile *file)
{
	GFileInfo *info = g_file_query_info(file,
		G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!info)
		return;

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
		if (g_app_info_should_show(iter->data) &&
			(!default_ || !g_app_info_equal(iter->data, default_)))
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
		if (g_app_info_should_show(iter->data) &&
			(!default_ || !g_app_info_equal(iter->data, default_)))
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
	gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
}

// --- Boilerplate -------------------------------------------------------------

// TODO(p): For proper navigation, we need to implement GtkScrollable.
G_DEFINE_TYPE_EXTENDED(FivBrowser, fiv_browser, GTK_TYPE_WIDGET, 0,
	/* G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE,
		fiv_browser_scrollable_init) */)

enum {
	PROP_THUMBNAIL_SIZE = 1,
	N_PROPERTIES
};

static GParamSpec *browser_properties[N_PROPERTIES];

enum {
	ITEM_ACTIVATED,
	LAST_SIGNAL,
};

// Globals are, sadly, the canonical way of storing signal numbers.
static guint browser_signals[LAST_SIGNAL];

static void
fiv_browser_finalize(GObject *gobject)
{
	FivBrowser *self = FIV_BROWSER(gobject);
	thumbnailer_abort(self);
	g_array_free(self->entries, TRUE);
	g_array_free(self->layouted_rows, TRUE);
	if (self->model) {
		g_signal_handlers_disconnect_by_data(self->model, self);
		g_clear_object(&self->model);
	}

	cairo_surface_destroy(self->glow);
	g_clear_object(&self->pointer);

	G_OBJECT_CLASS(fiv_browser_parent_class)->finalize(gobject);
}

static void
fiv_browser_get_property(
	GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	FivBrowser *self = FIV_BROWSER(object);
	switch (property_id) {
	case PROP_THUMBNAIL_SIZE:
		g_value_set_enum(value, self->item_size);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
set_item_size(FivBrowser *self, FivThumbnailSize size)
{
	if (size < FIV_THUMBNAIL_SIZE_MIN || size > FIV_THUMBNAIL_SIZE_MAX)
		return;

	if (size != self->item_size) {
		self->item_size = size;
		self->item_height = fiv_thumbnail_sizes[self->item_size].size;
		reload_thumbnails(self);

		g_object_notify_by_pspec(
			G_OBJECT(self), browser_properties[PROP_THUMBNAIL_SIZE]);
	}
}

static void
fiv_browser_set_property(
	GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	FivBrowser *self = FIV_BROWSER(object);
	switch (property_id) {
	case PROP_THUMBNAIL_SIZE:
		set_item_size(self, g_value_get_enum(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static GtkSizeRequestMode
fiv_browser_get_request_mode(G_GNUC_UNUSED GtkWidget *widget)
{
	return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
fiv_browser_get_preferred_width(GtkWidget *widget, gint *minimum, gint *natural)
{
	FivBrowser *self = FIV_BROWSER(widget);
	GtkStyleContext *style = gtk_widget_get_style_context(widget);

	GtkBorder padding = {};
	gtk_style_context_get_padding(style, GTK_STATE_FLAG_NORMAL, &padding);
	*minimum = *natural =
		FIV_THUMBNAIL_WIDE_COEFFICIENT * self->item_height + padding.left +
		2 * self->item_border_x + padding.right;
}

static void
fiv_browser_get_preferred_height_for_width(
	GtkWidget *widget, gint width, gint *minimum, gint *natural)
{
	// XXX: This is rather ugly, the caller is only asking.
	*minimum = *natural = relayout(FIV_BROWSER(widget), width);
}

static void
fiv_browser_realize(GtkWidget *widget)
{
	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);

	GdkWindowAttr attributes = {
		.window_type = GDK_WINDOW_CHILD,
		.x = allocation.x,
		.y = allocation.y,
		.width = allocation.width,
		.height = allocation.height,

		// Input-only would presumably also work (as in GtkPathBar, e.g.),
		// but it merely seems to involve more work.
		.wclass = GDK_INPUT_OUTPUT,

		.visual = gtk_widget_get_visual(widget),
		.event_mask = gtk_widget_get_events(widget) | GDK_KEY_PRESS_MASK |
			GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK,
	};

	// We need this window to receive input events at all.
	// TODO(p): See if input events bubble up to parents.
	GdkWindow *window = gdk_window_new(gtk_widget_get_parent_window(widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
	gtk_widget_register_window(widget, window);
	gtk_widget_set_window(widget, window);
	gtk_widget_set_realized(widget, TRUE);

	FivBrowser *self = FIV_BROWSER(widget);
	g_clear_object(&self->pointer);
	self->pointer =
		gdk_cursor_new_from_name(gdk_window_get_display(window), "pointer");
}

static void
fiv_browser_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->size_allocate(widget, allocation);

	relayout(FIV_BROWSER(widget), allocation->width);
}

static gboolean
fiv_browser_draw(GtkWidget *widget, cairo_t *cr)
{
	FivBrowser *self = FIV_BROWSER(widget);
	if (!gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget)))
		return TRUE;

	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);
	gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0,
		allocation.width, allocation.height);

	GdkRectangle clip = {};
	gboolean have_clip = gdk_cairo_get_clip_rectangle(cr, &clip);

	for (guint i = 0; i < self->layouted_rows->len; i++) {
		const Row *row = &g_array_index(self->layouted_rows, Row, i);
		GdkRectangle extents = {
			.x = 0,
			.y = row->y_offset - self->item_border_y,
			.width = allocation.width,
			.height = self->item_height + 2 * self->item_border_y,
		};
		if (!have_clip || gdk_rectangle_intersect(&clip, &extents, NULL))
			draw_row(self, cr, row);
	}
	return TRUE;
}

static gboolean
open_entry(GtkWidget *self, const Entry *entry, gboolean new_window)
{
	GFile *location = g_file_new_for_uri(entry->uri);
	g_signal_emit(self, browser_signals[ITEM_ACTIVATED], 0, location,
		new_window ? GTK_PLACES_OPEN_NEW_WINDOW : GTK_PLACES_OPEN_NORMAL);
	g_object_unref(location);
	return TRUE;
}

static gboolean
fiv_browser_button_press_event(GtkWidget *widget, GdkEventButton *event)
{
	GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->button_press_event(widget, event);

	FivBrowser *self = FIV_BROWSER(widget);
	if (event->type != GDK_BUTTON_PRESS)
		return FALSE;

	guint state = event->state & gtk_accelerator_get_default_mod_mask();
	if (event->button == GDK_BUTTON_PRIMARY && state == 0 &&
		gtk_widget_get_focus_on_click(widget))
		gtk_widget_grab_focus(widget);

	const Entry *entry = entry_at(self, event->x, event->y);
	if (!entry && event->button == GDK_BUTTON_SECONDARY) {
		show_context_menu(widget, fiv_io_model_get_location(self->model));
		return TRUE;
	}
	if (!entry)
		return FALSE;

	switch (event->button) {
	case GDK_BUTTON_PRIMARY:
		if (state == 0)
			return open_entry(widget, entry, FALSE);
		if (state == GDK_CONTROL_MASK)
			return open_entry(widget, entry, TRUE);
		return FALSE;
	case GDK_BUTTON_MIDDLE:
		if (state == 0)
			return open_entry(widget, entry, TRUE);
		return FALSE;
	case GDK_BUTTON_SECONDARY:
		// On X11, after closing the menu, the pointer otherwise remains,
		// no matter what its new location is.
		gdk_window_set_cursor(gtk_widget_get_window(widget), NULL);

		GFile *file = g_file_new_for_uri(entry->uri);
		show_context_menu(widget, file);
		g_object_unref(file);
		return TRUE;
	default:
		return FALSE;
	}
}

gboolean
fiv_browser_motion_notify_event(GtkWidget *widget, GdkEventMotion *event)
{
	GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->motion_notify_event(widget, event);

	FivBrowser *self = FIV_BROWSER(widget);
	if (event->state != 0)
		return FALSE;

	const Entry *entry = entry_at(self, event->x, event->y);
	GdkWindow *window = gtk_widget_get_window(widget);
	gdk_window_set_cursor(window, entry ? self->pointer : NULL);
	return TRUE;
}

static gboolean
fiv_browser_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	if ((event->state & gtk_accelerator_get_default_mod_mask()) !=
		GDK_CONTROL_MASK)
		return FALSE;

	switch (event->direction) {
	case GDK_SCROLL_UP:
		set_item_size(self, self->item_size + 1);
		return TRUE;
	case GDK_SCROLL_DOWN:
		set_item_size(self, self->item_size - 1);
		return TRUE;
	default:
		// For some reason, we can also get GDK_SCROLL_SMOOTH.
		// Left/right are good to steal from GtkScrolledWindow for consistency.
		return TRUE;
	}
}

static gboolean
fiv_browser_query_tooltip(GtkWidget *widget, gint x, gint y,
	G_GNUC_UNUSED gboolean keyboard_tooltip, GtkTooltip *tooltip)
{
	FivBrowser *self = FIV_BROWSER(widget);
	const Entry *entry = entry_at(self, x, y);
	if (!entry)
		return FALSE;

	GFile *file = g_file_new_for_uri(entry->uri);
	GFileInfo *info = g_file_query_info(file,
		G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	g_object_unref(file);
	if (!info)
		return FALSE;

	gtk_tooltip_set_text(tooltip, g_file_info_get_display_name(info));
	g_object_unref(info);
	return TRUE;
}

static void
fiv_browser_style_updated(GtkWidget *widget)
{
	GTK_WIDGET_CLASS(fiv_browser_parent_class)->style_updated(widget);

	FivBrowser *self = FIV_BROWSER(widget);
	GtkStyleContext *style = gtk_widget_get_style_context(widget);
	GtkBorder border = {}, margin = {};

	int item_spacing = self->item_spacing;
	gtk_widget_style_get(widget, "spacing", &self->item_spacing, NULL);
	if (item_spacing != self->item_spacing)
		gtk_widget_queue_resize(widget);

	// Using a pseudo-class, because GTK+ regions are deprecated.
	gtk_style_context_save(style);
	gtk_style_context_add_class(style, "item");
	gtk_style_context_get_margin(style, GTK_STATE_FLAG_NORMAL, &margin);
	gtk_style_context_get_border(style, GTK_STATE_FLAG_NORMAL, &border);
	gtk_style_context_restore(style);

	const int glow_w = (margin.left + margin.right) / 2;
	const int glow_h = (margin.top + margin.bottom) / 2;

	// Don't set different opposing sides, it will misrender, your problem.
	// When the style of the class changes, this virtual method isn't invoked,
	// so the update check is mildly pointless.
	int item_border_x = glow_w + (border.left + border.right) / 2;
	int item_border_y = glow_h + (border.top + border.bottom) / 2;
	if (item_border_x != self->item_border_x ||
		item_border_y != self->item_border_y) {
		self->item_border_x = item_border_x;
		self->item_border_y = item_border_y;
		gtk_widget_queue_resize(widget);
	}

	if (self->glow)
		cairo_surface_destroy(self->glow);
	if (glow_w <= 0 || glow_h <= 0) {
		self->glow = cairo_image_surface_create(CAIRO_FORMAT_A1, 0, 0);
		return;
	}

	self->glow = cairo_image_surface_create(CAIRO_FORMAT_A8, glow_w, glow_h);
	unsigned char *data = cairo_image_surface_get_data(self->glow);
	int stride = cairo_image_surface_get_stride(self->glow);

	// Smooth out the curve, so that the edge of the glow isn't too jarring.
	const double fade_factor = 1.5;

	const int x_max = glow_w - 1;
	const int y_max = glow_h - 1;
	const double x_scale = 1. / MAX(1, x_max);
	const double y_scale = 1. / MAX(1, y_max);
	for (int y = 0; y <= y_max; y++)
		for (int x = 0; x <= x_max; x++) {
			const double xn = x_scale * (x_max - x);
			const double yn = y_scale * (y_max - y);
			double v = MIN(sqrt(xn * xn + yn * yn), 1);
			data[y * stride + x] = round(pow(1 - v, fade_factor) * 255);
		}
	cairo_surface_mark_dirty(self->glow);
}

static void
fiv_browser_class_init(FivBrowserClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fiv_browser_finalize;
	object_class->get_property = fiv_browser_get_property;
	object_class->set_property = fiv_browser_set_property;

	browser_properties[PROP_THUMBNAIL_SIZE] = g_param_spec_enum(
		"thumbnail-size", "Thumbnail size", "The thumbnail height to use",
		FIV_TYPE_THUMBNAIL_SIZE, FIV_THUMBNAIL_SIZE_NORMAL,
		G_PARAM_READWRITE);
	g_object_class_install_properties(
		object_class, N_PROPERTIES, browser_properties);

	browser_signals[ITEM_ACTIVATED] = g_signal_new("item-activated",
		G_TYPE_FROM_CLASS(klass), 0, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_FILE, GTK_TYPE_PLACES_OPEN_FLAGS);

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->get_request_mode = fiv_browser_get_request_mode;
	widget_class->get_preferred_width = fiv_browser_get_preferred_width;
	widget_class->get_preferred_height_for_width =
		fiv_browser_get_preferred_height_for_width;
	widget_class->realize = fiv_browser_realize;
	widget_class->draw = fiv_browser_draw;
	widget_class->size_allocate = fiv_browser_size_allocate;
	widget_class->button_press_event = fiv_browser_button_press_event;
	widget_class->motion_notify_event = fiv_browser_motion_notify_event;
	widget_class->scroll_event = fiv_browser_scroll_event;
	widget_class->query_tooltip = fiv_browser_query_tooltip;
	widget_class->style_updated = fiv_browser_style_updated;

	// Could be split to also-idiomatic row-spacing/column-spacing properties.
	// The GParamSpec is sinked by this call.
	gtk_widget_class_install_style_property(widget_class,
		g_param_spec_int("spacing", "Spacing", "Space between items",
			0, G_MAXINT, 1, G_PARAM_READWRITE));

	// TODO(p): Later override "screen_changed", recreate Pango layouts there,
	// if we get to have any, or otherwise reflect DPI changes.
	gtk_widget_class_set_css_name(widget_class, "fiv-browser");
}

static void
fiv_browser_init(FivBrowser *self)
{
	gtk_widget_set_can_focus(GTK_WIDGET(self), TRUE);
	gtk_widget_set_has_tooltip(GTK_WIDGET(self), TRUE);

	self->entries = g_array_new(FALSE, TRUE, sizeof(Entry));
	g_array_set_clear_func(self->entries, (GDestroyNotify) entry_free);
	self->layouted_rows = g_array_new(FALSE, TRUE, sizeof(Row));
	g_array_set_clear_func(self->layouted_rows, (GDestroyNotify) row_free);

	set_item_size(self, FIV_THUMBNAIL_SIZE_NORMAL);
	self->selected = -1;
	self->glow = cairo_image_surface_create(CAIRO_FORMAT_A1, 0, 0);

	g_signal_connect_swapped(gtk_settings_get_default(),
		"notify::gtk-icon-theme-name", G_CALLBACK(reload_thumbnails), self);
}

// --- Public interface --------------------------------------------------------

static void
on_model_files_changed(FivIoModel *model, FivBrowser *self)
{
	g_return_if_fail(model == self->model);

	// TODO(p): Later implement arguments.
	thumbnailer_abort(self);
	g_array_set_size(self->entries, 0);
	g_array_set_size(self->layouted_rows, 0);

	GPtrArray *files = fiv_io_model_get_files(self->model);
	for (guint i = 0; i < files->len; i++) {
		g_array_append_val(self->entries,
			((Entry) {.thumbnail = NULL, .uri = files->pdata[i]}));
		files->pdata[i] = NULL;
	}
	g_ptr_array_free(files, TRUE);

	reload_thumbnails(self);
	thumbnailer_start(self);
}

GtkWidget *
fiv_browser_new(FivIoModel *model)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(model), NULL);

	FivBrowser *self = g_object_new(FIV_TYPE_BROWSER, NULL);
	self->model = g_object_ref(model);

	g_signal_connect(
		self->model, "files-changed", G_CALLBACK(on_model_files_changed), self);
	on_model_files_changed(self->model, self);
	return GTK_WIDGET(self);
}

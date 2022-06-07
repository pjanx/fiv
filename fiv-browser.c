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

typedef struct entry Entry;
typedef struct item Item;
typedef struct row Row;

typedef struct {
	FivBrowser *self;                   ///< Parent browser
	Entry *target;                      ///< Currently processed Entry pointer
	GSubprocess *minion;                ///< A slave for the current queue head
	GCancellable *cancel;               ///< Cancellable handle
} Thumbnailer;

struct _FivBrowser {
	GtkWidget parent_instance;
	GtkAdjustment *hadjustment;         ///< GtkScrollable boilerplate
	GtkAdjustment *vadjustment;         ///< GtkScrollable boilerplate
	GtkScrollablePolicy hscroll_policy; ///< GtkScrollable boilerplate
	GtkScrollablePolicy vscroll_policy; ///< GtkScrollable boilerplate

	FivThumbnailSize item_size;         ///< Thumbnail size
	int item_height;                    ///< Thumbnail height in pixels
	int item_spacing;                   ///< Space between items in pixels

	FivIoModel *model;                  ///< Filesystem model
	GArray *entries;                    ///< []Entry
	GArray *layouted_rows;              ///< []Row
	const Entry *selected;              ///< Selected entry or NULL

	guint tracked_button;               ///< Pressed mouse button number or 0
	double drag_begin_x;                ///< Viewport start X coordinate or -1
	double drag_begin_y;                ///< Viewport start Y coordinate or -1

	GHashTable *thumbnail_cache;        ///< [URI]cairo_surface_t, for item_size

	Thumbnailer *thumbnailers;          ///< Parallelized thumbnailers
	size_t thumbnailers_len;            ///< Thumbnailers array size
	GList *thumbnailers_queue;          ///< Queued up Entry pointers

	GdkCursor *pointer;                 ///< Cached pointer cursor
	cairo_surface_t *glow;              ///< CAIRO_FORMAT_A8 mask
	int item_border_x;                  ///< L/R .item margin + border
	int item_border_y;                  ///< T/B .item margin + border
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// The "last modified" timestamp of source images for thumbnails.
static cairo_user_data_key_t fiv_browser_key_mtime_msec;

struct entry {
	gchar *uri;                         ///< GIO URI
	gint64 mtime_msec;                  ///< Modification time in milliseconds
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
	gsize len;                          ///< Length of items
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
	Row row = {.x_offset = x, .y_offset = *y};
	row.items = g_array_steal(items_array, &row.len),
	g_array_append_val(self->layouted_rows, row);

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
	// Whatever self->drag_begin_* used to point at might no longer be there,
	// but thumbnail reloading would disrupt mouse clicks if we cleared them.

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
	int total_height = y + padding.bottom;
	if (self->hadjustment) {
		// TODO(p): Set it to the width. Ideally, bump it to the minimum width.
	}
	if (self->vadjustment) {
		gtk_adjustment_set_lower(self->vadjustment, 0);
		gtk_adjustment_set_upper(self->vadjustment, total_height);
		gtk_adjustment_set_page_size(
			self->vadjustment, gtk_widget_get_allocated_height(widget));
		gtk_adjustment_set_page_increment(
			self->vadjustment, gtk_widget_get_allocated_height(widget));
		gtk_adjustment_set_step_increment(self->vadjustment,
			self->item_height + self->item_spacing + 2 * self->item_border_y);
	}
	return total_height;
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
	if (self->hadjustment)
		x += round(gtk_adjustment_get_value(self->hadjustment));
	if (self->vadjustment)
		y += round(gtk_adjustment_get_value(self->vadjustment));

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

static GdkRectangle
entry_rect(FivBrowser *self, const Entry *entry)
{
	GdkRectangle rect = {};
	for (guint i = 0; i < self->layouted_rows->len; i++) {
		const Row *row = &g_array_index(self->layouted_rows, Row, i);
		for (Item *item = row->items; item->entry; item++) {
			if (item->entry == entry) {
				rect = item_extents(self, item, row);
				break;
			}
		}
	}
	if (self->hadjustment)
		rect.x -= round(gtk_adjustment_get_value(self->hadjustment));
	if (self->vadjustment)
		rect.y -= round(gtk_adjustment_get_value(self->vadjustment));
	return rect;
}

static void
draw_row(FivBrowser *self, cairo_t *cr, const Row *row)
{
	GtkStyleContext *style = gtk_widget_get_style_context(GTK_WIDGET(self));
	gtk_style_context_save(style);
	gtk_style_context_add_class(style, "item");

	GtkBorder border;
	GtkStateFlags common_state = gtk_style_context_get_state(style);
	gtk_style_context_get_border(style, common_state, &border);
	for (Item *item = row->items; item->entry; item++) {
		cairo_save(cr);
		GdkRectangle extents = item_extents(self, item, row);
		cairo_translate(cr, extents.x - border.left, extents.y - border.top);

		GtkStateFlags state = common_state;
		if (item->entry == self->selected)
			state |= GTK_STATE_FLAG_SELECTED;

		gtk_style_context_save(style);
		gtk_style_context_set_state(style, state);
		if (item->entry->icon) {
			gtk_style_context_add_class(style, "symbolic");
		} else {
			GdkRGBA glow_color = {};
			gtk_style_context_get_color(style, state, &glow_color);
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

			// Here, we could consider multiplying
			// the whole rectangle with the selection color.
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
	cairo_surface_t *cached =
		g_hash_table_lookup(browser->thumbnail_cache, self->uri);
	if (cached &&
		(intptr_t) cairo_surface_get_user_data(
			cached, &fiv_browser_key_mtime_msec) == self->mtime_msec) {
		self->thumbnail = cairo_surface_reference(cached);
		// TODO(p): If this hit is low-quality, see if a high-quality thumbnail
		// hasn't been produced without our knowledge (avoid launching a minion
		// unnecessarily; we might also shift the concern there).
	} else {
		cairo_surface_t *found = fiv_thumbnail_lookup(
			self->uri, self->mtime_msec, browser->item_size);
		self->thumbnail = rescale_thumbnail(found, browser->item_height);
	}

	if (self->thumbnail) {
		// This choice of mtime favours unnecessary thumbnail reloading.
		cairo_surface_set_user_data(self->thumbnail,
			&fiv_browser_key_mtime_msec, (void *) (intptr_t) self->mtime_msec,
			NULL);
		return;
	}

	// Fall back to symbolic icons, though there's only so much we can do
	// in parallel--GTK+ isn't thread-safe.
	GFile *file = g_file_new_for_uri(self->uri);
	GFileInfo *info = g_file_query_info(file,
		G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	g_object_unref(file);
	if (info) {
		GIcon *icon = g_file_info_get_symbolic_icon(info);
		if (icon)
			self->icon = g_object_ref(icon);
		g_object_unref(info);
	}

	// The GVfs backend may not be friendly.
	if (!self->icon)
		self->icon = g_icon_new_for_string("text-x-generic-symbolic", NULL);
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

	// Once a URI disappears from the model, its thumbnail is forgotten.
	g_hash_table_remove_all(self->thumbnail_cache);

	for (guint i = 0; i < self->entries->len; i++) {
		Entry *entry = &g_array_index(self->entries, Entry, i);
		if (entry->thumbnail) {
			g_hash_table_insert(self->thumbnail_cache, g_strdup(entry->uri),
				cairo_surface_reference(entry->thumbnail));
		}

		materialize_icon(self, entry);
	}

	gtk_widget_queue_resize(GTK_WIDGET(self));
}

// --- Minion management -------------------------------------------------------

#if !GLIB_CHECK_VERSION(2, 70, 0)
#define g_spawn_check_wait_status g_spawn_check_exit_status
#endif

static gboolean thumbnailer_next(Thumbnailer *t);

static void
thumbnailer_reprocess_entry(FivBrowser *self, GBytes *output, Entry *entry)
{
	g_clear_object(&entry->icon);
	g_clear_pointer(&entry->thumbnail, cairo_surface_destroy);

	gtk_widget_queue_resize(GTK_WIDGET(self));

	guint64 flags = 0;
	if (!output || !(entry->thumbnail = rescale_thumbnail(
			fiv_io_deserialize(output, &flags), self->item_height))) {
		entry_add_thumbnail(entry, self);
		materialize_icon(self, entry);
		return;
	}
	if ((flags & FIV_IO_SERIALIZE_LOW_QUALITY)) {
		cairo_surface_set_user_data(entry->thumbnail, &fiv_thumbnail_key_lq,
			(void *) (intptr_t) 1, NULL);

		// TODO(p): Improve complexity; this will iterate the whole linked list.
		self->thumbnailers_queue =
			g_list_append(self->thumbnailers_queue, entry);
	}

	// This choice of mtime favours unnecessary thumbnail reloading.
	cairo_surface_set_user_data(entry->thumbnail,
		&fiv_browser_key_mtime_msec, (void *) (intptr_t) entry->mtime_msec,
		NULL);
	g_hash_table_insert(self->thumbnail_cache, g_strdup(entry->uri),
		cairo_surface_reference(entry->thumbnail));
}

static void
on_thumbnailer_ready(GObject *object, GAsyncResult *res, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS(object);
	Thumbnailer *t = user_data;

	// Reading out pixel data directly from a thumbnailer serves two purposes:
	// 1. it avoids pointless delays with large thumbnail sizes,
	// 2. it enables thumbnailing things that cannot be placed in the cache.
	GError *error = NULL;
	GBytes *out = NULL;
	gboolean succeeded = FALSE;
	if (!g_subprocess_communicate_finish(subprocess, res, &out, NULL, &error)) {
		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free(error);
			return;
		}
	} else if (!g_subprocess_get_if_exited(subprocess)) {
		// If it exited, it probably printed its own message.
		g_spawn_check_wait_status(g_subprocess_get_status(subprocess), &error);
	} else {
		succeeded = g_subprocess_get_exit_status(subprocess) == EXIT_SUCCESS;
	}

	if (error) {
		g_warning("%s", error->message);
		g_error_free(error);
	}

	g_return_if_fail(subprocess == t->minion);
	g_clear_object(&t->minion);
	if (!t->target) {
		g_warning("finished thumbnailing an unknown image");
		g_clear_pointer(&out, g_bytes_unref);
		return;
	}

	if (succeeded)
		thumbnailer_reprocess_entry(t->self, out, t->target);
	else
		g_clear_pointer(&out, g_bytes_unref);

	t->target = NULL;
	thumbnailer_next(t);
}

static gboolean
thumbnailer_next(Thumbnailer *t)
{
	// TODO(p): Try to keep the minions alive (stdout will be a problem).
	FivBrowser *self = t->self;
	GList *link = self->thumbnailers_queue;
	if (!link)
		return FALSE;

	t->target = link->data;
	self->thumbnailers_queue =
		g_list_delete_link(self->thumbnailers_queue, self->thumbnailers_queue);

	// Case analysis:
	//  - We haven't found any thumbnail for the entry at all
	//    (and it has a symbolic icon as a result):
	//    we want to fill the void ASAP, so go for embedded thumbnails first.
	//  - We've found one, but we're not quite happy with it:
	//    always run the full process for a high-quality wide thumbnail.
	//  - We can't end up here in any other cases.
	const char *argv_faster[] = {PROJECT_NAME, "--extract-thumbnail",
		"--thumbnail", fiv_thumbnail_sizes[self->item_size].thumbnail_spec_name,
		"--", t->target->uri, NULL};
	const char *argv_slower[] = {PROJECT_NAME,
		"--thumbnail", fiv_thumbnail_sizes[self->item_size].thumbnail_spec_name,
		"--", t->target->uri, NULL};

	GError *error = NULL;
	t->minion = g_subprocess_newv(t->target->icon ? argv_faster : argv_slower,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
	if (error) {
		g_warning("%s", error->message);
		g_error_free(error);
		return FALSE;
	}

	t->cancel = g_cancellable_new();
	g_subprocess_communicate_async(
		t->minion, NULL, t->cancel, on_thumbnailer_ready, t);
	return TRUE;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
thumbnailers_abort(FivBrowser *self)
{
	g_list_free(self->thumbnailers_queue);
	self->thumbnailers_queue = NULL;

	for (size_t i = 0; i < self->thumbnailers_len; i++) {
		Thumbnailer *t = self->thumbnailers + i;
		if (t->cancel) {
			g_cancellable_cancel(t->cancel);
			g_clear_object(&t->cancel);
		}

		// Just let them exit on their own.
		g_clear_object(&t->minion);
		t->target = NULL;
	}
}

static void
thumbnailers_start(FivBrowser *self)
{
	thumbnailers_abort(self);
	if (!self->model)
		return;

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

	self->thumbnailers_queue = g_list_concat(missing, lq);
	for (size_t i = 0; i < self->thumbnailers_len; i++) {
		if (!thumbnailer_next(self->thumbnailers + i))
			break;
	}
}

// --- Context menu-------------------------------------------------------------

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

static GtkMenu *
make_context_menu(GtkWidget *widget, GFile *file)
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

static void
show_context_menu(GtkWidget *widget, GFile *file)
{
	gtk_menu_popup_at_pointer(make_context_menu(widget, file), NULL);
}

// --- Boilerplate -------------------------------------------------------------

G_DEFINE_TYPE_EXTENDED(FivBrowser, fiv_browser, GTK_TYPE_WIDGET, 0,
	G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, NULL))

enum {
	PROP_THUMBNAIL_SIZE = 1,
	N_PROPERTIES,

	// These are overriden, we do not register them.
	PROP_HADJUSTMENT,
	PROP_VADJUSTMENT,
	PROP_HSCROLL_POLICY,
	PROP_VSCROLL_POLICY,
};

static GParamSpec *browser_properties[N_PROPERTIES];

enum {
	ITEM_ACTIVATED,
	LAST_SIGNAL,
};

// Globals are, sadly, the canonical way of storing signal numbers.
static guint browser_signals[LAST_SIGNAL];

static void
on_adjustment_value_changed(
	G_GNUC_UNUSED GtkAdjustment *adjustment, gpointer user_data)
{
	FivBrowser *self = FIV_BROWSER(user_data);
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

static gboolean
replace_adjustment(
	FivBrowser *self, GtkAdjustment **adjustment, GtkAdjustment *replacement)
{
	if (*adjustment == replacement)
		return FALSE;

	if (*adjustment) {
		g_signal_handlers_disconnect_by_func(
			*adjustment, on_adjustment_value_changed, self);
		g_clear_object(adjustment);
	}
	if (replacement) {
		*adjustment = g_object_ref(replacement);
		g_signal_connect(*adjustment, "value-changed",
			G_CALLBACK(on_adjustment_value_changed), self);
		// TODO(p): We should set it up, as it is done in relayout().
	}
	return TRUE;
}

static void
fiv_browser_finalize(GObject *gobject)
{
	FivBrowser *self = FIV_BROWSER(gobject);
	thumbnailers_abort(self);
	g_array_free(self->entries, TRUE);
	g_array_free(self->layouted_rows, TRUE);
	if (self->model) {
		g_signal_handlers_disconnect_by_data(self->model, self);
		g_clear_object(&self->model);
	}

	g_hash_table_destroy(self->thumbnail_cache);

	cairo_surface_destroy(self->glow);
	g_clear_object(&self->pointer);

	replace_adjustment(self, &self->hadjustment, NULL);
	replace_adjustment(self, &self->vadjustment, NULL);

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
	case PROP_HADJUSTMENT:
		g_value_set_object(value, self->hadjustment);
		break;
	case PROP_VADJUSTMENT:
		g_value_set_object(value, self->vadjustment);
		break;
	case PROP_HSCROLL_POLICY:
		g_value_set_enum(value, self->hscroll_policy);
		break;
	case PROP_VSCROLL_POLICY:
		g_value_set_enum(value, self->vscroll_policy);
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

		g_hash_table_remove_all(self->thumbnail_cache);
		reload_thumbnails(self);
		thumbnailers_start(self);

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
	case PROP_HADJUSTMENT:
		if (replace_adjustment(
				self, &self->hadjustment, g_value_get_object(value)))
			g_object_notify_by_pspec(object, pspec);
		break;
	case PROP_VADJUSTMENT:
		if (replace_adjustment(
				self, &self->vadjustment, g_value_get_object(value)))
			g_object_notify_by_pspec(object, pspec);
		break;
	case PROP_HSCROLL_POLICY:
		if ((gint) self->hscroll_policy != g_value_get_enum(value)) {
			self->hscroll_policy = g_value_get_enum(value);
			gtk_widget_queue_resize(GTK_WIDGET(self));
			g_object_notify_by_pspec(object, pspec);
		}
		break;
	case PROP_VSCROLL_POLICY:
		if ((gint) self->vscroll_policy != g_value_get_enum(value)) {
			self->vscroll_policy = g_value_get_enum(value);
			gtk_widget_queue_resize(GTK_WIDGET(self));
			g_object_notify_by_pspec(object, pspec);
		}
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
		.event_mask = gtk_widget_get_events(widget) | GDK_POINTER_MOTION_MASK |
			GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK |
			GDK_KEY_PRESS_MASK,
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
	FivBrowser *self = FIV_BROWSER(widget);
	GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->size_allocate(widget, allocation);

	int height = relayout(FIV_BROWSER(widget), allocation->width);

	// Avoid fresh blank space.
	if (self->vadjustment) {
		double y1 = gtk_adjustment_get_value(self->vadjustment);
		double ph = gtk_adjustment_get_page_size(self->vadjustment);
		if (y1 + ph > height)
			gtk_adjustment_set_value(self->vadjustment, height - ph);
	}
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

	if (self->hadjustment)
		cairo_translate(
			cr, -round(gtk_adjustment_get_value(self->hadjustment)), 0);
	if (self->vadjustment)
		cairo_translate(
			cr, 0, -round(gtk_adjustment_get_value(self->vadjustment)));

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

static void
abort_button_tracking(FivBrowser *self)
{
	self->tracked_button = 0;
	self->drag_begin_x = self->drag_begin_y = -1;
}

static gboolean
fiv_browser_button_press_event(GtkWidget *widget, GdkEventButton *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->button_press_event(widget, event);

	// Make pressing multiple mouse buttons at once cancel a click.
	if (self->tracked_button) {
		abort_button_tracking(self);
		return TRUE;
	}
	if (event->type != GDK_BUTTON_PRESS)
		return FALSE;

	guint state = event->state & gtk_accelerator_get_default_mod_mask();
	if (event->button == GDK_BUTTON_PRIMARY && state == 0 &&
		gtk_widget_get_focus_on_click(widget))
		gtk_widget_grab_focus(widget);

	const Entry *entry = entry_at(self, event->x, event->y);
	if (!entry && state == 0) {
		if (event->button == GDK_BUTTON_SECONDARY)
			show_context_menu(widget, fiv_io_model_get_location(self->model));
		else if (event->button != GDK_BUTTON_PRIMARY)
			return FALSE;

		if (self->selected) {
			self->selected = NULL;
			gtk_widget_queue_draw(widget);
		}
		return TRUE;
	}

	// In accordance with Nautilus, Thunar, and the mildly confusing
	// Apple Human Interface Guidelines, but not with the ugly Windows User
	// Experience Interaction Guidelines, open the context menu on button press.
	if (entry && event->button == GDK_BUTTON_SECONDARY) {
		self->selected = entry;
		gtk_widget_queue_draw(widget);

		// On X11, after closing the menu, the pointer otherwise remains,
		// no matter what its new location is.
		gdk_window_set_cursor(gtk_widget_get_window(widget), NULL);

		GFile *file = g_file_new_for_uri(entry->uri);
		show_context_menu(widget, file);
		g_object_unref(file);
		return TRUE;
	}

	// gtk_drag_source_set() would span the whole widget area, we'd have to
	// un/set it as needed, in particular to handle empty space.
	// It might be a good idea to use GtkGestureDrag instead.
	if (event->button == GDK_BUTTON_PRIMARY ||
		event->button == GDK_BUTTON_MIDDLE) {
		self->tracked_button = event->button;
		self->drag_begin_x = event->x;
		self->drag_begin_y = event->y;
		return TRUE;
	}
	return FALSE;
}

static gboolean
fiv_browser_button_release_event(GtkWidget *widget, GdkEventButton *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->button_release_event(widget, event);
	if (event->button != self->tracked_button)
		return FALSE;

	// Middle clicks should only work on the starting entry.
	const Entry *entry = entry_at(self, self->drag_begin_x, self->drag_begin_y);
	abort_button_tracking(self);
	if (!entry || entry != entry_at(self, event->x, event->y))
		return FALSE;

	guint state = event->state & gtk_accelerator_get_default_mod_mask();
	if ((event->button == GDK_BUTTON_PRIMARY && state == 0))
		return open_entry(widget, entry, FALSE);
	if ((event->button == GDK_BUTTON_PRIMARY && state == GDK_CONTROL_MASK) ||
		(event->button == GDK_BUTTON_MIDDLE && state == 0))
		return open_entry(widget, entry, TRUE);
	return FALSE;
}

static gboolean
fiv_browser_motion_notify_event(GtkWidget *widget, GdkEventMotion *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->motion_notify_event(widget, event);

	guint state = event->state & gtk_accelerator_get_default_mod_mask();
	if (state != 0 || self->tracked_button != GDK_BUTTON_PRIMARY ||
		!gtk_drag_check_threshold(widget, self->drag_begin_x,
			self->drag_begin_y, event->x, event->y)) {
		const Entry *entry = entry_at(self, event->x, event->y);
		GdkWindow *window = gtk_widget_get_window(widget);
		gdk_window_set_cursor(window, entry ? self->pointer : NULL);
		return FALSE;
	}

	// The "correct" behaviour is to set the selection on a left mouse button
	// press immediately, but that is regarded as visual noise.
	const Entry *entry = entry_at(self, self->drag_begin_x, self->drag_begin_y);
	abort_button_tracking(self);
	if (!entry)
		return TRUE;

	self->selected = entry;
	gtk_widget_queue_draw(widget);

	GtkTargetList *target_list = gtk_target_list_new(NULL, 0);
	gtk_target_list_add_uri_targets(target_list, 0);
	GdkDragAction actions = GDK_ACTION_COPY | GDK_ACTION_MOVE |
		GDK_ACTION_LINK | GDK_ACTION_ASK;
	gtk_drag_begin_with_coordinates(widget, target_list, actions,
		self->tracked_button, (GdkEvent *) event, event->x, event->y);
	gtk_target_list_unref(target_list);
	return TRUE;
}

static gboolean
fiv_browser_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	abort_button_tracking(self);
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

static void
fiv_browser_drag_begin(GtkWidget *widget, GdkDragContext *context)
{
	FivBrowser *self = FIV_BROWSER(widget);
	if (self->selected) {
		// There doesn't seem to be a size limit.
		gtk_drag_set_icon_surface(context, self->selected->thumbnail);
	}
}

static void
fiv_browser_drag_data_get(GtkWidget *widget,
	G_GNUC_UNUSED GdkDragContext *context, GtkSelectionData *data,
	G_GNUC_UNUSED guint info, G_GNUC_UNUSED guint time)
{
	FivBrowser *self = FIV_BROWSER(widget);
	if (self->selected) {
		(void) gtk_selection_data_set_uris(
			data, (gchar *[]){self->selected->uri, NULL});
	}
}

static void
select_closest(FivBrowser *self, const Row *row, int target)
{
	int closest = G_MAXINT;
	for (guint i = 0; i < row->len; i++) {
		GdkRectangle extents = item_extents(self, row->items + i, row);
		int distance = ABS(extents.x + extents.width / 2 - target);
		if (distance > closest)
			break;
		self->selected = row->items[i].entry;
		closest = distance;
	}
}

static void
scroll_to_row(FivBrowser *self, const Row *row)
{
	if (!self->vadjustment)
		return;

	double y1 = gtk_adjustment_get_value(self->vadjustment);
	double ph = gtk_adjustment_get_page_size(self->vadjustment);
	if (row->y_offset < y1) {
		gtk_adjustment_set_value(
			self->vadjustment, row->y_offset - self->item_border_y);
	} else if (row->y_offset + self->item_height > y1 + ph) {
		gtk_adjustment_set_value(self->vadjustment,
			row->y_offset - ph + self->item_height + self->item_border_y);
	}
}

static void
move_selection(FivBrowser *self, GtkDirectionType dir)
{
	GtkWidget *widget = GTK_WIDGET(self);
	if (!self->layouted_rows->len)
		return;

	const Row *selected_row = NULL;
	if (!self->selected) {
		switch (dir) {
		case GTK_DIR_RIGHT:
		case GTK_DIR_DOWN:
			selected_row = &g_array_index(self->layouted_rows, Row, 0);
			self->selected = selected_row->items->entry;
			goto adjust;
		case GTK_DIR_LEFT:
		case GTK_DIR_UP:
			selected_row = &g_array_index(
				self->layouted_rows, Row, self->layouted_rows->len - 1);
			self->selected = selected_row->items[selected_row->len - 1].entry;
			goto adjust;
		default:
			g_assert_not_reached();
		}
	}

	gsize x = 0, y = 0;
	int target_offset = 0;
	for (y = 0; y < self->layouted_rows->len; y++) {
		const Row *row = &g_array_index(self->layouted_rows, Row, y);
		for (x = 0; x < row->len; x++) {
			const Item *item = row->items + x;
			if (item->entry == self->selected) {
				GdkRectangle extents = item_extents(self, item, row);
				target_offset = extents.x + extents.width / 2;
				goto found;
			}
		}
	}
found:
	g_return_if_fail(y < self->layouted_rows->len);
	selected_row = &g_array_index(self->layouted_rows, Row, y);

	switch (dir) {
	case GTK_DIR_LEFT:
		if (x > 0) {
			self->selected = selected_row->items[--x].entry;
		} else if (y-- > 0) {
			selected_row = &g_array_index(self->layouted_rows, Row, y);
			self->selected = selected_row->items[selected_row->len - 1].entry;
		}
		break;
	case GTK_DIR_RIGHT:
		if (++x < selected_row->len) {
			self->selected = selected_row->items[x].entry;
		} else if (++y < self->layouted_rows->len) {
			selected_row = &g_array_index(self->layouted_rows, Row, y);
			self->selected = selected_row->items[0].entry;
		}
		break;
	case GTK_DIR_UP:
		if (y-- > 0) {
			selected_row = &g_array_index(self->layouted_rows, Row, y);
			select_closest(self, selected_row, target_offset);
		}
		break;
	case GTK_DIR_DOWN:
		if (++y < self->layouted_rows->len) {
			selected_row = &g_array_index(self->layouted_rows, Row, y);
			select_closest(self, selected_row, target_offset);
		}
		break;
	default:
		g_assert_not_reached();
	}

adjust:
	// TODO(p): We should also do it horizontally, although we don't use it.
	scroll_to_row(self, selected_row);
	gtk_widget_queue_draw(widget);
}

static void
move_selection_home(FivBrowser *self)
{
	if (self->layouted_rows->len) {
		const Row *row = &g_array_index(self->layouted_rows, Row, 0);
		self->selected = row->items[0].entry;
		scroll_to_row(self, row);
		gtk_widget_queue_draw(GTK_WIDGET(self));
	}
}

static void
move_selection_end(FivBrowser *self)
{
	if (self->layouted_rows->len) {
		const Row *row = &g_array_index(
			self->layouted_rows, Row, self->layouted_rows->len - 1);
		self->selected = row->items[row->len - 1].entry;
		scroll_to_row(self, row);
		gtk_widget_queue_draw(GTK_WIDGET(self));
	}
}

static gboolean
fiv_browser_key_press_event(GtkWidget *widget, GdkEventKey *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	switch ((event->state & gtk_accelerator_get_default_mod_mask())) {
	case 0:
		switch (event->keyval) {
		case GDK_KEY_Return:
			if (self->selected)
				return open_entry(widget, self->selected, FALSE);
			return TRUE;
		case GDK_KEY_Left:
			move_selection(self, GTK_DIR_LEFT);
			return TRUE;
		case GDK_KEY_Right:
			move_selection(self, GTK_DIR_RIGHT);
			return TRUE;
		case GDK_KEY_Up:
			move_selection(self, GTK_DIR_UP);
			return TRUE;
		case GDK_KEY_Down:
			move_selection(self, GTK_DIR_DOWN);
			return TRUE;
		case GDK_KEY_Home:
			move_selection_home(self);
			return TRUE;
		case GDK_KEY_End:
			move_selection_end(self);
			return TRUE;
		}
		break;
	case GDK_CONTROL_MASK:
	case GDK_CONTROL_MASK | GDK_SHIFT_MASK:
		switch (event->keyval) {
		case GDK_KEY_plus:
			set_item_size(self, self->item_size + 1);
			return TRUE;
		case GDK_KEY_minus:
			set_item_size(self, self->item_size - 1);
			return TRUE;
		}
	}

	return GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->key_press_event(widget, event);
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

static gboolean
fiv_browser_popup_menu(GtkWidget *widget)
{
	FivBrowser *self = FIV_BROWSER(widget);

	// This is what Windows Explorer does, and what you want to be done.
	// Although invoking the menu outside the widget is questionable.
	GFile *file = NULL;
	GdkRectangle rect = {};
	if (self->selected) {
		file = g_file_new_for_uri(self->selected->uri);
		rect = entry_rect(self, self->selected);
		rect.x += rect.width / 2;
		rect.y += rect.height / 2;
	} else {
		file = g_object_ref(fiv_io_model_get_location(self->model));
	}

	gtk_menu_popup_at_rect(make_context_menu(widget, file),
		gtk_widget_get_window(widget), &rect, GDK_GRAVITY_NORTH_WEST,
		GDK_GRAVITY_NORTH_WEST, NULL);
	g_object_unref(file);
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

	g_object_class_override_property(
		object_class, PROP_HADJUSTMENT, "hadjustment");
	g_object_class_override_property(
		object_class, PROP_VADJUSTMENT, "vadjustment");
	g_object_class_override_property(
		object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
	g_object_class_override_property(
		object_class, PROP_VSCROLL_POLICY, "vscroll-policy");

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
	widget_class->button_release_event = fiv_browser_button_release_event;
	widget_class->motion_notify_event = fiv_browser_motion_notify_event;
	widget_class->drag_begin = fiv_browser_drag_begin;
	widget_class->drag_data_get = fiv_browser_drag_data_get;
	widget_class->scroll_event = fiv_browser_scroll_event;
	widget_class->key_press_event = fiv_browser_key_press_event;
	widget_class->query_tooltip = fiv_browser_query_tooltip;
	widget_class->popup_menu = fiv_browser_popup_menu;
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
	abort_button_tracking(self);

	self->thumbnail_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, (GDestroyNotify) cairo_surface_destroy);

	self->thumbnailers_len = g_get_num_processors();
	self->thumbnailers =
		g_malloc0_n(self->thumbnailers_len, sizeof *self->thumbnailers);
	for (size_t i = 0; i < self->thumbnailers_len; i++)
		self->thumbnailers[i].self = self;

	set_item_size(self, FIV_THUMBNAIL_SIZE_NORMAL);
	self->glow = cairo_image_surface_create(CAIRO_FORMAT_A1, 0, 0);

	g_signal_connect_swapped(gtk_settings_get_default(),
		"notify::gtk-icon-theme-name", G_CALLBACK(reload_thumbnails), self);
}

// --- Public interface --------------------------------------------------------

// TODO(p): Later implement any arguments of this FivIoModel signal.
static void
on_model_files_changed(FivIoModel *model, FivBrowser *self)
{
	g_return_if_fail(model == self->model);

	gchar *selected_uri = NULL;
	if (self->selected)
		selected_uri = g_strdup(self->selected->uri);

	thumbnailers_abort(self);
	g_array_set_size(self->entries, 0);
	g_array_set_size(self->layouted_rows, 0);

	gsize len = 0;
	const FivIoModelEntry *files = fiv_io_model_get_files(self->model, &len);
	for (gsize i = 0; i < len; i++) {
		g_array_append_val(self->entries, ((Entry) {.thumbnail = NULL,
			.uri = g_strdup(files[i].uri), .mtime_msec = files[i].mtime_msec}));
	}

	fiv_browser_select(self, selected_uri);
	g_free(selected_uri);

	reload_thumbnails(self);
	thumbnailers_start(self);
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

static void
scroll_to_selection(FivBrowser *self)
{
	for (gsize y = 0; y < self->layouted_rows->len; y++) {
		const Row *row = &g_array_index(self->layouted_rows, Row, y);
		for (gsize x = 0; x < row->len; x++) {
			if (row->items[x].entry == self->selected) {
				scroll_to_row(self, row);
				return;
			}
		}
	}
}

void
fiv_browser_select(FivBrowser *self, const char *uri)
{
	g_return_if_fail(FIV_IS_BROWSER(self));

	self->selected = NULL;
	gtk_widget_queue_draw(GTK_WIDGET(self));
	if (!uri)
		return;

	for (guint i = 0; i < self->entries->len; i++) {
		const Entry *entry = &g_array_index(self->entries, Entry, i);
		if (!g_strcmp0(entry->uri, uri)) {
			self->selected = entry;
			scroll_to_selection(self);
			break;
		}
	}
}

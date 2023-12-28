//
// fiv-browser.c: filesystem browsing widget
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

#include "config.h"

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif  // GDK_WINDOWING_X11
#ifdef GDK_WINDOWING_QUARTZ
#include <gdk/gdkquartz.h>
#endif  // GDK_WINDOWING_QUARTZ
#include <pixman.h>

#include <math.h>
#include <stdlib.h>

#include "fiv-browser.h"
#include "fiv-collection.h"
#include "fiv-context-menu.h"
#include "fiv-io.h"
#include "fiv-io-model.h"
#include "fiv-thumbnail.h"

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
//                    │       l  a  b  e  l
//                    │    s  p  a  c  i  n  g
//                    │   ╭┄┄┄┄┄┄┄┄┄┄┄┄╮   ╭┄┄┄┄┄┄┄┄┄┄┄┄
//
// The glow is actually a glowing margin, the border is rendered in two parts.
// When labels are hidden, the surrounding spacing is collapsed.
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

	gboolean show_labels;               ///< Show labels underneath items

	FivIoModel *model;                  ///< Filesystem model
	GPtrArray *entries;                 ///< []*Entry
	GArray *layouted_rows;              ///< []Row
	const Entry *selected;              ///< Selected entry or NULL

	guint tracked_button;               ///< Pressed mouse button number or 0
	double drag_begin_x;                ///< Viewport start X coordinate or -1
	double drag_begin_y;                ///< Viewport start Y coordinate or -1

	GHashTable *thumbnail_cache;        ///< [URI]cairo_surface_t, for item_size

	Thumbnailer *thumbnailers;          ///< Parallelized thumbnailers
	size_t thumbnailers_len;            ///< Thumbnailers array size
	GQueue thumbnailers_queue;          ///< Queued up Entry pointers

	GdkCursor *pointer;                 ///< Cached pointer cursor
	cairo_pattern_t *glow;              ///< CAIRO_FORMAT_A8 mask for corners
	cairo_pattern_t *glow_padded;       ///< CAIRO_FORMAT_A8 mask
	int glow_w;                         ///< Glow corner width
	int glow_h;                         ///< Glow corner height
	int item_border_x;                  ///< L/R .item margin + border
	int item_border_y;                  ///< T/B .item margin + border
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// The "last modified" timestamp of source images for thumbnails.
static cairo_user_data_key_t fiv_browser_key_mtime_msec;
/// The original file size of source images for thumbnails.
static cairo_user_data_key_t fiv_browser_key_filesize;

struct entry {
	FivIoModelEntry *e;                 ///< Reference to model entry
	cairo_surface_t *thumbnail;         ///< Prescaled thumbnail
	GIcon *icon;                        ///< If no thumbnail, use this icon

	gboolean removed;                   ///< Model announced removal
};

static Entry *
entry_new(FivIoModelEntry *e)
{
	Entry *self = g_slice_alloc0(sizeof *self);
	self->e = e;
	return self;
}

static void
entry_destroy(Entry *self)
{
	fiv_io_model_entry_unref(self->e);
	g_clear_pointer(&self->thumbnail, cairo_surface_destroy);
	g_clear_object(&self->icon);
	g_slice_free1(sizeof *self, self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct item {
	const Entry *entry;
	PangoLayout *label;                 ///< Label
	int x_offset;                       ///< X offset within the row
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
	for (gsize i = 0; i < self->len; i++)
		g_clear_object(&self->items[i].label);
	g_free(self->items);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static double
row_subheight(const FivBrowser *self, const Row *row)
{
	if (!self->show_labels)
		return 0;

	// If we didn't ellipsize labels, this should be made to account
	// for vertical centering as well.
	int tallest_label = 0;
	for (gsize i = 0; i < row->len; i++) {
		PangoRectangle ink = {}, logical = {};
		pango_layout_get_extents(row->items[i].label, &ink, &logical);

		int height = (logical.y + logical.height) / PANGO_SCALE;
		if (tallest_label < height)
			tallest_label = height;
	}

	return self->item_spacing + tallest_label;
}

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
	*y += row_subheight(self, &row);
}

static int
relayout(FivBrowser *self, int width)
{
	GtkWidget *widget = GTK_WIDGET(self);
	GtkStyleContext *style = gtk_widget_get_style_context(widget);

	GtkBorder padding = {};
	gtk_style_context_get_padding(style, GTK_STATE_FLAG_NORMAL, &padding);
	int available_width = width - padding.left - padding.right, max_width = 0;

	g_array_set_size(self->layouted_rows, 0);
	// Whatever self->drag_begin_* used to point at might no longer be there,
	// but thumbnail reloading would disrupt mouse clicks if we cleared them.

	GArray *items = g_array_new(TRUE, TRUE, sizeof(Item));
	int x = 0, y = padding.top;
	for (guint i = 0; i < self->entries->len; i++) {
		const Entry *entry = self->entries->pdata[i];
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

		PangoLayout *label = NULL;
		if (self->show_labels) {
			label = gtk_widget_create_pango_layout(
				widget, entry->e->display_name);
			pango_layout_set_width(
				label, (width - 2 * self->glow_w) * PANGO_SCALE);
			pango_layout_set_alignment(label, PANGO_ALIGN_CENTER);
			pango_layout_set_wrap(label, PANGO_WRAP_WORD_CHAR);
			pango_layout_set_ellipsize(label, PANGO_ELLIPSIZE_END);

#if PANGO_VERSION_CHECK(1, 44, 0)
			PangoAttrList *attrs = pango_attr_list_new();
			pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));
			pango_layout_set_attributes(label, attrs);
			pango_attr_list_unref (attrs);
#endif
		}

		g_array_append_val(items, ((Item) {
			.entry = entry,
			.label = label,
			.x_offset = x + self->item_border_x,
		}));

		x += width;
		if (max_width < width)
			max_width = width;
	}
	if (items->len) {
		append_row(self, &y,
			padding.left + MAX(0, available_width - x) / 2, items);
	}

	g_array_free(items, TRUE);
	int total_height = y + padding.bottom;
	if (self->hadjustment) {
		gtk_adjustment_set_lower(self->hadjustment, 0);
		gtk_adjustment_set_upper(self->hadjustment,
			width + MAX(0, max_width - available_width));
		gtk_adjustment_set_step_increment(self->hadjustment, width * 0.1);
		gtk_adjustment_set_page_increment(self->hadjustment, width * 0.9);
		gtk_adjustment_set_page_size(self->hadjustment, width);
	}
	if (self->vadjustment) {
		int height = gtk_widget_get_allocated_height(widget);
		gtk_adjustment_set_lower(self->vadjustment, 0);
		gtk_adjustment_set_upper(self->vadjustment, MAX(height, total_height));
		gtk_adjustment_set_step_increment(self->vadjustment,
			self->item_height + self->item_spacing + 2 * self->item_border_y);
		gtk_adjustment_set_page_increment(self->vadjustment, height * 0.9);
		gtk_adjustment_set_page_size(self->vadjustment, height);
	}
	return total_height;
}

static void
draw_outer_border(FivBrowser *self, cairo_t *cr, int width, int height)
{
	cairo_matrix_t matrix;

	cairo_save(cr);
	cairo_translate(cr, -self->glow_w, -self->glow_h);
	cairo_rectangle(cr, 0, 0, self->glow_w + width, self->glow_h + height);
	cairo_clip(cr);
	cairo_mask(cr, self->glow_padded);
	cairo_restore(cr);
	cairo_save(cr);
	cairo_translate(cr, width + self->glow_w, height + self->glow_h);
	cairo_rectangle(cr, 0, 0, -self->glow_w - width, -self->glow_h - height);
	cairo_clip(cr);
	cairo_scale(cr, -1, -1);
	cairo_mask(cr, self->glow_padded);
	cairo_restore(cr);

	cairo_matrix_init_scale(&matrix, -1, 1);
	cairo_matrix_translate(&matrix, -width - self->glow_w, self->glow_h);
	cairo_pattern_set_matrix(self->glow, &matrix);
	cairo_mask(cr, self->glow);
	cairo_matrix_init_scale(&matrix, 1, -1);
	cairo_matrix_translate(&matrix, self->glow_w, -height - self->glow_h);
	cairo_pattern_set_matrix(self->glow, &matrix);
	cairo_mask(cr, self->glow);
}

static GdkRectangle
item_extents(FivBrowser *self, const Item *item, const Row *row)
{
	int width = cairo_image_surface_get_width(item->entry->thumbnail);
	int height = cairo_image_surface_get_height(item->entry->thumbnail);
	return (GdkRectangle) {
		.x = row->x_offset + item->x_offset,
		.y = row->y_offset + (self->item_height - height) / 2,
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
				CAIRO_FORMAT_RGB24 || item->entry->removed) {
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
			// Distinguish removed items by rendering them only faintly.
			if (item->entry->removed)
				cairo_push_group(cr);

			cairo_set_source_surface(
				cr, item->entry->thumbnail, border.left, border.top);
			cairo_paint(cr);

			// Here, we could also consider multiplying
			// the whole rectangle with the selection color.
			if (item->entry->removed) {
				cairo_pop_group_to_source(cr);
				cairo_paint_with_alpha(cr, 0.25);
			}
		}

		// This rendition is about the best I could come up with.
		// It might be possible to use more such emblems with entries,
		// though they would deserve some kind of a blur-glow.
		if (item->entry->removed) {
			int size = 32;
			cairo_surface_t *cross = gtk_icon_theme_load_surface(
				gtk_icon_theme_get_default(), "cross-large-symbolic",
				size, gtk_widget_get_scale_factor(GTK_WIDGET(self)),
				gtk_widget_get_window(GTK_WIDGET(self)),
				GTK_ICON_LOOKUP_FORCE_SYMBOLIC, NULL);
			if (cross) {
				cairo_set_source_rgb(cr, 1, 0, 0);
				cairo_mask_surface(cr, cross,
					border.left + extents.width - size - size / 4,
					border.top + extents.height - size - size / 4);
				cairo_surface_destroy(cross);
			}
		}

		if (self->show_labels) {
			gtk_style_context_save(style);
			gtk_style_context_add_class(style, "label");
			gtk_render_layout(style, cr, -border.left,
				border.top + extents.height + self->item_border_y +
					self->item_spacing,
				item->label);
			gtk_style_context_restore(style);
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

static const char *
entry_system_wide_uri(const Entry *self)
{
	// "recent" and "trash", e.g., also have "standard::target-uri" set,
	// but we'd like to avoid saving their thumbnails.
	if (self->e->target_uri && fiv_collection_uri_matches(self->e->uri))
		return self->e->target_uri;

	return self->e->uri;
}

static void
entry_set_surface_user_data(const Entry *self)
{
	// This choice of mtime favours unnecessary thumbnail reloading
	// over retaining stale data (consider both calling functions).
	cairo_surface_set_user_data(self->thumbnail,
		&fiv_browser_key_mtime_msec, (void *) (intptr_t) self->e->mtime_msec,
		NULL);
	cairo_surface_set_user_data(self->thumbnail,
		&fiv_browser_key_filesize, (void *) (uintptr_t) self->e->filesize,
		NULL);
}

static cairo_surface_t *
entry_lookup_thumbnail(Entry *self, FivBrowser *browser)
{
	cairo_surface_t *cached =
		g_hash_table_lookup(browser->thumbnail_cache, self->e->uri);
	if (cached &&
		(intptr_t) cairo_surface_get_user_data(cached,
			&fiv_browser_key_mtime_msec) == (intptr_t) self->e->mtime_msec &&
		(uintptr_t) cairo_surface_get_user_data(cached,
			&fiv_browser_key_filesize) == (uintptr_t) self->e->filesize) {
		// TODO(p): If this hit is low-quality, see if a high-quality thumbnail
		// hasn't been produced without our knowledge (avoid launching a minion
		// unnecessarily; we might also shift the concern there).
		return cairo_surface_reference(cached);
	}

	cairo_surface_t *found = fiv_thumbnail_lookup(
		entry_system_wide_uri(self), self->e->mtime_msec, self->e->filesize,
		browser->item_size);
	return rescale_thumbnail(found, browser->item_height);
}

static void
entry_add_thumbnail(gpointer data, gpointer user_data)
{
	Entry *self = data;
	FivBrowser *browser = FIV_BROWSER(user_data);
	if (self->removed) {
		// Keep whatever size of thumbnail we had at the time up until reload.
		// g_file_query_info() fails for removed files, so keep the icon, too.
		if (self->icon) {
			g_clear_pointer(&self->thumbnail, cairo_surface_destroy);
		} else {
			self->thumbnail =
				rescale_thumbnail(self->thumbnail, browser->item_height);
		}
		return;
	}

	g_clear_object(&self->icon);
	g_clear_pointer(&self->thumbnail, cairo_surface_destroy);

	if ((self->thumbnail = entry_lookup_thumbnail(self, browser))) {
		// Yes, this is a pointless action in case it's been found in the cache.
		entry_set_surface_user_data(self);
		return;
	}

	// Fall back to symbolic icons, though there's only so much we can do
	// in parallel--GTK+ isn't thread-safe.
	GFile *file = g_file_new_for_uri(self->e->uri);
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
	// TODO(p): We might want to populate these on an as-needed basis.
	GtkIconTheme *theme = gtk_icon_theme_get_default();
	GtkIconInfo *icon_info = gtk_icon_theme_lookup_by_gicon(theme, entry->icon,
		self->item_height / 2, GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
	if (!icon_info) {
		// This icon is included within GTK+.
		icon_info = gtk_icon_theme_lookup_icon(theme, "text-x-generic",
			self->item_height / 2, GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
	}
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
reload_one_thumbnail_finish(FivBrowser *self, Entry *entry)
{
	if (!entry->removed && entry->thumbnail) {
		g_hash_table_insert(self->thumbnail_cache, g_strdup(entry->e->uri),
			cairo_surface_reference(entry->thumbnail));
	}

	materialize_icon(self, entry);
}

static void
reload_one_thumbnail(FivBrowser *self, Entry *entry)
{
	entry_add_thumbnail(entry, self);
	reload_one_thumbnail_finish(self, entry);

	gtk_widget_queue_resize(GTK_WIDGET(self));
}

static void
reload_thumbnails(FivBrowser *self)
{
	GThreadPool *pool = g_thread_pool_new(
		entry_add_thumbnail, self, g_get_num_processors(), FALSE, NULL);
	for (guint i = 0; i < self->entries->len; i++)
		g_thread_pool_push(pool, self->entries->pdata[i], NULL);
	g_thread_pool_free(pool, FALSE, TRUE);

	// Once a URI disappears from the model, its thumbnail is forgotten.
	g_hash_table_remove_all(self->thumbnail_cache);
	for (guint i = 0; i < self->entries->len; i++)
		reload_one_thumbnail_finish(self, self->entries->pdata[i]);

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
		g_queue_push_tail(&self->thumbnailers_queue, entry);
	}

	entry_set_surface_user_data(entry);
	g_hash_table_insert(self->thumbnail_cache, g_strdup(entry->e->uri),
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
	if (!(t->target = g_queue_pop_head(&self->thumbnailers_queue)))
		return FALSE;

	// Case analysis:
	//  - We haven't found any thumbnail for the entry at all
	//    (and it has a symbolic icon as a result):
	//    we want to fill the void ASAP, so go for embedded thumbnails first.
	//  - We've found one, but we're not quite happy with it:
	//    always run the full process for a high-quality wide thumbnail.
	//  - We can't end up here in any other cases.
	const char *uri = entry_system_wide_uri(t->target);
	const char *argv_faster[] = {PROJECT_NAME, "--extract-thumbnail",
		"--thumbnail", fiv_thumbnail_sizes[self->item_size].thumbnail_spec_name,
		"--", uri, NULL};
	const char *argv_slower[] = {PROJECT_NAME,
		"--thumbnail", fiv_thumbnail_sizes[self->item_size].thumbnail_spec_name,
		"--", uri, NULL};

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
	g_queue_clear(&self->thumbnailers_queue);

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

	GQueue lq = G_QUEUE_INIT;
	for (guint i = 0; i < self->entries->len; i++) {
		Entry *entry = self->entries->pdata[i];
		if (entry->removed)
			continue;

		if (entry->icon)
			g_queue_push_tail(&self->thumbnailers_queue, entry);
		else if (cairo_surface_get_user_data(
			entry->thumbnail, &fiv_thumbnail_key_lq))
			g_queue_push_tail(&lq, entry);
	}
	while (!g_queue_is_empty(&lq)) {
		g_queue_push_tail_link(
			&self->thumbnailers_queue, g_queue_pop_head_link(&lq));
	}

	for (size_t i = 0; i < self->thumbnailers_len; i++) {
		if (!thumbnailer_next(self->thumbnailers + i))
			break;
	}
}

// --- Boilerplate -------------------------------------------------------------

G_DEFINE_TYPE_EXTENDED(FivBrowser, fiv_browser, GTK_TYPE_WIDGET, 0,
	G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, NULL))

enum {
	PROP_THUMBNAIL_SIZE = 1,
	PROP_SHOW_LABELS,
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
	gtk_widget_queue_draw(GTK_WIDGET(user_data));
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
	g_ptr_array_free(self->entries, TRUE);
	g_array_free(self->layouted_rows, TRUE);
	if (self->model) {
		g_signal_handlers_disconnect_by_data(self->model, self);
		g_clear_object(&self->model);
	}

	g_hash_table_destroy(self->thumbnail_cache);

	cairo_pattern_destroy(self->glow_padded);
	cairo_pattern_destroy(self->glow);
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
	case PROP_SHOW_LABELS:
		g_value_set_boolean(value, self->show_labels);
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
	case PROP_SHOW_LABELS:
		if (self->show_labels != g_value_get_boolean(value)) {
			self->show_labels = g_value_get_boolean(value);
			gtk_widget_queue_resize(GTK_WIDGET(self));
			g_object_notify_by_pspec(object, pspec);
		}
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

	// This should ideally reflect thumbnails, but we're in a GtkScrolledWindow,
	// making this rather inconsequential.
	int fluff = padding.left + 2 * self->item_border_x + padding.right;
	*minimum = fluff + self->item_height /* Icons are rectangular. */;
	*natural = fluff + FIV_THUMBNAIL_WIDE_COEFFICIENT * self->item_height;
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

	// On Wayland, touchpad scrolling doesn't emulate the scroll wheel,
	// making GDK_SMOOTH_SCROLL_MASK necessary for our GtkScrolledWindow.
	// On X11 and Windows, this merely makes touchpad scrolling smoother.
	//
	// Note that Apple Magic Mouse's touchpad also sends out smooth scrolling
	// events, and is indistinguishable from a mouse wheel (GDK_SOURCE_MOUSE,
	// sends LIBINPUT_EVENT_POINTER_SCROLL_WHEEL).  Yet, curiously,
	// something in the stack on Wayland makes scrolling events discrete.
#ifdef GDK_WINDOWING_X11
	// XXX: On X11 (at least, not on Wayland or Windows), the first scroll wheel
	// event only produces a smooth stop event.  Not our bug, yet annoying.
	// We might make smooth scrolling support optional.
	if (!GDK_IS_X11_WINDOW(gtk_widget_get_parent_window(widget)))
#endif  // GDK_WINDOWING_X11
		attributes.event_mask |= GDK_SMOOTH_SCROLL_MASK;

	// We need this window to receive input events at all.
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

	relayout(FIV_BROWSER(widget), allocation->width);

	// Avoid fresh blank space.
	if (self->hadjustment) {
		gtk_adjustment_set_value(
			self->hadjustment, gtk_adjustment_get_value(self->hadjustment));
	}
	if (self->vadjustment) {
		gtk_adjustment_set_value(
			self->vadjustment, gtk_adjustment_get_value(self->vadjustment));
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
	GFile *location = g_file_new_for_uri(entry->e->uri);
	g_signal_emit(self, browser_signals[ITEM_ACTIVATED], 0, location,
		new_window ? GTK_PLACES_OPEN_NEW_WINDOW : GTK_PLACES_OPEN_NORMAL);
	g_object_unref(location);
	return TRUE;
}

static void
show_context_menu(GtkWidget *widget, GFile *file)
{
	GtkMenu *menu = fiv_context_menu_new(widget, file);
	if (menu)
		gtk_menu_popup_at_pointer(menu, NULL);
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
	if (GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->button_press_event(widget, event))
		return GDK_EVENT_STOP;

	// Make pressing multiple mouse buttons at once cancel a click.
	if (self->tracked_button) {
		abort_button_tracking(self);
		return GDK_EVENT_STOP;
	}
	if (event->type != GDK_BUTTON_PRESS)
		return GDK_EVENT_PROPAGATE;

	guint state = event->state & gtk_accelerator_get_default_mod_mask();
	if (event->button == GDK_BUTTON_PRIMARY && state == 0 &&
		gtk_widget_get_focus_on_click(widget))
		gtk_widget_grab_focus(widget);

	// In accordance with Nautilus, Thunar, and the mildly confusing
	// Apple Human Interface Guidelines, but not with the ugly Windows User
	// Experience Interaction Guidelines, open the context menu on button press.
	// (Originally our own behaviour, but the GDK3 function also does this.)
	gboolean triggers_menu =
		gdk_event_triggers_context_menu((const GdkEvent *) event);

	const Entry *entry = entry_at(self, event->x, event->y);
	if (!entry) {
		if (triggers_menu)
			show_context_menu(widget, fiv_io_model_get_location(self->model));
		else if (state || event->button != GDK_BUTTON_PRIMARY)
			return GDK_EVENT_PROPAGATE;

		if (self->selected) {
			self->selected = NULL;
			gtk_widget_queue_draw(widget);
		}
		return GDK_EVENT_STOP;
	}

	if (entry && triggers_menu) {
		self->selected = entry;
		gtk_widget_queue_draw(widget);

		// On X11, after closing the menu, the pointer otherwise remains,
		// no matter what its new location is.
		gdk_window_set_cursor(gtk_widget_get_window(widget), NULL);

		GFile *file = g_file_new_for_uri(entry->e->uri);
		show_context_menu(widget, file);
		g_object_unref(file);
		return GDK_EVENT_STOP;
	}

	// gtk_drag_source_set() would span the whole widget area, we'd have to
	// un/set it as needed, in particular to handle empty space.
	// It might be a good idea to use GtkGestureDrag instead.
	if (event->button == GDK_BUTTON_PRIMARY ||
		event->button == GDK_BUTTON_MIDDLE) {
		self->tracked_button = event->button;
		self->drag_begin_x = event->x;
		self->drag_begin_y = event->y;
		return GDK_EVENT_STOP;
	}
	return GDK_EVENT_PROPAGATE;
}

static gboolean
fiv_browser_button_release_event(GtkWidget *widget, GdkEventButton *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	if (GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->button_release_event(widget, event))
		return GDK_EVENT_STOP;
	if (event->button != self->tracked_button)
		return GDK_EVENT_PROPAGATE;

	// Middle clicks should only work on the starting entry.
	const Entry *entry = entry_at(self, self->drag_begin_x, self->drag_begin_y);
	abort_button_tracking(self);
	if (!entry || entry != entry_at(self, event->x, event->y))
		return GDK_EVENT_PROPAGATE;

	guint state = event->state & gtk_accelerator_get_default_mod_mask();
	if ((event->button == GDK_BUTTON_PRIMARY && state == 0))
		return open_entry(widget, entry, FALSE);
	if ((event->button == GDK_BUTTON_PRIMARY && state == GDK_CONTROL_MASK) ||
		(event->button == GDK_BUTTON_MIDDLE && state == 0))
		return open_entry(widget, entry, TRUE);
	return GDK_EVENT_PROPAGATE;
}

static gboolean
fiv_browser_motion_notify_event(GtkWidget *widget, GdkEventMotion *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	if (GTK_WIDGET_CLASS(fiv_browser_parent_class)
		->motion_notify_event(widget, event))
		return GDK_EVENT_STOP;

	// Touch screen dragging is how you scroll the parent GtkScrolledWindow,
	// don't steal that gesture.
	//
	// While we could allow dragging items that have been selected,
	// it's currently impossible to select on touch screens without opening,
	// and this behaviour/distinction seems unintuitive/surprising.
	guint state = event->state & gtk_accelerator_get_default_mod_mask();
	if (state != 0 || self->tracked_button != GDK_BUTTON_PRIMARY ||
		!gtk_drag_check_threshold(widget, self->drag_begin_x,
			self->drag_begin_y, event->x, event->y) ||
		gdk_device_get_source(gdk_event_get_source_device(
			(GdkEvent *) event)) == GDK_SOURCE_TOUCHSCREEN) {
		const Entry *entry = entry_at(self, event->x, event->y);
		GdkWindow *window = gtk_widget_get_window(widget);
		gdk_window_set_cursor(window, entry ? self->pointer : NULL);
		return GDK_EVENT_PROPAGATE;
	}

	// The "correct" behaviour is to set the selection on a left mouse button
	// press immediately, but that is regarded as visual noise.
	const Entry *entry = entry_at(self, self->drag_begin_x, self->drag_begin_y);
	abort_button_tracking(self);
	if (!entry)
		return GDK_EVENT_STOP;

	self->selected = entry;
	gtk_widget_queue_draw(widget);

	GtkTargetList *target_list = gtk_target_list_new(NULL, 0);
	gtk_target_list_add_uri_targets(target_list, 0);
	GdkDragAction actions = GDK_ACTION_COPY | GDK_ACTION_MOVE |
		GDK_ACTION_LINK | GDK_ACTION_ASK;
	gtk_drag_begin_with_coordinates(widget, target_list, actions,
		self->tracked_button, (GdkEvent *) event, event->x, event->y);
	gtk_target_list_unref(target_list);
	return GDK_EVENT_STOP;
}

static gboolean
fiv_browser_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
	FivBrowser *self = FIV_BROWSER(widget);
	abort_button_tracking(self);
	if ((event->state & gtk_accelerator_get_default_mod_mask()) !=
		GDK_CONTROL_MASK)
		return GDK_EVENT_PROPAGATE;

	static double delta = 0;
	switch (event->direction) {
	case GDK_SCROLL_UP:
		set_item_size(self, self->item_size + 1);
		return GDK_EVENT_STOP;
	case GDK_SCROLL_DOWN:
		set_item_size(self, self->item_size - 1);
		return GDK_EVENT_STOP;
	case GDK_SCROLL_SMOOTH:
		// On GDK/Wayland, the mouse wheel will typically create 1.5 deltas,
		// after dividing a 15 degree click angle from libinput by 10.
		// (Noticed on Arch + Sway, cannot reproduce on Ubuntu 22.04.)
		// On X11, as libinput(4) indicates, the delta will always be 1.0.
		if ((delta += event->delta_y) <= -1)
			set_item_size(self, self->item_size + 1);
		else if (delta >= +1)
			set_item_size(self, self->item_size - 1);
		else if (!event->is_stop)
			return GDK_EVENT_STOP;

		delta = 0;
		return GDK_EVENT_STOP;
	default:
		// Left/right are good to steal from GtkScrolledWindow for consistency.
		return GDK_EVENT_STOP;
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
		(void) gtk_selection_data_set_uris(data, (gchar *[])
			{(gchar *) entry_system_wide_uri(self->selected), NULL});
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
	double sh = self->item_border_y + row_subheight(self, row);
	if (row->y_offset < y1) {
		gtk_adjustment_set_value(
			self->vadjustment, row->y_offset - self->item_border_y);
	} else if (row->y_offset + self->item_height + sh > y1 + ph) {
		gtk_adjustment_set_value(
			self->vadjustment, row->y_offset - ph + self->item_height + sh);
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
			return GDK_EVENT_STOP;
		case GDK_KEY_Left:
			move_selection(self, GTK_DIR_LEFT);
			return GDK_EVENT_STOP;
		case GDK_KEY_Right:
			move_selection(self, GTK_DIR_RIGHT);
			return GDK_EVENT_STOP;
		case GDK_KEY_Up:
			move_selection(self, GTK_DIR_UP);
			return GDK_EVENT_STOP;
		case GDK_KEY_Down:
			move_selection(self, GTK_DIR_DOWN);
			return GDK_EVENT_STOP;
		case GDK_KEY_Home:
			move_selection_home(self);
			return GDK_EVENT_STOP;
		case GDK_KEY_End:
			move_selection_end(self);
			return GDK_EVENT_STOP;
		}
		break;
	case GDK_MOD1_MASK:
		switch (event->keyval) {
		case GDK_KEY_Return:
			if (self->selected) {
				GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(widget));
				fiv_context_menu_information(window, self->selected->e->uri);
			}
			return GDK_EVENT_STOP;
		}
		break;
	case GDK_CONTROL_MASK:
	case GDK_CONTROL_MASK | GDK_SHIFT_MASK:
		switch (event->keyval) {
		case GDK_KEY_plus:
			set_item_size(self, self->item_size + 1);
			return GDK_EVENT_STOP;
		case GDK_KEY_minus:
			set_item_size(self, self->item_size - 1);
			return GDK_EVENT_STOP;
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

	// TODO(p): Consider getting rid of tooltips altogether.
	if (self->show_labels)
		return FALSE;

	const Entry *entry = entry_at(self, x, y);
	if (!entry)
		return FALSE;

	gtk_tooltip_set_text(tooltip, entry->e->display_name);
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
		file = g_file_new_for_uri(self->selected->e->uri);
		rect = entry_rect(self, self->selected);
		rect.x += rect.width / 2;
		rect.y += rect.height / 2;
	} else {
		file = g_object_ref(fiv_io_model_get_location(self->model));
	}

	gtk_menu_popup_at_rect(fiv_context_menu_new(widget, file),
		gtk_widget_get_window(widget), &rect, GDK_GRAVITY_NORTH_WEST,
		GDK_GRAVITY_NORTH_WEST, NULL);
	g_object_unref(file);
	return TRUE;
}

static void
on_long_press(GtkGestureLongPress *lp, gdouble x, gdouble y, gpointer user_data)
{
	FivBrowser *self = FIV_BROWSER(user_data);
	const Entry *entry = entry_at(self, x, y);
	abort_button_tracking(self);
	if (!entry)
		return;

	GtkGesture *gesture = GTK_GESTURE(lp);
	GdkEventSequence *sequence = gtk_gesture_get_last_updated_sequence(gesture);
	const GdkEvent *event = gtk_gesture_get_last_event(gesture, sequence);

	GtkWidget *widget = GTK_WIDGET(self);
	GdkWindow *window = gtk_widget_get_window(widget);
#ifdef GDK_WINDOWING_X11
	// FIXME: Once the finger is lifted, this menu is immediately closed.
	if (GDK_IS_X11_WINDOW(window))
		return;
#endif  // GDK_WINDOWING_X11

	// It might also be possible to have long-press just select items,
	// and show some kind of toolbar with available actions.
	GFile *file = g_file_new_for_uri(entry->e->uri);
	gtk_menu_popup_at_rect(fiv_context_menu_new(widget, file), window,
		&(GdkRectangle) {.x = x, .y = y}, GDK_GRAVITY_NORTH_WEST,
		GDK_GRAVITY_NORTH_WEST, event);
	g_object_unref(file);

	self->selected = entry;
	gtk_widget_queue_draw(widget);
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
	// XXX: Right now, specifying custom fonts within our CSS pseudo-regions
	// has no effect, so it might be appropriate to also add .label/.symbolic
	// classes here, remember the resulting GTK_STYLE_PROPERTY_FONT,
	// and apply them in relayout() with pango_layout_set_font_description().
	// There is virtually nothing to be gained from this flexibility, though.
	// XXX: We should also invoke relayout() here, because different states
	// might theoretically use different fonts.
	gtk_style_context_restore(style);

	self->glow_w = (margin.left + margin.right) / 2;
	self->glow_h = (margin.top + margin.bottom) / 2;

	// Don't set different opposing sides, it will misrender, your problem.
	// When the style of the class changes, this virtual method isn't invoked,
	// so the update check is mildly pointless.
	int item_border_x = self->glow_w + (border.left + border.right) / 2;
	int item_border_y = self->glow_h + (border.top + border.bottom) / 2;
	if (item_border_x != self->item_border_x ||
		item_border_y != self->item_border_y) {
		self->item_border_x = item_border_x;
		self->item_border_y = item_border_y;
		gtk_widget_queue_resize(widget);
	}

	if (self->glow_padded)
		cairo_pattern_destroy(self->glow_padded);
	if (self->glow)
		cairo_pattern_destroy(self->glow);

	cairo_surface_t *corner = cairo_image_surface_create(
		CAIRO_FORMAT_A8, MAX(0, self->glow_w), MAX(0, self->glow_h));
	unsigned char *data = cairo_image_surface_get_data(corner);
	int stride = cairo_image_surface_get_stride(corner);

	// Smooth out the curve, so that the edge of the glow isn't too jarring.
	const double fade_factor = 1.5;

	const int x_max = self->glow_w - 1;
	const int y_max = self->glow_h - 1;
	const double x_scale = 1. / MAX(1, x_max);
	const double y_scale = 1. / MAX(1, y_max);
	for (int y = 0; y <= y_max; y++)
		for (int x = 0; x <= x_max; x++) {
			const double xn = x_scale * (x_max - x);
			const double yn = y_scale * (y_max - y);
			double v = MIN(sqrt(xn * xn + yn * yn), 1);
			data[y * stride + x] = round(pow(1 - v, fade_factor) * 255);
		}

	cairo_surface_mark_dirty(corner);
	self->glow = cairo_pattern_create_for_surface(corner);
	self->glow_padded = cairo_pattern_create_for_surface(corner);
	cairo_pattern_set_extend(self->glow_padded, CAIRO_EXTEND_PAD);

#ifdef GDK_WINDOWING_QUARTZ
	// Cairo's Quartz backend doesn't support CAIRO_EXTEND_PAD, work around it.
	if (GDK_IS_QUARTZ_DISPLAY(gtk_widget_get_display(widget))) {
		int max_size = fiv_thumbnail_sizes[FIV_THUMBNAIL_SIZE_MAX].size;
		cairo_surface_t *padded = cairo_image_surface_create(CAIRO_FORMAT_A8,
			cairo_image_surface_get_width(corner) +
				max_size * FIV_THUMBNAIL_WIDE_COEFFICIENT,
			cairo_image_surface_get_height(corner) + max_size);
		cairo_t *cr = cairo_create(padded);
		cairo_set_source(cr, self->glow_padded);
		cairo_paint(cr);
		cairo_destroy(cr);

		cairo_pattern_destroy(self->glow_padded);
		self->glow_padded = cairo_pattern_create_for_surface(padded);
		cairo_surface_destroy(padded);
	}
#endif  // GDK_WINDOWING_QUARTZ

	cairo_surface_destroy(corner);
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
	browser_properties[PROP_SHOW_LABELS] = g_param_spec_boolean(
		"show-labels", "Show labels", "Whether to show filename labels",
		FALSE, G_PARAM_READWRITE);
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

	self->entries =
		g_ptr_array_new_with_free_func((GDestroyNotify) entry_destroy);
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
	g_queue_init(&self->thumbnailers_queue);

	set_item_size(self, FIV_THUMBNAIL_SIZE_NORMAL);
	self->show_labels = FALSE;
	self->glow_padded = cairo_pattern_create_rgba(0, 0, 0, 0);
	self->glow = cairo_pattern_create_rgba(0, 0, 0, 0);

	g_signal_connect_swapped(gtk_settings_get_default(),
		"notify::gtk-icon-theme-name", G_CALLBACK(reload_thumbnails), self);

	GtkGesture *lp = gtk_gesture_long_press_new(GTK_WIDGET(self));
	gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(lp), TRUE);
	gtk_event_controller_set_propagation_phase(
		GTK_EVENT_CONTROLLER(lp), GTK_PHASE_BUBBLE);
	g_object_set_data_full(
		G_OBJECT(self), "fiv-browser-long-press-gesture", lp, g_object_unref);

	g_signal_connect(lp, "pressed", G_CALLBACK(on_long_press), self);
}

// --- Public interface --------------------------------------------------------

static void
on_model_reloaded(FivIoModel *model, FivBrowser *self)
{
	g_return_if_fail(model == self->model);

	gchar *selected_uri = NULL;
	if (self->selected)
		selected_uri = g_strdup(self->selected->e->uri);

	thumbnailers_abort(self);
	g_array_set_size(self->layouted_rows, 0);
	g_ptr_array_set_size(self->entries, 0);

	gsize len = 0;
	FivIoModelEntry *const *files = fiv_io_model_get_files(self->model, &len);
	for (gsize i = 0; i < len; i++) {
		g_ptr_array_add(
			self->entries, entry_new(fiv_io_model_entry_ref(files[i])));
	}

	fiv_browser_select(self, selected_uri);
	g_free(selected_uri);

	reload_thumbnails(self);
	thumbnailers_start(self);
}

static void
on_model_changed(FivIoModel *model, FivIoModelEntry *old, FivIoModelEntry *new,
	FivBrowser *self)
{
	g_return_if_fail(model == self->model);

	// Add new entries to the end, so as to not disturb the layout.
	if (!old) {
		Entry *entry = entry_new(fiv_io_model_entry_ref(new));
		g_ptr_array_add(self->entries, entry);

		reload_one_thumbnail(self, entry);
		// TODO(p): Try to add to thumbnailer queue if already started.
		thumbnailers_start(self);
		return;
	}

	Entry *found = NULL;
	for (guint i = 0; i < self->entries->len; i++) {
		Entry *entry = self->entries->pdata[i];
		if (entry->e == old) {
			found = entry;
			break;
		}
	}
	if (!found)
		return;

	// Rename entries in place, so as to not disturb the layout.
	// XXX: This behaves differently from FivIoModel, and by extension fiv.c.
	if (new) {
		fiv_io_model_entry_unref(found->e);
		found->e = fiv_io_model_entry_ref(new);
		found->removed = FALSE;

		// TODO(p): If there is a URI mismatch, don't reload thumbnails,
		// so that there's no jumping around. Or, a bit more properly,
		// move the thumbnail cache entry to the new URI.
		reload_one_thumbnail(self, found);
		// TODO(p): Try to add to thumbnailer queue if already started.
		thumbnailers_start(self);
	} else {
		found->removed = TRUE;
		gtk_widget_queue_draw(GTK_WIDGET(self));
	}
}

GtkWidget *
fiv_browser_new(FivIoModel *model)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(model), NULL);

	FivBrowser *self = g_object_new(FIV_TYPE_BROWSER, NULL);
	self->model = g_object_ref(model);

	g_signal_connect(self->model, "reloaded",
		G_CALLBACK(on_model_reloaded), self);
	g_signal_connect(self->model, "files-changed",
		G_CALLBACK(on_model_changed), self);
	on_model_reloaded(self->model, self);
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
		const Entry *entry = self->entries->pdata[i];
		if (!g_strcmp0(entry->e->uri, uri)) {
			self->selected = entry;
			scroll_to_selection(self);
			break;
		}
	}
}

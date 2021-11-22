//
// fastiv-browser.c: fast image viewer - filesystem browser widget
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

#include <math.h>
#include <pixman.h>

#include "fastiv-browser.h"
#include "fastiv-io.h"
#include "fastiv-view.h"

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

struct _FastivBrowser {
	GtkWidget parent_instance;

	FastivIoThumbnailSize item_size;    ///< Thumbnail size
	int item_height;                    ///< Thumbnail height in pixels
	int item_spacing;                   ///< Space between items in pixels

	GArray *entries;                    ///< [Entry]
	GArray *layouted_rows;              ///< [Row]
	int selected;

	GdkCursor *pointer;                 ///< Cached pointer cursor
	cairo_surface_t *glow;              ///< CAIRO_FORMAT_A8 mask
	int item_border_x;                  ///< L/R .item margin + border
	int item_border_y;                  ///< T/B .item margin + border
};

typedef struct entry Entry;
typedef struct item Item;
typedef struct row Row;

static const double g_permitted_width_multiplier = 2;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct entry {
	char *filename;                     ///< Absolute path
	cairo_surface_t *thumbnail;         ///< Prescaled thumbnail
	GIcon *icon;                        ///< If no thumbnail, use this icon
};

static void
entry_free(Entry *self)
{
	g_free(self->filename);
	if (self->thumbnail)
		cairo_surface_destroy(self->thumbnail);
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
append_row(FastivBrowser *self, int *y, int x, GArray *items_array)
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
relayout(FastivBrowser *self, int width)
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
draw_outer_border(FastivBrowser *self, cairo_t *cr, int width, int height)
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
item_extents(FastivBrowser *self, const Item *item, const Row *row)
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
entry_at(FastivBrowser *self, int x, int y)
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
draw_row(FastivBrowser *self, cairo_t *cr, const Row *row)
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

		gtk_render_background(
			style, cr, border.left, border.top, extents.width, extents.height);

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
	if (width > g_permitted_width_multiplier * height) {
		scale_x = g_permitted_width_multiplier * row_height / width;
		scale_y = round(scale_x * height) / height;
	} else {
		scale_y = row_height / height;
		scale_x = round(scale_y * width) / width;
	}
	if (scale_x == 1 && scale_y == 1)
		return thumbnail;

	int projected_width = round(scale_x * width);
	int projected_height = round(scale_y * height);
	cairo_surface_t *scaled = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, projected_width, projected_height);

	// pixman can take gamma into account when scaling, unlike Cairo.
	struct pixman_f_transform xform_floating;
	struct pixman_transform xform;

	// PIXMAN_a8r8g8b8_sRGB can be used for gamma-correct results,
	// but it's an incredibly slow transformation
	pixman_format_code_t format = PIXMAN_a8r8g8b8;

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

	cairo_surface_destroy(thumbnail);
	cairo_surface_mark_dirty(scaled);
	return scaled;
}

static void
entry_add_thumbnail(gpointer data, gpointer user_data)
{
	Entry *self = data;
	g_clear_object(&self->icon);
	if (self->thumbnail)
		cairo_surface_destroy(self->thumbnail);

	FastivBrowser *browser = FASTIV_BROWSER(user_data);
	self->thumbnail = rescale_thumbnail(
		fastiv_io_lookup_thumbnail(self->filename, browser->item_size),
		browser->item_height);
	if (self->thumbnail)
		return;

	// Fall back to symbolic icons, though there's only so much we can do
	// in parallel--GTK+ isn't thread-safe.
	GFile *file = g_file_new_for_path(self->filename);
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
}

static void
materialize_icon(FastivBrowser *self, Entry *entry)
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
reload_thumbnails(FastivBrowser *self)
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

// --- Boilerplate -------------------------------------------------------------

// TODO(p): For proper navigation, we need to implement GtkScrollable.
G_DEFINE_TYPE_EXTENDED(FastivBrowser, fastiv_browser, GTK_TYPE_WIDGET, 0,
	/* G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE,
		fastiv_browser_scrollable_init) */)

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
fastiv_browser_finalize(GObject *gobject)
{
	FastivBrowser *self = FASTIV_BROWSER(gobject);
	g_array_free(self->entries, TRUE);
	g_array_free(self->layouted_rows, TRUE);
	cairo_surface_destroy(self->glow);
	g_clear_object(&self->pointer);

	G_OBJECT_CLASS(fastiv_browser_parent_class)->finalize(gobject);
}

static void
fastiv_browser_get_property(
	GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	FastivBrowser *self = FASTIV_BROWSER(object);
	switch (property_id) {
	case PROP_THUMBNAIL_SIZE:
		g_value_set_enum(value, self->item_size);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
fastiv_browser_set_property(
	GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	FastivBrowser *self = FASTIV_BROWSER(object);
	switch (property_id) {
	case PROP_THUMBNAIL_SIZE:
		if (g_value_get_enum(value) != (int) self->item_size) {
			self->item_size = g_value_get_enum(value);
			self->item_height = fastiv_io_thumbnail_sizes[self->item_size].size;
			reload_thumbnails(self);
		}
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static GtkSizeRequestMode
fastiv_browser_get_request_mode(G_GNUC_UNUSED GtkWidget *widget)
{
	return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
fastiv_browser_get_preferred_width(
	GtkWidget *widget, gint *minimum, gint *natural)
{
	FastivBrowser *self = FASTIV_BROWSER(widget);
	GtkStyleContext *style = gtk_widget_get_style_context(widget);

	GtkBorder padding = {};
	gtk_style_context_get_padding(style, GTK_STATE_FLAG_NORMAL, &padding);
	*minimum = *natural =
		g_permitted_width_multiplier * self->item_height +
		padding.left + 2 * self->item_border_x + padding.right;
}

static void
fastiv_browser_get_preferred_height_for_width(
	GtkWidget *widget, gint width, gint *minimum, gint *natural)
{
	// XXX: This is rather ugly, the caller is only asking.
	*minimum = *natural = relayout(FASTIV_BROWSER(widget), width);
}

static void
fastiv_browser_realize(GtkWidget *widget)
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
			GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK,
	};

	// We need this window to receive input events at all.
	// TODO(p): See if input events bubble up to parents.
	GdkWindow *window = gdk_window_new(gtk_widget_get_parent_window(widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
	gtk_widget_register_window(widget, window);
	gtk_widget_set_window(widget, window);
	gtk_widget_set_realized(widget, TRUE);

	FastivBrowser *self = FASTIV_BROWSER(widget);
	g_clear_object(&self->pointer);
	self->pointer =
		gdk_cursor_new_from_name(gdk_window_get_display(window), "pointer");
}

static void
fastiv_browser_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	GTK_WIDGET_CLASS(fastiv_browser_parent_class)
		->size_allocate(widget, allocation);

	relayout(FASTIV_BROWSER(widget), allocation->width);
}

static gboolean
fastiv_browser_draw(GtkWidget *widget, cairo_t *cr)
{
	FastivBrowser *self = FASTIV_BROWSER(widget);
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
fastiv_browser_button_press_event(GtkWidget *widget, GdkEventButton *event)
{
	GTK_WIDGET_CLASS(fastiv_browser_parent_class)
		->button_press_event(widget, event);

	FastivBrowser *self = FASTIV_BROWSER(widget);
	if (event->type != GDK_BUTTON_PRESS || event->state != 0)
		return FALSE;
	if (event->button == GDK_BUTTON_PRIMARY &&
		gtk_widget_get_focus_on_click(widget))
		gtk_widget_grab_focus(widget);

	const Entry *entry = entry_at(self, event->x, event->y);
	if (!entry)
		return FALSE;

	switch (event->button) {
	case GDK_BUTTON_PRIMARY:
		g_signal_emit(widget, browser_signals[ITEM_ACTIVATED], 0,
			entry->filename, GTK_PLACES_OPEN_NORMAL);
		return TRUE;
	case GDK_BUTTON_MIDDLE:
		g_signal_emit(widget, browser_signals[ITEM_ACTIVATED], 0,
			entry->filename, GTK_PLACES_OPEN_NEW_WINDOW);
		return TRUE;
	default:
		return FALSE;
	}
}

gboolean
fastiv_browser_motion_notify_event(GtkWidget *widget, GdkEventMotion *event)
{
	GTK_WIDGET_CLASS(fastiv_browser_parent_class)
		->motion_notify_event(widget, event);

	FastivBrowser *self = FASTIV_BROWSER(widget);
	if (event->state != 0)
		return FALSE;

	const Entry *entry = entry_at(self, event->x, event->y);
	GdkWindow *window = gtk_widget_get_window(widget);
	gdk_window_set_cursor(window, entry ? self->pointer : NULL);
	return TRUE;
}

static void
fastiv_browser_style_updated(GtkWidget *widget)
{
	GTK_WIDGET_CLASS(fastiv_browser_parent_class)->style_updated(widget);

	FastivBrowser *self = FASTIV_BROWSER(widget);
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

	self->glow =
		cairo_image_surface_create(CAIRO_FORMAT_A8, glow_w, glow_h);
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
fastiv_browser_class_init(FastivBrowserClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fastiv_browser_finalize;
	object_class->get_property = fastiv_browser_get_property;
	object_class->set_property = fastiv_browser_set_property;

	browser_properties[PROP_THUMBNAIL_SIZE] = g_param_spec_enum(
		"thumbnail-size", "Thumbnail size", "The thumbnail height to use",
		FASTIV_TYPE_IO_THUMBNAIL_SIZE, FASTIV_IO_THUMBNAIL_SIZE_NORMAL,
		G_PARAM_READWRITE);
	g_object_class_install_properties(
		object_class, N_PROPERTIES, browser_properties);

	browser_signals[ITEM_ACTIVATED] = g_signal_new("item-activated",
		G_TYPE_FROM_CLASS(klass), 0, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 2, G_TYPE_STRING, GTK_TYPE_PLACES_OPEN_FLAGS);

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->get_request_mode = fastiv_browser_get_request_mode;
	widget_class->get_preferred_width = fastiv_browser_get_preferred_width;
	widget_class->get_preferred_height_for_width =
		fastiv_browser_get_preferred_height_for_width;
	widget_class->realize = fastiv_browser_realize;
	widget_class->draw = fastiv_browser_draw;
	widget_class->size_allocate = fastiv_browser_size_allocate;
	widget_class->button_press_event = fastiv_browser_button_press_event;
	widget_class->motion_notify_event = fastiv_browser_motion_notify_event;
	widget_class->style_updated = fastiv_browser_style_updated;

	// Could be split to also-idiomatic row-spacing/column-spacing properties.
	// The GParamSpec is sinked by this call.
	gtk_widget_class_install_style_property(widget_class,
		g_param_spec_int("spacing", "Spacing", "Space between items",
			0, G_MAXINT, 1, G_PARAM_READWRITE));

	// TODO(p): Later override "screen_changed", recreate Pango layouts there,
	// if we get to have any, or otherwise reflect DPI changes.
	gtk_widget_class_set_css_name(widget_class, "fastiv-browser");
}

static void
fastiv_browser_init(FastivBrowser *self)
{
	gtk_widget_set_can_focus(GTK_WIDGET(self), TRUE);

	self->entries = g_array_new(FALSE, TRUE, sizeof(Entry));
	g_array_set_clear_func(self->entries, (GDestroyNotify) entry_free);
	self->layouted_rows = g_array_new(FALSE, TRUE, sizeof(Row));
	g_array_set_clear_func(self->layouted_rows, (GDestroyNotify) row_free);

	self->item_size = FASTIV_IO_THUMBNAIL_SIZE_NORMAL;
	self->item_height = fastiv_io_thumbnail_sizes[self->item_size].size;
	self->selected = -1;
	self->glow = cairo_image_surface_create(CAIRO_FORMAT_A1, 0, 0);

	g_signal_connect_swapped(gtk_settings_get_default(),
		"notify::gtk-icon-theme-name", G_CALLBACK(reload_thumbnails), self);
}

// --- Public interface --------------------------------------------------------

static gint
entry_compare(gconstpointer a, gconstpointer b)
{
	const Entry *entry1 = a;
	const Entry *entry2 = b;
	GFile *location1 = g_file_new_for_path(entry1->filename);
	GFile *location2 = g_file_new_for_path(entry2->filename);
	gint result = fastiv_io_filecmp(location1, location2);
	g_object_unref(location1);
	g_object_unref(location2);
	return result;
}

void
fastiv_browser_load(
	FastivBrowser *self, FastivBrowserFilterCallback cb, const char *path)
{
	g_array_set_size(self->entries, 0);
	g_array_set_size(self->layouted_rows, 0);

	GFile *file = g_file_new_for_path(path);
	GFileEnumerator *enumerator = g_file_enumerate_children(file,
		G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	g_object_unref(file);
	if (!enumerator)
		return;

	while (TRUE) {
		GFileInfo *info = NULL;
		GFile *child = NULL;
		if (!g_file_enumerator_iterate(enumerator, &info, &child, NULL, NULL) ||
			!info)
			break;
		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
			continue;
		if (cb && !cb(g_file_info_get_name(info)))
			continue;

		g_array_append_val(self->entries,
			((Entry) {.thumbnail = NULL, .filename = g_file_get_path(child)}));
	}
	g_object_unref(enumerator);

	// TODO(p): Support being passed a sort function.
	g_array_sort(self->entries, entry_compare);

	reload_thumbnails(self);
}

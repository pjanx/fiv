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

	GArray *entries;                    ///< [Entry]
	GArray *layouted_rows;              ///< [Row]
	int selected;

	cairo_surface_t *glow;              ///< CAIRO_FORMAT_A8 mask
	int item_border_x;                  ///< L/R .item margin + border
	int item_border_y;                  ///< T/B .item margin + border
};

typedef struct entry Entry;
typedef struct item Item;
typedef struct row Row;

static const double g_row_height = 256;
static const double g_permitted_width_multiplier = 2;

// Could be split out to also-idiomatic row-spacing/column-spacing properties.
// TODO(p): Make a property for this.
static const int g_item_spacing = 1;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct entry {
	char *filename;                     ///< Absolute path
	cairo_surface_t *thumbnail;         ///< Prescaled thumbnail
};

static void
entry_free(Entry *self)
{
	g_free(self->filename);
	if (self->thumbnail)
		cairo_surface_destroy(self->thumbnail);
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
		*y += g_item_spacing;

	*y += self->item_border_y;
	g_array_append_val(self->layouted_rows, ((Row) {
		.items = g_array_steal(items_array, NULL),
		.x_offset = x,
		.y_offset = *y,
	}));

	// Not trying to pack them vertically, but this would be the place to do it.
	*y += g_row_height;
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
		} else if (x + g_item_spacing + width <= available_width) {
			x += g_item_spacing;
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
item_extents(const Item *item, const Row *row)
{
	int width = cairo_image_surface_get_width(item->entry->thumbnail);
	int height = cairo_image_surface_get_height(item->entry->thumbnail);
	return (GdkRectangle) {
		.x = row->x_offset + item->x_offset,
		.y = row->y_offset + g_row_height - height,
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
			GdkRectangle extents = item_extents(item, row);
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
		GdkRectangle extents = item_extents(item, row);
		cairo_translate(cr, extents.x - border.left, extents.y - border.top);

		gdk_cairo_set_source_rgba(cr, &glow_color);
		draw_outer_border(self, cr,
			border.left + extents.width + border.right,
			border.top + extents.height + border.bottom);

		gtk_render_background(
			style, cr, border.left, border.top, extents.width, extents.height);

		gtk_render_frame(style, cr, 0, 0,
			border.left + extents.width + border.right,
			border.top + extents.height + border.bottom);

		cairo_set_source_surface(
			cr, item->entry->thumbnail, border.left, border.top);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	gtk_style_context_restore(style);
}

// --- Boilerplate -------------------------------------------------------------

// TODO(p): For proper navigation, we need to implement GtkScrollable.
G_DEFINE_TYPE_EXTENDED(FastivBrowser, fastiv_browser, GTK_TYPE_WIDGET, 0,
	/* G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE,
		fastiv_browser_scrollable_init) */)

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

	G_OBJECT_CLASS(fastiv_browser_parent_class)->finalize(gobject);
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
	*minimum = *natural = g_permitted_width_multiplier * g_row_height +
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
			GDK_BUTTON_PRESS_MASK,
	};

	// We need this window to receive input events at all.
	// TODO(p): See if input events bubble up to parents.
	GdkWindow *window = gdk_window_new(gtk_widget_get_parent_window(widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
	gtk_widget_register_window(widget, window);
	gtk_widget_set_window(widget, window);
	gtk_widget_set_realized(widget, TRUE);
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
			.height = g_row_height + 2 * self->item_border_y,
		};
		if (!have_clip || gdk_rectangle_intersect(&clip, &extents, NULL))
			draw_row(self, cr, row);
	}
	return TRUE;
}

static gboolean
fastiv_browser_button_press_event(GtkWidget *widget, GdkEventButton *event)
{
	FastivBrowser *self = FASTIV_BROWSER(widget);
	if (event->type != GDK_BUTTON_PRESS || event->button != 1 ||
		event->state != 0)
		return FALSE;

	const Entry *entry = entry_at(self, event->x, event->y);
	if (!entry)
		return FALSE;

	g_signal_emit(widget, browser_signals[ITEM_ACTIVATED], 0, entry->filename);
	return TRUE;
}

static void
fastiv_browser_style_updated(GtkWidget *widget)
{
	GTK_WIDGET_CLASS(fastiv_browser_parent_class)->style_updated(widget);

	FastivBrowser *self = FASTIV_BROWSER(widget);
	GtkStyleContext *style = gtk_widget_get_style_context(widget);
	GtkBorder border = {}, margin = {};

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

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->get_request_mode = fastiv_browser_get_request_mode;
	widget_class->get_preferred_width = fastiv_browser_get_preferred_width;
	widget_class->get_preferred_height_for_width =
		fastiv_browser_get_preferred_height_for_width;
	widget_class->realize = fastiv_browser_realize;
	widget_class->draw = fastiv_browser_draw;
	widget_class->size_allocate = fastiv_browser_size_allocate;
	widget_class->button_press_event = fastiv_browser_button_press_event;
	widget_class->style_updated = fastiv_browser_style_updated;

	browser_signals[ITEM_ACTIVATED] =
		g_signal_new("item-activated", G_TYPE_FROM_CLASS(klass), 0, 0,
			NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

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

	self->selected = -1;
	self->glow = cairo_image_surface_create(CAIRO_FORMAT_A1, 0, 0);
}

static cairo_surface_t *
rescale_thumbnail(cairo_surface_t *thumbnail)
{
	if (!thumbnail)
		return thumbnail;

	int width = cairo_image_surface_get_width(thumbnail);
	int height = cairo_image_surface_get_height(thumbnail);

	double scale_x = 1;
	double scale_y = 1;
	if (width > g_permitted_width_multiplier * height) {
		scale_x = g_permitted_width_multiplier * g_row_height / width;
		scale_y = round(scale_x * height) / height;
	} else {
		scale_y = g_row_height / height;
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
entry_add_thumbnail(gpointer data, G_GNUC_UNUSED gpointer user_data)
{
	Entry *self = data;
	self->thumbnail =
		rescale_thumbnail(fastiv_io_lookup_thumbnail(self->filename));
}

void
fastiv_browser_load(FastivBrowser *self, const char *path)
{
	g_array_set_size(self->entries, 0);

	// TODO(p): Use opendir(), in order to get file type directly.
	GDir *dir = g_dir_open(path, 0, NULL);
	if (!dir)
		return;

	const char *filename;
	while ((filename = g_dir_read_name(dir))) {
		if (!strcmp(filename, ".") || !strcmp(filename, ".."))
			continue;

		gchar *subpath = g_build_filename(path, filename, NULL);
		g_array_append_val(
			self->entries, ((Entry) {.thumbnail = NULL, .filename = subpath}));
	}
	g_dir_close(dir);

	GThreadPool *pool = g_thread_pool_new(
		entry_add_thumbnail, NULL, g_get_num_processors(), FALSE, NULL);
	for (guint i = 0; i < self->entries->len; i++)
		g_thread_pool_push(pool, &g_array_index(self->entries, Entry, i), NULL);
	g_thread_pool_free(pool, FALSE, TRUE);

	// TODO(p): Sort the entries.
	gtk_widget_queue_resize(GTK_WIDGET(self));
}

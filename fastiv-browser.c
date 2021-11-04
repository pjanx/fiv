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

struct _FastivBrowser {
	GtkWidget parent_instance;

	GArray *entries;                    ///< [Entry]
	GArray *layouted_rows;              ///< [Row]
	int selected;
};

typedef struct entry Entry;
typedef struct item Item;
typedef struct row Row;

static const double g_row_height = 256;
static const double g_permitted_width_multiplier = 2;

// Could be split out to also-idiomatic row-spacing/column-spacing properties.
// TODO(p): Make a property for this.
static const int g_item_spacing = 5;

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
	int x_offset;                       ///< Start position including padding
	int y_offset;                       ///< Start position including padding
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

	g_array_append_val(self->layouted_rows, ((Row) {
		.items = g_array_steal(items_array, NULL),
		.x_offset = x,
		.y_offset = *y,
	}));

	// Not trying to pack them vertically, but this would be the place to do it.
	*y += g_row_height;
}

static int
relayout(FastivBrowser *self, int width)
{
	GtkWidget *widget = GTK_WIDGET(self);
	GtkStyleContext *context = gtk_widget_get_style_context(widget);

	GtkBorder padding = {};
	gtk_style_context_get_padding(context, GTK_STATE_FLAG_NORMAL, &padding);
	int available_width = width - padding.left - padding.right;

	g_array_set_size(self->layouted_rows, 0);

	GArray *items = g_array_new(TRUE, TRUE, sizeof(Item));
	int x = 0, y = padding.top;
	for (guint i = 0; i < self->entries->len; i++) {
		const Entry *entry = &g_array_index(self->entries, Entry, i);
		if (!entry->thumbnail)
			continue;

		int width = cairo_image_surface_get_width(entry->thumbnail);
		if (!items->len) {
			// Just insert it, whether or not there's any space.
		} else if (x + g_item_spacing + width <= available_width) {
			x += g_item_spacing;
		} else {
			append_row(self, &y,
				padding.left + MAX(0, available_width - x) / 2, items);
			x = 0;
		}

		g_array_append_val(items, ((Item) {.entry = entry, .x_offset = x}));
		x += width;
	}
	if (items->len) {
		append_row(self, &y,
			padding.left + MAX(0, available_width - x) / 2, items);
	}

	g_array_free(items, TRUE);
	return y + padding.bottom;
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
	G_GNUC_UNUSED FastivBrowser *self = FASTIV_BROWSER(gobject);
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
	GtkBorder padding = {};
	GtkStyleContext *context = gtk_widget_get_style_context(widget);
	gtk_style_context_get_padding(context, GTK_STATE_FLAG_NORMAL, &padding);
	*minimum = *natural = g_permitted_width_multiplier * g_row_height +
		padding.left + padding.right;;
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
	G_GNUC_UNUSED FastivBrowser *self = FASTIV_BROWSER(widget);
	if (!gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget)))
		return TRUE;

	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);
	gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0,
		allocation.width, allocation.height);

	for (guint i = 0; i < self->layouted_rows->len; i++) {
		const Row *row = &g_array_index(self->layouted_rows, Row, i);
		for (Item *item = row->items; item->entry; item++) {
			cairo_surface_t *thumbnail = item->entry->thumbnail;
			int width = cairo_image_surface_get_width(thumbnail);
			int height = cairo_image_surface_get_height(thumbnail);
			int x = row->x_offset + item->x_offset;
			int y = row->y_offset + g_row_height - height;

			// TODO(p): Test whether we need to render this first.

			cairo_save(cr);
			cairo_translate(cr, x, y);
			cairo_set_source_surface(cr, thumbnail, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	return TRUE;
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

	// TODO(p): Connect to this and emit it.
	browser_signals[ITEM_ACTIVATED] = g_signal_new("item-activated",
		G_TYPE_FROM_CLASS(klass), 0, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

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
		g_array_append_val(self->entries,
			((Entry){
				.thumbnail =
					rescale_thumbnail(fastiv_io_lookup_thumbnail(subpath)),
				.filename = subpath,
			}));
	}
	g_dir_close(dir);

	// TODO(p): Sort the entries.
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

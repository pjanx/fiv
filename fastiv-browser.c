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

#include "fastiv-browser.h"
#include "fastiv-io.h"
#include "fastiv-view.h"

typedef struct entry Entry;

struct entry {
	char *filename;
	cairo_surface_t *thumbnail;
};

static void
entry_free(Entry *self)
{
	g_free(self->filename);
	if (self->thumbnail)
		cairo_surface_destroy(self->thumbnail);
}

// --- Boilerplate -------------------------------------------------------------

struct _FastivBrowser {
	GtkWidget parent_instance;

	// TODO(p): We probably want to pre-arrange everything into rows.
	//  - All rows are the same height.
	GArray *entries;
	int selected;
};

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
fastiv_browser_get_preferred_width(GtkWidget *widget,
	gint *minimum, gint *natural)
{
	G_GNUC_UNUSED FastivBrowser *self = FASTIV_BROWSER(widget);
	// TODO(p): Set it to the width of the widget with one wide item within.
	*minimum = *natural = 0;
}

static void
fastiv_browser_get_preferred_height_for_width(GtkWidget *widget,
	G_GNUC_UNUSED gint width, gint *minimum, gint *natural)
{
	G_GNUC_UNUSED FastivBrowser *self = FASTIV_BROWSER(widget);
	// TODO(p): Re-layout, figure it out.
	*minimum = *natural = 0;
}

static void
fastiv_browser_realize(GtkWidget *widget)
{
	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);

	GdkWindowAttr attributes = {
		.window_type = GDK_WINDOW_CHILD,
		.x           = allocation.x,
		.y           = allocation.y,
		.width       = allocation.width,
		.height      = allocation.height,

		// Input-only would presumably also work (as in GtkPathBar, e.g.),
		// but it merely seems to involve more work.
		.wclass      = GDK_INPUT_OUTPUT,

		.visual      = gtk_widget_get_visual(widget),
		.event_mask  = gtk_widget_get_events(widget)
			| GDK_KEY_PRESS_MASK | GDK_BUTTON_PRESS_MASK,
	};

	// We need this window to receive input events at all.
	// TODO(p): See if input events bubble up to parents.
	GdkWindow *window = gdk_window_new(gtk_widget_get_parent_window(widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
	gtk_widget_register_window(widget, window);
	gtk_widget_set_window(widget, window);
	gtk_widget_set_realized(widget, TRUE);
}

static gboolean
fastiv_browser_draw(GtkWidget *widget, cairo_t *cr)
{
	G_GNUC_UNUSED FastivBrowser *self = FASTIV_BROWSER(widget);
	if (!gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget)))
		return TRUE;

	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);
	gtk_render_background(gtk_widget_get_style_context(widget), cr,
		0, 0, allocation.width, allocation.height);

	const double row_height = 256;

	gint occupied_width = 0, y = 0;
	for (guint i = 0; i < self->entries->len; i++) {
		const Entry *entry = &g_array_index(self->entries, Entry, i);
		if (!entry->thumbnail)
			continue;

		int width = cairo_image_surface_get_width(entry->thumbnail);
		int height = cairo_image_surface_get_height(entry->thumbnail);

		double scale = row_height / height;
		if (width * scale > 2 * row_height)
			scale = 2 * row_height / width;

		int projected_width = round(scale * width);
		int projected_height = round(scale * height);

		if (occupied_width != 0
		 && occupied_width + projected_width > allocation.width) {
			occupied_width = 0;
			y += row_height;
		}

		cairo_save(cr);
		cairo_translate(cr, occupied_width, y + row_height - projected_height);
		cairo_scale(cr, scale, scale);
		cairo_set_source_surface(cr, entry->thumbnail, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);

		occupied_width += projected_width;
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
	widget_class->get_preferred_width =
		fastiv_browser_get_preferred_width;
	widget_class->get_preferred_height_for_width =
		fastiv_browser_get_preferred_height_for_width;
	widget_class->realize = fastiv_browser_realize;
	widget_class->draw = fastiv_browser_draw;

	// TODO(p): Connect to this and emit it.
	browser_signals[ITEM_ACTIVATED] =
		g_signal_new("item-activated", G_TYPE_FROM_CLASS(klass),
			0, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

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
	self->selected = -1;
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
		if (!strcmp(filename, ".")
		 || !strcmp(filename, ".."))
			continue;

		gchar *subpath = g_build_filename(path, filename, NULL);
		g_array_append_val(self->entries, ((Entry) {
			.thumbnail = fastiv_io_lookup_thumbnail(subpath),
			.filename = subpath,
		}));
	}
	g_dir_close(dir);

	// TODO(p): Sort the entries.
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

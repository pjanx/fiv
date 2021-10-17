//
// fastiv-view.c: fast image viewer - view widget
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

#include "fastiv-view.h"

struct _FastivView {
	GtkWidget parent_instance;
	cairo_surface_t *surface;
	// TODO(p): Zoom-to-fit indication.
	double scale;
};

G_DEFINE_TYPE(FastivView, fastiv_view, GTK_TYPE_WIDGET)

static int
get_display_width(FastivView *self)
{
	if (!self->surface)
		return 0;

	return ceil(cairo_image_surface_get_width(self->surface) * self->scale);
}

static int
get_display_height(FastivView *self)
{
	if (!self->surface)
		return 0;

	return ceil(cairo_image_surface_get_height(self->surface) * self->scale);
}

static void
fastiv_view_finalize(GObject *gobject)
{
	FastivView *self = FASTIV_VIEW(gobject);
	cairo_surface_destroy(self->surface);

	G_OBJECT_CLASS(fastiv_view_parent_class)->finalize(gobject);
}

static void
fastiv_view_get_preferred_height(GtkWidget *widget,
	gint *minimum, gint *natural)
{
	*minimum = 0;
	*natural = 0;

	FastivView *self = FASTIV_VIEW(widget);
	*natural = get_display_height(self);
}

static void
fastiv_view_get_preferred_width(GtkWidget *widget,
	gint *minimum, gint *natural)
{
	*minimum = 0;
	*natural = 0;

	FastivView *self = FASTIV_VIEW(widget);
	*natural = get_display_width(self);
}

static void
fastiv_view_realize(GtkWidget *widget)
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

		// Assuming here that we can't ask for a higher-precision Visual
		// than what we get automatically.
		.visual      = gtk_widget_get_visual(widget),
		.event_mask  = gtk_widget_get_events(widget) | GDK_SCROLL_MASK,
	};

	// We need this window to receive input events at all.
	GdkWindow *window = gdk_window_new(gtk_widget_get_parent_window(widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
	gtk_widget_register_window(widget, window);
	gtk_widget_set_window(widget, window);
	gtk_widget_set_realized(widget, TRUE);
}

static gboolean
fastiv_view_draw(GtkWidget *widget, cairo_t *cr)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (!self->surface
	 || !gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget)))
		return TRUE;

	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);
	gtk_render_background(gtk_widget_get_style_context(widget), cr,
		0, 0, allocation.width, allocation.height);

	int w = get_display_width(self);
	int h = get_display_height(self);

	double x = 0;
	double y = 0;
	if (w < allocation.width)
		x = round((allocation.width - w) / 2.);
	if (h < allocation.height)
		y = round((allocation.height - h) / 2.);

	cairo_scale(cr, self->scale, self->scale);
	cairo_set_source_surface(cr, self->surface,
		x / self->scale, y / self->scale);

	// TODO(p): Prescale it ourselves to an off-screen bitmap, gamma-correctly.
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);

	cairo_paint(cr);
	return TRUE;
}

static gboolean
fastiv_view_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (!self->surface)
		return FALSE;

	switch (event->direction) {
	case GDK_SCROLL_UP:
		self->scale *= 1.4;
		gtk_widget_queue_resize(widget);
		return TRUE;
	case GDK_SCROLL_DOWN:
		self->scale /= 1.4;
		gtk_widget_queue_resize(widget);
		return TRUE;
	default:
		return FALSE;
	}
}

static void
fastiv_view_class_init(FastivViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fastiv_view_finalize;

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->get_preferred_height = fastiv_view_get_preferred_height;
	widget_class->get_preferred_width = fastiv_view_get_preferred_width;
	widget_class->realize = fastiv_view_realize;
	widget_class->draw = fastiv_view_draw;
	widget_class->scroll_event = fastiv_view_scroll_event;

	// TODO(p): Later override "screen_changed", recreate Pango layouts there,
	// if we get to have any, or otherwise reflect DPI changes.
	gtk_widget_class_set_css_name(widget_class, "fastiv-view");
}

static void
fastiv_view_init(FastivView *self)
{
	self->scale = 1.0;
}

// --- Picture loading ---------------------------------------------------------

// TODO(p): Progressive picture loading, or at least async/cancellable.
gboolean
fastiv_view_open(FastivView *self, const gchar *path, GError **error)
{
	cairo_surface_t *surface = fastiv_io_open(path, error);
	if (!surface)
		return FALSE;
	if (self->surface)
		cairo_surface_destroy(self->surface);

	self->surface = surface;
	gtk_widget_queue_resize(GTK_WIDGET(self));
	return TRUE;
}

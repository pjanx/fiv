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

static gboolean
fastiv_view_draw(GtkWidget *widget, cairo_t *cr)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (!self->surface)
		return TRUE;

	// TODO(p): Make this adjustable later.
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);

	int w = get_display_width(self);
	int h = get_display_height(self);

	double x = 0;
	double y = 0;
	if (w < allocation.width)
		x = (allocation.width - w) / 2;
	if (h < allocation.height)
		y = (allocation.height - h) / 2;

	cairo_scale(cr, self->scale, self->scale);
	cairo_set_source_surface(cr, self->surface,
		x / self->scale, y / self->scale);

	cairo_paint(cr);
	return TRUE;
}

static void
fastiv_view_class_init(FastivViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fastiv_view_finalize;

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->get_preferred_height = fastiv_view_get_preferred_height;
	widget_class->get_preferred_width = fastiv_view_get_preferred_width;
	widget_class->draw = fastiv_view_draw;
}

static void
fastiv_view_init(FastivView *self)
{
	self->scale = 1.0;

	gtk_widget_set_has_window(GTK_WIDGET(self), FALSE);
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

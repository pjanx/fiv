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
#include <stdbool.h>

#include "fastiv-io.h"
#include "fastiv-view.h"

struct _FastivView {
	GtkWidget parent_instance;
	cairo_surface_t *surface;
	bool scale_to_fit;
	double scale;
};

G_DEFINE_TYPE(FastivView, fastiv_view, GTK_TYPE_WIDGET)

static void
get_surface_dimensions(FastivView *self, double *width, double *height)
{
	*width = *height = 0;
	if (!self->surface)
		return;

	cairo_rectangle_t extents = {};
	switch (cairo_surface_get_type(self->surface)) {
	case CAIRO_SURFACE_TYPE_IMAGE:
		*width = cairo_image_surface_get_width(self->surface);
		*height = cairo_image_surface_get_height(self->surface);
		return;
	case CAIRO_SURFACE_TYPE_RECORDING:
		(void) cairo_recording_surface_get_extents(self->surface, &extents);
		*width = extents.width;
		*height = extents.height;
		return;
	default:
		g_assert_not_reached();
	}
}

static void
get_display_dimensions(FastivView *self, int *width, int *height)
{
	double w, h;
	get_surface_dimensions(self, &w, &h);

	*width = ceil(w * self->scale);
	*height = ceil(h * self->scale);
}

static void
fastiv_view_finalize(GObject *gobject)
{
	FastivView *self = FASTIV_VIEW(gobject);
	cairo_surface_destroy(self->surface);

	G_OBJECT_CLASS(fastiv_view_parent_class)->finalize(gobject);
}

static void
fastiv_view_get_preferred_height(
	GtkWidget *widget, gint *minimum, gint *natural)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (self->scale_to_fit) {
		double sw, sh;
		get_surface_dimensions(self, &sw, &sh);
		*natural = ceil(sh);
		*minimum = 1;
	} else {
		int dw, dh;
		get_display_dimensions(self, &dw, &dh);
		*minimum = *natural = dh;
	}
}

static void
fastiv_view_get_preferred_width(GtkWidget *widget, gint *minimum, gint *natural)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (self->scale_to_fit) {
		double sw, sh;
		get_surface_dimensions(self, &sw, &sh);
		*natural = ceil(sw);
		*minimum = 1;
	} else {
		int dw, dh;
		get_display_dimensions(self, &dw, &dh);
		*minimum = *natural = dw;
	}
}

static void
fastiv_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	GTK_WIDGET_CLASS(fastiv_view_parent_class)
		->size_allocate(widget, allocation);

	FastivView *self = FASTIV_VIEW(widget);
	if (!self->surface || !self->scale_to_fit)
		return;

	double w, h;
	get_surface_dimensions(self, &w, &h);

	self->scale = 1;
	if (ceil(w * self->scale) > allocation->width)
		self->scale = allocation->width / w;
	if (ceil(h * self->scale) > allocation->height)
		self->scale = allocation->height / h;
}

static void
fastiv_view_realize(GtkWidget *widget)
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

		// Assuming here that we can't ask for a higher-precision Visual
		// than what we get automatically.
		.visual = gtk_widget_get_visual(widget),
		.event_mask = gtk_widget_get_events(widget) | GDK_SCROLL_MASK |
			GDK_KEY_PRESS_MASK | GDK_BUTTON_PRESS_MASK,
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
	if (!self->surface ||
		!gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget)))
		return TRUE;

	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);
	gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0,
		allocation.width, allocation.height);

	int w, h;
	get_display_dimensions(self, &w, &h);

	double x = 0;
	double y = 0;
	if (w < allocation.width)
		x = round((allocation.width - w) / 2.);
	if (h < allocation.height)
		y = round((allocation.height - h) / 2.);

	// FIXME: Recording surfaces do not work well with CAIRO_SURFACE_TYPE_XLIB,
	// we always get a shitty pixmap, where transparency contains junk.
	if (cairo_surface_get_type(self->surface) == CAIRO_SURFACE_TYPE_RECORDING) {
		cairo_surface_t *image =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		cairo_t *tcr = cairo_create(image);
		cairo_scale(tcr, self->scale, self->scale);
		cairo_set_source_surface(tcr, self->surface, 0, 0);
		cairo_paint(tcr);
		cairo_destroy(tcr);

		cairo_set_source_surface(cr, image, x, y);
		cairo_paint(cr);
		cairo_surface_destroy(image);
		return TRUE;
	}

	// XXX: The rounding together with padding may result in up to
	// a pixel's worth of made-up picture data.
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);

	cairo_scale(cr, self->scale, self->scale);
	cairo_set_source_surface(
		cr, self->surface, x / self->scale, y / self->scale);

	cairo_pattern_t *pattern = cairo_get_source(cr);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
	// TODO(p): Prescale it ourselves to an off-screen bitmap, gamma-correctly.
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);

	cairo_paint(cr);
	return TRUE;
}

#define SCALE_STEP 1.4

static gboolean
on_scale_updated(FastivView *self)
{
	self->scale_to_fit = false;
	gtk_widget_queue_resize(GTK_WIDGET(self));
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
		self->scale *= SCALE_STEP;
		return on_scale_updated(self);
	case GDK_SCROLL_DOWN:
		self->scale /= SCALE_STEP;
		return on_scale_updated(self);
	default:
		return FALSE;
	}
}

static gboolean
fastiv_view_key_press_event(GtkWidget *widget, GdkEventKey *event)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (event->state & gtk_accelerator_get_default_mod_mask())
		return FALSE;

	switch (event->keyval) {
	case GDK_KEY_1:
		self->scale = 1;
		return on_scale_updated(self);
	case GDK_KEY_plus:
		self->scale *= SCALE_STEP;
		return on_scale_updated(self);
	case GDK_KEY_minus:
		self->scale /= SCALE_STEP;
		return on_scale_updated(self);
	case GDK_KEY_f:
		self->scale_to_fit = !self->scale_to_fit;
		gtk_widget_queue_resize(GTK_WIDGET(self));
	}
	return FALSE;
}

static void
fastiv_view_class_init(FastivViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fastiv_view_finalize;

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->get_preferred_height = fastiv_view_get_preferred_height;
	widget_class->get_preferred_width = fastiv_view_get_preferred_width;
	widget_class->size_allocate = fastiv_view_size_allocate;
	widget_class->realize = fastiv_view_realize;
	widget_class->draw = fastiv_view_draw;
	widget_class->scroll_event = fastiv_view_scroll_event;
	widget_class->key_press_event = fastiv_view_key_press_event;

	// TODO(p): Later override "screen_changed", recreate Pango layouts there,
	// if we get to have any, or otherwise reflect DPI changes.
	gtk_widget_class_set_css_name(widget_class, "fastiv-view");
}

static void
fastiv_view_init(FastivView *self)
{
	gtk_widget_set_can_focus(GTK_WIDGET(self), TRUE);

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
	self->scale = 1.0;
	self->scale_to_fit = true;
	gtk_widget_queue_resize(GTK_WIDGET(self));
	return TRUE;
}

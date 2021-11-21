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

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif  // GDK_WINDOWING_X11
#ifdef GDK_WINDOWING_QUARTZ
#include <gdk/gdkquartz.h>
#endif  // GDK_WINDOWING_QUARTZ

#include "fastiv-io.h"
#include "fastiv-view.h"

struct _FastivView {
	GtkWidget parent_instance;
	cairo_surface_t *surface;
	bool scale_to_fit;
	double scale;
};

G_DEFINE_TYPE(FastivView, fastiv_view, GTK_TYPE_WIDGET)

enum {
	PROP_SCALE = 1,
	PROP_SCALE_TO_FIT,
	N_PROPERTIES
};

static GParamSpec *view_properties[N_PROPERTIES];

static void
fastiv_view_finalize(GObject *gobject)
{
	FastivView *self = FASTIV_VIEW(gobject);
	cairo_surface_destroy(self->surface);

	G_OBJECT_CLASS(fastiv_view_parent_class)->finalize(gobject);
}

static void
fastiv_view_get_property(
	GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	FastivView *self = FASTIV_VIEW(object);
	switch (property_id) {
	case PROP_SCALE:
		g_value_set_double(value, self->scale);
		break;
	case PROP_SCALE_TO_FIT:
		g_value_set_boolean(value, self->scale_to_fit);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

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
		if (!cairo_recording_surface_get_extents(self->surface, &extents))
			cairo_recording_surface_ink_extents(self->surface,
				&extents.x, &extents.y, &extents.width, &extents.height);

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
	g_object_notify_by_pspec(G_OBJECT(widget), view_properties[PROP_SCALE]);
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

	// Without the following call, or the rendering mode set to "recording",
	// RGB30 degrades to RGB24, because gdk_window_begin_paint_internal()
	// creates backing stores using cairo_content_t constants.
	//
	// It completely breaks the Quartz backend, so limit it to X11.
#ifdef GDK_WINDOWING_X11
	// FIXME: This causes some flicker while scrolling, because it disables
	// double buffering, see: https://gitlab.gnome.org/GNOME/gtk/-/issues/2560
	//
	// If GTK+'s OpenGL integration fails to deliver, we need to use the window
	// directly, sidestepping the toolkit entirely.
	if (GDK_IS_X11_WINDOW(window))
		gdk_window_ensure_native(window);
#endif  // GDK_WINDOWING_X11

	gtk_widget_register_window(widget, window);
	gtk_widget_set_window(widget, window);
	gtk_widget_set_realized(widget, TRUE);
}

static gboolean
fastiv_view_draw(GtkWidget *widget, cairo_t *cr)
{
	// Placed here due to our using a native GdkWindow on X11,
	// which makes the widget have no double buffering or default background.
	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);
	gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0,
		allocation.width, allocation.height);

	FastivView *self = FASTIV_VIEW(widget);
	if (!self->surface ||
		!gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget)))
		return TRUE;

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

#ifdef GDK_WINDOWING_QUARTZ
	// Not supported there. Acts a bit like repeating, but weirdly offset.
	if (GDK_IS_QUARTZ_WINDOW(gtk_widget_get_window(widget)))
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);
#endif  // GDK_WINDOWING_QUARTZ

	cairo_paint(cr);
	return TRUE;
}

#define SCALE_STEP 1.4

static gboolean
set_scale_to_fit(FastivView *self, bool scale_to_fit)
{
	self->scale_to_fit = scale_to_fit;

	gtk_widget_queue_resize(GTK_WIDGET(self));
	g_object_notify_by_pspec(
		G_OBJECT(self), view_properties[PROP_SCALE_TO_FIT]);
	return TRUE;
}

static gboolean
set_scale(FastivView *self, double scale)
{
	self->scale = scale;
	g_object_notify_by_pspec(
		G_OBJECT(self), view_properties[PROP_SCALE]);
	return set_scale_to_fit(self, false);
}

static gboolean
fastiv_view_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (!self->surface)
		return FALSE;
	if (event->state & gtk_accelerator_get_default_mod_mask())
		return FALSE;

	switch (event->direction) {
	case GDK_SCROLL_UP:
		return set_scale(self, self->scale * SCALE_STEP);
	case GDK_SCROLL_DOWN:
		return set_scale(self, self->scale / SCALE_STEP);
	default:
		return FALSE;
	}
}

static gboolean
fastiv_view_key_press_event(GtkWidget *widget, GdkEventKey *event)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (event->state & ~GDK_SHIFT_MASK & gtk_accelerator_get_default_mod_mask())
		return FALSE;

	switch (event->keyval) {
	case GDK_KEY_1:
		return set_scale(self, 1.0);
	case GDK_KEY_plus:
		return set_scale(self, self->scale * SCALE_STEP);
	case GDK_KEY_minus:
		return set_scale(self, self->scale / SCALE_STEP);
	case GDK_KEY_F:
		return set_scale_to_fit(self, !self->scale_to_fit);
	}
	return FALSE;
}

static void
fastiv_view_class_init(FastivViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fastiv_view_finalize;
	object_class->get_property = fastiv_view_get_property;

	view_properties[PROP_SCALE] = g_param_spec_double(
		"scale", "Scale", "Zoom level",
		0, G_MAXDOUBLE, 1.0, G_PARAM_READABLE);
	view_properties[PROP_SCALE_TO_FIT] = g_param_spec_boolean(
		"scale-to-fit", "Scale to fit", "Scale images down to fit the window",
		TRUE, G_PARAM_READABLE);
	g_object_class_install_properties(
		object_class, N_PROPERTIES, view_properties);

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
	set_scale_to_fit(self, true);
	return TRUE;
}

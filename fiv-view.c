//
// fiv-view.c: image viewing widget
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

#include "config.h"

#include "fiv-io.h"
#include "fiv-view.h"

#include <math.h>
#include <stdbool.h>

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif  // GDK_WINDOWING_X11
#ifdef GDK_WINDOWING_QUARTZ
#include <gdk/gdkquartz.h>
#endif  // GDK_WINDOWING_QUARTZ

struct _FivView {
	GtkWidget parent_instance;
	gchar *uri;                         ///< Path to the current image (if any)
	cairo_surface_t *image;             ///< The loaded image (sequence)
	cairo_surface_t *page;              ///< Current page within image, weak
	cairo_surface_t *frame;             ///< Current frame within page, weak
	FivIoOrientation orientation;       ///< Current page orientation
	bool enable_cms : 1;                ///< Smooth scaling toggle
	bool filter : 1;                    ///< Smooth scaling toggle
	bool checkerboard : 1;              ///< Show checkerboard background
	bool enhance : 1;                   ///< Try to enhance picture data
	bool scale_to_fit : 1;              ///< Image no larger than the allocation
	double scale;                       ///< Scaling factor

	cairo_surface_t *enhance_swap;      ///< Quick swap in/out
	FivIoProfile screen_cms_profile;    ///< Target colour profile for widget

	int remaining_loops;                ///< Greater than zero if limited
	gint64 frame_time;                  ///< Current frame's start, µs precision
	gulong frame_update_connection;     ///< GdkFrameClock::update
};

G_DEFINE_TYPE(FivView, fiv_view, GTK_TYPE_WIDGET)

typedef struct _Dimensions {
	double width, height;
} Dimensions;

static FivIoOrientation view_left[9] = {
	[FivIoOrientationUnknown]   = FivIoOrientationUnknown,
	[FivIoOrientation0]         = FivIoOrientation270,
	[FivIoOrientationMirror0]   = FivIoOrientationMirror270,
	[FivIoOrientation180]       = FivIoOrientation90,
	[FivIoOrientationMirror180] = FivIoOrientationMirror90,
	[FivIoOrientationMirror270] = FivIoOrientationMirror180,
	[FivIoOrientation90]        = FivIoOrientation0,
	[FivIoOrientationMirror90]  = FivIoOrientationMirror0,
	[FivIoOrientation270]       = FivIoOrientation180,
};

static FivIoOrientation view_mirror[9] = {
	[FivIoOrientationUnknown]   = FivIoOrientationUnknown,
	[FivIoOrientation0]         = FivIoOrientationMirror0,
	[FivIoOrientationMirror0]   = FivIoOrientation0,
	[FivIoOrientation180]       = FivIoOrientationMirror180,
	[FivIoOrientationMirror180] = FivIoOrientation180,
	[FivIoOrientationMirror270] = FivIoOrientation270,
	[FivIoOrientation90]        = FivIoOrientationMirror270,
	[FivIoOrientationMirror90]  = FivIoOrientation90,
	[FivIoOrientation270]       = FivIoOrientationMirror270,
};

static FivIoOrientation view_right[9] = {
	[FivIoOrientationUnknown]   = FivIoOrientationUnknown,
	[FivIoOrientation0]         = FivIoOrientation90,
	[FivIoOrientationMirror0]   = FivIoOrientationMirror90,
	[FivIoOrientation180]       = FivIoOrientation270,
	[FivIoOrientationMirror180] = FivIoOrientationMirror270,
	[FivIoOrientationMirror270] = FivIoOrientationMirror0,
	[FivIoOrientation90]        = FivIoOrientation180,
	[FivIoOrientationMirror90]  = FivIoOrientationMirror180,
	[FivIoOrientation270]       = FivIoOrientation0,
};

enum {
	PROP_SCALE = 1,
	PROP_SCALE_TO_FIT,
	PROP_ENABLE_CMS,
	PROP_FILTER,
	PROP_CHECKERBOARD,
	PROP_ENHANCE,
	PROP_PLAYING,
	PROP_HAS_IMAGE,
	PROP_CAN_ANIMATE,
	PROP_HAS_PREVIOUS_PAGE,
	PROP_HAS_NEXT_PAGE,
	N_PROPERTIES
};

static GParamSpec *view_properties[N_PROPERTIES];

static void
fiv_view_finalize(GObject *gobject)
{
	FivView *self = FIV_VIEW(gobject);
	g_clear_pointer(&self->screen_cms_profile, fiv_io_profile_free);
	g_clear_pointer(&self->enhance_swap, cairo_surface_destroy);
	g_clear_pointer(&self->image, cairo_surface_destroy);
	g_free(self->uri);

	G_OBJECT_CLASS(fiv_view_parent_class)->finalize(gobject);
}

static void
fiv_view_get_property(
	GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	FivView *self = FIV_VIEW(object);
	switch (property_id) {
	case PROP_SCALE:
		g_value_set_double(value, self->scale);
		break;
	case PROP_SCALE_TO_FIT:
		g_value_set_boolean(value, self->scale_to_fit);
		break;
	case PROP_ENABLE_CMS:
		g_value_set_boolean(value, self->enable_cms);
		break;
	case PROP_FILTER:
		g_value_set_boolean(value, self->filter);
		break;
	case PROP_CHECKERBOARD:
		g_value_set_boolean(value, self->checkerboard);
		break;
	case PROP_ENHANCE:
		g_value_set_boolean(value, self->enhance);
		break;
	case PROP_PLAYING:
		g_value_set_boolean(value, !!self->frame_update_connection);
		break;
	case PROP_HAS_IMAGE:
		g_value_set_boolean(value, !!self->image);
		break;
	case PROP_CAN_ANIMATE:
		g_value_set_boolean(value, self->page &&
			cairo_surface_get_user_data(self->page, &fiv_io_key_frame_next));
		break;
	case PROP_HAS_PREVIOUS_PAGE:
		g_value_set_boolean(value, self->image && self->page != self->image);
		break;
	case PROP_HAS_NEXT_PAGE:
		g_value_set_boolean(value, self->page &&
			cairo_surface_get_user_data(self->page, &fiv_io_key_page_next));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
fiv_view_set_property(
	GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	FivView *self = FIV_VIEW(object);
	switch (property_id) {
	case PROP_SCALE_TO_FIT:
		if (self->scale_to_fit != g_value_get_boolean(value))
			fiv_view_command(self, FIV_VIEW_COMMAND_TOGGLE_SCALE_TO_FIT);
		break;
	case PROP_ENABLE_CMS:
		if (self->enable_cms != g_value_get_boolean(value))
			fiv_view_command(self, FIV_VIEW_COMMAND_TOGGLE_CMS);
		break;
	case PROP_FILTER:
		if (self->filter != g_value_get_boolean(value))
			fiv_view_command(self, FIV_VIEW_COMMAND_TOGGLE_FILTER);
		break;
	case PROP_CHECKERBOARD:
		if (self->checkerboard != g_value_get_boolean(value))
			fiv_view_command(self, FIV_VIEW_COMMAND_TOGGLE_CHECKERBOARD);
		break;
	case PROP_ENHANCE:
		if (self->enhance != g_value_get_boolean(value))
			fiv_view_command(self, FIV_VIEW_COMMAND_TOGGLE_ENHANCE);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static Dimensions
get_surface_dimensions(FivView *self)
{
	if (!self->image)
		return (Dimensions) {};

	cairo_rectangle_t extents = {};
	switch (cairo_surface_get_type(self->page)) {
	case CAIRO_SURFACE_TYPE_IMAGE:
		extents.width = cairo_image_surface_get_width(self->page);
		extents.height = cairo_image_surface_get_height(self->page);
		break;
	case CAIRO_SURFACE_TYPE_RECORDING:
		if (!cairo_recording_surface_get_extents(self->page, &extents))
			cairo_recording_surface_ink_extents(self->page,
				&extents.x, &extents.y, &extents.width, &extents.height);
		break;
	default:
		g_assert_not_reached();
	}

	if (fiv_io_orientation_is_sideways(self->orientation))
		return (Dimensions) {extents.height, extents.width};

	return (Dimensions) {extents.width, extents.height};
}

static void
get_display_dimensions(FivView *self, int *width, int *height)
{
	Dimensions surface_dimensions = get_surface_dimensions(self);
	*width = ceil(surface_dimensions.width * self->scale);
	*height = ceil(surface_dimensions.height * self->scale);
}

static void
fiv_view_get_preferred_height(GtkWidget *widget, gint *minimum, gint *natural)
{
	FivView *self = FIV_VIEW(widget);
	if (self->scale_to_fit) {
		*natural = ceil(get_surface_dimensions(self).height);
		*minimum = 1;
	} else {
		int dw, dh;
		get_display_dimensions(self, &dw, &dh);
		*minimum = *natural = dh;
	}
}

static void
fiv_view_get_preferred_width(GtkWidget *widget, gint *minimum, gint *natural)
{
	FivView *self = FIV_VIEW(widget);
	if (self->scale_to_fit) {
		*natural = ceil(get_surface_dimensions(self).width);
		*minimum = 1;
	} else {
		int dw, dh;
		get_display_dimensions(self, &dw, &dh);
		*minimum = *natural = dw;
	}
}

static void
fiv_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	GTK_WIDGET_CLASS(fiv_view_parent_class)->size_allocate(widget, allocation);

	FivView *self = FIV_VIEW(widget);
	if (!self->image || !self->scale_to_fit)
		return;

	Dimensions surface_dimensions = get_surface_dimensions(self);
	self->scale = 1;

	if (ceil(surface_dimensions.width * self->scale) > allocation->width)
		self->scale = allocation->width / surface_dimensions.width;
	if (ceil(surface_dimensions.height * self->scale) > allocation->height)
		self->scale = allocation->height / surface_dimensions.height;
	g_object_notify_by_pspec(G_OBJECT(widget), view_properties[PROP_SCALE]);
}

// https://www.freedesktop.org/wiki/OpenIcc/ICC_Profiles_in_X_Specification_0.4
// has disappeared, but you can use the wayback machine.
//
// Note that Wayland does not have any appropriate protocol, as of writing:
// https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/14
static void
reload_screen_cms_profile(FivView *self, GdkWindow *window)
{
	g_clear_pointer(&self->screen_cms_profile, fiv_io_profile_free);

	GdkDisplay *display = gdk_window_get_display(window);
	GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, window);

	int num = -1;
	for (int i = gdk_display_get_n_monitors(display); num < 0 && i--; )
		if (gdk_display_get_monitor(display, i) == monitor)
			num = i;
	if (num < 0)
		goto out;

	char atom[32] = "";
	g_snprintf(atom, sizeof atom, "_ICC_PROFILE%c%d", num ? '_' : '\0', num);

	// Sadly, there is no nice GTK+/GDK mechanism to watch this for changes.
	int format = 0, length = 0;
	GdkAtom type = GDK_NONE;
	guchar *data = NULL;
	GdkWindow *root = gdk_screen_get_root_window(gdk_window_get_screen(window));
	if (gdk_property_get(root, gdk_atom_intern(atom, FALSE), GDK_NONE, 0,
			8 << 20 /* MiB */, FALSE, &type, &format, &length, &data)) {
		if (format == 8 && length > 0)
			self->screen_cms_profile = fiv_io_profile_new(data, length);
		g_free(data);
	}

out:
	if (!self->screen_cms_profile)
		self->screen_cms_profile = fiv_io_profile_new_sRGB();
}

static void
fiv_view_realize(GtkWidget *widget)
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

	reload_screen_cms_profile(FIV_VIEW(widget), window);
}

static gboolean
fiv_view_draw(GtkWidget *widget, cairo_t *cr)
{
	// Placed here due to our using a native GdkWindow on X11,
	// which makes the widget have no double buffering or default background.
	GtkAllocation allocation;
	gtk_widget_get_allocation(widget, &allocation);
	GtkStyleContext *style = gtk_widget_get_style_context(widget);
	gtk_render_background(style, cr, 0, 0, allocation.width, allocation.height);

	FivView *self = FIV_VIEW(widget);
	if (!self->image ||
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

	Dimensions surface_dimensions = get_surface_dimensions(self);
	cairo_matrix_t matrix = fiv_io_orientation_matrix(
		self->orientation, surface_dimensions.width, surface_dimensions.height);
	cairo_translate(cr, x, y);
	if (self->checkerboard) {
		gtk_style_context_save(style);
		gtk_style_context_add_class(style, "checkerboard");
		gtk_render_background(style, cr, 0, 0, w, h);
		gtk_style_context_restore(style);
	}

	// FIXME: Recording surfaces do not work well with CAIRO_SURFACE_TYPE_XLIB,
	// we always get a shitty pixmap, where transparency contains junk.
	if (cairo_surface_get_type(self->frame) == CAIRO_SURFACE_TYPE_RECORDING) {
		cairo_surface_t *image =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		cairo_t *tcr = cairo_create(image);
		cairo_scale(tcr, self->scale, self->scale);
		cairo_set_source_surface(tcr, self->frame, 0, 0);
		cairo_pattern_set_matrix(cairo_get_source(tcr), &matrix);
		cairo_paint(tcr);
		cairo_destroy(tcr);

		cairo_set_source_surface(cr, image, 0, 0);
		cairo_paint(cr);
		cairo_surface_destroy(image);
		return TRUE;
	}

	// XXX: The rounding together with padding may result in up to
	// a pixel's worth of made-up picture data.
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_clip(cr);

	cairo_scale(cr, self->scale, self->scale);
	cairo_set_source_surface(cr, self->frame, 0, 0);

	cairo_pattern_t *pattern = cairo_get_source(cr);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);

	// TODO(p): Prescale it ourselves to an off-screen bitmap, gamma-correctly.
	if (self->filter)
		cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);
	else
		cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);

#ifdef GDK_WINDOWING_QUARTZ
	// Not supported there. Acts a bit like repeating, but weirdly offset.
	if (GDK_IS_QUARTZ_WINDOW(gtk_widget_get_window(widget)))
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);
#endif  // GDK_WINDOWING_QUARTZ

	cairo_paint(cr);
	return TRUE;
}

static gboolean
fiv_view_button_press_event(GtkWidget *widget, GdkEventButton *event)
{
	GTK_WIDGET_CLASS(fiv_view_parent_class)->button_press_event(widget, event);

	if (event->button == GDK_BUTTON_PRIMARY &&
		gtk_widget_get_focus_on_click(widget))
		gtk_widget_grab_focus(widget);

	// TODO(p): Use for left button scroll drag, which may rather be a gesture.
	return FALSE;
}

#define SCALE_STEP 1.25

static gboolean
set_scale_to_fit(FivView *self, bool scale_to_fit)
{
	if (self->scale_to_fit != scale_to_fit) {
		self->scale_to_fit = scale_to_fit;
		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_SCALE_TO_FIT]);
		gtk_widget_queue_resize(GTK_WIDGET(self));
	}
	return TRUE;
}

static gboolean
set_scale(FivView *self, double scale)
{
	self->scale = scale;
	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_SCALE]);
	gtk_widget_queue_resize(GTK_WIDGET(self));
	return set_scale_to_fit(self, false);
}

static gboolean
set_scale_to_fit_width(FivView *self)
{
	double w = get_surface_dimensions(self).width;
	int allocated = gtk_widget_get_allocated_width(
		gtk_widget_get_parent(GTK_WIDGET(self)));
	if (ceil(w * self->scale) > allocated)
		return set_scale(self, allocated / w);
	return TRUE;
}

static gboolean
set_scale_to_fit_height(FivView *self)
{
	double h = get_surface_dimensions(self).height;
	int allocated = gtk_widget_get_allocated_height(
		gtk_widget_get_parent(GTK_WIDGET(self)));
	if (ceil(h * self->scale) > allocated)
		return set_scale(self, allocated / h);
	return TRUE;
}

static gboolean
fiv_view_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
	FivView *self = FIV_VIEW(widget);
	if (!self->image)
		return FALSE;
	if (event->state & gtk_accelerator_get_default_mod_mask())
		return FALSE;

	switch (event->direction) {
	case GDK_SCROLL_UP:
		return set_scale(self, self->scale * SCALE_STEP);
	case GDK_SCROLL_DOWN:
		return set_scale(self, self->scale / SCALE_STEP);
	default:
		// For some reason, we can also get GDK_SCROLL_SMOOTH.
		// Left/right are good to steal from GtkScrolledWindow for consistency.
		return TRUE;
	}
}

static void
stop_animating(FivView *self)
{
	GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(self));
	if (!clock || !self->frame_update_connection)
		return;

	g_signal_handler_disconnect(clock, self->frame_update_connection);
	gdk_frame_clock_end_updating(clock);

	self->frame_time = 0;
	self->frame_update_connection = 0;
	self->remaining_loops = 0;
	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_PLAYING]);
}

static gboolean
advance_frame(FivView *self)
{
	cairo_surface_t *next =
		cairo_surface_get_user_data(self->frame, &fiv_io_key_frame_next);
	if (next) {
		self->frame = next;
	} else {
		if (self->remaining_loops && !--self->remaining_loops)
			return FALSE;

		self->frame = self->page;
	}
	return TRUE;
}

static gboolean
advance_animation(FivView *self, GdkFrameClock *clock)
{
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	while (true) {
		// TODO(p): See if infinite frames can actually happen, and how.
		intptr_t duration = (intptr_t) cairo_surface_get_user_data(
			self->frame, &fiv_io_key_frame_duration);
		if (duration < 0)
			return FALSE;

		// Do not busy loop. GIF timings are given in hundredths of a second.
		// Note that browsers seem to do [< 10] => 100:
		// https://bugs.webkit.org/show_bug.cgi?id=36082
		if (duration == 0)
			duration = gdk_frame_timings_get_refresh_interval(
				gdk_frame_clock_get_current_timings(clock)) / 1000;
		if (duration == 0)
			duration = 1;

		gint64 then = self->frame_time + duration * 1000;
		if (then > now)
			return TRUE;
		if (!advance_frame(self))
			return FALSE;

		self->frame_time = then;
		gtk_widget_queue_draw(GTK_WIDGET(self));
	}
}

static void
on_frame_clock_update(GdkFrameClock *clock, gpointer user_data)
{
	FivView *self = FIV_VIEW(user_data);
	if (!advance_animation(self, clock))
		stop_animating(self);
}

static void
start_animating(FivView *self)
{
	stop_animating(self);

	GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(self));
	if (!clock || !self->image ||
		!cairo_surface_get_user_data(self->page, &fiv_io_key_frame_next))
		return;

	self->frame_time = gdk_frame_clock_get_frame_time(clock);
	self->frame_update_connection = g_signal_connect(
		clock, "update", G_CALLBACK(on_frame_clock_update), self);
	self->remaining_loops =
		(uintptr_t) cairo_surface_get_user_data(self->page, &fiv_io_key_loops);

	gdk_frame_clock_begin_updating(clock);
	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_PLAYING]);
}

static void
switch_page(FivView *self, cairo_surface_t *page)
{
	self->frame = self->page = page;
	if ((self->orientation = (uintptr_t) cairo_surface_get_user_data(
			self->page, &fiv_io_key_orientation)) == FivIoOrientationUnknown)
		self->orientation = FivIoOrientation0;

	start_animating(self);
	gtk_widget_queue_resize(GTK_WIDGET(self));

	g_object_notify_by_pspec(
		G_OBJECT(self), view_properties[PROP_CAN_ANIMATE]);
	g_object_notify_by_pspec(
		G_OBJECT(self), view_properties[PROP_HAS_PREVIOUS_PAGE]);
	g_object_notify_by_pspec(
		G_OBJECT(self), view_properties[PROP_HAS_NEXT_PAGE]);
}

static void
fiv_view_map(GtkWidget *widget)
{
	GTK_WIDGET_CLASS(fiv_view_parent_class)->map(widget);

	// Loading before mapping will fail to obtain a GdkFrameClock.
	start_animating(FIV_VIEW(widget));
}

void
fiv_view_unmap(GtkWidget *widget)
{
	stop_animating(FIV_VIEW(widget));
	GTK_WIDGET_CLASS(fiv_view_parent_class)->unmap(widget);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
show_error_dialog(GtkWindow *parent, GError *error)
{
	GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", error->message);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	g_error_free(error);
}

static GtkWindow *
get_toplevel(GtkWidget *widget)
{
	if (GTK_IS_WINDOW((widget = gtk_widget_get_toplevel(widget))))
		return GTK_WINDOW(widget);
	return NULL;
}

static void
on_draw_page(G_GNUC_UNUSED GtkPrintOperation *operation,
	GtkPrintContext *context, G_GNUC_UNUSED int page_nr, FivView *self)
{
	// Any DPI will be wrong, unless we import that information from the image.
	double scale = 1 / 96.;
	Dimensions surface_dimensions = get_surface_dimensions(self);
	double w = surface_dimensions.width * scale;
	double h = surface_dimensions.height * scale;

	// Scale down to fit the print area, taking care to not divide by zero.
	double areaw = gtk_print_context_get_width(context);
	double areah = gtk_print_context_get_height(context);
	scale *= fmin((areaw < w) ? areaw / w : 1, (areah < h) ? areah / h : 1);

	cairo_t *cr = gtk_print_context_get_cairo_context(context);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, self->frame, 0, 0);
	cairo_matrix_t matrix = fiv_io_orientation_matrix(
		self->orientation, surface_dimensions.width, surface_dimensions.height);
	cairo_pattern_set_matrix(cairo_get_source(cr), &matrix);
	cairo_paint(cr);
}

static void
print(FivView *self)
{
	GtkPrintOperation *print = gtk_print_operation_new();
	gtk_print_operation_set_n_pages(print, 1);
	gtk_print_operation_set_embed_page_setup(print, TRUE);
	gtk_print_operation_set_unit(print, GTK_UNIT_INCH);
	gtk_print_operation_set_job_name(print, "Image");
	g_signal_connect(print, "draw-page", G_CALLBACK(on_draw_page), self);

	static GtkPrintSettings *settings = NULL;
	if (settings != NULL)
		gtk_print_operation_set_print_settings(print, settings);

	GError *error = NULL;
	GtkWindow *window = get_toplevel(GTK_WIDGET(self));
	GtkPrintOperationResult res = gtk_print_operation_run(
		print, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, window, &error);
	if (res == GTK_PRINT_OPERATION_RESULT_APPLY) {
		if (settings != NULL)
			g_object_unref(settings);
		settings = g_object_ref(gtk_print_operation_get_print_settings(print));
	}
	if (error)
		show_error_dialog(window, error);

	g_object_unref(print);
}

static gboolean
save_as(FivView *self, cairo_surface_t *frame)
{
	GtkWindow *window = get_toplevel(GTK_WIDGET(self));
	FivIoProfile target = NULL;
	if (self->enable_cms && (target = self->screen_cms_profile)) {
		GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL,
			GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE, "%s",
			"Color management overrides attached color profiles.");
		gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
	}

	GtkWidget *dialog = gtk_file_chooser_dialog_new(
		frame ? "Save frame as" : "Save page as",
		window, GTK_FILE_CHOOSER_ACTION_SAVE,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);

	GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
	gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);

	GFile *file = g_file_new_for_uri(self->uri);
	const gchar *path = g_file_peek_path(file);
	// TODO(p): Use g_file_info_get_display_name().
	gchar *basename = g_filename_display_basename(path ? path : self->uri);

	// Note that GTK+'s save dialog is too stupid to automatically change
	// the extension when user changes the filter. Presumably,
	// gtk_file_chooser_set_extra_widget() can be used to circumvent this.
	gchar *name =
		g_strdup_printf(frame ? "%s.frame.webp" : "%s.webp", basename);
	gtk_file_chooser_set_current_name(chooser, name);
	g_free(name);
	if (path) {
		gchar *dirname = g_path_get_dirname(path);
		gtk_file_chooser_set_current_folder(chooser, dirname);
		g_free(dirname);
	}
	g_free(basename);
	g_object_unref(file);

	// This is the best general format: supports lossless encoding, animations,
	// alpha channel, and Exif and ICC profile metadata.
	// PNG is another viable option, but sPNG can't do APNG, Wuffs can't save,
	// and libpng is a pain in the arse.
	GtkFileFilter *webp_filter = gtk_file_filter_new();
	gtk_file_filter_add_mime_type(webp_filter, "image/webp");
	gtk_file_filter_add_pattern(webp_filter, "*.webp");
	gtk_file_filter_set_name(webp_filter, "Lossless WebP (*.webp)");
	gtk_file_chooser_add_filter(chooser, webp_filter);

	// The format is supported by Exiv2 and ExifTool.
	// This is mostly a developer tool.
	GtkFileFilter *exv_filter = gtk_file_filter_new();
	gtk_file_filter_add_mime_type(exv_filter, "image/x-exv");
	gtk_file_filter_add_pattern(exv_filter, "*.exv");
	gtk_file_filter_set_name(exv_filter, "Exiv2 metadata (*.exv)");
	gtk_file_chooser_add_filter(chooser, exv_filter);

	GError *error = NULL;
	switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
		gchar *path;
	case GTK_RESPONSE_ACCEPT:
		path = gtk_file_chooser_get_filename(chooser);
		if (!(gtk_file_chooser_get_filter(chooser) == webp_filter
					? fiv_io_save(self->page, frame, target, path, &error)
					: fiv_io_save_metadata(self->page, path, &error)))
			show_error_dialog(window, error);
		g_free(path);
		// Fall-through.
	default:
		gtk_widget_destroy(dialog);
		// Fall-through.
	case GTK_RESPONSE_NONE:
		return TRUE;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static GtkWidget *
info_start_group(GtkWidget *vbox, const char *group)
{
	GtkWidget *label = gtk_label_new(group);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_widget_set_halign(label, GTK_ALIGN_FILL);
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
	gtk_label_set_attributes(GTK_LABEL(label), attrs);
	pango_attr_list_unref(attrs);

	GtkWidget *grid = gtk_grid_new();
	GtkWidget *expander = gtk_expander_new(NULL);
	gtk_expander_set_label_widget(GTK_EXPANDER(expander), label);
	gtk_expander_set_expanded(GTK_EXPANDER(expander), TRUE);
	gtk_container_add(GTK_CONTAINER(expander), grid);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_box_pack_start(GTK_BOX(vbox), expander, FALSE, FALSE, 0);
	return grid;
}

static GtkWidget *
info_parse(char *tsv)
{
	GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

	const char *last_group = NULL;
	GtkWidget *grid = NULL;
	int line = 1, row = 0;
	for (char *nl; (nl = strchr(tsv, '\n')); line++, tsv = ++nl) {
		*nl = 0;
		if (nl > tsv && nl[-1] == '\r')
			nl[-1] = 0;

		char *group = tsv, *tag = strchr(group, '\t');
		if (!tag) {
			g_warning("ExifTool parse error on line %d", line);
			continue;
		}

		*tag++ = 0;
		for (char *p = group; *p; p++)
			if (*p == '_')
				*p = ' ';

		char *value = strchr(tag, '\t');
		if (!value) {
			g_warning("ExifTool parse error on line %d", line);
			continue;
		}

		*value++ = 0;
		if (!last_group || strcmp(last_group, group)) {
			grid = info_start_group(vbox, (last_group = group));
			row = 0;
		}

		GtkWidget *a = gtk_label_new(tag);
		gtk_size_group_add_widget(sg, a);
		gtk_label_set_selectable(GTK_LABEL(a), TRUE);
		gtk_label_set_xalign(GTK_LABEL(a), 0.);
		gtk_grid_attach(GTK_GRID(grid), a, 0, row, 1, 1);

		GtkWidget *b = gtk_label_new(value);
		gtk_label_set_selectable(GTK_LABEL(b), TRUE);
		gtk_label_set_xalign(GTK_LABEL(b), 0.);
		gtk_label_set_line_wrap(GTK_LABEL(b), TRUE);
		gtk_widget_set_hexpand(b, TRUE);
		gtk_grid_attach(GTK_GRID(grid), b, 1, row, 1, 1);
		row++;
	}
	g_object_unref(sg);
	return vbox;
}

static void
info(FivView *self)
{
	// TODO(p): Add a fallback to internal capabilities.
	// The simplest is to specify the filename and the resolution.
	GtkWindow *window = get_toplevel(GTK_WIDGET(self));
	int flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;

	GError *error = NULL;
	gchar *path = g_filename_from_uri(self->uri, NULL, &error);
	if (!path) {
		show_error_dialog(window, error);
		return;
	}

	GSubprocess *subprocess = g_subprocess_new(flags, &error, "exiftool",
		"-tab", "-groupNames", "-duplicates", "-extractEmbedded", "--binary",
		"-quiet", "--", path, NULL);
	g_free(path);
	if (error) {
		show_error_dialog(window, error);
		return;
	}

	gchar *out = NULL, *err = NULL;
	if (!g_subprocess_communicate_utf8(
			subprocess, NULL, NULL, &out, &err, &error)) {
		show_error_dialog(window, error);
		return;
	}

	GtkWidget *dialog = gtk_widget_new(GTK_TYPE_DIALOG,
		"use-header-bar", TRUE,
		"title", "Information",
		"transient-for", window,
		"destroy-with-parent", TRUE, NULL);

	GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	if (*err) {
		GtkWidget *info = gtk_info_bar_new();
		GtkInfoBar *info_bar = GTK_INFO_BAR(info);
		gtk_info_bar_set_message_type(info_bar, GTK_MESSAGE_WARNING);

		GtkWidget *info_area = gtk_info_bar_get_content_area(info_bar);
		GtkWidget *label = gtk_label_new(g_strstrip(err));
		gtk_container_add(GTK_CONTAINER(info_area), label);

		gtk_container_add(GTK_CONTAINER(content_area), info);
	}

	GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
	GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(scroller);
	gtk_scrolled_window_set_max_content_width(sw, 600);
	gtk_scrolled_window_set_max_content_height(sw, 800);
	gtk_scrolled_window_set_propagate_natural_width(sw, TRUE);
	gtk_scrolled_window_set_propagate_natural_height(sw, TRUE);

	GtkWidget *info = info_parse(out);
	gtk_widget_set_hexpand(info, TRUE);
	gtk_widget_set_vexpand(info, TRUE);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(GTK_WIDGET(info)), "fiv-information");
	gtk_container_add(GTK_CONTAINER(scroller), info);
	gtk_container_add(GTK_CONTAINER(content_area), scroller);

	g_free(out);
	g_free(err);
	g_object_unref(subprocess);
	gtk_widget_show_all(dialog);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static inline gboolean
command(FivView *self, FivViewCommand command)
{
	fiv_view_command(self, command);
	return TRUE;
}

static gboolean
fiv_view_key_press_event(GtkWidget *widget, GdkEventKey *event)
{
	FivView *self = FIV_VIEW(widget);
	if (!self->image)
		return FALSE;

	// It should not matter that GDK_KEY_plus involves holding Shift.
	guint state = event->state & gtk_accelerator_get_default_mod_mask() &
		~GDK_SHIFT_MASK;

	// The standard, intuitive bindings.
	if (state == GDK_CONTROL_MASK) {
		switch (event->keyval) {
		case GDK_KEY_0:
			return command(self, FIV_VIEW_COMMAND_ZOOM_1);
		case GDK_KEY_plus:
			return command(self, FIV_VIEW_COMMAND_ZOOM_IN);
		case GDK_KEY_minus:
			return command(self, FIV_VIEW_COMMAND_ZOOM_OUT);
		case GDK_KEY_p:
			return command(self, FIV_VIEW_COMMAND_PRINT);
		case GDK_KEY_s:
			return command(self, FIV_VIEW_COMMAND_SAVE_PAGE);
		case GDK_KEY_S:
			return save_as(self, self->frame);
		}
	}
	if (state == GDK_MOD1_MASK) {
		switch (event->keyval) {
		case GDK_KEY_Return:
			return command(self, FIV_VIEW_COMMAND_INFO);
		}
	}
	if (state != 0)
		return FALSE;

	switch (event->keyval) {
	case GDK_KEY_1:
	case GDK_KEY_2:
	case GDK_KEY_3:
	case GDK_KEY_4:
	case GDK_KEY_5:
	case GDK_KEY_6:
	case GDK_KEY_7:
	case GDK_KEY_8:
	case GDK_KEY_9:
		return set_scale(self, event->keyval - GDK_KEY_0);
	case GDK_KEY_plus:
		return command(self, FIV_VIEW_COMMAND_ZOOM_IN);
	case GDK_KEY_minus:
		return command(self, FIV_VIEW_COMMAND_ZOOM_OUT);

	case GDK_KEY_w:
		return set_scale_to_fit_width(self);
	case GDK_KEY_h:
		return set_scale_to_fit_height(self);

	case GDK_KEY_c:
		return command(self, FIV_VIEW_COMMAND_TOGGLE_CMS);
	case GDK_KEY_x:  // Inspired by gThumb, which has more such modes.
		return command(self, FIV_VIEW_COMMAND_TOGGLE_SCALE_TO_FIT);
	case GDK_KEY_i:
		return command(self, FIV_VIEW_COMMAND_TOGGLE_FILTER);
	case GDK_KEY_t:
		return command(self, FIV_VIEW_COMMAND_TOGGLE_CHECKERBOARD);
	case GDK_KEY_e:
		return command(self, FIV_VIEW_COMMAND_TOGGLE_ENHANCE);

	case GDK_KEY_less:
		return command(self, FIV_VIEW_COMMAND_ROTATE_LEFT);
	case GDK_KEY_equal:
		return command(self, FIV_VIEW_COMMAND_MIRROR);
	case GDK_KEY_greater:
		return command(self, FIV_VIEW_COMMAND_ROTATE_RIGHT);

	case GDK_KEY_bracketleft:
		return command(self, FIV_VIEW_COMMAND_PAGE_PREVIOUS);
	case GDK_KEY_bracketright:
		return command(self, FIV_VIEW_COMMAND_PAGE_NEXT);

	case GDK_KEY_braceleft:
		return command(self, FIV_VIEW_COMMAND_FRAME_PREVIOUS);
	case GDK_KEY_braceright:
		return command(self, FIV_VIEW_COMMAND_FRAME_NEXT);
	case GDK_KEY_space:
		return command(self, FIV_VIEW_COMMAND_TOGGLE_PLAYBACK);
	}
	return FALSE;
}

static void
fiv_view_class_init(FivViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fiv_view_finalize;
	object_class->get_property = fiv_view_get_property;
	object_class->set_property = fiv_view_set_property;

	view_properties[PROP_SCALE] = g_param_spec_double(
		"scale", "Scale", "Zoom level",
		0, G_MAXDOUBLE, 1.0, G_PARAM_READABLE);
	view_properties[PROP_SCALE_TO_FIT] = g_param_spec_boolean(
		"scale-to-fit", "Scale to fit", "Scale images down to fit the window",
		TRUE, G_PARAM_READWRITE);
	view_properties[PROP_ENABLE_CMS] = g_param_spec_boolean(
		"enable-cms", "Enable CMS", "Enable color management",
		TRUE, G_PARAM_READWRITE);
	view_properties[PROP_FILTER] = g_param_spec_boolean(
		"filter", "Use filtering", "Scale images smoothly",
		TRUE, G_PARAM_READWRITE);
	view_properties[PROP_CHECKERBOARD] = g_param_spec_boolean(
		"checkerboard", "Show checkerboard", "Highlight transparent background",
		FALSE, G_PARAM_READWRITE);
	view_properties[PROP_ENHANCE] = g_param_spec_boolean(
		"enhance", "Enhance JPEG", "Enhance low-quality JPEG",
		FALSE, G_PARAM_READWRITE);
	view_properties[PROP_PLAYING] = g_param_spec_boolean(
		"playing", "Playing animation", "An animation is running",
		FALSE, G_PARAM_READABLE);
	view_properties[PROP_HAS_IMAGE] = g_param_spec_boolean(
		"has-image", "Has an image", "An image is loaded",
		FALSE, G_PARAM_READABLE);
	view_properties[PROP_CAN_ANIMATE] = g_param_spec_boolean(
		"can-animate", "Can animate", "An animation is loaded",
		FALSE, G_PARAM_READABLE);
	view_properties[PROP_HAS_PREVIOUS_PAGE] = g_param_spec_boolean(
		"has-previous-page", "Has a previous page", "Preceding pages exist",
		FALSE, G_PARAM_READABLE);
	view_properties[PROP_HAS_NEXT_PAGE] = g_param_spec_boolean(
		"has-next-page", "Has a next page", "Following pages exist",
		FALSE, G_PARAM_READABLE);
	g_object_class_install_properties(
		object_class, N_PROPERTIES, view_properties);

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->get_preferred_height = fiv_view_get_preferred_height;
	widget_class->get_preferred_width = fiv_view_get_preferred_width;
	widget_class->size_allocate = fiv_view_size_allocate;
	widget_class->map = fiv_view_map;
	widget_class->unmap = fiv_view_unmap;
	widget_class->realize = fiv_view_realize;
	widget_class->draw = fiv_view_draw;
	widget_class->button_press_event = fiv_view_button_press_event;
	widget_class->scroll_event = fiv_view_scroll_event;
	widget_class->key_press_event = fiv_view_key_press_event;

	// TODO(p): Later override "screen_changed", recreate Pango layouts there,
	// if we get to have any, or otherwise reflect DPI changes.
	gtk_widget_class_set_css_name(widget_class, "fiv-view");
}

static void
fiv_view_init(FivView *self)
{
	gtk_widget_set_can_focus(GTK_WIDGET(self), TRUE);

	self->enable_cms = true;
	self->filter = true;
	self->checkerboard = false;
	self->scale = 1.0;
}

// --- Public interface --------------------------------------------------------

// TODO(p): Progressive picture loading, or at least async/cancellable.
gboolean
fiv_view_open(FivView *self, const gchar *uri, GError **error)
{
	cairo_surface_t *surface = fiv_io_open(
		uri, self->enable_cms ? self->screen_cms_profile : NULL, FALSE, error);
	if (!surface)
		return FALSE;
	if (self->image)
		cairo_surface_destroy(self->image);

	// This is extremely expensive, and only works sometimes.
	g_clear_pointer(&self->enhance_swap, cairo_surface_destroy);
	if (self->enhance) {
		self->enhance = FALSE;
		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_ENHANCE]);
	}

	self->frame = self->page = NULL;
	self->image = surface;
	switch_page(self, self->image);
	set_scale_to_fit(self, true);

	g_free(self->uri);
	self->uri = g_strdup(uri);

	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_HAS_IMAGE]);
	return TRUE;
}

static void
page_step(FivView *self, int step)
{
	cairo_user_data_key_t *key =
		step < 0 ? &fiv_io_key_page_previous : &fiv_io_key_page_next;
	cairo_surface_t *page = cairo_surface_get_user_data(self->page, key);
	if (page)
		switch_page(self, page);
}

static void
frame_step(FivView *self, int step)
{
	stop_animating(self);
	cairo_user_data_key_t *key =
		step < 0 ? &fiv_io_key_frame_previous : &fiv_io_key_frame_next;
	if (!step || !(self->frame = cairo_surface_get_user_data(self->frame, key)))
		self->frame = self->page;
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

static gboolean
reload(FivView *self)
{
	GError *error = NULL;
	cairo_surface_t *surface = fiv_io_open(self->uri,
		self->enable_cms ? self->screen_cms_profile : NULL, self->enhance,
		&error);
	if (!surface) {
		show_error_dialog(get_toplevel(GTK_WIDGET(self)), error);
		return FALSE;
	}

	g_clear_pointer(&self->image, cairo_surface_destroy);
	g_clear_pointer(&self->enhance_swap, cairo_surface_destroy);
	switch_page(self, (self->image = surface));
	return TRUE;
}

static void
swap_enhanced_image(FivView *self)
{
	cairo_surface_t *saved = self->image;
	self->image = self->page = self->frame = NULL;

	if (self->enhance_swap) {
		switch_page(self, (self->image = self->enhance_swap));
		self->enhance_swap = saved;
	} else if (reload(self)) {
		self->enhance_swap = saved;
	} else {
		switch_page(self, (self->image = saved));
	}
}

void
fiv_view_command(FivView *self, FivViewCommand command)
{
	g_return_if_fail(FIV_IS_VIEW(self));

	GtkWidget *widget = GTK_WIDGET(self);
	if (!self->image)
		return;

	switch (command) {
	break; case FIV_VIEW_COMMAND_ROTATE_LEFT:
		self->orientation = view_left[self->orientation];
		gtk_widget_queue_resize(widget);
	break; case FIV_VIEW_COMMAND_MIRROR:
		self->orientation = view_mirror[self->orientation];
		gtk_widget_queue_resize(widget);
	break; case FIV_VIEW_COMMAND_ROTATE_RIGHT:
		self->orientation = view_right[self->orientation];
		gtk_widget_queue_resize(widget);

	break; case FIV_VIEW_COMMAND_PAGE_FIRST:
		switch_page(self, self->image);
	break; case FIV_VIEW_COMMAND_PAGE_PREVIOUS:
		page_step(self, -1);
	break; case FIV_VIEW_COMMAND_PAGE_NEXT:
		page_step(self, +1);
	break; case FIV_VIEW_COMMAND_PAGE_LAST:
		for (cairo_surface_t *s = self->page;
			 (s = cairo_surface_get_user_data(s, &fiv_io_key_page_next)); )
			self->page = s;
		switch_page(self, self->page);

	break; case FIV_VIEW_COMMAND_FRAME_FIRST:
		frame_step(self, 0);
	break; case FIV_VIEW_COMMAND_FRAME_PREVIOUS:
		frame_step(self, -1);
	break; case FIV_VIEW_COMMAND_FRAME_NEXT:
		frame_step(self, +1);
	break; case FIV_VIEW_COMMAND_TOGGLE_PLAYBACK:
		self->frame_update_connection
			? stop_animating(self)
			: start_animating(self);

	break; case FIV_VIEW_COMMAND_TOGGLE_CMS:
		self->enable_cms = !self->enable_cms;
		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_ENABLE_CMS]);
		reload(self);
	break; case FIV_VIEW_COMMAND_TOGGLE_FILTER:
		self->filter = !self->filter;
		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_FILTER]);
		gtk_widget_queue_draw(widget);
	break; case FIV_VIEW_COMMAND_TOGGLE_CHECKERBOARD:
		self->checkerboard = !self->checkerboard;
		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_CHECKERBOARD]);
		gtk_widget_queue_draw(widget);
	break; case FIV_VIEW_COMMAND_TOGGLE_ENHANCE:
		self->enhance = !self->enhance;
		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_ENHANCE]);
		swap_enhanced_image(self);

	break; case FIV_VIEW_COMMAND_PRINT:
		print(self);
	break; case FIV_VIEW_COMMAND_SAVE_PAGE:
		save_as(self, NULL);
	break; case FIV_VIEW_COMMAND_INFO:
		info(self);

	break; case FIV_VIEW_COMMAND_ZOOM_IN:
		set_scale(self, self->scale * SCALE_STEP);
	break; case FIV_VIEW_COMMAND_ZOOM_OUT:
		set_scale(self, self->scale / SCALE_STEP);
	break; case FIV_VIEW_COMMAND_ZOOM_1:
		set_scale(self, 1.0);
	break; case FIV_VIEW_COMMAND_TOGGLE_SCALE_TO_FIT:
		set_scale_to_fit(self, !self->scale_to_fit);
	}
}

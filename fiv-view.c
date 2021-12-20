//
// fiv-view.c: fast image viewer - view widget
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

#include <math.h>
#include <stdbool.h>

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif  // GDK_WINDOWING_X11
#ifdef GDK_WINDOWING_QUARTZ
#include <gdk/gdkquartz.h>
#endif  // GDK_WINDOWING_QUARTZ

#include "fiv-io.h"
#include "fiv-view.h"

struct _FivView {
	GtkWidget parent_instance;
	cairo_surface_t *image;             ///< The loaded image (sequence)
	cairo_surface_t *page;              ///< Current page within image, weak
	cairo_surface_t *frame;             ///< Current frame within page, weak
	FivIoOrientation orientation;       ///< Current page orientation
	bool filter;                        ///< Smooth scaling toggle
	bool checkerboard;                  ///< Show checkerboard background
	bool scale_to_fit;                  ///< Image no larger than the allocation
	double scale;                       ///< Scaling factor

	int remaining_loops;                ///< Greater than zero if limited
	gint64 frame_time;                  ///< Current frame's start, µs precision
	gulong frame_update_connection;     ///< GdkFrameClock::update
};

G_DEFINE_TYPE(FivView, fiv_view, GTK_TYPE_WIDGET)

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
	PROP_FILTER,
	PROP_CHECKERBOARD,
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
	cairo_surface_destroy(self->image);

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
	case PROP_FILTER:
		g_value_set_boolean(value, self->filter);
		break;
	case PROP_CHECKERBOARD:
		g_value_set_boolean(value, self->checkerboard);
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
	case PROP_FILTER:
		if (self->filter != g_value_get_boolean(value))
			fiv_view_command(self, FIV_VIEW_COMMAND_TOGGLE_FILTER);
		break;
	case PROP_CHECKERBOARD:
		if (self->checkerboard != g_value_get_boolean(value))
			fiv_view_command(self, FIV_VIEW_COMMAND_TOGGLE_CHECKERBOARD);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
get_surface_dimensions(FivView *self, double *width, double *height)
{
	*width = *height = 0;
	if (!self->image)
		return;

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

	switch (self->orientation) {
	case FivIoOrientation90:
	case FivIoOrientationMirror90:
	case FivIoOrientation270:
	case FivIoOrientationMirror270:
		*width = extents.height;
		*height = extents.width;
		return;
	default:
		*width = extents.width;
		*height = extents.height;
	}
}

static void
get_display_dimensions(FivView *self, int *width, int *height)
{
	double w, h;
	get_surface_dimensions(self, &w, &h);

	*width = ceil(w * self->scale);
	*height = ceil(h * self->scale);
}

static cairo_matrix_t
get_orientation_matrix(FivIoOrientation o, double width, double height)
{
	cairo_matrix_t matrix = {};
	cairo_matrix_init_identity(&matrix);
	switch (o) {
	case FivIoOrientation90:
		cairo_matrix_rotate(&matrix, -M_PI_2);
		cairo_matrix_translate(&matrix, -width, 0);
		break;
	case FivIoOrientation180:
		cairo_matrix_scale(&matrix, -1, -1);
		cairo_matrix_translate(&matrix, -width, -height);
		break;
	case FivIoOrientation270:
		cairo_matrix_rotate(&matrix, +M_PI_2);
		cairo_matrix_translate(&matrix, 0, -height);
		break;
	case FivIoOrientationMirror0:
		cairo_matrix_scale(&matrix, -1, +1);
		cairo_matrix_translate(&matrix, -width, 0);
		break;
	case FivIoOrientationMirror90:
		cairo_matrix_rotate(&matrix, +M_PI_2);
		cairo_matrix_scale(&matrix, -1, +1);
		cairo_matrix_translate(&matrix, -width, -height);
		break;
	case FivIoOrientationMirror180:
		cairo_matrix_scale(&matrix, +1, -1);
		cairo_matrix_translate(&matrix, 0, -height);
		break;
	case FivIoOrientationMirror270:
		cairo_matrix_rotate(&matrix, -M_PI_2);
		cairo_matrix_scale(&matrix, -1, +1);
	default:
		break;
	}
	return matrix;
}

static void
fiv_view_get_preferred_height(GtkWidget *widget, gint *minimum, gint *natural)
{
	FivView *self = FIV_VIEW(widget);
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
fiv_view_get_preferred_width(GtkWidget *widget, gint *minimum, gint *natural)
{
	FivView *self = FIV_VIEW(widget);
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
fiv_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	GTK_WIDGET_CLASS(fiv_view_parent_class)->size_allocate(widget, allocation);

	FivView *self = FIV_VIEW(widget);
	if (!self->image || !self->scale_to_fit)
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
	double sw, sh;
	get_display_dimensions(self, &w, &h);
	get_surface_dimensions(self, &sw, &sh);

	double x = 0;
	double y = 0;
	if (w < allocation.width)
		x = round((allocation.width - w) / 2.);
	if (h < allocation.height)
		y = round((allocation.height - h) / 2.);

	cairo_matrix_t matrix = get_orientation_matrix(self->orientation, sw, sh);
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
	double surface_width_px = 0, surface_height_px = 0;
	get_surface_dimensions(self, &surface_width_px, &surface_height_px);

	// Any DPI will be wrong, unless we import that information from the image.
	double scale = 1 / 96.;
	double w = surface_width_px * scale, h = surface_height_px * scale;

	// Scale down to fit the print area, taking care to not divide by zero.
	double areaw = gtk_print_context_get_width(context);
	double areah = gtk_print_context_get_height(context);
	scale *= fmin((areaw < w) ? areaw / w : 1, (areah < h) ? areah / h : 1);

	cairo_t *cr = gtk_print_context_get_cairo_context(context);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, self->frame, 0, 0);
	cairo_matrix_t matrix = get_orientation_matrix(
		self->orientation, surface_width_px, surface_height_px);
	cairo_pattern_set_matrix(cairo_get_source(cr), &matrix);
	cairo_paint(cr);
}

static gboolean
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
	return TRUE;
}

static gboolean
save_as(FivView *self, gboolean frame)
{
	GtkWindow *window = get_toplevel(GTK_WIDGET(self));
	GtkWidget *dialog = gtk_file_chooser_dialog_new(
		frame ? "Save frame as" : "Save page as",
		window, GTK_FILE_CHOOSER_ACTION_SAVE,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);

	GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);

	// TODO(p): Consider a hard dependency on libwebp, or clean this up.
#ifdef HAVE_LIBWEBP
	// This is the best general format: supports lossless encoding, animations,
	// alpha channel, and Exif and ICC profile metadata.
	// PNG is another viable option, but sPNG can't do APNG, Wuffs can't save,
	// and libpng is a pain in the arse.
	GtkFileFilter *webp_filter = gtk_file_filter_new();
	gtk_file_filter_add_mime_type(webp_filter, "image/webp");
	gtk_file_filter_add_pattern(webp_filter, "*.webp");
	gtk_file_filter_set_name(webp_filter, "Lossless WebP");
	gtk_file_chooser_add_filter(chooser, webp_filter);

	// TODO(p): Derive it from the currently displayed filename,
	// and set the directory to the same place.
	gtk_file_chooser_set_current_name(
		chooser, frame ? "frame.webp" : "page.webp");
#endif  // HAVE_LIBWEBP

	// The format is supported by Exiv2 and ExifTool.
	// This is mostly a developer tool.
	GtkFileFilter *exv_filter = gtk_file_filter_new();
	gtk_file_filter_add_mime_type(exv_filter, "image/x-exv");
	gtk_file_filter_add_pattern(exv_filter, "*.exv");
	gtk_file_filter_set_name(exv_filter, "Exiv2 metadata");
	gtk_file_chooser_add_filter(chooser, exv_filter);

	switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
		gchar *path;
	case GTK_RESPONSE_ACCEPT:
		path = gtk_file_chooser_get_filename(chooser);

		GError *error = NULL;
#ifdef HAVE_LIBWEBP
		if (gtk_file_chooser_get_filter(chooser) == webp_filter)
			fiv_io_save(self->page, frame ? self->frame : NULL, path, &error);
		else
#endif  // HAVE_LIBWEBP
			fiv_io_save_metadata(self->page, path, &error);
		if (error)
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
			return save_as(self, TRUE);
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

	case GDK_KEY_x:  // Inspired by gThumb.
		return command(self, FIV_VIEW_COMMAND_TOGGLE_SCALE_TO_FIT);
	case GDK_KEY_i:
		return command(self, FIV_VIEW_COMMAND_TOGGLE_FILTER);
	case GDK_KEY_t:
		return command(self, FIV_VIEW_COMMAND_TOGGLE_CHECKERBOARD);

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
	view_properties[PROP_FILTER] = g_param_spec_boolean(
		"filter", "Use filtering", "Scale images smoothly",
		TRUE, G_PARAM_READWRITE);
	view_properties[PROP_CHECKERBOARD] = g_param_spec_boolean(
		"checkerboard", "Show checkerboard", "Highlight transparent background",
		TRUE, G_PARAM_READWRITE);
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

	self->filter = true;
	self->checkerboard = false;
	self->scale = 1.0;
}

// --- Picture loading ---------------------------------------------------------

// TODO(p): Progressive picture loading, or at least async/cancellable.
gboolean
fiv_view_open(FivView *self, const gchar *path, GError **error)
{
	cairo_surface_t *surface = fiv_io_open(path, error);
	if (!surface)
		return FALSE;
	if (self->image)
		cairo_surface_destroy(self->image);

	self->frame = self->page = NULL;
	self->image = surface;
	switch_page(self, self->image);
	set_scale_to_fit(self, true);

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
	break; case FIV_VIEW_COMMAND_PRINT:
		print(self);
	break; case FIV_VIEW_COMMAND_SAVE_PAGE:
		save_as(self, FALSE);

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

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

#include "fastiv-io.h"
#include "fastiv-view.h"

struct _FastivView {
	GtkWidget parent_instance;
	cairo_surface_t *image;             ///< The loaded image (sequence)
	cairo_surface_t *page;              ///< Current page within image, weak
	cairo_surface_t *frame;             ///< Current frame within page, weak
	FastivIoOrientation orientation;    ///< Current page orientation
	bool filter;                        ///< Smooth scaling toggle
	bool scale_to_fit;                  ///< Image no larger than the allocation
	double scale;                       ///< Scaling factor

	int remaining_loops;                ///< Greater than zero if limited
	gint64 frame_time;                  ///< Current frame's start, µs precision
	gulong frame_update_connection;     ///< GdkFrameClock::update
};

G_DEFINE_TYPE(FastivView, fastiv_view, GTK_TYPE_WIDGET)

static FastivIoOrientation view_left[9] = {
	[FastivIoOrientationUnknown] = FastivIoOrientationUnknown,
	[FastivIoOrientation0] = FastivIoOrientation270,
	[FastivIoOrientationMirror0] = FastivIoOrientationMirror270,
	[FastivIoOrientation180] = FastivIoOrientation90,
	[FastivIoOrientationMirror180] = FastivIoOrientationMirror90,
	[FastivIoOrientationMirror270] = FastivIoOrientationMirror180,
	[FastivIoOrientation90] = FastivIoOrientation0,
	[FastivIoOrientationMirror90] = FastivIoOrientationMirror0,
	[FastivIoOrientation270] = FastivIoOrientation180
};

static FastivIoOrientation view_mirror[9] = {
	[FastivIoOrientationUnknown] = FastivIoOrientationUnknown,
	[FastivIoOrientation0] = FastivIoOrientationMirror0,
	[FastivIoOrientationMirror0] = FastivIoOrientation0,
	[FastivIoOrientation180] = FastivIoOrientationMirror180,
	[FastivIoOrientationMirror180] = FastivIoOrientation180,
	[FastivIoOrientationMirror270] = FastivIoOrientation270,
	[FastivIoOrientation90] = FastivIoOrientationMirror270,
	[FastivIoOrientationMirror90] = FastivIoOrientation90,
	[FastivIoOrientation270] = FastivIoOrientationMirror270
};

static FastivIoOrientation view_right[9] = {
	[FastivIoOrientationUnknown] = FastivIoOrientationUnknown,
	[FastivIoOrientation0] = FastivIoOrientation90,
	[FastivIoOrientationMirror0] = FastivIoOrientationMirror90,
	[FastivIoOrientation180] = FastivIoOrientation270,
	[FastivIoOrientationMirror180] = FastivIoOrientationMirror270,
	[FastivIoOrientationMirror270] = FastivIoOrientationMirror0,
	[FastivIoOrientation90] = FastivIoOrientation180,
	[FastivIoOrientationMirror90] = FastivIoOrientationMirror180,
	[FastivIoOrientation270] = FastivIoOrientation0
};

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
	cairo_surface_destroy(self->image);

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
	if (!self->image)
		return;

	cairo_rectangle_t extents = {};
	switch (cairo_surface_get_type(self->page)) {
	case CAIRO_SURFACE_TYPE_IMAGE:
		switch (self->orientation) {
		case FastivIoOrientation90:
		case FastivIoOrientationMirror90:
		case FastivIoOrientation270:
		case FastivIoOrientationMirror270:
			*width = cairo_image_surface_get_height(self->page);
			*height = cairo_image_surface_get_width(self->page);
			break;
		default:
			*width = cairo_image_surface_get_width(self->page);
			*height = cairo_image_surface_get_height(self->page);
		}
		return;
	case CAIRO_SURFACE_TYPE_RECORDING:
		if (!cairo_recording_surface_get_extents(self->page, &extents)) {
			cairo_recording_surface_ink_extents(self->page,
				&extents.x, &extents.y, &extents.width, &extents.height);
		}

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

	// FIXME: Recording surfaces do not work well with CAIRO_SURFACE_TYPE_XLIB,
	// we always get a shitty pixmap, where transparency contains junk.
	if (cairo_surface_get_type(self->frame) == CAIRO_SURFACE_TYPE_RECORDING) {
		cairo_surface_t *image =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		cairo_t *tcr = cairo_create(image);
		cairo_scale(tcr, self->scale, self->scale);
		cairo_set_source_surface(tcr, self->frame, 0, 0);
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

	cairo_translate(cr, x, y);
	cairo_scale(cr, self->scale, self->scale);
	cairo_set_source_surface(cr, self->frame, 0, 0);

	cairo_matrix_t matrix = {};
	cairo_matrix_init_identity(&matrix);
	switch (self->orientation) {
	case FastivIoOrientation90:
		cairo_matrix_rotate(&matrix, -M_PI_2);
		cairo_matrix_translate(&matrix, -sw, 0);
		break;
	case FastivIoOrientation180:
		cairo_matrix_scale(&matrix, -1, -1);
		cairo_matrix_translate(&matrix, -sw, -sh);
		break;
	case FastivIoOrientation270:
		cairo_matrix_rotate(&matrix, +M_PI_2);
		cairo_matrix_translate(&matrix, 0, -sh);
		break;
	case FastivIoOrientationMirror0:
		cairo_matrix_scale(&matrix, -1, +1);
		cairo_matrix_translate(&matrix, -sw, 0);
		break;
	case FastivIoOrientationMirror90:
		cairo_matrix_rotate(&matrix, +M_PI_2);
		cairo_matrix_scale(&matrix, -1, +1);
		cairo_matrix_translate(&matrix, -sw, -sh);
		break;
	case FastivIoOrientationMirror180:
		cairo_matrix_scale(&matrix, +1, -1);
		cairo_matrix_translate(&matrix, 0, -sh);
		break;
	case FastivIoOrientationMirror270:
		cairo_matrix_rotate(&matrix, -M_PI_2);
		cairo_matrix_scale(&matrix, -1, +1);
	default:
		break;
	}

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
fastiv_view_button_press_event(GtkWidget *widget, GdkEventButton *event)
{
	GTK_WIDGET_CLASS(fastiv_view_parent_class)
		->button_press_event(widget, event);

	if (event->button == GDK_BUTTON_PRIMARY &&
		gtk_widget_get_focus_on_click(widget))
		gtk_widget_grab_focus(widget);

	// TODO(p): Use for left button scroll drag, which may rather be a gesture.
	return FALSE;
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
stop_animating(FastivView *self)
{
	GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(self));
	if (!clock || !self->frame_update_connection)
		return;

	g_signal_handler_disconnect(clock, self->frame_update_connection);
	gdk_frame_clock_end_updating(clock);

	self->frame_time = 0;
	self->frame_update_connection = 0;
	self->remaining_loops = 0;
}

static gboolean
advance_frame(FastivView *self)
{
	cairo_surface_t *next =
		cairo_surface_get_user_data(self->frame, &fastiv_io_key_frame_next);
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
advance_animation(FastivView *self, GdkFrameClock *clock)
{
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	while (true) {
		// TODO(p): See if infinite frames can actually happen, and how.
		intptr_t duration = (intptr_t) cairo_surface_get_user_data(
			self->frame, &fastiv_io_key_frame_duration);
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
	FastivView *self = FASTIV_VIEW(user_data);
	if (!advance_animation(self, clock))
		stop_animating(self);
}

static void
start_animating(FastivView *self)
{
	stop_animating(self);

	GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(self));
	if (!clock || !self->image ||
		!cairo_surface_get_user_data(self->page, &fastiv_io_key_frame_next))
		return;

	self->frame_time = gdk_frame_clock_get_frame_time(clock);
	self->frame_update_connection = g_signal_connect(
		clock, "update", G_CALLBACK(on_frame_clock_update), self);
	self->remaining_loops = (uintptr_t) cairo_surface_get_user_data(
		self->page, &fastiv_io_key_loops);

	gdk_frame_clock_begin_updating(clock);
}

static void
switch_page(FastivView *self, cairo_surface_t *page)
{
	GtkWidget *widget = GTK_WIDGET(self);
	self->frame = self->page = page;
	if ((self->orientation = (uintptr_t) cairo_surface_get_user_data(
			 self->page, &fastiv_io_key_orientation)) ==
		FastivIoOrientationUnknown)
		self->orientation = FastivIoOrientation0;

	start_animating(self);
	gtk_widget_queue_resize(widget);
}

static void
fastiv_view_map(GtkWidget *widget)
{
	GTK_WIDGET_CLASS(fastiv_view_parent_class)->map(widget);

	// Loading before mapping will fail to obtain a GdkFrameClock.
	start_animating(FASTIV_VIEW(widget));
}

void
fastiv_view_unmap(GtkWidget *widget)
{
	stop_animating(FASTIV_VIEW(widget));
	GTK_WIDGET_CLASS(fastiv_view_parent_class)->unmap(widget);
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

static gboolean
save_as(FastivView *self, gboolean frame)
{
	GtkWindow *window = NULL;
	GtkWidget *widget = NULL;
	if (GTK_IS_WINDOW((widget = gtk_widget_get_toplevel(GTK_WIDGET(self)))))
		window = GTK_WINDOW(widget);

	GtkWidget *dialog =
		gtk_file_chooser_dialog_new(frame ? "Save frame as" : "Save page as",
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
			fastiv_io_save(self->page,
				frame ? self->frame : NULL, path, &error);
		else
#endif  // HAVE_LIBWEBP
			fastiv_io_save_metadata(self->page, path, &error);
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

static gboolean
fastiv_view_key_press_event(GtkWidget *widget, GdkEventKey *event)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (!self->image)
		return FALSE;

	// It should not matter that GDK_KEY_plus involves holding Shift.
	guint state = event->state & gtk_accelerator_get_default_mod_mask() &
		~GDK_SHIFT_MASK;

	// The standard, intuitive bindings.
	if (state == GDK_CONTROL_MASK) {
		switch (event->keyval) {
		case GDK_KEY_0:
			return set_scale(self, 1.0);
		case GDK_KEY_plus:
			return set_scale(self, self->scale * SCALE_STEP);
		case GDK_KEY_minus:
			return set_scale(self, self->scale / SCALE_STEP);
		case GDK_KEY_s:
			return save_as(self, FALSE);
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
		return set_scale(self, self->scale * SCALE_STEP);
	case GDK_KEY_minus:
		return set_scale(self, self->scale / SCALE_STEP);

	case GDK_KEY_x:  // Inspired by gThumb.
		return set_scale_to_fit(self, !self->scale_to_fit);

	case GDK_KEY_i:
		self->filter = !self->filter;
		gtk_widget_queue_draw(widget);
		return TRUE;

	case GDK_KEY_less:
		self->orientation = view_left[self->orientation];
		gtk_widget_queue_resize(widget);
		return TRUE;
	case GDK_KEY_equal:
		self->orientation = view_mirror[self->orientation];
		gtk_widget_queue_draw(widget);
		return TRUE;
	case GDK_KEY_greater:
		self->orientation = view_right[self->orientation];
		gtk_widget_queue_resize(widget);
		return TRUE;

	case GDK_KEY_bracketleft: {
		cairo_surface_t *page = cairo_surface_get_user_data(
			self->page, &fastiv_io_key_page_previous);
		if (page)
			switch_page(self, page);
		return TRUE;
	}
	case GDK_KEY_bracketright: {
		cairo_surface_t *page = cairo_surface_get_user_data(
			self->page, &fastiv_io_key_page_next);
		if (page)
			switch_page(self, page);
		return TRUE;
	}

	case GDK_KEY_braceleft:
		stop_animating(self);
		if (!(self->frame = cairo_surface_get_user_data(
				self->frame, &fastiv_io_key_frame_previous)))
			self->frame = self->page;
		gtk_widget_queue_draw(widget);
		return TRUE;
	case GDK_KEY_braceright:
		stop_animating(self);
		if (!(self->frame = cairo_surface_get_user_data(
				self->frame, &fastiv_io_key_frame_next)))
			self->frame = self->page;
		gtk_widget_queue_draw(widget);
		return TRUE;
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
	widget_class->map = fastiv_view_map;
	widget_class->unmap = fastiv_view_unmap;
	widget_class->realize = fastiv_view_realize;
	widget_class->draw = fastiv_view_draw;
	widget_class->button_press_event = fastiv_view_button_press_event;
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

	self->filter = true;
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
	if (self->image)
		cairo_surface_destroy(self->image);

	self->frame = self->page = NULL;
	self->image = surface;
	switch_page(self, self->image);
	set_scale_to_fit(self, true);
	return TRUE;
}

//
// fiv-view.c: image viewing widget
//
// Copyright (c) 2021 - 2024, Přemysl Eric Janouch <p@janouch.name>
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
#include "fiv-context-menu.h"

#include <math.h>
#include <stdbool.h>

#include <epoxy/gl.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif  // GDK_WINDOWING_X11
#ifdef GDK_WINDOWING_QUARTZ
#include <gdk/gdkquartz.h>
#endif  // GDK_WINDOWING_QUARTZ
#ifdef GDK_WINDOWING_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gdk/gdkwin32.h>
#endif  // GDK_WINDOWING_WIN32

GType
fiv_view_command_get_type(void)
{
	static gsize guard;
	if (g_once_init_enter(&guard)) {
#define XX(constant, name) {constant, #constant, name},
		static const GEnumValue values[] = {FIV_VIEW_COMMANDS(XX) {}};
#undef XX
		GType type = g_enum_register_static(
			g_intern_static_string("FivViewCommand"), values);
		g_once_init_leave(&guard, type);
	}
	return guard;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct _FivView {
	GtkWidget parent_instance;
	GtkAdjustment *hadjustment;         ///< GtkScrollable boilerplate
	GtkAdjustment *vadjustment;         ///< GtkScrollable boilerplate
	GtkScrollablePolicy hscroll_policy; ///< GtkScrollable boilerplate
	GtkScrollablePolicy vscroll_policy; ///< GtkScrollable boilerplate

	gchar *messages;                    ///< Image load information
	gchar *uri;                         ///< Path to the current image (if any)
	FivIoImage *image;                  ///< The loaded image (sequence)
	FivIoImage *page;                   ///< Current page within image, weak
	FivIoImage *page_scaled;            ///< Current page within image, scaled
	FivIoImage *frame;                  ///< Current frame within page, weak
	FivIoOrientation orientation;       ///< Current page orientation
	bool enable_cms : 1;                ///< Smooth scaling toggle
	bool filter : 1;                    ///< Smooth scaling toggle
	bool checkerboard : 1;              ///< Show checkerboard background
	bool enhance : 1;                   ///< Try to enhance picture data
	bool scale_to_fit : 1;              ///< Image no larger than the allocation
	bool fixate : 1;                    ///< Keep zoom and position
	double scale;                       ///< Scaling factor
	double drag_start[2];               ///< Adjustment values for drag origin

	FivIoImage *enhance_swap;           ///< Quick swap in/out
	FivIoProfile *screen_cms_profile;   ///< Target colour profile for widget

	int remaining_loops;                ///< Greater than zero if limited
	gint64 frame_time;                  ///< Current frame's start, µs precision
	gulong frame_update_connection;     ///< GdkFrameClock::update

	GdkGLContext *gl_context;           ///< OpenGL context
	bool gl_initialized;                ///< Objects have been created
	GLuint gl_program;                  ///< Linked render program
};

G_DEFINE_TYPE_EXTENDED(FivView, fiv_view, GTK_TYPE_WIDGET, 0,
	G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, NULL))

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
	[FivIoOrientationMirror270] = FivIoOrientation90,
	[FivIoOrientation90]        = FivIoOrientationMirror270,
	[FivIoOrientationMirror90]  = FivIoOrientation270,
	[FivIoOrientation270]       = FivIoOrientationMirror90,
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
	PROP_MESSAGES = 1,
	PROP_SCALE,
	PROP_SCALE_TO_FIT,
	PROP_FIXATE,
	PROP_ENABLE_CMS,
	PROP_FILTER,
	PROP_CHECKERBOARD,
	PROP_ENHANCE,
	PROP_PLAYING,
	PROP_HAS_IMAGE,
	PROP_CAN_ANIMATE,
	PROP_HAS_PREVIOUS_PAGE,
	PROP_HAS_NEXT_PAGE,
	N_PROPERTIES,

	// These are overriden, we do not register them.
	PROP_HADJUSTMENT,
	PROP_VADJUSTMENT,
	PROP_HSCROLL_POLICY,
	PROP_VSCROLL_POLICY,
};

static GParamSpec *view_properties[N_PROPERTIES];

enum {
	COMMAND,
	LAST_SIGNAL
};

// Globals are, sadly, the canonical way of storing signal numbers.
static guint view_signals[LAST_SIGNAL];

// --- OpenGL ------------------------------------------------------------------
// While GTK+ 3 technically still supports legacy desktop OpenGL 2.0[1],
// we will pick the 3.3 core profile, which is fairly old by now.
// It doesn't seem to make any sense to go below 3.2.
//
// [1] https://stackoverflow.com/a/37923507/76313
//
// OpenGL ES
//
// Currently, we do not support OpenGL ES at all--it needs its own shaders
// (if only because of different #version statements), and also further analysis
// as to what is our minimum version requirement. While GTK+ 3 can again go
// down as low as OpenGL ES 2.0, this might be too much of a hassle to support.
//
// ES can be forced via GDK_GL=gles, if gdk_gl_context_set_required_version()
// doesn't stand in the way.
//
// Let's not forget that this is a desktop image viewer first and foremost.

static const char *
gl_error_string(GLenum err)
{
	switch (err) {
	case GL_NO_ERROR:
		return "no error";
	case GL_CONTEXT_LOST:
		return "context lost";
	case GL_INVALID_ENUM:
		return "invalid enum";
	case GL_INVALID_VALUE:
		return "invalid value";
	case GL_INVALID_OPERATION:
		return "invalid operation";
	case GL_INVALID_FRAMEBUFFER_OPERATION:
		return "invalid framebuffer operation";
	case GL_OUT_OF_MEMORY:
		return "out of memory";
	case GL_STACK_UNDERFLOW:
		return "stack underflow";
	case GL_STACK_OVERFLOW:
		return "stack overflow";
	default:
		return NULL;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static const char *gl_vertex =
	"#version 330\n"
	"layout(location = 0) in vec4 position;\n"
	"out vec2 coordinates;\n"
	"void main() {\n"
	"\tcoordinates = position.zw;\n"
	"\tgl_Position = vec4(position.xy, 0., 1.);\n"
	"}\n";

static const char *gl_fragment =
	"#version 330\n"
	"in vec2 coordinates;\n"
	"layout(location = 0) out vec4 color;\n"
	"uniform sampler2D picture;\n"
	"uniform bool checkerboard;\n"
	"\n"
	"vec3 checker() {\n"
	"\tvec2 xy = gl_FragCoord.xy / 20.;\n"
	"\tif (checkerboard && (int(floor(xy.x) + floor(xy.y)) & 1) == 0)\n"
	"\t\treturn vec3(0.98);\n"
	"\telse\n"
	"\t\treturn vec3(1.00);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"\tvec3 c = checker();\n"
	"\tvec4 t = texture(picture, coordinates);\n"
	"\t// Premultiplied blending with a solid background.\n"
	"\t// XXX: This is only correct for linear components.\n"
	"\tcolor = vec4(c * (1. - t.a) + t.rgb, 1.);\n"
	"}\n";

static GLuint
gl_make_shader(int type, const char *glsl)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &glsl, NULL);
	glCompileShader(shader);

	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		GLint len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);

		GLchar *buffer = g_malloc0(len + 1);
		glGetShaderInfoLog(shader, len, NULL, buffer);
		g_warning("GL shader compilation failed: %s", buffer);
		g_free(buffer);

		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint
gl_make_program(void)
{
	GLuint vertex = gl_make_shader(GL_VERTEX_SHADER, gl_vertex);
	GLuint fragment = gl_make_shader(GL_FRAGMENT_SHADER, gl_fragment);
	if (!vertex || !fragment) {
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		return 0;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);

	GLint status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		GLint len = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);

		GLchar *buffer = g_malloc0(len + 1);
		glGetProgramInfoLog(program, len, NULL, buffer);
		g_warning("GL program linking failed: %s", buffer);
		g_free(buffer);

		glDeleteProgram(program);
		return 0;
	}
	return program;
}

// -----------------------------------------------------------------------------

static void
on_adjustment_value_changed(
	G_GNUC_UNUSED GtkAdjustment *adjustment, gpointer user_data)
{
	gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

static Dimensions
get_surface_dimensions(FivView *self)
{
	if (!self->image)
		return (Dimensions) {};

	Dimensions dimensions = {};
	fiv_io_orientation_dimensions(
		self->page, self->orientation, &dimensions.width, &dimensions.height);
	return dimensions;
}

static void
get_display_dimensions(FivView *self, int *width, int *height)
{
	Dimensions surface_dimensions = get_surface_dimensions(self);
	*width = ceil(surface_dimensions.width * self->scale);
	*height = ceil(surface_dimensions.height * self->scale);
}

static void
update_adjustments(FivView *self)
{
	int dw = 0, dh = 0;
	get_display_dimensions(self, &dw, &dh);
	GtkAllocation alloc;
	gtk_widget_get_allocation(GTK_WIDGET(self), &alloc);

	if (self->hadjustment) {
		gtk_adjustment_configure(self->hadjustment,
			gtk_adjustment_get_value(self->hadjustment),
			0, MAX(dw, alloc.width),
			alloc.width * 0.1, alloc.width * 0.9, alloc.width);
	}
	if (self->vadjustment) {
		gtk_adjustment_configure(self->vadjustment,
			gtk_adjustment_get_value(self->vadjustment),
			0, MAX(dh, alloc.height),
			alloc.height * 0.1, alloc.height * 0.9, alloc.height);
	}
}

static gboolean
replace_adjustment(
	FivView *self, GtkAdjustment **adjustment, GtkAdjustment *replacement)
{
	if (*adjustment == replacement)
		return FALSE;

	if (*adjustment) {
		g_signal_handlers_disconnect_by_func(
			*adjustment, on_adjustment_value_changed, self);
		g_clear_object(adjustment);
	}
	if (replacement) {
		*adjustment = g_object_ref(replacement);
		g_signal_connect(*adjustment, "value-changed",
			G_CALLBACK(on_adjustment_value_changed), self);
		update_adjustments(self);
	}
	return TRUE;
}

static void
fiv_view_finalize(GObject *gobject)
{
	FivView *self = FIV_VIEW(gobject);
	g_clear_pointer(&self->screen_cms_profile, fiv_io_profile_free);
	g_clear_pointer(&self->enhance_swap, fiv_io_image_unref);
	g_clear_pointer(&self->image, fiv_io_image_unref);
	g_clear_pointer(&self->page_scaled, fiv_io_image_unref);
	g_free(self->uri);
	g_free(self->messages);

	replace_adjustment(self, &self->hadjustment, NULL);
	replace_adjustment(self, &self->vadjustment, NULL);

	G_OBJECT_CLASS(fiv_view_parent_class)->finalize(gobject);
}

static void
fiv_view_get_property(
	GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	FivView *self = FIV_VIEW(object);
	switch (property_id) {
	case PROP_MESSAGES:
		g_value_set_string(value, self->messages);
		break;
	case PROP_SCALE:
		g_value_set_double(value, self->scale);
		break;
	case PROP_SCALE_TO_FIT:
		g_value_set_boolean(value, self->scale_to_fit);
		break;
	case PROP_FIXATE:
		g_value_set_boolean(value, self->fixate);
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
		g_value_set_boolean(value, self->page && self->page->frame_next);
		break;
	case PROP_HAS_PREVIOUS_PAGE:
		g_value_set_boolean(value, self->image && self->page != self->image);
		break;
	case PROP_HAS_NEXT_PAGE:
		g_value_set_boolean(value, self->page && self->page->page_next);
		break;

	case PROP_HADJUSTMENT:
		g_value_set_object(value, self->hadjustment);
		break;
	case PROP_VADJUSTMENT:
		g_value_set_object(value, self->vadjustment);
		break;
	case PROP_HSCROLL_POLICY:
		g_value_set_enum(value, self->hscroll_policy);
		break;
	case PROP_VSCROLL_POLICY:
		g_value_set_enum(value, self->vscroll_policy);
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
	case PROP_FIXATE:
		if (self->fixate != g_value_get_boolean(value))
			fiv_view_command(self, FIV_VIEW_COMMAND_TOGGLE_FIXATE);
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

	case PROP_HADJUSTMENT:
		if (replace_adjustment(
				self, &self->hadjustment, g_value_get_object(value)))
			g_object_notify_by_pspec(object, pspec);
		break;
	case PROP_VADJUSTMENT:
		if (replace_adjustment(
				self, &self->vadjustment, g_value_get_object(value)))
			g_object_notify_by_pspec(object, pspec);
		break;
	case PROP_HSCROLL_POLICY:
		if ((gint) self->hscroll_policy != g_value_get_enum(value)) {
			self->hscroll_policy = g_value_get_enum(value);
			gtk_widget_queue_resize(GTK_WIDGET(self));
			g_object_notify_by_pspec(object, pspec);
		}
		break;
	case PROP_VSCROLL_POLICY:
		if ((gint) self->vscroll_policy != g_value_get_enum(value)) {
			self->vscroll_policy = g_value_get_enum(value);
			gtk_widget_queue_resize(GTK_WIDGET(self));
			g_object_notify_by_pspec(object, pspec);
		}
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
fiv_view_get_preferred_height(GtkWidget *widget, gint *minimum, gint *natural)
{
	FivView *self = FIV_VIEW(widget);
	if (self->scale_to_fit) {
		*minimum = 1;
		*natural = MAX(*minimum, ceil(get_surface_dimensions(self).height));
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
		*minimum = 1;
		*natural = MAX(*minimum, ceil(get_surface_dimensions(self).width));
	} else {
		int dw, dh;
		get_display_dimensions(self, &dw, &dh);
		*minimum = *natural = dw;
	}
}

static void
prescale_page(FivView *self)
{
	FivIoRenderClosure *closure = NULL;
	if (!self->image || !(closure = self->page->render))
		return;

	// TODO(p): Restart the animation. No vector formats currently animate.
	g_return_if_fail(!self->frame_update_connection);

	// Optimization, taking into account the workaround in set_scale().
	if (!self->page_scaled &&
		(self->scale == 1 || self->scale == 0.999999999999999))
		return;

	// If it fails, the previous frame pointer may become invalid.
	g_clear_pointer(&self->page_scaled, fiv_io_image_unref);
	self->frame = self->page_scaled = closure->render(closure,
		self->enable_cms ? fiv_io_cmm_get_default() : NULL,
		self->enable_cms ? self->screen_cms_profile : NULL, self->scale);
	if (!self->page_scaled)
		self->frame = self->page;
}

static void
set_source_image(FivView *self, cairo_t *cr)
{
	cairo_surface_t *surface = fiv_io_image_to_surface_noref(self->frame);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_surface_destroy(surface);
}

static void
fiv_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	GTK_WIDGET_CLASS(fiv_view_parent_class)->size_allocate(widget, allocation);

	FivView *self = FIV_VIEW(widget);
	if (!self->image || !self->scale_to_fit)
		goto out;

	Dimensions surface_dimensions = get_surface_dimensions(self);
	double scale = 1;
	if (ceil(surface_dimensions.width * scale) > allocation->width)
		scale = allocation->width / surface_dimensions.width;
	if (ceil(surface_dimensions.height * scale) > allocation->height)
		scale = allocation->height / surface_dimensions.height;

	if (self->scale != scale) {
		self->scale = scale;
		g_object_notify_by_pspec(G_OBJECT(widget), view_properties[PROP_SCALE]);
		prescale_page(self);
	}

out:
	update_adjustments(self);
}

// https://www.freedesktop.org/wiki/OpenIcc/ICC_Profiles_in_X_Specification_0.4
// has disappeared, but you can use the wayback machine.
//
// Note that Wayland does not have any appropriate protocol, as of writing:
// https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/14
static FivIoProfile *
monitor_cms_profile(GdkWindow *root, int num)
{
	char atom[32] = "";
	g_snprintf(atom, sizeof atom, "_ICC_PROFILE%c%d", num ? '_' : '\0', num);

	// Sadly, there is no nice GTK+/GDK mechanism to watch this for changes.
	int format = 0, length = 0;
	GdkAtom type = GDK_NONE;
	guchar *data = NULL;
	FivIoProfile *result = NULL;
	if (gdk_property_get(root, gdk_atom_intern(atom, FALSE), GDK_NONE, 0,
			8 << 20 /* MiB */, FALSE, &type, &format, &length, &data)) {
		if (format == 8 && length > 0)
			result = fiv_io_cmm_get_profile(
				fiv_io_cmm_get_default(), data, length);
		g_free(data);
	}
	return result;
}

static void
reload_screen_cms_profile(FivView *self, GdkWindow *window)
{
	g_clear_pointer(&self->screen_cms_profile, fiv_io_profile_free);

#ifdef GDK_WINDOWING_WIN32
	if (GDK_IS_WIN32_WINDOW(window)) {
		HWND hwnd = GDK_WINDOW_HWND(window);
		HDC hdc = GetDC(hwnd);
		if (hdc) {
			DWORD len = 0;
			(void) GetICMProfile(hdc, &len, NULL);
			gchar *path = g_new(gchar, len);
			if (GetICMProfile(hdc, &len, path)) {
				gchar *data = NULL;
				gsize length = 0;
				if (g_file_get_contents(path, &data, &length, NULL))
					self->screen_cms_profile = fiv_io_cmm_get_profile(
						fiv_io_cmm_get_default(), data, length);
				g_free(data);
			}
			g_free(path);
			ReleaseDC(hwnd, hdc);
		}
		goto out;
	}
#endif  // GDK_WINDOWING_WIN32

	GdkDisplay *display = gdk_window_get_display(window);
	GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, window);
	GdkWindow *root = gdk_screen_get_root_window(gdk_window_get_screen(window));

	int num = -1;
	for (int i = gdk_display_get_n_monitors(display); num < 0 && i--; )
		if (gdk_display_get_monitor(display, i) == monitor)
			num = i;
	if (num < 0)
		goto out;

	// Cater to xiccd limitations (agalakhov/xiccd#33).
	if (!(self->screen_cms_profile = monitor_cms_profile(root, num)) && num)
		self->screen_cms_profile = monitor_cms_profile(root, 0);

out:
	if (!self->screen_cms_profile)
		self->screen_cms_profile =
			fiv_io_cmm_get_profile_sRGB(fiv_io_cmm_get_default());
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

		// Pointer motion/release enables GtkGestureDrag.
		.event_mask = gtk_widget_get_events(widget) | GDK_KEY_PRESS_MASK |
			GDK_SCROLL_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
			GDK_POINTER_MOTION_MASK,
	};

	// We need this window to receive input events at all.
	GdkWindow *window = gdk_window_new(gtk_widget_get_parent_window(widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);

	GSettings *settings = g_settings_new(PROJECT_NS PROJECT_NAME);
	gboolean opengl = g_settings_get_boolean(settings, "opengl");

	// Without the following call, or the rendering mode set to "recording",
	// RGB30 degrades to RGB24, because gdk_window_begin_paint_internal()
	// creates backing stores using cairo_content_t constants.
	//
	// It completely breaks the Quartz backend, so limit it to X11.
#ifdef GDK_WINDOWING_X11
	// Note that this disables double buffering, and sometimes causes artefacts,
	// see: https://gitlab.gnome.org/GNOME/gtk/-/issues/2560
	//
	// GTK+'s OpenGL integration is terrible, so we may need to use
	// the X11 subwindow directly, sidestepping the toolkit entirely.
	if (GDK_IS_X11_WINDOW(window) &&
		g_settings_get_boolean(settings, "native-view-window"))
		gdk_window_ensure_native(window);
#endif  // GDK_WINDOWING_X11
	g_object_unref(settings);

	gtk_widget_register_window(widget, window);
	gtk_widget_set_window(widget, window);
	gtk_widget_set_realized(widget, TRUE);

	reload_screen_cms_profile(FIV_VIEW(widget), window);

	FivView *self = FIV_VIEW(widget);
	g_clear_object(&self->gl_context);
	if (!opengl)
		return;

	GError *error = NULL;
	GdkGLContext *gl_context = gdk_window_create_gl_context(window, &error);
	if (!gl_context) {
		g_warning("GL: %s", error->message);
		g_error_free(error);
		return;
	}

	gdk_gl_context_set_use_es(gl_context, FALSE);
	gdk_gl_context_set_required_version(gl_context, 3, 3);
	gdk_gl_context_set_debug_enabled(gl_context, TRUE);

	if (!gdk_gl_context_realize(gl_context, &error)) {
		g_warning("GL: %s", error->message);
		g_error_free(error);
		g_object_unref(gl_context);
		return;
	}

	self->gl_context = gl_context;
}

static void GLAPIENTRY
gl_on_message(G_GNUC_UNUSED GLenum source, GLenum type, G_GNUC_UNUSED GLuint id,
	G_GNUC_UNUSED GLenum severity, G_GNUC_UNUSED GLsizei length,
	const GLchar *message, G_GNUC_UNUSED const void *user_data)
{
	if (type == GL_DEBUG_TYPE_ERROR)
		g_warning("GL: error: %s", message);
	else
		g_debug("GL: %s", message);
}

static void
fiv_view_unrealize(GtkWidget *widget)
{
	FivView *self = FIV_VIEW(widget);
	if (self->gl_context) {
		if (self->gl_initialized) {
			gdk_gl_context_make_current(self->gl_context);
			glDeleteProgram(self->gl_program);
		}
		if (self->gl_context == gdk_gl_context_get_current())
			gdk_gl_context_clear_current();

		g_clear_object(&self->gl_context);
	}

	GTK_WIDGET_CLASS(fiv_view_parent_class)->unrealize(widget);
}

static bool
gl_draw(FivView *self, cairo_t *cr)
{
	gdk_gl_context_make_current(self->gl_context);

	if (!self->gl_initialized) {
		GLuint program = gl_make_program();
		if (!program)
			return false;

		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		if (epoxy_has_gl_extension("GL_ARB_debug_output")) {
			glEnable(GL_DEBUG_OUTPUT);
			glDebugMessageCallback(gl_on_message, NULL);
		}

		self->gl_program = program;
		self->gl_initialized = true;
	}

	// This limit is always less than that of Cairo/pixman,
	// and we'd have to figure out tiling.
	GLint max = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
	if (max < (GLint) self->frame->width ||
		max < (GLint) self->frame->height) {
		g_warning("OpenGL max. texture size is too small");
		return false;
	}

	GtkAllocation allocation;
	gtk_widget_get_allocation(GTK_WIDGET(self), &allocation);
	int dw = 0, dh = 0, dx = 0, dy = 0;
	get_display_dimensions(self, &dw, &dh);

	int clipw = dw, cliph = dh;
	double x1 = 0., y1 = 0., x2 = 1., y2 = 1.;
	if (self->hadjustment)
		x1 = floor(gtk_adjustment_get_value(self->hadjustment)) / dw;
	if (self->vadjustment)
		y1 = floor(gtk_adjustment_get_value(self->vadjustment)) / dh;

	if (dw <= allocation.width) {
		dx = round((allocation.width - dw) / 2.);
	} else {
		x2 = x1 + (double) allocation.width / dw;
		clipw = allocation.width;
	}

	if (dh <= allocation.height) {
		dy = round((allocation.height - dh) / 2.);
	} else {
		y2 = y1 + (double) allocation.height / dh;
		cliph = allocation.height;
	}

	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(self));
	clipw *= scale;
	cliph *= scale;

	enum { SRC, DEST };
	GLuint textures[2] = {};
	glGenTextures(2, textures);

	// https://stackoverflow.com/questions/25157306 0..1
	// GL_TEXTURE_RECTANGLE seems kind-of useful
	glBindTexture(GL_TEXTURE_2D, textures[SRC]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	if (self->filter) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	// GL_UNPACK_ALIGNMENT is initially 4, which is fine for these.
	// Texture swizzling is OpenGL 3.3.
	if (self->frame->format == CAIRO_FORMAT_ARGB32) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
			self->frame->width, self->frame->height,
			0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, self->frame->data);
	} else if (self->frame->format == CAIRO_FORMAT_RGB24) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
			self->frame->width, self->frame->height,
			0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, self->frame->data);
	} else if (self->frame->format == CAIRO_FORMAT_RGB30) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
			self->frame->width, self->frame->height,
			0, GL_BGRA, GL_UNSIGNED_INT_2_10_10_10_REV, self->frame->data);
	} else {
		g_warning("GL: unsupported bitmap format");
	}

	// GtkGLArea creates textures like this.
	glBindTexture(GL_TEXTURE_2D, textures[DEST]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, clipw, cliph, 0, GL_BGRA,
		GL_UNSIGNED_BYTE, NULL);

	glViewport(0, 0, clipw, cliph);

	GLuint vao = 0;
	glGenVertexArrays(1, &vao);

	GLuint frame_buffer = 0;
	glGenFramebuffers(1, &frame_buffer);
	glBindFramebuffer(GL_FRAMEBUFFER, frame_buffer);
	glFramebufferTexture2D(
		GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[DEST], 0);

	glClearColor(0., 0., 0., 1.);
	glClear(GL_COLOR_BUFFER_BIT);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		g_warning("GL framebuffer status: %u", status);

	glUseProgram(self->gl_program);
	GLint position_location = glGetAttribLocation(
		self->gl_program, "position");
	GLint picture_location = glGetUniformLocation(
		self->gl_program, "picture");
	GLint checkerboard_location = glGetUniformLocation(
		self->gl_program, "checkerboard");

	glUniform1i(picture_location, 0);
	glUniform1i(checkerboard_location, self->checkerboard);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textures[SRC]);

	// Note that the Y axis is flipped in the table.
	double vertices[][4] = {
		{-1., -1., x1, y2},
		{+1., -1., x2, y2},
		{+1., +1., x2, y1},
		{-1., +1., x1, y1},
	};

	cairo_matrix_t matrix = fiv_io_orientation_matrix(self->orientation, 1, 1);
	cairo_matrix_transform_point(&matrix, &vertices[0][2], &vertices[0][3]);
	cairo_matrix_transform_point(&matrix, &vertices[1][2], &vertices[1][3]);
	cairo_matrix_transform_point(&matrix, &vertices[2][2], &vertices[2][3]);
	cairo_matrix_transform_point(&matrix, &vertices[3][2], &vertices[3][3]);

	GLuint vertex_buffer = 0;
	glGenBuffers(1, &vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
	glBindVertexArray(vao);
	glVertexAttribPointer(position_location,
		G_N_ELEMENTS(vertices[0]), GL_DOUBLE, GL_FALSE, sizeof vertices[0], 0);
	glEnableVertexAttribArray(position_location);
	glDrawArrays(GL_TRIANGLE_FAN, 0, G_N_ELEMENTS(vertices));
	glDisableVertexAttribArray(position_location);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// XXX: Native GdkWindows send this to the software fallback path.
	// XXX: This only reliably alpha blends when using the software fallback,
	// such as with a native window, because 7237f5d in GTK+ 3 is a regression.
	// We had to resort to rendering the checkerboard pattern in the shader.
	// Unfortunately, it is hard to retrieve the theme colours from CSS.
	GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(self));
	cairo_translate(cr, dx, dy);
	gdk_cairo_draw_from_gl(
		cr, window, textures[DEST], GL_TEXTURE, scale, 0, 0, clipw, cliph);
	gdk_gl_context_make_current(self->gl_context);

	glDeleteBuffers(1, &vertex_buffer);
	glDeleteTextures(2, textures);
	glDeleteVertexArrays(1, &vao);
	glDeleteFramebuffers(1, &frame_buffer);

	// TODO(p): Possibly use this clue as a hint to use Cairo rendering.
	GLenum err = 0;
	while ((err = glGetError()) != GL_NO_ERROR) {
		const char *string = gl_error_string(err);
		if (string)
			g_warning("GL: error: %s", string);
		else
			g_warning("GL: error: %u", err);
	}

	gdk_gl_context_clear_current();
	return true;
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
	if (self->gl_context && gl_draw(self, cr))
		return TRUE;

	int dw = 0, dh = 0;
	get_display_dimensions(self, &dw, &dh);

	double x = 0;
	double y = 0;
	if (self->hadjustment)
		x = -floor(gtk_adjustment_get_value(self->hadjustment));
	if (self->vadjustment)
		y = -floor(gtk_adjustment_get_value(self->vadjustment));
	if (dw < allocation.width)
		x = round((allocation.width - dw) / 2.);
	if (dh < allocation.height)
		y = round((allocation.height - dh) / 2.);

	// XXX: This naming is confusing, because it isn't actually for the surface,
	// but rather for our possibly rotated rendition of it.
	Dimensions surface_dimensions = {};
	cairo_matrix_t matrix = fiv_io_orientation_apply(
		self->page_scaled ? self->page_scaled : self->page, self->orientation,
		&surface_dimensions.width, &surface_dimensions.height);

	cairo_translate(cr, x, y);
	if (self->checkerboard) {
		gtk_style_context_save(style);
		gtk_style_context_add_class(style, "checkerboard");
		gtk_render_background(style, cr, 0, 0, dw, dh);
		gtk_style_context_restore(style);
	}

	// Then all frames are pre-scaled.
	if (self->page_scaled) {
		set_source_image(self, cr);
		cairo_pattern_set_matrix(cairo_get_source(cr), &matrix);
		cairo_paint(cr);
		return TRUE;
	}

	// XXX: The rounding together with padding may result in up to
	// a pixel's worth of made-up picture data.
	cairo_rectangle(cr, 0, 0, dw, dh);
	cairo_clip(cr);

	cairo_scale(cr, self->scale, self->scale);
	set_source_image(self, cr);

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
	if (GTK_WIDGET_CLASS(fiv_view_parent_class)
			->button_press_event(widget, event))
		return GDK_EVENT_STOP;

	if (event->button == GDK_BUTTON_PRIMARY &&
		gtk_widget_get_focus_on_click(widget))
		gtk_widget_grab_focus(widget);
	return GDK_EVENT_PROPAGATE;
}

#define SCALE_STEP 1.25

static gboolean
set_scale_to_fit(FivView *self, bool scale_to_fit)
{
	if (self->scale_to_fit != scale_to_fit) {
		if ((self->scale_to_fit = scale_to_fit)) {
			self->fixate = false;
			g_object_notify_by_pspec(
				G_OBJECT(self), view_properties[PROP_FIXATE]);
		}

		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_SCALE_TO_FIT]);
		gtk_widget_queue_resize(GTK_WIDGET(self));
	}
	return TRUE;
}

static void
widget_to_surface(FivView *self, double *x, double *y)
{
	int dw, dh;
	get_display_dimensions(self, &dw, &dh);
	GtkAllocation allocation;
	gtk_widget_get_allocation(GTK_WIDGET(self), &allocation);

	// Unneeded, thus unimplemented: this means zero adjustment values.
	if (!self->hadjustment || !self->vadjustment)
		return;

	*x = (*x + (dw < allocation.width
		? -round((allocation.width - dw) / 2.)
		: +floor(gtk_adjustment_get_value(self->hadjustment))))
			/ self->scale;
	*y = (*y + (dh < allocation.height
		? -round((allocation.height - dh) / 2.)
		: +floor(gtk_adjustment_get_value(self->vadjustment))))
			/ self->scale;
}

static gboolean
set_scale(FivView *self, double scale, const GdkEvent *event)
{
	// FIXME: Zooming to exactly 1:1 breaks rendering with some images
	// when using a native X11 Window. This is a silly workaround.
	GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(self));
	if (window && gdk_window_has_native(window) && scale == 1)
		scale = 0.999999999999999;

	if (self->scale == scale)
		goto out;

	GtkAllocation allocation;
	gtk_widget_get_allocation(GTK_WIDGET(self), &allocation);
	double focus_x = 0, focus_y = 0;
	if (!event || !gdk_event_get_coords(event, &focus_x, &focus_y)) {
		focus_x = 0.5 * allocation.width;
		focus_y = 0.5 * allocation.height;
	}

	double surface_x = focus_x;
	double surface_y = focus_y;
	widget_to_surface(self, &surface_x, &surface_y);

	self->scale = scale;
	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_SCALE]);
	prescale_page(self);

	// Similar to set_orientation().
	if (self->hadjustment && self->vadjustment) {
		Dimensions surface_dimensions = get_surface_dimensions(self);
		update_adjustments(self);

		if (surface_dimensions.width * self->scale > allocation.width)
			gtk_adjustment_set_value(
				self->hadjustment, surface_x * self->scale - focus_x);
		if (surface_dimensions.height * self->scale > allocation.height)
			gtk_adjustment_set_value(
				self->vadjustment, surface_y * self->scale - focus_y);
	}

	gtk_widget_queue_resize(GTK_WIDGET(self));

out:
	return set_scale_to_fit(self, false);
}

static void
set_scale_to_fit_width(FivView *self)
{
	double w = get_surface_dimensions(self).width;
	int allocated = gtk_widget_get_allocated_width(GTK_WIDGET(self));
	if (ceil(w * self->scale) > allocated)
		set_scale(self, allocated / w, NULL);
}

static void
set_scale_to_fit_height(FivView *self)
{
	double h = get_surface_dimensions(self).height;
	int allocated = gtk_widget_get_allocated_height(GTK_WIDGET(self));
	if (ceil(h * self->scale) > allocated)
		set_scale(self, allocated / h, NULL);
}

static gboolean
fiv_view_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
	FivView *self = FIV_VIEW(widget);
	if (!self->image)
		return GDK_EVENT_PROPAGATE;
	if (event->state & gtk_accelerator_get_default_mod_mask())
		return GDK_EVENT_PROPAGATE;

	switch (event->direction) {
	case GDK_SCROLL_UP:
		return set_scale(self, self->scale * SCALE_STEP, (GdkEvent *) event);
	case GDK_SCROLL_DOWN:
		return set_scale(self, self->scale / SCALE_STEP, (GdkEvent *) event);
	default:
		// For some reason, native GdkWindows may also get GDK_SCROLL_SMOOTH.
		// Left/right are good to steal from GtkScrolledWindow for consistency.
		return GDK_EVENT_STOP;
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
	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_PLAYING]);
}

static gboolean
advance_frame(FivView *self)
{
	FivIoImage *next = self->frame->frame_next;
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
		int64_t duration = self->frame->frame_duration;
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
	if (!clock || !self->image || !self->page->frame_next)
		return;

	self->frame_time = gdk_frame_clock_get_frame_time(clock);
	self->frame_update_connection = g_signal_connect(
		clock, "update", G_CALLBACK(on_frame_clock_update), self);

	// Only restart looping the animation if it has stopped at the end.
	if (!self->remaining_loops) {
		self->remaining_loops = self->page->loops;
		if (self->remaining_loops && !self->frame->frame_next) {
			self->frame = self->page;
			gtk_widget_queue_draw(GTK_WIDGET(self));
		}
	}

	gdk_frame_clock_begin_updating(clock);
	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_PLAYING]);
}

static void
switch_page(FivView *self, FivIoImage *page)
{
	g_clear_pointer(&self->page_scaled, fiv_io_image_unref);
	self->frame = self->page = page;

	// XXX: When self->scale_to_fit is in effect,
	// this uses an old value that may no longer be appropriate,
	// resulting in wasted effort.
	prescale_page(self);

	if (!self->page ||
		(self->orientation = self->page->orientation) ==
			FivIoOrientationUnknown)
		self->orientation = FivIoOrientation0;

	self->remaining_loops = 0;
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
on_drag_begin(GtkGestureDrag *drag, G_GNUC_UNUSED gdouble start_x,
	G_GNUC_UNUSED gdouble start_y, gpointer user_data)
{
	GtkGesture *gesture = GTK_GESTURE(drag);
	switch (
		gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))) {
	case GDK_BUTTON_PRIMARY:
	case GDK_BUTTON_MIDDLE:
		break;
	default:
		gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_DENIED);
		return;
	}

	GdkModifierType state = 0;
	GdkEventSequence *sequence = gtk_gesture_get_last_updated_sequence(gesture);
	gdk_event_get_state(gtk_gesture_get_last_event(gesture, sequence), &state);
	if (state & gtk_accelerator_get_default_mod_mask()) {
		gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_DENIED);
		return;
	}

	// Since we set this up as a pointer-only gesture, there is only the NULL
	// sequence, so gtk_gesture_set_sequence_state() is completely unneeded.
	gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);

	GdkWindow *window = gtk_widget_get_window(
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(drag)));
	GdkCursor *cursor =
		gdk_cursor_new_from_name(gdk_window_get_display(window), "grabbing");
	gdk_window_set_cursor(window, cursor);
	g_object_unref(cursor);

	FivView *self = FIV_VIEW(user_data);
	self->drag_start[0] = self->hadjustment ?
		gtk_adjustment_get_value(self->hadjustment) : 0;
	self->drag_start[1] = self->vadjustment ?
		gtk_adjustment_get_value(self->vadjustment) : 0;
}

static void
on_drag_update(G_GNUC_UNUSED GtkGestureDrag *drag, gdouble offset_x,
	gdouble offset_y, gpointer user_data)
{
	FivView *self = FIV_VIEW(user_data);
	if (self->hadjustment) {
		gtk_adjustment_set_value(
			self->hadjustment, self->drag_start[0] - offset_x);
	}
	if (self->vadjustment) {
		gtk_adjustment_set_value(
			self->vadjustment, self->drag_start[1] - offset_y);
	}
}

static void
on_drag_end(GtkGestureDrag *drag, G_GNUC_UNUSED gdouble start_x,
	G_GNUC_UNUSED gdouble start_y, G_GNUC_UNUSED gpointer user_data)
{
	GdkWindow *window = gtk_widget_get_window(
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(drag)));
	gdk_window_set_cursor(window, NULL);
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
copy(FivView *self)
{
	double fractional_width = 0, fractional_height = 0;
	cairo_matrix_t matrix = fiv_io_orientation_apply(
		self->frame, self->orientation, &fractional_width, &fractional_height);
	int w = ceil(fractional_width), h = ceil(fractional_height);

	// XXX: SVG is rendered pre-scaled.
	cairo_surface_t *transformed =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(transformed);
	set_source_image(self, cr);
	cairo_pattern_set_matrix(cairo_get_source(cr), &matrix);
	cairo_paint(cr);
	cairo_destroy(cr);

	// TODO(p): Use 16-bit PNGs for >8-bit Cairo surfaces: PNG-encode them
	// ourselves and fall back to gtk_selection_data_set_pixbuf().
	GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(transformed, 0, 0, w, h);
	cairo_surface_destroy(transformed);
	gtk_clipboard_set_image(
		gtk_clipboard_get_for_display(
			gtk_widget_get_display(GTK_WIDGET(self)), GDK_SELECTION_CLIPBOARD),
		pixbuf);
	g_object_unref(pixbuf);
}

static void
on_draw_page(G_GNUC_UNUSED GtkPrintOperation *operation,
	GtkPrintContext *context, G_GNUC_UNUSED int page_nr, FivView *self)
{
	// Any DPI will be wrong, unless we import that information from the image.
	double scale = 1 / 96.;
	Dimensions surface_dimensions = {};
	// XXX: Perhaps use self->frame, even though their sizes should match.
	cairo_matrix_t matrix =
		fiv_io_orientation_apply(self->page, self->orientation,
			&surface_dimensions.width, &surface_dimensions.height);

	double w = surface_dimensions.width * scale;
	double h = surface_dimensions.height * scale;

	// Scale down to fit the print area, taking care to not divide by zero.
	double areaw = gtk_print_context_get_width(context);
	double areah = gtk_print_context_get_height(context);
	scale *= fmin((areaw < w) ? areaw / w : 1, (areah < h) ? areah / h : 1);

	cairo_t *cr = gtk_print_context_get_cairo_context(context);
	cairo_scale(cr, scale, scale);
	set_source_image(self, cr);
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
save_as(FivView *self, FivIoImage *frame)
{
	GtkWindow *window = get_toplevel(GTK_WIDGET(self));
	FivIoProfile *target = NULL;
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
	GFileInfo *info =
		g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
			G_FILE_QUERY_INFO_NONE, NULL, NULL);

	// Note that GTK+'s save dialog is too stupid to automatically change
	// the extension when user changes the filter. Presumably,
	// gtk_file_chooser_set_extra_widget() can be used to circumvent this.
	const char *basename = info ? g_file_info_get_display_name(info) : "image";
	gchar *name = g_strconcat(basename, frame ? "-frame.webp" : ".webp", NULL);
	gtk_file_chooser_set_current_name(chooser, name);
	g_free(name);
	if (g_file_peek_path(file)) {
		GFile *parent = g_file_get_parent(file);
		(void) gtk_file_chooser_set_current_folder_file(chooser, parent, NULL);
		g_object_unref(parent);
	}
	g_object_unref(file);
	g_clear_object(&info);

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

static void
info(FivView *self)
{
	fiv_context_menu_information(get_toplevel(GTK_WIDGET(self)), self->uri);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
fiv_view_key_press_event(GtkWidget *widget, GdkEventKey *event)
{
	FivView *self = FIV_VIEW(widget);

	// So far, our commands cannot accept arguments, so these few are hardcoded.
	if (self->image &&
		!(event->state & gtk_accelerator_get_default_mod_mask()) &&
		event->keyval >= GDK_KEY_1 && event->keyval <= GDK_KEY_9)
		return set_scale(self, event->keyval - GDK_KEY_0, NULL);

	return GTK_WIDGET_CLASS(fiv_view_parent_class)
		->key_press_event(widget, event);
}

static void
bind(GtkBindingSet *bs, guint keyval, GdkModifierType modifiers,
	FivViewCommand command)
{
	gtk_binding_entry_add_signal(
		bs, keyval, modifiers, "command", 1, FIV_TYPE_VIEW_COMMAND, command);
}

static void
fiv_view_class_init(FivViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fiv_view_finalize;
	object_class->get_property = fiv_view_get_property;
	object_class->set_property = fiv_view_set_property;

	view_properties[PROP_MESSAGES] = g_param_spec_string(
		"messages", "Messages", "Informative messages from the last image load",
		NULL, G_PARAM_READABLE);
	view_properties[PROP_SCALE] = g_param_spec_double(
		"scale", "Scale", "Zoom level",
		0, G_MAXDOUBLE, 1.0, G_PARAM_READABLE);
	view_properties[PROP_SCALE_TO_FIT] = g_param_spec_boolean(
		"scale-to-fit", "Scale to fit", "Scale images down to fit the window",
		TRUE, G_PARAM_READWRITE);
	view_properties[PROP_FIXATE] = g_param_spec_boolean(
		"fixate", "Fixate", "Keep zoom and position",
		FALSE, G_PARAM_READWRITE);
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

	g_object_class_override_property(
		object_class, PROP_HADJUSTMENT, "hadjustment");
	g_object_class_override_property(
		object_class, PROP_VADJUSTMENT, "vadjustment");
	g_object_class_override_property(
		object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
	g_object_class_override_property(
		object_class, PROP_VSCROLL_POLICY, "vscroll-policy");

	view_signals[COMMAND] =
		g_signal_new_class_handler("command", G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, G_CALLBACK(fiv_view_command),
			NULL, NULL, NULL, G_TYPE_NONE, 1, FIV_TYPE_VIEW_COMMAND);

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->get_preferred_height = fiv_view_get_preferred_height;
	widget_class->get_preferred_width = fiv_view_get_preferred_width;
	widget_class->size_allocate = fiv_view_size_allocate;
	widget_class->map = fiv_view_map;
	widget_class->unmap = fiv_view_unmap;
	widget_class->realize = fiv_view_realize;
	widget_class->unrealize = fiv_view_unrealize;
	widget_class->draw = fiv_view_draw;
	widget_class->button_press_event = fiv_view_button_press_event;
	widget_class->scroll_event = fiv_view_scroll_event;
	widget_class->key_press_event = fiv_view_key_press_event;

	// _gtk_get_primary_accel_mod() is private.
	GdkModifierType primary = GDK_CONTROL_MASK;
	gtk_accelerator_parse_with_keycode("<Primary>", NULL, NULL, &primary);

	GtkBindingSet *bs = gtk_binding_set_by_class(klass);
	// First, the standard, intuitive bindings.
	bind(bs, GDK_KEY_0,      primary,       FIV_VIEW_COMMAND_ZOOM_1);
	bind(bs, GDK_KEY_plus,   primary,       FIV_VIEW_COMMAND_ZOOM_IN);
	bind(bs, GDK_KEY_minus,  primary,       FIV_VIEW_COMMAND_ZOOM_OUT);
	bind(bs, GDK_KEY_c,      primary,       FIV_VIEW_COMMAND_COPY);
	bind(bs, GDK_KEY_p,      primary,       FIV_VIEW_COMMAND_PRINT);
	bind(bs, GDK_KEY_r,      primary,       FIV_VIEW_COMMAND_RELOAD);
	bind(bs, GDK_KEY_s,      primary,       FIV_VIEW_COMMAND_SAVE_PAGE);
	bind(bs, GDK_KEY_s,      GDK_MOD1_MASK, FIV_VIEW_COMMAND_SAVE_FRAME);
	bind(bs, GDK_KEY_Return, GDK_MOD1_MASK, FIV_VIEW_COMMAND_INFO);

	// The scale-to-fit binding is from gThumb, which has more such modes.
	bind(bs, GDK_KEY_F5,           0, FIV_VIEW_COMMAND_RELOAD);
	bind(bs, GDK_KEY_r,            0, FIV_VIEW_COMMAND_RELOAD);
	bind(bs, GDK_KEY_plus,         0, FIV_VIEW_COMMAND_ZOOM_IN);
	bind(bs, GDK_KEY_minus,        0, FIV_VIEW_COMMAND_ZOOM_OUT);
	bind(bs, GDK_KEY_w,            0, FIV_VIEW_COMMAND_FIT_WIDTH);
	bind(bs, GDK_KEY_h,            0, FIV_VIEW_COMMAND_FIT_HEIGHT);
	bind(bs, GDK_KEY_k,            0, FIV_VIEW_COMMAND_TOGGLE_FIXATE);
	bind(bs, GDK_KEY_x,            0, FIV_VIEW_COMMAND_TOGGLE_SCALE_TO_FIT);
	bind(bs, GDK_KEY_c,            0, FIV_VIEW_COMMAND_TOGGLE_CMS);
	bind(bs, GDK_KEY_i,            0, FIV_VIEW_COMMAND_TOGGLE_FILTER);
	bind(bs, GDK_KEY_t,            0, FIV_VIEW_COMMAND_TOGGLE_CHECKERBOARD);
	bind(bs, GDK_KEY_e,            0, FIV_VIEW_COMMAND_TOGGLE_ENHANCE);

	bind(bs, GDK_KEY_less,         0, FIV_VIEW_COMMAND_ROTATE_LEFT);
	bind(bs, GDK_KEY_equal,        0, FIV_VIEW_COMMAND_MIRROR);
	bind(bs, GDK_KEY_greater,      0, FIV_VIEW_COMMAND_ROTATE_RIGHT);

	bind(bs, GDK_KEY_bracketleft,  0, FIV_VIEW_COMMAND_PAGE_PREVIOUS);
	bind(bs, GDK_KEY_bracketright, 0, FIV_VIEW_COMMAND_PAGE_NEXT);
	bind(bs, GDK_KEY_braceleft,    0, FIV_VIEW_COMMAND_FRAME_PREVIOUS);
	bind(bs, GDK_KEY_braceright,   0, FIV_VIEW_COMMAND_FRAME_NEXT);
	bind(bs, GDK_KEY_space,        0, FIV_VIEW_COMMAND_TOGGLE_PLAYBACK);

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

	GtkGesture *drag = gtk_gesture_drag_new(GTK_WIDGET(self));
	gtk_event_controller_set_propagation_phase(
		GTK_EVENT_CONTROLLER(drag), GTK_PHASE_BUBBLE);
	g_object_set_data_full(
		G_OBJECT(self), "fiv-view-drag-gesture", drag, g_object_unref);

	// GtkScrolledWindow's internal GtkGestureDrag is set to only look for
	// touch events (and its "event_controllers" are perfectly private,
	// so we can't change this), hopefully this is mutually exclusive with that.
	// Though note that the GdkWindow doesn't register for touch events now.
	gtk_gesture_single_set_exclusive(GTK_GESTURE_SINGLE(drag), TRUE);
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);

	g_signal_connect(drag, "drag-begin",
		G_CALLBACK(on_drag_begin), self);
	g_signal_connect(drag, "drag-update",
		G_CALLBACK(on_drag_update), self);
	g_signal_connect(drag, "drag-end",
		G_CALLBACK(on_drag_end), self);
}

// --- Public interface --------------------------------------------------------

static FivIoImage *
open_without_swapping_in(FivView *self, const char *uri)
{
	FivIoOpenContext ctx = {
		.uri = uri,
		.cmm = self->enable_cms ? fiv_io_cmm_get_default() : NULL,
		.screen_profile = self->enable_cms ? self->screen_cms_profile : NULL,
		.screen_dpi = 96,  // TODO(p): Try to retrieve it from the screen.
		.enhance = self->enhance,
		.warnings = g_ptr_array_new_with_free_func(g_free),
	};

	GError *error = NULL;
	FivIoImage *image = fiv_io_open(&ctx, &error);
	if (error) {
		g_ptr_array_add(ctx.warnings, g_strdup(error->message));
		g_error_free(error);
	}

	g_clear_pointer(&self->messages, g_free);
	if (ctx.warnings->len) {
		g_ptr_array_add(ctx.warnings, NULL);
		self->messages = g_strjoinv("\n", (gchar **) ctx.warnings->pdata);
	}

	g_ptr_array_free(ctx.warnings, TRUE);
	return image;
}

// TODO(p): Progressive picture loading, or at least async/cancellable.
gboolean
fiv_view_set_uri(FivView *self, const char *uri)
{
	// This is extremely expensive, and only works sometimes.
	g_clear_pointer(&self->enhance_swap, fiv_io_image_unref);
	if (self->enhance) {
		self->enhance = FALSE;
		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_ENHANCE]);
	}

	FivIoImage *image = open_without_swapping_in(self, uri);
	g_clear_pointer(&self->image, fiv_io_image_unref);

	self->frame = self->page = NULL;
	self->image = image;
	switch_page(self, self->image);

	// Otherwise, adjustment values and zoom are retained implicitly.
	if (!self->fixate)
		set_scale_to_fit(self, true);

	g_free(self->uri);
	self->uri = g_strdup(uri);

	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_MESSAGES]);
	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_HAS_IMAGE]);
	return image != NULL;
}

static void
page_step(FivView *self, int step)
{
	FivIoImage *page = step < 0
		? self->page->page_previous
		: self->page->page_next;
	if (page)
		switch_page(self, page);
}

static void
frame_step(FivView *self, int step)
{
	stop_animating(self);

	if (step > 0) {
		// Decrease the loop counter as if running on a timer.
		(void) advance_frame(self);
	} else if (!step || !(self->frame = self->frame->frame_previous)) {
		self->frame = self->page;
		self->remaining_loops = 0;
	}
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

static gboolean
reload(FivView *self)
{
	FivIoImage *image = open_without_swapping_in(self, self->uri);
	g_object_notify_by_pspec(G_OBJECT(self), view_properties[PROP_MESSAGES]);
	if (!image)
		return FALSE;

	g_clear_pointer(&self->image, fiv_io_image_unref);
	g_clear_pointer(&self->enhance_swap, fiv_io_image_unref);
	switch_page(self, (self->image = image));
	return TRUE;
}

static void
swap_enhanced_image(FivView *self)
{
	FivIoImage *saved = self->image;
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

static void
transformed_to_real(FivView *self, double *x, double *y)
{
	double sw = 0, sh = 0;
	cairo_matrix_t matrix =
		fiv_io_orientation_apply(self->page, self->orientation, &sw, &sh);
	cairo_matrix_transform_point(&matrix, x, y);
}

static void
set_orientation(FivView *self, FivIoOrientation orientation)
{
	GtkAllocation allocation;
	gtk_widget_get_allocation(GTK_WIDGET(self), &allocation);

	// In the future, rotating gestures can pick another centre point.
	double focus_x = 0.5 * allocation.width;
	double focus_y = 0.5 * allocation.height;

	double surface_x = focus_x;
	double surface_y = focus_y;
	widget_to_surface(self, &surface_x, &surface_y);
	transformed_to_real(self, &surface_x, &surface_y);

	self->orientation = orientation;

	// Similar to set_scale().
	Dimensions surface_dimensions = {};
	cairo_matrix_t matrix =
		fiv_io_orientation_apply(self->page, self->orientation,
			&surface_dimensions.width, &surface_dimensions.height);
	if (self->hadjustment && self->vadjustment &&
			cairo_matrix_invert(&matrix) == CAIRO_STATUS_SUCCESS) {
		cairo_matrix_transform_point(&matrix, &surface_x, &surface_y);
		update_adjustments(self);

		if (surface_dimensions.width * self->scale > allocation.width)
			gtk_adjustment_set_value(
				self->hadjustment, surface_x * self->scale - focus_x);
		if (surface_dimensions.height * self->scale > allocation.height)
			gtk_adjustment_set_value(
				self->vadjustment, surface_y * self->scale - focus_y);
	}

	gtk_widget_queue_resize(GTK_WIDGET(self));
}

void
fiv_view_command(FivView *self, FivViewCommand command)
{
	g_return_if_fail(FIV_IS_VIEW(self));

	GtkWidget *widget = GTK_WIDGET(self);
	if (!self->image)
		return;

	switch (command) {
	break; case FIV_VIEW_COMMAND_RELOAD:
		reload(self);

	break; case FIV_VIEW_COMMAND_ROTATE_LEFT:
		set_orientation(self, view_left[self->orientation]);
	break; case FIV_VIEW_COMMAND_MIRROR:
		set_orientation(self, view_mirror[self->orientation]);
	break; case FIV_VIEW_COMMAND_ROTATE_RIGHT:
		set_orientation(self, view_right[self->orientation]);

	break; case FIV_VIEW_COMMAND_PAGE_FIRST:
		switch_page(self, self->image);
	break; case FIV_VIEW_COMMAND_PAGE_PREVIOUS:
		page_step(self, -1);
	break; case FIV_VIEW_COMMAND_PAGE_NEXT:
		page_step(self, +1);
	break; case FIV_VIEW_COMMAND_PAGE_LAST:
		for (FivIoImage *I = self->page; (I = I->page_next); )
			self->page = I;
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

	break; case FIV_VIEW_COMMAND_COPY:
		copy(self);
	break; case FIV_VIEW_COMMAND_PRINT:
		print(self);
	break; case FIV_VIEW_COMMAND_SAVE_PAGE:
		save_as(self, NULL);
	break; case FIV_VIEW_COMMAND_SAVE_FRAME:
		save_as(self, self->frame);
	break; case FIV_VIEW_COMMAND_INFO:
		info(self);

	break; case FIV_VIEW_COMMAND_ZOOM_IN:
		set_scale(self, self->scale * SCALE_STEP, NULL);
	break; case FIV_VIEW_COMMAND_ZOOM_OUT:
		set_scale(self, self->scale / SCALE_STEP, NULL);
	break; case FIV_VIEW_COMMAND_ZOOM_1:
		set_scale(self, 1.0, NULL);
	break; case FIV_VIEW_COMMAND_FIT_WIDTH:
		set_scale_to_fit_width(self);
	break; case FIV_VIEW_COMMAND_FIT_HEIGHT:
		set_scale_to_fit_height(self);
	break; case FIV_VIEW_COMMAND_TOGGLE_SCALE_TO_FIT:
		set_scale_to_fit(self, !self->scale_to_fit);
	break; case FIV_VIEW_COMMAND_TOGGLE_FIXATE:
		if ((self->fixate = !self->fixate))
			set_scale_to_fit(self, false);
		g_object_notify_by_pspec(
			G_OBJECT(self), view_properties[PROP_FIXATE]);
	}
}

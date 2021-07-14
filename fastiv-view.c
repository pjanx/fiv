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

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB

#include "wuffs-mirror-release-c/release/c/wuffs-v0.3.c"
#include <turbojpeg.h>

#include "fastiv-view.h"

struct _FastivView {
	GtkWidget parent_instance;
	cairo_surface_t *surface;
	// TODO(p): Eventually, we'll want to have the zoom level (scale) here,
	// and some zoom-to-fit indication.
};

G_DEFINE_TYPE(FastivView, fastiv_view, GTK_TYPE_WIDGET)

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

	// TODO(p): Times the zoom.
	FastivView *self = FASTIV_VIEW(widget);
	if (self->surface)
		*natural = cairo_image_surface_get_height(self->surface);
}

static void
fastiv_view_get_preferred_width(GtkWidget *widget,
	gint *minimum, gint *natural)
{
	*minimum = 0;
	*natural = 0;

	// TODO(p): Times the zoom.
	FastivView *self = FASTIV_VIEW(widget);
	if (self->surface)
		*natural = cairo_image_surface_get_width(self->surface);
}

static gboolean
fastiv_view_draw(GtkWidget *widget, cairo_t *cr)
{
	FastivView *self = FASTIV_VIEW(widget);
	if (!self->surface)
		return TRUE;

	// TODO(p): Times the zoom.
	cairo_set_source_surface(cr, self->surface, 0, 0);
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
	gtk_widget_set_has_window(GTK_WIDGET(self), FALSE);
}

// --- Picture loading ---------------------------------------------------------

#define FASTIV_VIEW_ERROR  fastiv_view_error_quark()

G_DEFINE_QUARK(fastiv-view-error-quark, fastiv_view_error)

enum FastivViewError {
	FASTIV_VIEW_ERROR_OPEN,
};

static void
set_error(GError **error, const char *message)
{
	g_set_error_literal(error,
		FASTIV_VIEW_ERROR, FASTIV_VIEW_ERROR_OPEN, message);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// https://github.com/google/wuffs/blob/main/example/gifplayer/gifplayer.c
// is pure C, and a good reference. I can't use the auxiliary libraries,
// since they depend on C++, which is undesirable.
static cairo_surface_t *
open_wuffs(wuffs_base__image_decoder *dec,
	wuffs_base__io_buffer src, GError **error)
{
	wuffs_base__image_config cfg;
	wuffs_base__status status =
		wuffs_base__image_decoder__decode_image_config(dec, &cfg, &src);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		return NULL;
	}
	if (!wuffs_base__image_config__is_valid(&cfg)) {
		set_error(error, "invalid Wuffs image configuration");
		return NULL;
	}

	// We need to check because of the Cairo API.
	uint32_t width = wuffs_base__pixel_config__width(&cfg.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&cfg.pixcfg);
	if (width > INT_MAX || height > INT_MAX) {
		set_error(error, "image dimensions overflow");
		return NULL;
	}

	// CAIRO_FORMAT_ARGB32: "The 32-bit quantities are stored native-endian.
	//   Pre-multiplied alpha is used."
	// CAIRO_FORMAT_RGB{24,30}: analogous, not going to use these so far.
	//
	// Wuffs: /doc/note/pixel-formats.md specifies it as "memory order", which,
	// for our purposes, means big endian.  Currently useful formats, as per
	// support within wuffs_base__pixel_swizzler__prepare__*():
	//  - WUFFS_BASE__PIXEL_FORMAT__ARGB_PREMUL: big-endian
	//  - WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL: little-endian
	//  - WUFFS_BASE__PIXEL_FORMAT__XRGB: big-endian
	//  - WUFFS_BASE__PIXEL_FORMAT__BGRX: little-endian
	//
	// TODO(p): Make Wuffs support RGB30 as a destination format,
	// so far we only have WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE
	// and in general, 16-bit depth swizzlers are stubbed.
	wuffs_base__pixel_config__set(&cfg.pixcfg,
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
		WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
#else
		WUFFS_BASE__PIXEL_FORMAT__ARGB_PREMUL,
#endif
		WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

	wuffs_base__slice_u8 workbuf = {0};
	uint64_t workbuf_len_max_incl =
		wuffs_base__image_decoder__workbuf_len(dec).max_incl;
	if (workbuf_len_max_incl) {
		workbuf = wuffs_base__malloc_slice_u8(malloc, workbuf_len_max_incl);
		if (!workbuf.ptr) {
			set_error(error, "failed to allocate a work buffer");
			return NULL;
		}
	}

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (!surface) {
		set_error(error, "failed to allocate an image surface");
		free(workbuf.ptr);
		return NULL;
	}

	// CAIRO_STRIDE_ALIGNMENT is 4 bytes, so there will be no padding with
	// ARGB/BGR/XRGB/BGRX.  This function does not support a stride different
	// from the width, maybe Wuffs internals do not either.
	wuffs_base__pixel_buffer pb = {0};
	status = wuffs_base__pixel_buffer__set_from_slice(&pb, &cfg.pixcfg,
		wuffs_base__make_slice_u8(cairo_image_surface_get_data(surface),
			cairo_image_surface_get_stride(surface) *
			cairo_image_surface_get_height(surface)));
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		cairo_surface_destroy(surface);
		free(workbuf.ptr);
		return NULL;
	}

#if 0  // We're not using this right now.
	wuffs_base__frame_config fc = {0};
	status = wuffs_png__decoder__decode_frame_config(&dec, &fc, &src);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		cairo_surface_destroy(surface);
		free(workbuf.ptr);
		return NULL;
	}
#endif

	// Starting to modify pixel data directly. Probably an unnecessary call.
	cairo_surface_flush(surface);

	status = wuffs_base__image_decoder__decode_frame(dec, &pb, &src,
		WUFFS_BASE__PIXEL_BLEND__SRC, workbuf, NULL);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		cairo_surface_destroy(surface);
		free(workbuf.ptr);
		return NULL;
	}

	// Pixel data has been written, need to let Cairo know.
	cairo_surface_mark_dirty(surface);

	free(workbuf.ptr);
	return surface;
}

static cairo_surface_t *
open_wuffs_using(wuffs_base__image_decoder *(*allocate)(),
	const gchar *data, gsize len, GError **error)
{
	wuffs_base__image_decoder *dec = allocate();
	if (!dec) {
		set_error(error, "memory allocation failed or internal error");
		return NULL;
	}

	cairo_surface_t *surface = open_wuffs(dec,
		wuffs_base__ptr_u8__reader((uint8_t *) data, len, TRUE), error);
	free(dec);
	return surface;
}

static void
trivial_cmyk_to_bgra(unsigned char *p, int len)
{
	// Inspired by gdk-pixbuf's io-jpeg.c:
	//
	// Assume that all YCCK/CMYK JPEG files use inverted CMYK, as Photoshop
	// does, see https://bugzilla.gnome.org/show_bug.cgi?id=618096
	while (len--) {
		int c = p[0], m = p[1], y = p[2], k = p[3];
		p[0] = k * y / 255;
		p[1] = k * m / 255;
		p[2] = k * c / 255;
		p[3] = 255;
		p += 4;
	}
}

static cairo_surface_t *
open_libjpeg_turbo(const gchar *data, gsize len, GError **error)
{
	tjhandle dec = tjInitDecompress();
	if (!dec) {
		set_error(error, tjGetErrorStr2(dec));
		return NULL;
	}

	int width = 0, height = 0, subsampling = TJSAMP_444, colorspace = TJCS_RGB;
	if (tjDecompressHeader3(dec, (const unsigned char *) data, len,
			&width, &height, &subsampling, &colorspace)) {
		set_error(error, tjGetErrorStr2(dec));
		tjDestroy(dec);
		return NULL;
	}

	int pixel_format = (colorspace == TJCS_CMYK || colorspace == TJCS_YCCK)
		? TJPF_CMYK
		: (G_BYTE_ORDER == G_LITTLE_ENDIAN ? TJPF_BGRA : TJPF_ARGB);

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (!surface) {
		set_error(error, "failed to allocate an image surface");
		tjDestroy(dec);
		return NULL;
	}

	// Starting to modify pixel data directly. Probably an unnecessary call.
	cairo_surface_flush(surface);

	int stride = cairo_image_surface_get_stride(surface);
	if (tjDecompress2(dec, (const unsigned char *) data, len,
			cairo_image_surface_get_data(surface), width, stride,
			height, pixel_format, TJFLAG_ACCURATEDCT)) {
		set_error(error, tjGetErrorStr2(dec));
		cairo_surface_destroy(surface);
		tjDestroy(dec);
		return NULL;
	}

	// TODO(p): Support big-endian machines, too, those need ARGB ordering.
	if (pixel_format == TJPF_CMYK) {
		// CAIRO_STRIDE_ALIGNMENT is 4 bytes, so there will be no padding with
		// ARGB/BGR/XRGB/BGRX.
		trivial_cmyk_to_bgra(cairo_image_surface_get_data(surface),
			width * height);
	}

	// Pixel data has been written, need to let Cairo know.
	cairo_surface_mark_dirty(surface);

	tjDestroy(dec);
	return surface;
}

// TODO(p): Progressive picture loading, or at least async/cancellable.
gboolean
fastiv_view_open(FastivView *self, const gchar *path, GError **error)
{
	// TODO(p): Don't always load everything into memory, test type first,
	// for which we only need the first 16 bytes right now.
	gchar *data = NULL;
	gsize len = 0;
	if (!g_file_get_contents(path, &data, &len, error))
		return FALSE;

	wuffs_base__slice_u8 prefix =
		wuffs_base__make_slice_u8((uint8_t *) data, len);

	cairo_surface_t *surface = NULL;
	switch (wuffs_base__magic_number_guess_fourcc(prefix)) {
	case WUFFS_BASE__FOURCC__BMP:
		// Note that BMP can redirect into another format, which is unsupported.
		surface = open_wuffs_using(
			wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder,
			data, len, error);
		break;
	case WUFFS_BASE__FOURCC__GIF:
		surface = open_wuffs_using(
			wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder,
			data, len, error);
		break;
	case WUFFS_BASE__FOURCC__PNG:
		surface = open_wuffs_using(
			wuffs_png__decoder__alloc_as__wuffs_base__image_decoder,
			data, len, error);
		break;
	case WUFFS_BASE__FOURCC__JPEG:
		surface = open_libjpeg_turbo(data, len, error);
		break;
	default:
		// TODO(p): Integrate gdk-pixbuf as a fallback (optional dependency).
		set_error(error, "unsupported file type");
	}

	free(data);
	if (!surface)
		return FALSE;

	if (self->surface)
		cairo_surface_destroy(self->surface);

	self->surface = surface;
	gtk_widget_queue_resize(GTK_WIDGET(self));
	return TRUE;
}

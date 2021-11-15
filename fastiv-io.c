//
// fastiv-io.c: image loaders
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

#include <cairo.h>
#include <glib.h>
#include <turbojpeg.h>
#ifdef HAVE_LIBRAW
#include <libraw.h>
#endif  // HAVE_LIBRAW
#ifdef HAVE_LIBRSVG
#include <librsvg/rsvg.h>
#endif  // HAVE_LIBRSVG
#ifdef HAVE_GDKPIXBUF
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif  // HAVE_GDKPIXBUF

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

#include <glib/gstdio.h>
#include <spng.h>

#include "xdg.h"
#include "fastiv-io.h"

#if CAIRO_VERSION >= 11702 && X11_ACTUALLY_SUPPORTS_RGBA128F_OR_WE_USE_OPENGL
#define FASTIV_CAIRO_RGBA128F
#endif

// A subset of shared-mime-info that produces an appropriate list of
// file extensions. Chiefly motivated by the suckiness of RAW images:
// someone else will maintain the list of file extensions for us.
const char *fastiv_io_supported_media_types[] = {
	"image/bmp",
	"image/gif",
	"image/png",
	"image/jpeg",
#ifdef HAVE_LIBRAW
	"image/x-dcraw",
#endif  // HAVE_LIBRAW
#ifdef HAVE_LIBRSVG
	"image/svg+xml",
#endif  // HAVE_LIBRSVG
	NULL
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define FASTIV_IO_ERROR fastiv_io_error_quark()

G_DEFINE_QUARK(fastiv-io-error-quark, fastiv_io_error)

enum FastivIoError {
	FASTIV_IO_ERROR_OPEN,
};

static void
set_error(GError **error, const char *message)
{
	g_set_error_literal(error, FASTIV_IO_ERROR, FASTIV_IO_ERROR_OPEN, message);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// https://github.com/google/wuffs/blob/main/example/gifplayer/gifplayer.c
// is pure C, and a good reference. I can't use the auxiliary libraries,
// since they depend on C++, which is undesirable.
static cairo_surface_t *
open_wuffs(
	wuffs_base__image_decoder *dec, wuffs_base__io_buffer src, GError **error)
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

	// Wuffs maps tRNS to BGRA in `decoder.decode_trns?`, we should be fine.
	// wuffs_base__pixel_format__transparency() doesn't reflect the image file.
	bool opaque = wuffs_base__image_config__first_frame_is_opaque(&cfg);

	// Wuffs' API is kind of awful--we want to catch deep RGB and deep grey.
	wuffs_base__pixel_format srcfmt =
		wuffs_base__pixel_config__pixel_format(&cfg.pixcfg);
	uint32_t bpp = wuffs_base__pixel_format__bits_per_pixel(&srcfmt);

	// Cairo doesn't support transparency with RGB30, so no premultiplication.
	bool pack_16_10 = opaque && (bpp > 24 || (bpp < 24 && bpp > 8));
#ifdef FASTIV_CAIRO_RGBA128F
	bool expand_16_float = !opaque && (bpp > 24 || (bpp < 24 && bpp > 8));
#endif  // FASTIV_CAIRO_RGBA128F

	// In Wuffs, /doc/note/pixel-formats.md declares "memory order", which,
	// for our purposes, means big endian, and BGRA results in 32-bit ARGB
	// on most machines.
	//
	// XXX: WUFFS_BASE__PIXEL_FORMAT__ARGB_PREMUL is not expressible, only RGBA.
	// Wuffs doesn't support big-endian architectures at all, we might want to
	// fall back to spng in such cases, or do a second conversion.
	uint32_t wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL;

	// CAIRO_FORMAT_ARGB32: "The 32-bit quantities are stored native-endian.
	// Pre-multiplied alpha is used." CAIRO_FORMAT_RGB{24,30} are analogous.
	cairo_format_t cairo_format = CAIRO_FORMAT_ARGB32;

#ifdef FASTIV_CAIRO_RGBA128F
	if (expand_16_float) {
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE;
		cairo_format = CAIRO_FORMAT_RGBA128F;
	} else
#endif  // FASTIV_CAIRO_RGBA128F
	if (pack_16_10) {
		// TODO(p): Make Wuffs support RGB30 as a destination format;
		// in general, 16-bit depth swizzlers are stubbed.
		// See also wuffs_base__pixel_swizzler__prepare__*().
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE;
		cairo_format = CAIRO_FORMAT_RGB30;
	} else if (opaque) {
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRX;
		cairo_format = CAIRO_FORMAT_RGB24;
	}

	wuffs_base__pixel_config__set(&cfg.pixcfg, wuffs_format,
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

	unsigned char *targetbuf = NULL;
	cairo_surface_t *result = NULL, *surface =
		cairo_image_surface_create(cairo_format, width, height);
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		goto fail;
	}

	// CAIRO_STRIDE_ALIGNMENT is 4 bytes, so there will be no padding with
	// ARGB/BGR/XRGB/BGRX.  This function does not support a stride different
	// from the width, maybe Wuffs internals do not either.
	unsigned char *surface_data = cairo_image_surface_get_data(surface);
	int surface_stride = cairo_image_surface_get_stride(surface);
	wuffs_base__pixel_buffer pb = {0};
#ifdef FASTIV_CAIRO_RGBA128F
	if (expand_16_float) {
		uint32_t targetbuf_size = height * width * 64;
		targetbuf = g_malloc(targetbuf_size);
		status = wuffs_base__pixel_buffer__set_from_slice(&pb, &cfg.pixcfg,
			wuffs_base__make_slice_u8(targetbuf, targetbuf_size));
	} else
#endif  // FASTIV_CAIRO_RGBA128F
	if (pack_16_10) {
		uint32_t targetbuf_size = height * width * 16;
		targetbuf = g_malloc(targetbuf_size);
		status = wuffs_base__pixel_buffer__set_from_slice(&pb, &cfg.pixcfg,
			wuffs_base__make_slice_u8(targetbuf, targetbuf_size));
	} else {
		status = wuffs_base__pixel_buffer__set_from_slice(&pb, &cfg.pixcfg,
			wuffs_base__make_slice_u8(surface_data,
				surface_stride * cairo_image_surface_get_height(surface)));
	}
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		goto fail;
	}

#if 0  // We're not using this right now.
	wuffs_base__frame_config fc = {0};
	status = wuffs_png__decoder__decode_frame_config(&dec, &fc, &src);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		goto fail;
	}
#endif

	// Starting to modify pixel data directly. Probably an unnecessary call.
	cairo_surface_flush(surface);

	status = wuffs_base__image_decoder__decode_frame(
		dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, workbuf, NULL);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		goto fail;
	}

#ifdef FASTIV_CAIRO_RGBA128F
	if (expand_16_float) {
		g_debug("Wuffs to Cairo RGBA128F");
		uint16_t *in = (uint16_t *) targetbuf;
		float *out = (float *) surface_data;
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				float b = *in++ / 65535., g = *in++ / 65535.,
					r = *in++ / 65535., a = *in++ / 65535.;
				*out++ = r * a;
				*out++ = g * a;
				*out++ = b * a;
				*out++ = a;
			}
		}
	} else
#endif  // FASTIV_CAIRO_RGBA128F
	if (pack_16_10) {
		g_debug("Wuffs to Cairo RGB30");
		uint16_t *in = (uint16_t *) targetbuf;
		uint32_t *out = (uint32_t *) surface_data;
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				uint16_t b = *in++, g = *in++, r = *in++, x = *in++;
				*out++ = (x >> 14) << 30 |
					(r >> 6) << 20 | (g >> 6) << 10 | (b >> 6);
			}
		}
	}

	// Pixel data has been written, need to let Cairo know.
	cairo_surface_mark_dirty((result = surface));

fail:
	if (!result)
		cairo_surface_destroy(surface);

	g_free(targetbuf);
	free(workbuf.ptr);
	return result;
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

	cairo_surface_t *surface = open_wuffs(
		dec, wuffs_base__ptr_u8__reader((uint8_t *) data, len, TRUE), error);
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
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
		tjDestroy(dec);
		return NULL;
	}

	// Starting to modify pixel data directly. Probably an unnecessary call.
	cairo_surface_flush(surface);

	int stride = cairo_image_surface_get_stride(surface);
	if (tjDecompress2(dec, (const unsigned char *) data, len,
			cairo_image_surface_get_data(surface), width, stride, height,
			pixel_format, TJFLAG_ACCURATEDCT)) {
		set_error(error, tjGetErrorStr2(dec));
		cairo_surface_destroy(surface);
		tjDestroy(dec);
		return NULL;
	}

	// TODO(p): Support big-endian machines, too, those need ARGB ordering.
	if (pixel_format == TJPF_CMYK) {
		// CAIRO_STRIDE_ALIGNMENT is 4 bytes, so there will be no padding with
		// ARGB/BGR/XRGB/BGRX.
		trivial_cmyk_to_bgra(
			cairo_image_surface_get_data(surface), width * height);
	}

	// Pixel data has been written, need to let Cairo know.
	cairo_surface_mark_dirty(surface);

	tjDestroy(dec);
	return surface;
}

#ifdef HAVE_LIBRAW  // ---------------------------------------------------------

static cairo_surface_t *
open_libraw(const gchar *data, gsize len, GError **error)
{
	// https://github.com/LibRaw/LibRaw/issues/418
	libraw_data_t *iprc = libraw_init(
		LIBRAW_OPIONS_NO_MEMERR_CALLBACK | LIBRAW_OPIONS_NO_DATAERR_CALLBACK);
	if (!iprc) {
		set_error(error, "failed to obtain a LibRaw handle");
		return NULL;
	}

#if 0
	// TODO(p): Consider setting this--the image is still likely to be
	// rendered suboptimally, so why not make it faster.
	iprc->params.half_size = 1;
#endif

	// TODO(p): Check if we need to set anything for autorotation (sizes.flip).
	iprc->params.use_camera_wb = 1;
	iprc->params.output_color = 1;  // sRGB, TODO(p): Is this used?
	iprc->params.output_bps = 8;    // This should be the default value.

	int err = 0;
	if ((err = libraw_open_buffer(iprc, (void *) data, len))) {
		set_error(error, libraw_strerror(err));
		libraw_close(iprc);
		return NULL;
	}

	// TODO(p): Do we need to check iprc->idata.raw_count? Maybe for TIFFs?
	if ((err = libraw_unpack(iprc))) {
		set_error(error, libraw_strerror(err));
		libraw_close(iprc);
		return NULL;
	}

#if 0
	// TODO(p): I'm not sure when this is necessary or useful yet.
	if ((err = libraw_adjust_sizes_info_only(iprc))) {
		set_error(error, libraw_strerror(err));
		libraw_close(iprc);
		return NULL;
	}
#endif

	// TODO(p): Documentation says I should look at the code and do it myself.
	if ((err = libraw_dcraw_process(iprc))) {
		set_error(error, libraw_strerror(err));
		libraw_close(iprc);
		return NULL;
	}

	// FIXME: This is shittily written to iterate over the range of
	// idata.colors, and will be naturally slow.
	libraw_processed_image_t *image = libraw_dcraw_make_mem_image(iprc, &err);
	if (!image) {
		set_error(error, libraw_strerror(err));
		libraw_close(iprc);
		return NULL;
	}

	// This should have been transformed, and kept, respectively.
	if (image->colors != 3 || image->bits != 8) {
		set_error(error, "unexpected number of colours, or bit depth");
		libraw_dcraw_clear_mem(image);
		libraw_close(iprc);
		return NULL;
	}

	int width = image->width, height = image->height;
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
		libraw_dcraw_clear_mem(image);
		libraw_close(iprc);
		return NULL;
	}

	// Starting to modify pixel data directly. Probably an unnecessary call.
	cairo_surface_flush(surface);

	uint32_t *pixels = (uint32_t *) cairo_image_surface_get_data(surface);
	unsigned char *p = image->data;
	for (ushort y = 0; y < image->height; y++) {
		for (ushort x = 0; x < image->width; x++) {
			*pixels++ = 0xff000000 | (uint32_t) p[0] << 16 |
				(uint32_t) p[1] << 8 | (uint32_t) p[2];
			p += 3;
		}
	}

	// Pixel data has been written, need to let Cairo know.
	cairo_surface_mark_dirty(surface);

	libraw_dcraw_clear_mem(image);
	libraw_close(iprc);
	return surface;
}

#endif  // HAVE_LIBRAW ---------------------------------------------------------
#ifdef HAVE_LIBRSVG  // --------------------------------------------------------

#ifdef FASTIV_RSVG_DEBUG
#include <cairo/cairo-script.h>
#include <cairo/cairo-svg.h>
#endif

// FIXME: librsvg rasterizes filters, so this method isn't fully appropriate.
static cairo_surface_t *
open_librsvg(const gchar *data, gsize len, const gchar *path, GError **error)
{
	GFile *base_file = g_file_new_for_path(path);
	GInputStream *is = g_memory_input_stream_new_from_data(data, len, NULL);
	RsvgHandle *handle = rsvg_handle_new_from_stream_sync(is, base_file,
		RSVG_HANDLE_FLAG_KEEP_IMAGE_DATA, NULL, error);
	g_object_unref(base_file);
	g_object_unref(is);
	if (!handle)
		return NULL;

	// TODO(p): Acquire this from somewhere else.
	rsvg_handle_set_dpi(handle, 96);

	double w = 0, h = 0;
	if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &w, &h)) {
		RsvgRectangle viewbox = {};
		gboolean has_viewport = FALSE;
		rsvg_handle_get_intrinsic_dimensions(handle, NULL, NULL, NULL, NULL,
			&has_viewport, &viewbox);
		if (!has_viewport) {
			set_error(error, "cannot compute pixel dimensions");
			g_object_unref(handle);
			return NULL;
		}

		w = viewbox.width;
		h = viewbox.height;
	}

	cairo_rectangle_t extents = {
		.x = 0, .y = 0, .width = ceil(w), .height = ceil(h)};
	cairo_surface_t *surface =
		cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, &extents);

#ifdef FASTIV_RSVG_DEBUG
	cairo_device_t *script = cairo_script_create("cairo.script");
	cairo_surface_t *tee =
		cairo_script_surface_create_for_target(script, surface);
	cairo_t *cr = cairo_create(tee);
	cairo_device_destroy(script);
	cairo_surface_destroy(tee);
#else
	cairo_t *cr = cairo_create(surface);
#endif

	RsvgRectangle viewport = {.x = 0, .y = 0, .width = w, .height = h};
	if (!rsvg_handle_render_document(handle, cr, &viewport, error)) {
		cairo_surface_destroy(surface);
		cairo_destroy(cr);
		g_object_unref(handle);
		return NULL;
	}

	cairo_destroy(cr);
	g_object_unref(handle);

#ifdef FASTIV_RSVG_DEBUG
	cairo_surface_t *svg = cairo_svg_surface_create("cairo.svg", w, h);
	cr = cairo_create(svg);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(svg);

	cairo_surface_t *png =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * 10, h * 10);
	cr = cairo_create(png);
	cairo_scale(cr, 10, 10);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_write_to_png(png, "cairo.png");
	cairo_surface_destroy(png);
#endif
	return surface;
}

#endif  // HAVE_LIBRSVG --------------------------------------------------------
#ifdef HAVE_GDKPIXBUF  // ------------------------------------------------------

static cairo_surface_t *
open_gdkpixbuf(const gchar *data, gsize len, GError **error)
{
	GInputStream *is = g_memory_input_stream_new_from_data(data, len, NULL);
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(is, NULL, error);
	g_object_unref(is);
	if (!pixbuf)
		return NULL;

	cairo_surface_t *surface =
		gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, NULL);
	g_object_unref(pixbuf);
	return surface;
}

#endif  // HAVE_GDKPIXBUF ------------------------------------------------------

cairo_surface_t *
fastiv_io_open(const gchar *path, GError **error)
{
	// TODO(p): Don't always load everything into memory, test type first,
	// so that we can reject non-pictures early.  Wuffs only needs the first
	// 16 bytes to make a guess right now.
	//
	// LibRaw poses an issue--there is no good registry for identification
	// of supported files.  Many of them are compliant TIFF files.
	// The only good filtering method for RAWs are currently file extensions
	// extracted from shared-mime-info.
	//
	// SVG is also problematic, an unbounded search for its root element.
	// But problematic files can be assumed to be crafted.
	//
	// gdk-pixbuf exposes its detection data through gdk_pixbuf_get_formats().
	// This may also be unbounded, as per format_check().
	gchar *data = NULL;
	gsize len = 0;
	if (!g_file_get_contents(path, &data, &len, error))
		return NULL;

	cairo_surface_t *surface = fastiv_io_open_from_data(data, len, path, error);
	free(data);
	return surface;
}

cairo_surface_t *
fastiv_io_open_from_data(const char *data, size_t len, const gchar *path,
	GError **error)
{
	wuffs_base__slice_u8 prefix =
		wuffs_base__make_slice_u8((uint8_t *) data, len);

	cairo_surface_t *surface = NULL;
	switch (wuffs_base__magic_number_guess_fourcc(prefix)) {
	case WUFFS_BASE__FOURCC__BMP:
		// Note that BMP can redirect into another format,
		// which is so far unsupported here.
		surface = open_wuffs_using(
			wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			error);
		break;
	case WUFFS_BASE__FOURCC__GIF:
		surface = open_wuffs_using(
			wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			error);
		break;
	case WUFFS_BASE__FOURCC__PNG:
		surface = open_wuffs_using(
			wuffs_png__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			error);
		break;
	case WUFFS_BASE__FOURCC__JPEG:
		surface = open_libjpeg_turbo(data, len, error);
		break;
	default:
#ifdef HAVE_LIBRAW  // ---------------------------------------------------------
		if ((surface = open_libraw(data, len, error)))
			break;

		// TODO(p): We should try to pass actual processing errors through,
		// notably only continue with LIBRAW_FILE_UNSUPPORTED.
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_LIBRAW ---------------------------------------------------------
#ifdef HAVE_LIBRSVG  // --------------------------------------------------------
		if ((surface = open_librsvg(data, len, path, error)))
			break;

		// XXX: It doesn't look like librsvg can return sensible errors.
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_LIBRSVG --------------------------------------------------------
#ifdef HAVE_GDKPIXBUF  // ------------------------------------------------------
		// This is only used as a last resort, the rest above is special-cased.
		if ((surface = open_gdkpixbuf(data, len, error)) ||
			(error && (*error)->code != GDK_PIXBUF_ERROR_UNKNOWN_TYPE))
			break;

		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_GDKPIXBUF ------------------------------------------------------

		set_error(error, "unsupported file type");
	}
	return surface;
}

// --- Thumbnails --------------------------------------------------------------

static int  // tri-state
check_spng_thumbnail_texts(struct spng_text *texts, uint32_t texts_len,
	const gchar *target, time_t mtime)
{
	// May contain Thumb::Image::Width Thumb::Image::Height,
	// but those aren't interesting currently (would be for fast previews).
	bool need_uri = true, need_mtime = true;
	for (uint32_t i = 0; i < texts_len; i++) {
		struct spng_text *text = texts + i;
		if (!strcmp(text->keyword, "Thumb::URI")) {
			need_uri = false;
			if (strcmp(target, text->text))
				return false;
		}
		if (!strcmp(text->keyword, "Thumb::MTime")) {
			need_mtime = false;
			if (atol(text->text) != mtime)
				return false;
		}
	}
	return need_uri || need_mtime ? -1 : true;
}

static int  // tri-state
check_spng_thumbnail(spng_ctx *ctx, const gchar *target, time_t mtime, int *err)
{
	uint32_t texts_len = 0;
	if ((*err = spng_get_text(ctx, NULL, &texts_len)))
		return false;

	int result = false;
	struct spng_text *texts = g_malloc0_n(texts_len, sizeof *texts);
	if (!(*err = spng_get_text(ctx, texts, &texts_len)))
		result = check_spng_thumbnail_texts(texts, texts_len, target, mtime);
	g_free(texts);
	return result;
}

static cairo_surface_t *
read_spng_thumbnail(
	const gchar *path, const gchar *uri, time_t mtime, GError **error)
{
	FILE *fp;
	cairo_surface_t *result = NULL;
	if (!(fp = fopen(path, "rb"))) {
		set_error(error, g_strerror(errno));
		return NULL;
	}

	errno = 0;
	spng_ctx *ctx = spng_ctx_new(0);
	if (!ctx) {
		set_error(error, g_strerror(errno));
		goto fail_init;
	}

	int err;
	size_t size = 0;
	if ((err = spng_set_png_file(ctx, fp)) ||
		(err = spng_set_image_limits(ctx, INT16_MAX, INT16_MAX)) ||
		(err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &size))) {
		set_error(error, spng_strerror(err));
		goto fail;
	}
	if (check_spng_thumbnail(ctx, uri, mtime, &err) == false) {
		set_error(error, err ? spng_strerror(err) : "mismatch");
		goto fail;
	}

	struct spng_ihdr ihdr = {};
	spng_get_ihdr(ctx, &ihdr);
	cairo_surface_t *surface = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, ihdr.width, ihdr.height);

	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		goto fail_data;
	}

	uint32_t *data = (uint32_t *) cairo_image_surface_get_data(surface);
	g_assert((size_t) cairo_image_surface_get_stride(surface) *
		cairo_image_surface_get_height(surface) == size);

	cairo_surface_flush(surface);
	if ((err = spng_decode_image(ctx, data, size, SPNG_FMT_RGBA8,
		SPNG_DECODE_TRNS | SPNG_DECODE_GAMMA))) {
		set_error(error, spng_strerror(err));
		goto fail_data;
	}

	// The specification does not say where the required metadata should be,
	// it could very well be broken up into two parts.
	if (check_spng_thumbnail(ctx, uri, mtime, &err) != true) {
		set_error(
			error, err ? spng_strerror(err) : "mismatch or not a thumbnail");
		goto fail_data;
	}

	// pixman can be mildly abused to do this operation, but it won't be faster.
	struct spng_trns trns = {};
	if (ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA ||
		ihdr.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA ||
		!spng_get_trns(ctx, &trns)) {
		for (size_t i = size / sizeof *data; i--; ) {
			const uint8_t *unit = (const uint8_t *) &data[i],
				a = unit[3],
				b = unit[2] * a / 255,
				g = unit[1] * a / 255,
				r = unit[0] * a / 255;
			data[i] = a << 24 | r << 16 | g << 8 | b;
		}
	} else {
		for (size_t i = size / sizeof *data; i--; ) {
			uint32_t rgba = g_ntohl(data[i]);
			data[i] = rgba << 24 | rgba >> 8;
		}
	}

	cairo_surface_mark_dirty((result = surface));

fail_data:
	if (!result)
		cairo_surface_destroy(surface);
fail:
	spng_ctx_free(ctx);
fail_init:
	fclose(fp);
	return result;
}

cairo_surface_t *
fastiv_io_lookup_thumbnail(const gchar *target)
{
	GStatBuf st;
	if (g_stat(target, &st))
		return NULL;

	// TODO(p): Consider making the `target` an absolute path, if it isn't.
	// Or maybe let it fail, and document the requirement.
	gchar *uri = g_filename_to_uri(target, NULL, NULL);
	if (!uri)
		return NULL;

	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	gchar *cache_dir = get_xdg_home_dir("XDG_CACHE_HOME", ".cache");

	cairo_surface_t *result = NULL;
	const gchar *sizes[] = {"large", "x-large", "xx-large", "normal"};
	GError *error = NULL;
	for (gsize i = 0; !result && i < G_N_ELEMENTS(sizes); i++) {
		gchar *path = g_strdup_printf(
			"%s/thumbnails/%s/%s.png", cache_dir, sizes[i], sum);
		result = read_spng_thumbnail(path, uri, st.st_mtim.tv_sec, &error);
		if (error) {
			g_debug("%s: %s", path, error->message);
			g_clear_error(&error);
		}
		g_free(path);
	}

	g_free(cache_dir);
	g_free(sum);
	g_free(uri);
	return result;
}

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

#include "xdg.h"
#include "fastiv-io.h"

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
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
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

	status = wuffs_base__image_decoder__decode_frame(
		dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, workbuf, NULL);
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
		set_error(error, "cannot compute pixel dimensions");
		g_object_unref(handle);
		return NULL;
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

cairo_surface_t *
fastiv_io_open(const gchar *path, GError **error)
{
	// TODO(p): Don't always load everything into memory, test type first,
	// for which we only need the first 16 bytes right now.
	// Though LibRaw poses an issue--we may want to try to map RAW formats
	// to FourCC values--many of them are compliant TIFF files.
	// We might want to employ a more generic way of magic identification,
	// and with some luck, it could even be integrated into Wuffs.
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
		g_clear_error(error);
#endif  // HAVE_LIBRAW ---------------------------------------------------------
#ifdef HAVE_LIBRSVG  // --------------------------------------------------------
		if ((surface = open_librsvg(data, len, path, error)))
			break;

		// XXX: It doesn't look like librsvg can return sensible errors.
		g_clear_error(error);
#endif  // HAVE_LIBRSVG --------------------------------------------------------

		// TODO(p): Integrate gdk-pixbuf as a fallback (optional dependency).
		set_error(error, "unsupported file type");
	}
	return surface;
}

// --- Thumbnails --------------------------------------------------------------

// NOTE: "It is important to note that when an image with an alpha channel is
// scaled, linear encoded, pre-multiplied component values must be used!"
//
// We can use the pixman library to scale, PIXMAN_a8r8g8b8_sRGB.
#include <glib/gstdio.h>
#include <png.h>

static void
redirect_png_error(png_structp pngp, const char *error)
{
	set_error(png_get_error_ptr(pngp), error);
	png_longjmp(pngp, 1);
}

static void
discard_png_warning(png_structp pngp, const char *warning)
{
	(void) pngp;
	(void) warning;
}

static int
check_png_thumbnail(
	png_structp pngp, png_infop infop, const gchar *target, time_t mtime)
{
	// May contain Thumb::Image::Width Thumb::Image::Height,
	// but those aren't interesting currently (would be for fast previews).
	int texts_len = 0;
	png_textp texts = NULL;
	png_get_text(pngp, infop, &texts, &texts_len);

	gboolean need_uri = TRUE, need_mtime = TRUE;
	for (int i = 0; i < texts_len; i++) {
		png_textp text = texts + i;
		if (!strcmp(text->key, "Thumb::URI")) {
			need_uri = FALSE;
			if (strcmp(target, text->text))
				return FALSE;
		}
		if (!strcmp(text->key, "Thumb::MTime")) {
			need_mtime = FALSE;
			if (atol(text->text) != mtime)
				return FALSE;
		}
	}
	return need_uri || need_mtime ? -1 : TRUE;
}

// TODO(p): Support spng as well (it can't premultiply alpha by itself,
// but at least it won't gamma-adjust it for us).
static cairo_surface_t *
read_png_thumbnail(
	const gchar *path, const gchar *uri, time_t mtime, GError **error)
{
	FILE *fp;
	if (!(fp = fopen(path, "rb"))) {
		set_error(error, g_strerror(errno));
		return NULL;
	}

	cairo_surface_t *volatile surface = NULL;
	png_structp pngp = png_create_read_struct(
		PNG_LIBPNG_VER_STRING, error, redirect_png_error, discard_png_warning);
	png_infop infop = png_create_info_struct(pngp);
	if (!infop) {
		set_error(error, g_strerror(errno));
		goto fail_preread;
	}

	volatile png_bytepp row_pointers = NULL;
	if (setjmp(png_jmpbuf(pngp))) {
		if (surface) {
			cairo_surface_destroy(surface);
			surface = NULL;
		}
		goto fail;
	}

	png_init_io(pngp, fp);

	// XXX: libpng will premultiply with the alpha, but it also gamma-adjust it.
	png_set_alpha_mode(pngp, PNG_ALPHA_BROKEN, PNG_DEFAULT_sRGB);
	png_read_info(pngp, infop);
	if (check_png_thumbnail(pngp, infop, uri, mtime) == FALSE)
		png_error(pngp, "mismatch");

	// Asking for at least 8-bit channels. This call is a superset of:
	//  - png_set_palette_to_rgb(),
	//  - png_set_tRNS_to_alpha(),
	//  - png_set_expand_gray_1_2_4_to_8().
	png_set_expand(pngp);

	// Reduce the possibilities further to RGB or RGBA...
	png_set_gray_to_rgb(pngp);

	// ...and /exactly/ 8-bit channels.
	// Alternatively, use png_set_expand_16() above to obtain 16-bit channels.
	png_set_scale_16(pngp);

	// PNG uses RGBA order, we want either ARGB (BE) or BGRA (LE).
	if (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
		png_set_bgr(pngp);
		png_set_add_alpha(pngp, 0xFFFF, PNG_FILLER_AFTER);
		png_set_swap(pngp);
	} else {
		// This doesn't change a row's `color_type` in png_do_read_filler(),
		// and the following transformation thus ignores it.
		png_set_add_alpha(pngp, 0xFFFF, PNG_FILLER_BEFORE);
		png_set_swap_alpha(pngp);
	}

	(void) png_set_interlace_handling(pngp);
	png_read_update_info(pngp, infop);

	png_uint_32 w = png_get_image_width(pngp, infop);
	png_uint_32 h = png_get_image_height(pngp, infop);
	if (w > INT16_MAX || h > INT16_MAX)
		png_error(pngp, "the image is too large");

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS)
		png_error(pngp, cairo_status_to_string(surface_status));

	size_t row_bytes = png_get_rowbytes(pngp, infop);
	g_assert((size_t) cairo_image_surface_get_stride(surface) == row_bytes);

	unsigned char *buffer = cairo_image_surface_get_data(surface);
	png_uint_32 height = png_get_image_height(pngp, infop);
	if (!(row_pointers = calloc(height, sizeof *row_pointers)))
		png_error(pngp, g_strerror(errno));
	for (size_t y = 0; y < height; y++)
		row_pointers[y] = buffer + y * row_bytes;

	cairo_surface_flush(surface);
	png_read_image(pngp, row_pointers);
	cairo_surface_mark_dirty(surface);

	// The specification does not say where the required metadata should be,
	// it could very well be broken up into two parts.
	png_read_end(pngp, infop);
	if (check_png_thumbnail(pngp, infop, uri, mtime) != TRUE)
		png_error(pngp, "mismatch or not a thumbnail");

fail:
	free(row_pointers);
fail_preread:
	png_destroy_read_struct(&pngp, &infop, NULL);
	fclose(fp);
	return surface;
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
		result = read_png_thumbnail(path, uri, st.st_mtim.tv_sec, &error);
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

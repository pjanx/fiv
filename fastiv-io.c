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
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <spng.h>
#include <turbojpeg.h>
#ifdef HAVE_LIBRAW
#include <libraw.h>
#endif  // HAVE_LIBRAW
#ifdef HAVE_LIBRSVG
#include <librsvg/rsvg.h>
#endif  // HAVE_LIBRSVG
#ifdef HAVE_XCURSOR
#include <X11/Xcursor/Xcursor.h>
#endif  // HAVE_XCURSOR
#ifdef HAVE_LIBTIFF
#include <tiff.h>
#include <tiffio.h>
#endif  // HAVE_LIBTIFF
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

#include "xdg.h"
#include "fastiv-io.h"

#if CAIRO_VERSION >= 11702 && X11_ACTUALLY_SUPPORTS_RGBA128F_OR_WE_USE_OPENGL
#define FASTIV_CAIRO_RGBA128F
#endif

// A subset of shared-mime-info that produces an appropriate list of
// file extensions. Chiefly motivated by the suckiness of raw photo formats:
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
#ifdef HAVE_XCURSOR
	"image/x-xcursor",
#endif  // HAVE_XCURSOR
#ifdef HAVE_LIBTIFF
	"image/tiff",
#endif  // HAVE_LIBTIFF
	NULL
};

char **
fastiv_io_all_supported_media_types(void)
{
	GPtrArray *types = g_ptr_array_new();
	for (const char **p = fastiv_io_supported_media_types; *p; p++)
		g_ptr_array_add(types, g_strdup(*p));

#ifdef HAVE_GDKPIXBUF
	GSList *formats = gdk_pixbuf_get_formats();
	for (GSList *iter = formats; iter; iter = iter->next) {
		gchar **subtypes = gdk_pixbuf_format_get_mime_types(iter->data);
		for (gchar **p = subtypes; *p; p++)
			g_ptr_array_add(types, *p);
		g_free(subtypes);
	}
	g_slist_free(formats);
#endif  // HAVE_GDKPIXBUF

	g_ptr_array_add(types, NULL);
	return (char **) g_ptr_array_free(types, FALSE);
}

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

static bool
pull_passthrough(const wuffs_base__more_information *minfo,
	wuffs_base__io_buffer *src, wuffs_base__io_buffer *dst, GError **error)
{
	wuffs_base__range_ie_u64 r =
		wuffs_base__more_information__metadata_raw_passthrough__range(minfo);
	if (wuffs_base__range_ie_u64__is_empty(&r))
		return true;

	// This should currently be zero, because we read files all at once.
	uint64_t pos = src->meta.pos;
	if (pos > r.min_incl ||
		wuffs_base__u64__sat_sub(r.max_excl, pos) > src->meta.wi) {
		set_error(error, "metadata is outside the read buffer");
		return false;
	}

	// Mimic WUFFS_BASE__MORE_INFORMATION__FLAVOR__METADATA_RAW_TRANSFORM.
	*dst = wuffs_base__make_io_buffer(src->data,
		wuffs_base__make_io_buffer_meta(
			wuffs_base__u64__sat_sub(r.max_excl, pos),
			wuffs_base__u64__sat_sub(r.min_incl, pos), pos, TRUE));

	// Seeking to the end of it seems to be a requirement in decode_gif.wuffs.
	// Just not in case the block was empty. :^)
	src->meta.ri = dst->meta.wi;
	return true;
}

static GBytes *
pull_metadata(wuffs_base__image_decoder *dec, wuffs_base__io_buffer *src,
	wuffs_base__more_information *minfo, GError **error)
{
	uint8_t buf[8192] = {};
	GByteArray *array = g_byte_array_new();
	while (true) {
		*minfo = wuffs_base__empty_more_information();
		wuffs_base__io_buffer dst = wuffs_base__ptr_u8__writer(buf, sizeof buf);
		wuffs_base__status status =
			wuffs_base__image_decoder__tell_me_more(dec, &dst, minfo, src);
		switch (minfo->flavor) {
		case 0:
			// Most likely as a result of an error, we'll handle that below.
		case WUFFS_BASE__MORE_INFORMATION__FLAVOR__METADATA_RAW_TRANSFORM:
			// Wuffs is reading it into the buffer.
		case WUFFS_BASE__MORE_INFORMATION__FLAVOR__METADATA_PARSED:
			// Use Wuffs accessor functions in the caller.
			break;
		default:
			set_error(error, "Wuffs metadata API incompatibility");
			goto fail;

		case WUFFS_BASE__MORE_INFORMATION__FLAVOR__METADATA_RAW_PASSTHROUGH:
			// The insane case: error checking really should come first,
			// and it can say "even more information". See decode_gif.wuffs.
			if (!pull_passthrough(minfo, src, &dst, error))
				goto fail;
		}

		g_byte_array_append(array,
			wuffs_base__io_buffer__reader_pointer(&dst),
			wuffs_base__io_buffer__reader_length(&dst));
		if (wuffs_base__status__is_ok(&status))
			return g_byte_array_free_to_bytes(array);

		if (status.repr != wuffs_base__suspension__even_more_information &&
			status.repr != wuffs_base__suspension__short_write) {
			set_error(error, wuffs_base__status__message(&status));
			goto fail;
		}
	}

fail:
	g_byte_array_unref(array);
	return NULL;
}

struct load_wuffs_frame_context {
	wuffs_base__image_decoder *dec;     ///< Wuffs decoder abstraction
	wuffs_base__io_buffer *src;         ///< Wuffs source buffer
	wuffs_base__image_config cfg;       ///< Wuffs image configuration
	wuffs_base__slice_u8 workbuf;       ///< Work buffer for Wuffs
	wuffs_base__frame_config last_fc;   ///< Previous frame configuration
	uint32_t width;                     ///< Copied from cfg.pixcfg
	uint32_t height;                    ///< Copied from cfg.pixcfg
	cairo_format_t cairo_format;        ///< Target format for surfaces
	bool pack_16_10;                    ///< Custom copying swizzle for RGB30
	bool expand_16_float;               ///< Custom copying swizzle for RGBA128F
	GBytes *meta_exif;                  ///< Reference-counted Exif
	GBytes *meta_iccp;                  ///< Reference-counted ICC profile

	cairo_surface_t *result;            ///< The resulting surface (referenced)
	cairo_surface_t *result_tail;       ///< The final animation frame
};

static bool
load_wuffs_frame(struct load_wuffs_frame_context *ctx, GError **error)
{
	wuffs_base__frame_config fc = {};
	wuffs_base__status status =
		wuffs_base__image_decoder__decode_frame_config(ctx->dec, &fc, ctx->src);
	if (status.repr == wuffs_base__note__end_of_data && ctx->result)
		return false;
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		return false;
	}

	bool success = false;
	unsigned char *targetbuf = NULL;
	cairo_surface_t *surface =
		cairo_image_surface_create(ctx->cairo_format, ctx->width, ctx->height);
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		goto fail;
	}

	// CAIRO_STRIDE_ALIGNMENT is 4 bytes, so there will be no padding with
	// ARGB/BGR/XRGB/BGRX. This function does not support a stride different
	// from the width, maybe Wuffs internals do not either.
	unsigned char *surface_data = cairo_image_surface_get_data(surface);
	int surface_stride = cairo_image_surface_get_stride(surface);
	wuffs_base__pixel_buffer pb = {0};
	if (ctx->expand_16_float) {
		uint32_t targetbuf_size = ctx->height * ctx->width * 64;
		targetbuf = g_malloc(targetbuf_size);
		status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ctx->cfg.pixcfg,
			wuffs_base__make_slice_u8(targetbuf, targetbuf_size));
	} else if (ctx->pack_16_10) {
		uint32_t targetbuf_size = ctx->height * ctx->width * 16;
		targetbuf = g_malloc(targetbuf_size);
		status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ctx->cfg.pixcfg,
			wuffs_base__make_slice_u8(targetbuf, targetbuf_size));
	} else {
		status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ctx->cfg.pixcfg,
			wuffs_base__make_slice_u8(surface_data,
				surface_stride * cairo_image_surface_get_height(surface)));
	}
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		goto fail;
	}

	// Starting to modify pixel data directly. Probably an unnecessary call.
	cairo_surface_flush(surface);

	status = wuffs_base__image_decoder__decode_frame(ctx->dec, &pb, ctx->src,
		WUFFS_BASE__PIXEL_BLEND__SRC, ctx->workbuf, NULL);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		goto fail;
	}

	if (ctx->expand_16_float) {
		g_debug("Wuffs to Cairo RGBA128F");
		uint16_t *in = (uint16_t *) targetbuf;
		float *out = (float *) surface_data;
		for (uint32_t y = 0; y < ctx->height; y++) {
			for (uint32_t x = 0; x < ctx->width; x++) {
				float b = *in++ / 65535., g = *in++ / 65535.,
					r = *in++ / 65535., a = *in++ / 65535.;
				*out++ = r * a;
				*out++ = g * a;
				*out++ = b * a;
				*out++ = a;
			}
		}
	} else if (ctx->pack_16_10) {
		g_debug("Wuffs to Cairo RGB30");
		uint16_t *in = (uint16_t *) targetbuf;
		uint32_t *out = (uint32_t *) surface_data;
		for (uint32_t y = 0; y < ctx->height; y++) {
			for (uint32_t x = 0; x < ctx->width; x++) {
				uint32_t b = *in++, g = *in++, r = *in++, X = *in++;
				*out++ = (X >> 14) << 30 |
					(r >> 6) << 20 | (g >> 6) << 10 | (b >> 6);
			}
		}
	}

	// Pixel data has been written, need to let Cairo know.
	cairo_surface_mark_dirty(surface);

	// Single-frame images get a fast path, animations are are handled slowly:
	if (wuffs_base__frame_config__index(&fc) > 0) {
		// Copy the previous frame to a new surface.
		cairo_surface_t *canvas = cairo_image_surface_create(
			ctx->cairo_format, ctx->width, ctx->height);
		int stride = cairo_image_surface_get_stride(canvas);
		int height = cairo_image_surface_get_height(canvas);
		memcpy(cairo_image_surface_get_data(canvas),
			cairo_image_surface_get_data(ctx->result_tail), stride * height);
		cairo_surface_mark_dirty(canvas);

		// Apply that frame's disposal method.
		wuffs_base__rect_ie_u32 bounds =
			wuffs_base__frame_config__bounds(&ctx->last_fc);
		wuffs_base__color_u32_argb_premul bg =
			wuffs_base__frame_config__background_color(&ctx->last_fc);

		double a = (bg >> 24) / 255., r = 0, g = 0, b = 0;
		if (a) {
			r = (uint8_t) (bg >> 16) / 255. / a;
			g = (uint8_t) (bg >> 8)  / 255. / a;
			b = (uint8_t) (bg)       / 255. / a;
		}

		cairo_t *cr = cairo_create(canvas);
		switch (wuffs_base__frame_config__disposal(&ctx->last_fc)) {
		case WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND:
			cairo_rectangle(cr, bounds.min_incl_x, bounds.min_incl_y,
				bounds.max_excl_x - bounds.min_incl_x,
				bounds.max_excl_y - bounds.min_incl_y);
			cairo_set_source_rgba(cr, r, g, b, a);
			cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
			cairo_fill(cr);
			break;
		case WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS:
			// TODO(p): Implement, it seems tricky.
			// Might need another surface to keep track of the state.
			break;
		}

		// Paint the current frame over that, within its bounds.
		bounds = wuffs_base__frame_config__bounds(&fc);
		cairo_rectangle(cr, bounds.min_incl_x, bounds.min_incl_y,
			bounds.max_excl_x - bounds.min_incl_x,
			bounds.max_excl_y - bounds.min_incl_y);
		cairo_clip(cr);

		cairo_set_operator(cr,
			wuffs_base__frame_config__overwrite_instead_of_blend(&fc)
				? CAIRO_OPERATOR_SOURCE
				: CAIRO_OPERATOR_OVER);

		cairo_set_source_surface(cr, surface, 0, 0);
		cairo_paint(cr);
		cairo_destroy(cr);
		cairo_surface_destroy(surface);
		surface = canvas;
	}

	if (ctx->meta_exif)
		cairo_surface_set_user_data(surface, &fastiv_io_key_exif,
			g_bytes_ref(ctx->meta_exif), (cairo_destroy_func_t) g_bytes_unref);
	if (ctx->meta_iccp)
		cairo_surface_set_user_data(surface, &fastiv_io_key_icc,
			g_bytes_ref(ctx->meta_iccp), (cairo_destroy_func_t) g_bytes_unref);

	cairo_surface_set_user_data(surface, &fastiv_io_key_loops,
		(void *) (uintptr_t) wuffs_base__image_decoder__num_animation_loops(
			ctx->dec), NULL);
	cairo_surface_set_user_data(surface, &fastiv_io_key_frame_duration,
		(void *) (intptr_t) (wuffs_base__frame_config__duration(&fc) /
			WUFFS_BASE__FLICKS_PER_MILLISECOND), NULL);

	cairo_surface_set_user_data(surface, &fastiv_io_key_frame_previous,
		ctx->result_tail, NULL);
	if (ctx->result_tail)
		cairo_surface_set_user_data(ctx->result_tail, &fastiv_io_key_frame_next,
			surface, (cairo_destroy_func_t) cairo_surface_destroy);
	else
		ctx->result = surface;

	success = true;
	ctx->result_tail = surface;
	ctx->last_fc = fc;

fail:
	if (!success) {
		cairo_surface_destroy(surface);
		g_clear_pointer(&ctx->result, cairo_surface_destroy);
		ctx->result_tail = NULL;
	}

	g_free(targetbuf);
	return success;
}

// https://github.com/google/wuffs/blob/main/example/gifplayer/gifplayer.c
// is pure C, and a good reference. I can't use the auxiliary libraries,
// since they depend on C++, which is undesirable.
static cairo_surface_t *
open_wuffs(
	wuffs_base__image_decoder *dec, wuffs_base__io_buffer src, GError **error)
{
	struct load_wuffs_frame_context ctx = {.dec = dec, .src = &src};

	// TODO(p): PNG also has sRGB and gAMA, as well as text chunks (Wuffs #58).
	// The former two use WUFFS_BASE__MORE_INFORMATION__FLAVOR__METADATA_PARSED.
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__EXIF, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__ICCP, true);

	while (true) {
		wuffs_base__status status =
			wuffs_base__image_decoder__decode_image_config(
				ctx.dec, &ctx.cfg, ctx.src);
		if (wuffs_base__status__is_ok(&status))
			break;

		if (status.repr != wuffs_base__note__metadata_reported) {
			set_error(error, wuffs_base__status__message(&status));
			goto fail;
		}

		wuffs_base__more_information minfo = {};
		GBytes *bytes = NULL;
		if (!(bytes = pull_metadata(ctx.dec, ctx.src, &minfo, error)))
			goto fail;

		switch (wuffs_base__more_information__metadata__fourcc(&minfo)) {
		case WUFFS_BASE__FOURCC__EXIF:
			if (ctx.meta_exif) {
				g_warning("ignoring repeated Exif");
				break;
			}
			ctx.meta_exif = bytes;
			continue;
		case WUFFS_BASE__FOURCC__ICCP:
			if (ctx.meta_iccp) {
				g_warning("ignoring repeated ICC profile");
				break;
			}
			ctx.meta_iccp = bytes;
			continue;
		}

		g_bytes_unref(bytes);
	}

	// This, at least currently, seems excessive.
	if (!wuffs_base__image_config__is_valid(&ctx.cfg)) {
		set_error(error, "invalid Wuffs image configuration");
		goto fail;
	}

	// We need to check because of the Cairo API.
	ctx.width = wuffs_base__pixel_config__width(&ctx.cfg.pixcfg);
	ctx.height = wuffs_base__pixel_config__height(&ctx.cfg.pixcfg);
	if (ctx.width > INT_MAX || ctx.height > INT_MAX) {
		set_error(error, "image dimensions overflow");
		goto fail;
	}

	// Wuffs maps tRNS to BGRA in `decoder.decode_trns?`, we should be fine.
	// wuffs_base__pixel_format__transparency() doesn't reflect the image file.
	// TODO(p): See if wuffs_base__image_config__first_frame_is_opaque() causes
	// issues with animations, and eventually ensure an alpha-capable format.
	bool opaque = wuffs_base__image_config__first_frame_is_opaque(&ctx.cfg);

	// Wuffs' API is kind of awful--we want to catch deep RGB and deep grey.
	wuffs_base__pixel_format srcfmt =
		wuffs_base__pixel_config__pixel_format(&ctx.cfg.pixcfg);
	uint32_t bpp = wuffs_base__pixel_format__bits_per_pixel(&srcfmt);

	// Cairo doesn't support transparency with RGB30, so no premultiplication.
	ctx.pack_16_10 = opaque && (bpp > 24 || (bpp < 24 && bpp > 8));
#ifdef FASTIV_CAIRO_RGBA128F
	ctx.expand_16_float = !opaque && (bpp > 24 || (bpp < 24 && bpp > 8));
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
	ctx.cairo_format = CAIRO_FORMAT_ARGB32;

#ifdef FASTIV_CAIRO_RGBA128F
	if (ctx.expand_16_float) {
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE;
		ctx.cairo_format = CAIRO_FORMAT_RGBA128F;
	} else
#endif  // FASTIV_CAIRO_RGBA128F
	if (ctx.pack_16_10) {
		// TODO(p): Make Wuffs support A2RGB30 as a destination format;
		// in general, 16-bit depth swizzlers are stubbed.
		// See also wuffs_base__pixel_swizzler__prepare__*().
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE;
		ctx.cairo_format = CAIRO_FORMAT_RGB30;
	} else if (opaque) {
		// BGRX doesn't have as wide swizzler support, namely in GIF.
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL;
		ctx.cairo_format = CAIRO_FORMAT_RGB24;
	}

	wuffs_base__pixel_config__set(&ctx.cfg.pixcfg, wuffs_format,
		WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, ctx.width, ctx.height);

	uint64_t workbuf_len_max_incl =
		wuffs_base__image_decoder__workbuf_len(ctx.dec).max_incl;
	if (workbuf_len_max_incl) {
		ctx.workbuf = wuffs_base__malloc_slice_u8(malloc, workbuf_len_max_incl);
		if (!ctx.workbuf.ptr) {
			set_error(error, "failed to allocate a work buffer");
			goto fail;
		}
	}

	while (load_wuffs_frame(&ctx, error))
		;

	// Wrap the chain around, since our caller receives only one pointer.
	if (ctx.result)
		cairo_surface_set_user_data(ctx.result, &fastiv_io_key_frame_previous,
			ctx.result_tail, NULL);

fail:
	free(ctx.workbuf.ptr);
	g_clear_pointer(&ctx.meta_exif, g_bytes_unref);
	g_clear_pointer(&ctx.meta_iccp, g_bytes_unref);
	return ctx.result;
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
trivial_cmyk_to_host_byte_order_argb(unsigned char *p, int len)
{
	// Inspired by gdk-pixbuf's io-jpeg.c:
	//
	// Assume that all YCCK/CMYK JPEG files use inverted CMYK, as Photoshop
	// does, see https://bugzilla.gnome.org/show_bug.cgi?id=618096
	while (len--) {
		int c = p[0], m = p[1], y = p[2], k = p[3];
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
		p[0] = k * y / 255;
		p[1] = k * m / 255;
		p[2] = k * c / 255;
		p[3] = 255;
#else
		p[3] = k * y / 255;
		p[2] = k * m / 255;
		p[1] = k * c / 255;
		p[0] = 255;
#endif
		p += 4;
	}
}

static void
parse_jpeg_metadata(cairo_surface_t *surface, const gchar *data, gsize len)
{
	// Because the JPEG file format is simple, just do it manually.
	// See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
	enum {
		APP0 = 0xE0,
		APP1,
		APP2,
		RST0 = 0xD0,
		RST1,
		RST2,
		RST3,
		RST4,
		RST5,
		RST6,
		RST7,
		SOI = 0xD8,
		EOI = 0xD9,
		SOS = 0xDA,
		TEM = 0x01,
	};

	GByteArray *exif = g_byte_array_new(), *icc = g_byte_array_new();
	int icc_sequence = 0, icc_done = FALSE;

	const guint8 *p = (const guint8 *) data, *end = p + len;
	while (p + 3 < end && *p++ == 0xFF && *p != SOS && *p != EOI) {
		// The previous byte is a fill byte, restart.
		if (*p == 0xFF)
			continue;

		// These markers stand alone, not starting a marker segment.
		guint8 marker = *p++;
		switch (marker) {
		case RST0:
		case RST1:
		case RST2:
		case RST3:
		case RST4:
		case RST5:
		case RST6:
		case RST7:
		case SOI:
		case TEM:
			continue;
		}

		// Do not bother validating the structure.
		guint16 length = p[0] << 8 | p[1];
		const guint8 *payload = p + 2;
		if ((p += length) > end)
			break;

		// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf 4.7.2
		// Do not check the padding byte.
		if (marker == APP1 && p - payload >= 6 &&
			!memcmp(payload, "Exif\0", 5) && !exif->len) {
			payload += 6;
			g_byte_array_append(exif, payload, p - payload);
		}

		// https://www.color.org/specification/ICC1v43_2010-12.pdf B.4
		if (marker == APP2 && p - payload >= 14 &&
			!memcmp(payload, "ICC_PROFILE\0", 12) && !icc_done &&
			payload[12] == ++icc_sequence && payload[13] >= payload[12]) {
			payload += 14;
			g_byte_array_append(icc, payload, p - payload);
			icc_done = payload[-1] == icc_sequence;
		}
	}

	if (exif->len)
		cairo_surface_set_user_data(surface, &fastiv_io_key_exif,
			g_byte_array_free_to_bytes(exif),
			(cairo_destroy_func_t) g_bytes_unref);
	else
		g_byte_array_free(exif, TRUE);

	if (icc_done)
		cairo_surface_set_user_data(surface, &fastiv_io_key_icc,
			g_byte_array_free_to_bytes(icc),
			(cairo_destroy_func_t) g_bytes_unref);
	else
		g_byte_array_free(icc, TRUE);
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

	if (pixel_format == TJPF_CMYK) {
		// CAIRO_STRIDE_ALIGNMENT is 4 bytes, so there will be no padding with
		// ARGB/BGR/XRGB/BGRX.
		trivial_cmyk_to_host_byte_order_argb(
			cairo_image_surface_get_data(surface), width * height);
	}

	// Pixel data has been written, need to let Cairo know.
	cairo_surface_mark_dirty(surface);

	tjDestroy(dec);
	parse_jpeg_metadata(surface, data, len);
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
#if LIBRSVG_CHECK_VERSION(2, 51, 0)
	if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &w, &h)) {
#else
	RsvgDimensionData dd = {};
	rsvg_handle_get_dimensions(handle, &dd);
	if ((w = dd.width) <= 0 || (h = dd.height) <= 0) {
#endif
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
#ifdef HAVE_XCURSOR  //---------------------------------------------------------

// fmemopen is part of POSIX-1.2008, this exercise is technically unnecessary.
// libXcursor checks for EOF rather than -1, it may eat your hamster.
struct fastiv_io_xcursor {
	XcursorFile parent;
	unsigned char *data;
	long position, len;
};

static int
fastiv_io_xcursor_read(XcursorFile *file, unsigned char *buf, int len)
{
	struct fastiv_io_xcursor *fix = (struct fastiv_io_xcursor *) file;
	if (fix->position < 0 || fix->position > fix->len) {
		errno = EOVERFLOW;
		return -1;
	}
	long n = MIN(fix->len - fix->position, len);
	if (n > G_MAXINT) {
		errno = EIO;
		return -1;
	}
	memcpy(buf, fix->data + fix->position, n);
	fix->position += n;
	return n;
}

static int
fastiv_io_xcursor_write(G_GNUC_UNUSED XcursorFile *file,
	G_GNUC_UNUSED unsigned char *buf, G_GNUC_UNUSED int len)
{
	errno = EBADF;
	return -1;
}

static int
fastiv_io_xcursor_seek(XcursorFile *file, long offset, int whence)
{
	struct fastiv_io_xcursor *fix = (struct fastiv_io_xcursor *) file;
	switch (whence) {
	case SEEK_SET:
		fix->position = offset;
		break;
	case SEEK_CUR:
		fix->position += offset;
		break;
	case SEEK_END:
		fix->position = fix->len + offset;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	// This is technically too late for fseek(), but libXcursor doesn't care.
	if (fix->position < 0) {
		errno = EINVAL;
		return -1;
	}
	return fix->position;
}

static const XcursorFile fastiv_io_xcursor_adaptor = {
	.closure = NULL,
	.read    = fastiv_io_xcursor_read,
	.write   = fastiv_io_xcursor_write,
	.seek    = fastiv_io_xcursor_seek,
};

static cairo_surface_t *
open_xcursor(const gchar *data, gsize len, GError **error)
{
	if (len > G_MAXLONG) {
		set_error(error, "size overflow");
		return NULL;
	}

	struct fastiv_io_xcursor file = {
		.parent = fastiv_io_xcursor_adaptor,
		.data = (unsigned char *) data,
		.position = 0,
		.len = len,
	};

	XcursorImages *images = XcursorXcFileLoadAllImages(&file.parent);
	if (!images) {
		set_error(error, "general failure");
		return NULL;
	}

	// Interpret cursors as animated pages.
	cairo_surface_t *pages = NULL, *frames_head = NULL, *frames_tail = NULL;

	// XXX: Assuming that all "nominal sizes" have the same dimensions.
	XcursorDim last_nominal = -1;
	for (int i = 0; i < images->nimage; i++) {
		XcursorImage *image = images->images[i];

		// The library automatically byte swaps in _XcursorReadImage().
		cairo_surface_t *surface = cairo_image_surface_create_for_data(
			(unsigned char *) image->pixels, CAIRO_FORMAT_ARGB32,
			image->width, image->height, image->width * sizeof *image->pixels);
		cairo_surface_set_user_data(surface, &fastiv_io_key_frame_duration,
			(void *) (intptr_t) image->delay, NULL);

		if (pages && image->size == last_nominal) {
			cairo_surface_set_user_data(surface,
				&fastiv_io_key_frame_previous, frames_tail, NULL);
			cairo_surface_set_user_data(frames_tail,
				&fastiv_io_key_frame_next, surface,
				(cairo_destroy_func_t) cairo_surface_destroy);
		} else if (frames_head) {
			cairo_surface_set_user_data(frames_head,
				&fastiv_io_key_frame_previous, frames_tail, NULL);

			cairo_surface_set_user_data(frames_head,
				&fastiv_io_key_page_next, surface,
				(cairo_destroy_func_t) cairo_surface_destroy);
			cairo_surface_set_user_data(surface,
				&fastiv_io_key_page_previous, frames_head, NULL);
			frames_head = surface;
		} else {
			pages = frames_head = surface;
		}

		frames_tail = surface;
		last_nominal = image->size;
	}
	if (!pages) {
		XcursorImagesDestroy(images);
		return NULL;
	}

	// Wrap around animations in the last page.
	cairo_surface_set_user_data(
		frames_head, &fastiv_io_key_frame_previous, frames_tail, NULL);

	// There is no need to copy data, assign it to the surface.
	static cairo_user_data_key_t key = {};
	cairo_surface_set_user_data(
		pages, &key, images, (cairo_destroy_func_t) XcursorImagesDestroy);
	return pages;
}

#endif  // HAVE_XCURSOR --------------------------------------------------------
#ifdef HAVE_LIBTIFF  //---------------------------------------------------------

struct fastiv_io_tiff {
	unsigned char *data;
	gchar *error;

	// No, libtiff, the offset is not supposed to be unsigned (also see:
	// man 0p sys_types.h), but at least it's fewer cases for us to care about.
	toff_t position, len;
};

static tsize_t
fastiv_io_tiff_read(thandle_t h, tdata_t buf, tsize_t len)
{
	struct fastiv_io_tiff *io = h;
	if (len < 0) {
		// What the FUCK! This argument is not supposed to be signed!
		// How many mistakes can you make in such a basic API?
		errno = EOWNERDEAD;
		return -1;
	}
	if (io->position > io->len) {
		errno = EOVERFLOW;
		return -1;
	}
	toff_t n = MIN(io->len - io->position, (toff_t) len);
	if (n > TIFF_TMSIZE_T_MAX) {
		errno = EIO;
		return -1;
	}
	memcpy(buf, io->data + io->position, n);
	io->position += n;
	return n;
}

static tsize_t
fastiv_io_tiff_write(G_GNUC_UNUSED thandle_t h,
	G_GNUC_UNUSED tdata_t buf, G_GNUC_UNUSED tsize_t len)
{
	errno = EBADF;
	return -1;
}

static toff_t
fastiv_io_tiff_seek(thandle_t h, toff_t offset, int whence)
{
	struct fastiv_io_tiff *io = h;
	switch (whence) {
	case SEEK_SET:
		io->position = offset;
		break;
	case SEEK_CUR:
		io->position += offset;
		break;
	case SEEK_END:
		io->position = io->len + offset;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	return io->position;
}

static int
fastiv_io_tiff_close(G_GNUC_UNUSED thandle_t h)
{
	return 0;
}

static toff_t
fastiv_io_tiff_size(thandle_t h)
{
	return ((struct fastiv_io_tiff *) h)->len;
}

static void
fastiv_io_tiff_error(thandle_t h,
	const char *module, const char *format, va_list ap)
{
	struct fastiv_io_tiff *io = h;
	gchar *message = g_strdup_vprintf(format, ap);
	if (io->error)
		// I'm not sure if two errors can ever come in a succession,
		// but make sure to log them in any case.
		g_warning("tiff: %s: %s", module, message);
	else
		io->error = g_strconcat(module, ": ", message, NULL);
	g_free(message);
}

static void
fastiv_io_tiff_warning(G_GNUC_UNUSED thandle_t h,
	const char *module, const char *format, va_list ap)
{
	gchar *message = g_strdup_vprintf(format, ap);
	g_debug("tiff: %s: %s", module, message);
	g_free(message);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static cairo_surface_t *
load_libtiff_directory(TIFF *tiff, GError **error)
{
	char emsg[1024] = "";
	if (!TIFFRGBAImageOK(tiff, emsg)) {
		set_error(error, emsg);
		return NULL;
	}

	// TODO(p): Are there cases where we might not want to "stop on error"?
	TIFFRGBAImage image;
	if (!TIFFRGBAImageBegin(&image, tiff, 1 /* stop on error */, emsg)) {
		set_error(error, emsg);
		return NULL;
	}

	cairo_surface_t *surface = NULL;
	if (image.width > G_MAXINT || image.height >= G_MAXINT ||
		G_MAXUINT32 / image.width < image.height) {
		set_error(error, "image dimensions too large");
		goto fail;
	}

	surface = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, image.width, image.height);

	image.req_orientation = ORIENTATION_LEFTTOP;
	uint32_t *raster = (uint32_t *) cairo_image_surface_get_data(surface);
	if (!TIFFRGBAImageGet(&image, raster, image.width, image.height)) {
		g_clear_pointer(&surface, cairo_surface_destroy);
		goto fail;
	}

	// Needs to be converted from ABGR to alpha-premultiplied ARGB for Cairo.
	for (uint32_t i = image.width * image.height; i--;) {
		uint32_t pixel = raster[i],
			a = TIFFGetA(pixel),
			b = TIFFGetB(pixel) * a / 255,
			g = TIFFGetG(pixel) * a / 255,
			r = TIFFGetR(pixel) * a / 255;
		raster[i] = a << 24 | r << 16 | g << 8 | b;
	}

	cairo_surface_mark_dirty(surface);
	// XXX: The whole file is essentially an Exif, any ideas?

	const uint32_t icc_length = 0;
	const void *icc_profile = NULL;
	if (TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &icc_length, &icc_profile)) {
		cairo_surface_set_user_data(surface, &fastiv_io_key_icc,
			g_bytes_new(icc_profile, icc_length),
			(cairo_destroy_func_t) g_bytes_unref);
	}

	// Don't ask. The API is high, alright, I'm just not sure about the level.
	uint16_t orientation = 0;
	if (TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation)) {
		if (orientation == 5 || orientation == 7)
			cairo_surface_set_user_data(surface, &fastiv_io_key_orientation,
				(void *) (uintptr_t) 5, NULL);
		if (orientation == 6 || orientation == 8)
			cairo_surface_set_user_data(surface, &fastiv_io_key_orientation,
				(void *) (uintptr_t) 7, NULL);
	}

fail:
	TIFFRGBAImageEnd(&image);
	return surface;
}

static cairo_surface_t *
open_libtiff(const gchar *data, gsize len, const gchar *path, GError **error)
{
	// Both kinds of handlers are called, redirect everything.
	TIFFErrorHandler eh = TIFFSetErrorHandler(NULL);
	TIFFErrorHandler wh = TIFFSetWarningHandler(NULL);
	TIFFErrorHandlerExt ehe = TIFFSetErrorHandlerExt(fastiv_io_tiff_error);
	TIFFErrorHandlerExt whe = TIFFSetWarningHandlerExt(fastiv_io_tiff_warning);
	struct fastiv_io_tiff h = {
		.data = (unsigned char *) data,
		.position = 0,
		.len = len,
	};

	cairo_surface_t *result = NULL, *result_tail = NULL;
	TIFF *tiff = TIFFClientOpen(path, "rm" /* Avoid mmap. */, &h,
		fastiv_io_tiff_read, fastiv_io_tiff_write, fastiv_io_tiff_seek,
		fastiv_io_tiff_close, fastiv_io_tiff_size, NULL, NULL);
	if (!tiff)
		goto fail;

	do {
		// We inform about unsupported directories, but do not fail on them.
		GError *err = NULL;
		cairo_surface_t *surface = load_libtiff_directory(tiff, &err);
		if (err) {
			g_warning("%s: %s", path, err->message);
			g_error_free(err);
		}
		if (!surface)
			continue;

		if (result) {
			cairo_surface_set_user_data(result_tail,
				&fastiv_io_key_page_next, surface,
				(cairo_destroy_func_t) cairo_surface_destroy);
			cairo_surface_set_user_data(surface,
				&fastiv_io_key_page_previous, result_tail, NULL);
			result_tail = surface;
		} else {
			result = result_tail = surface;
		}
	} while (TIFFReadDirectory(tiff));
	TIFFClose(tiff);

fail:
	if (h.error) {
		g_clear_pointer(&result, cairo_surface_destroy);
		set_error(error, h.error);
		g_free(h.error);
	} else if (!result) {
		set_error(error, "empty or unsupported image");
	}

	TIFFSetErrorHandlerExt(ehe);
	TIFFSetWarningHandlerExt(whe);
	TIFFSetErrorHandler(eh);
	TIFFSetWarningHandler(wh);
	return result;
}

#endif  // HAVE_LIBTIFF --------------------------------------------------------
#ifdef HAVE_GDKPIXBUF  // ------------------------------------------------------

static cairo_surface_t *
open_gdkpixbuf(const gchar *data, gsize len, GError **error)
{
	// gdk-pixbuf controls the playback itself, there is no reliable method of
	// extracting individual frames (due to loops).
	GInputStream *is = g_memory_input_stream_new_from_data(data, len, NULL);
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(is, NULL, error);
	g_object_unref(is);
	if (!pixbuf)
		return NULL;

	cairo_surface_t *surface =
		gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, NULL);

	const char *orientation = gdk_pixbuf_get_option(pixbuf, "orientation");
	if (orientation && strlen(orientation) == 1) {
		int n = *orientation - '0';
		if (n >= 1 && n <= 8)
			cairo_surface_set_user_data(surface, &fastiv_io_key_orientation,
				(void *) (uintptr_t) n, NULL);
	}

	const char *icc_profile = gdk_pixbuf_get_option(pixbuf, "icc-profile");
	if (icc_profile) {
		gsize out_len = 0;
		guchar *raw = g_base64_decode(icc_profile, &out_len);
		if (raw) {
			cairo_surface_set_user_data(surface, &fastiv_io_key_icc,
				g_bytes_new_take(raw, out_len),
				(cairo_destroy_func_t) g_bytes_unref);
		}
	}

	g_object_unref(pixbuf);
	return surface;
}

#endif  // HAVE_GDKPIXBUF ------------------------------------------------------

cairo_user_data_key_t fastiv_io_key_exif;
cairo_user_data_key_t fastiv_io_key_orientation;
cairo_user_data_key_t fastiv_io_key_icc;

cairo_user_data_key_t fastiv_io_key_frame_next;
cairo_user_data_key_t fastiv_io_key_frame_previous;
cairo_user_data_key_t fastiv_io_key_frame_duration;
cairo_user_data_key_t fastiv_io_key_loops;

cairo_user_data_key_t fastiv_io_key_page_next;
cairo_user_data_key_t fastiv_io_key_page_previous;

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
#ifdef HAVE_XCURSOR  //---------------------------------------------------------
		if ((surface = open_xcursor(data, len, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_XCURSOR --------------------------------------------------------
#ifdef HAVE_LIBTIFF  //---------------------------------------------------------
		// This needs to be positioned after LibRaw.
		if ((surface = open_libtiff(data, len, path, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_LIBTIFF --------------------------------------------------------
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

	// gdk-pixbuf only gives out this single field--cater to its limitations,
	// since we'd really like to have it.
	GBytes *exif = NULL;
	gsize exif_len = 0;
	gconstpointer exif_data = NULL;
	if (surface &&
		(exif = cairo_surface_get_user_data(surface, &fastiv_io_key_exif)) &&
		(exif_data = g_bytes_get_data(exif, &exif_len))) {
		cairo_surface_set_user_data(surface, &fastiv_io_key_orientation,
			(void *) (uintptr_t) fastiv_io_exif_orientation(
				exif_data, exif_len), NULL);
	}
	return surface;
}

// --- Metadata ----------------------------------------------------------------

FastivIoOrientation
fastiv_io_exif_orientation(const guint8 *tiff, gsize len)
{
	// libtiff also knows how to do this, but it's not a lot of code.
	// The "Orientation" tag/field is part of Baseline TIFF 6.0 (1992),
	// it just so happens that Exif is derived from this format.
	// There is no other meaningful placement for this than right in IFD0,
	// describing the main image.
	const uint8_t *end = tiff + len,
		le[4] = {'I', 'I', 42, 0},
		be[4] = {'M', 'M', 0, 42};

	uint16_t (*u16)(const uint8_t *) = NULL;
	uint32_t (*u32)(const uint8_t *) = NULL;
	if (tiff + 8 > end) {
		return FastivIoOrientationUnknown;
	} else if (!memcmp(tiff, le, sizeof le)) {
		u16 = wuffs_base__peek_u16le__no_bounds_check;
		u32 = wuffs_base__peek_u32le__no_bounds_check;
	} else if (!memcmp(tiff, be, sizeof be)) {
		u16 = wuffs_base__peek_u16be__no_bounds_check;
		u32 = wuffs_base__peek_u32be__no_bounds_check;
	} else {
		return FastivIoOrientationUnknown;
	}

	const uint8_t *ifd0 = tiff + u32(tiff + 4);
	if (ifd0 + 2 > end)
		return FastivIoOrientationUnknown;

	uint16_t fields = u16(ifd0);
	enum { BYTE = 1, ASCII, SHORT, LONG, RATIONAL,
		SBYTE, UNDEFINED, SSHORT, SLONG, SRATIONAL, FLOAT, DOUBLE };
	enum { Orientation = 274 };
	for (const guint8 *p = ifd0 + 2; fields-- && p + 12 <= end; p += 12) {
		uint16_t tag = u16(p), type = u16(p + 2), value16 = u16(p + 8);
		uint32_t count = u32(p + 4);
		if (tag == Orientation && type == SHORT && count == 1 &&
			value16 >= 1 && value16 <= 8)
			return value16;
	}
	return FastivIoOrientationUnknown;
}

// --- Thumbnails --------------------------------------------------------------

GType
fastiv_io_thumbnail_size_get_type(void)
{
	static gsize guard;
	if (g_once_init_enter(&guard)) {
#define XX(name, value, dir) {FASTIV_IO_THUMBNAIL_SIZE_ ## name, \
	"FASTIV_IO_THUMBNAIL_SIZE_" #name, #name},
		static const GEnumValue values[] = {FASTIV_IO_THUMBNAIL_SIZES(XX) {}};
#undef XX
		GType type = g_enum_register_static(
			g_intern_static_string("FastivIoThumbnailSize"), values);
		g_once_init_leave(&guard, type);
	}
	return guard;
}

#define XX(name, value, dir) {value, dir},
FastivIoThumbnailSizeInfo
	fastiv_io_thumbnail_sizes[FASTIV_IO_THUMBNAIL_SIZE_COUNT] = {
		FASTIV_IO_THUMBNAIL_SIZES(XX)};
#undef XX

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#ifndef __linux__
#define st_mtim st_mtimespec
#endif  // ! __linux__

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
			const uint8_t *unit = (const uint8_t *) &data[i];
			uint32_t a = unit[3],
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
fastiv_io_lookup_thumbnail(GFile *target, FastivIoThumbnailSize size)
{
	g_return_val_if_fail(size >= FASTIV_IO_THUMBNAIL_SIZE_MIN &&
		size <= FASTIV_IO_THUMBNAIL_SIZE_MAX, NULL);

	// Local files only, at least for now.
	gchar *path = g_file_get_path(target);
	if (!path)
		return NULL;

	GStatBuf st = {};
	int err = g_stat(path, &st);
	g_free(path);
	if (err)
		return NULL;

	gchar *uri = g_file_get_uri(target);
	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	gchar *cache_dir = get_xdg_home_dir("XDG_CACHE_HOME", ".cache");

	// The lookup sequence is: nominal..max, then mirroring back to ..min.
	cairo_surface_t *result = NULL;
	GError *error = NULL;
	for (int i = 0; i < FASTIV_IO_THUMBNAIL_SIZE_COUNT; i++) {
		int use = size + i;
		if (use > FASTIV_IO_THUMBNAIL_SIZE_MAX)
			use = FASTIV_IO_THUMBNAIL_SIZE_MAX - i;

		gchar *path = g_strdup_printf("%s/thumbnails/%s/%s.png", cache_dir,
			fastiv_io_thumbnail_sizes[use].thumbnail_spec_name, sum);
		result = read_spng_thumbnail(path, uri, st.st_mtim.tv_sec, &error);
		if (error) {
			g_debug("%s: %s", path, error->message);
			g_clear_error(&error);
		}
		g_free(path);
		if (result)
			break;
	}

	g_free(cache_dir);
	g_free(sum);
	g_free(uri);
	return result;
}

int
fastiv_io_filecmp(GFile *location1, GFile *location2)
{
	if (g_file_has_prefix(location1, location2))
		return +1;
	if (g_file_has_prefix(location2, location1))
		return -1;

	gchar *name1 = g_file_get_parse_name(location1);
	gchar *name2 = g_file_get_parse_name(location2);
	int result = g_utf8_collate(name1, name2);
	g_free(name1);
	g_free(name2);
	return result;
}

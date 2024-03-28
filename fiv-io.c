//
// fiv-io.c: image operations
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

#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>

#include <cairo.h>
#include <glib.h>
#include <jpeglib.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>
#ifdef HAVE_JPEG_QS
#include <libjpegqs.h>
#endif  // HAVE_JPEG_QS

#define TIFF_TABLES_CONSTANTS_ONLY
#include "tiff-tables.h"
#include "tiffer.h"

#ifdef HAVE_LIBRAW
#include <libraw.h>
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 21, 0)
#define LIBRAW_OPIONS_NO_MEMERR_CALLBACK 0
#else
#define rawparams params
#endif
#endif  // HAVE_LIBRAW
#ifdef HAVE_RESVG
#include <resvg.h>
#endif  // HAVE_RESVG
#ifdef HAVE_LIBRSVG
#include <librsvg/rsvg.h>
#endif  // HAVE_LIBRSVG
#ifdef HAVE_XCURSOR
#include <X11/Xcursor/Xcursor.h>
#endif  // HAVE_XCURSOR
#ifdef HAVE_LIBHEIF
#include <libheif/heif.h>
#endif  // HAVE_LIBHEIF
#ifdef HAVE_LIBTIFF
#include <tiff.h>
#include <tiffio.h>
#endif  // HAVE_LIBTIFF
#ifdef HAVE_GDKPIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
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
#define WUFFS_CONFIG__MODULE__TGA
#define WUFFS_CONFIG__MODULE__ZLIB
#include "submodules/wuffs-mirror-release-c/release/c/wuffs-v0.3.c"

#include "fiv-io.h"

#if CAIRO_VERSION >= 11702 && X11_ACTUALLY_SUPPORTS_RGBA128F_OR_WE_USE_OPENGL
#define FIV_CAIRO_RGBA128F
#endif

// A subset of shared-mime-info that produces an appropriate list of
// file extensions. Chiefly motivated by the suckiness of raw photo formats:
// someone else will maintain the list of file extensions for us.
const char *fiv_io_supported_media_types[] = {
	"image/bmp",
	"image/gif",
	"image/png",
	"image/x-tga",
	"image/jpeg",
	"image/webp",
#ifdef HAVE_LIBRAW
	"image/x-dcraw",
#endif  // HAVE_LIBRAW
#if defined HAVE_RESVG || defined HAVE_LIBRSVG
	"image/svg+xml",
#endif  // HAVE_RESVG || HAVE_LIBRSVG
#ifdef HAVE_XCURSOR
	"image/x-xcursor",
#endif  // HAVE_XCURSOR
#ifdef HAVE_LIBHEIF
	"image/heic",
	"image/heif",
	"image/avif",
#endif  // HAVE_LIBHEIF
#ifdef HAVE_LIBTIFF
	"image/tiff",
#endif  // HAVE_LIBTIFF
	NULL
};

gchar **
fiv_io_all_supported_media_types(void)
{
	GHashTable *unique =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GPtrArray *types = g_ptr_array_new();
	for (const char **p = fiv_io_supported_media_types; *p; p++)
		if (g_hash_table_insert(unique, g_strdup(*p), NULL))
			g_ptr_array_add(types, g_strdup(*p));

#ifdef HAVE_GDKPIXBUF
	GSList *formats = gdk_pixbuf_get_formats();
	for (GSList *iter = formats; iter; iter = iter->next) {
		gchar **subtypes = gdk_pixbuf_format_get_mime_types(iter->data);
		for (gchar **p = subtypes; *p; p++)
			if (g_hash_table_insert(unique, *p, NULL))
				g_ptr_array_add(types, g_strdup(*p));
		g_free(subtypes);
	}
	g_slist_free(formats);
#endif  // HAVE_GDKPIXBUF

	g_hash_table_unref(unique);
	g_ptr_array_add(types, NULL);
	return (gchar **) g_ptr_array_free(types, FALSE);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define FIV_IO_ERROR fiv_io_error_quark()

G_DEFINE_QUARK(fiv-io-error-quark, fiv_io_error)

enum FivIoError {
	FIV_IO_ERROR_OPEN
};

static void
set_error(GError **error, const char *message)
{
	g_set_error_literal(error, FIV_IO_ERROR, FIV_IO_ERROR_OPEN, message);
}

static void add_warning(const FivIoOpenContext *ctx, const char *format, ...)
	G_GNUC_PRINTF(2, 3);

static void
add_warning(const FivIoOpenContext *ctx, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	if (ctx->warnings)
		g_ptr_array_add(ctx->warnings, g_strdup_vprintf(format, ap));
	else
		g_logv(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, format, ap);
	va_end(ap);
}

// --- Images ------------------------------------------------------------------

FivIoImage *
fiv_io_image_new(cairo_format_t format, uint32_t width, uint32_t height)
{
	// CAIRO_STRIDE_ALIGNMENT is 4 bytes, we only use multiples.
	size_t unit = 0;
	switch (format) {
	case CAIRO_FORMAT_RGB24:
	case CAIRO_FORMAT_RGB30:
	case CAIRO_FORMAT_ARGB32:
		unit = 4;
		break;
#if CAIRO_VERSION >= 11702
	case CAIRO_FORMAT_RGB96F:
		unit = 12;
		break;
	case CAIRO_FORMAT_RGBA128F:
		unit = 16;
		break;
#endif
	default:
		return NULL;
	}

	uint8_t *data = g_try_malloc0(unit * width * height);
	if (!data)
		return NULL;

	FivIoImage *image = g_rc_box_new0(FivIoImage);
	image->data = data;
	image->format = format;
	image->width = width;
	image->stride = width * unit;
	image->height = height;
	return image;
}

FivIoImage *
fiv_io_image_ref(FivIoImage *self)
{
	return g_rc_box_acquire(self);
}

static void
fiv_io_image_finalize(FivIoImage *image)
{
	g_free(image->data);

	g_bytes_unref(image->exif);
	g_bytes_unref(image->icc);
	g_bytes_unref(image->xmp);
	g_bytes_unref(image->thum);

	if (image->text)
		g_hash_table_unref(image->text);

	if (image->render)
		image->render->destroy(image->render);

	if (image->page_next)
		fiv_io_image_unref(image->page_next);
	if (image->frame_next)
		fiv_io_image_unref(image->frame_next);
}

void
fiv_io_image_unref(FivIoImage *self)
{
	g_rc_box_release_full(self, (GDestroyNotify) fiv_io_image_finalize);
}

cairo_surface_t *
fiv_io_image_to_surface_noref(const FivIoImage *image)
{
	return cairo_image_surface_create_for_data(
		image->data, image->format, image->width, image->height, image->stride);
}

cairo_surface_t *
fiv_io_image_to_surface(FivIoImage *image)
{
	// TODO(p): Remove this shortcut eventually. And the function.
	if (!image)
		return NULL;

	static cairo_user_data_key_t key_image;
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		image->data, image->format, image->width, image->height, image->stride);
	cairo_surface_set_user_data(surface, &key_image,
		image, (cairo_destroy_func_t) fiv_io_image_unref);
	return surface;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
try_append_page(
	FivIoImage *image, FivIoImage **result, FivIoImage **result_tail)
{
	if (!image)
		return false;

	if (*result) {
		(*result_tail)->page_next = image;
		image->page_previous = *result_tail;
		*result_tail = image;
	} else {
		*result = *result_tail = image;
	}
	return true;
}

// --- Wuffs -------------------------------------------------------------------

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

		g_byte_array_append(array, wuffs_base__io_buffer__reader_pointer(&dst),
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
	GBytes *meta_xmp;                   ///< Reference-counted XMP

	FivIoCmm *cmm;                      ///< CMM context, if any
	FivIoProfile *target;               ///< Target device profile, if any
	FivIoProfile *source;               ///< Source colour profile, if any

	FivIoImage *result;                 ///< The resulting image (referenced)
	FivIoImage *result_tail;            ///< The final animation frame
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

	// TODO(p): Maybe pre-clear with
	// wuffs_base__frame_config__background_color(&fc).

	// Wuffs' test/data/animated-red-blue.gif, e.g., needs this handling.
	cairo_format_t decode_format = ctx->cairo_format;
	if (wuffs_base__frame_config__index(&fc) > 0 &&
		wuffs_base__pixel_config__pixel_format(&ctx->cfg.pixcfg).repr ==
			WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL)
		decode_format = CAIRO_FORMAT_ARGB32;

	unsigned char *targetbuf = NULL;
	FivIoImage *image =
		fiv_io_image_new(decode_format, ctx->width, ctx->height);
	if (!image) {
		set_error(error, "image allocation failure");
		goto fail;
	}

	// There is no padding with ARGB/BGR/XRGB/BGRX.
	// This function does not support a stride different from the width,
	// maybe Wuffs internals do not either.
	wuffs_base__pixel_buffer pb = {0};
	if (ctx->expand_16_float || ctx->pack_16_10) {
		uint32_t targetbuf_size = image->height * image->width * 8;
		targetbuf = g_malloc(targetbuf_size);
		status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ctx->cfg.pixcfg,
			wuffs_base__make_slice_u8(targetbuf, targetbuf_size));
	} else {
		status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ctx->cfg.pixcfg,
			wuffs_base__make_slice_u8(
				image->data, image->stride * image->height));
	}
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		goto fail;
	}

	status = wuffs_base__image_decoder__decode_frame(ctx->dec, &pb, ctx->src,
		WUFFS_BASE__PIXEL_BLEND__SRC, ctx->workbuf, NULL);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));

		// The PNG decoder, at minimum, will flush any pixel data upon
		// finding out that the input is truncated, so accept whatever we get.
	}

	if (ctx->target) {
		if (ctx->expand_16_float || ctx->pack_16_10) {
			fiv_io_cmm_4x16le_direct(ctx->cmm,
				targetbuf, ctx->width, ctx->height, ctx->source, ctx->target);
			// The first one premultiplies below, the second doesn't need to.
		} else {
			fiv_io_cmm_argb32_premultiply(
				ctx->cmm, image, ctx->source, ctx->target);
		}
	}

	if (ctx->expand_16_float) {
		g_debug("Wuffs to Cairo RGBA128F");
		uint16_t *in = (uint16_t *) targetbuf;
		float *out = (float *) image->data;
		for (uint32_t y = 0; y < image->height; y++) {
			for (uint32_t x = 0; x < image->width; x++) {
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
		uint32_t *out = (uint32_t *) image->data;
		for (uint32_t y = 0; y < image->height; y++) {
			for (uint32_t x = 0; x < image->width; x++) {
				uint32_t b = *in++, g = *in++, r = *in++, X = *in++;
				*out++ = (X >> 14) << 30 |
					(r >> 6) << 20 | (g >> 6) << 10 | (b >> 6);
			}
		}
	}

	// Single-frame images get a fast path, animations are are handled slowly:
	if (wuffs_base__frame_config__index(&fc) > 0) {
		// Copy the previous frame to a new image.
		FivIoImage *prev = ctx->result_tail, *canvas = fiv_io_image_new(
			prev->format, prev->width, prev->height);
		if (!canvas) {
			set_error(error, "image allocation failure");
			goto fail;
		}

		memcpy(canvas->data, prev->data, prev->stride * prev->height);

		// Apply that frame's disposal method.
		// XXX: We do not expect opaque pictures to receive holes this way.
		wuffs_base__rect_ie_u32 bounds =
			wuffs_base__frame_config__bounds(&ctx->last_fc);
		// TODO(p): This field needs to be colour-managed.
		wuffs_base__color_u32_argb_premul bg =
			wuffs_base__frame_config__background_color(&ctx->last_fc);

		double a = (bg >> 24) / 255., r = 0, g = 0, b = 0;
		if (a) {
			r = (uint8_t) (bg >> 16) / 255. / a;
			g = (uint8_t) (bg >> 8)  / 255. / a;
			b = (uint8_t) (bg)       / 255. / a;
		}

		cairo_surface_t *surface = fiv_io_image_to_surface_noref(canvas);
		cairo_t *cr = cairo_create(surface);
		cairo_surface_destroy(surface);
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
		case WUFFS_BASE__ANIMATION_DISPOSAL__NONE:
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

		surface = fiv_io_image_to_surface_noref(image);
		cairo_set_source_surface(cr, surface, 0, 0);
		cairo_surface_destroy(surface);
		cairo_paint(cr);
		cairo_destroy(cr);

		fiv_io_image_unref(image);
		image = canvas;
	}

	if (ctx->meta_exif)
		image->exif = g_bytes_ref(ctx->meta_exif);
	if (ctx->meta_iccp)
		image->icc = g_bytes_ref(ctx->meta_iccp);
	if (ctx->meta_xmp)
		image->xmp = g_bytes_ref(ctx->meta_xmp);

	image->loops = wuffs_base__image_decoder__num_animation_loops(ctx->dec);
	image->frame_duration = wuffs_base__frame_config__duration(&fc) /
		WUFFS_BASE__FLICKS_PER_MILLISECOND;

	image->frame_previous = ctx->result_tail;
	if (ctx->result_tail)
		ctx->result_tail->frame_next = image;
	else
		ctx->result = image;

	ctx->result_tail = image;
	ctx->last_fc = fc;
	g_free(targetbuf);
	return wuffs_base__status__is_ok(&status);

fail:
	g_clear_pointer(&image, fiv_io_image_unref);
	g_clear_pointer(&ctx->result, fiv_io_image_unref);
	ctx->result_tail = NULL;
	g_free(targetbuf);
	return false;
}

// https://github.com/google/wuffs/blob/main/example/gifplayer/gifplayer.c
// is pure C, and a good reference. I can't use the auxiliary libraries,
// since they depend on C++, which is undesirable.
static FivIoImage *
open_wuffs(wuffs_base__image_decoder *dec, wuffs_base__io_buffer src,
	const FivIoOpenContext *ioctx, GError **error)
{
	struct load_wuffs_frame_context ctx = {
		.dec = dec, .src = &src,
		.cmm = ioctx->cmm, .target = ioctx->screen_profile};

	// TODO(p): PNG text chunks, like we do with PNG thumbnails.
	// TODO(p): See if something could and should be done about
	// https://www.w3.org/TR/png-hdr-pq/
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__EXIF, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__ICCP, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__SRGB, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__GAMA, true);

	double gamma = 0;
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
				add_warning(ioctx, "ignoring repeated Exif");
				break;
			}
			ctx.meta_exif = bytes;
			continue;
		case WUFFS_BASE__FOURCC__ICCP:
			if (ctx.meta_iccp) {
				add_warning(ioctx, "ignoring repeated ICC profile");
				break;
			}
			ctx.meta_iccp = bytes;
			continue;
		case WUFFS_BASE__FOURCC__XMP:
			if (ctx.meta_xmp) {
				add_warning(ioctx, "ignoring repeated XMP");
				break;
			}
			ctx.meta_xmp = bytes;
			continue;

		case WUFFS_BASE__FOURCC__SRGB:
			gamma = 2.2;
			break;
		case WUFFS_BASE__FOURCC__GAMA:
			gamma = 1e5 /
				wuffs_base__more_information__metadata_parsed__gama(&minfo);
			break;
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

	// TODO(p): Improve our simplistic PNG handling of: gAMA, cHRM, sRGB.
	if (ctx.target) {
		if (ctx.meta_iccp)
			ctx.source = fiv_io_cmm_get_profile_from_bytes(
				ctx.cmm, ctx.meta_iccp);
		else if (isfinite(gamma) && gamma > 0)
			ctx.source = fiv_io_cmm_get_profile_sRGB_gamma(
				ctx.cmm, gamma);
	}

	// Wuffs maps tRNS to BGRA in `decoder.decode_trns?`, we should be fine.
	// wuffs_base__pixel_format__transparency() doesn't reflect the image file.
	bool opaque = wuffs_base__image_config__first_frame_is_opaque(&ctx.cfg);

	// Wuffs' API is kind of awful--we want to catch wide RGB and wide grey.
	wuffs_base__pixel_format srcfmt =
		wuffs_base__pixel_config__pixel_format(&ctx.cfg.pixcfg);
	uint32_t bpp = wuffs_base__pixel_format__bits_per_pixel(&srcfmt);

	// Cairo doesn't support transparency with RGB30, so no premultiplication.
	ctx.pack_16_10 = opaque && (bpp > 24 || (bpp < 24 && bpp > 8));
#ifdef FIV_CAIRO_RGBA128F
	ctx.expand_16_float = !opaque && (bpp > 24 || (bpp < 24 && bpp > 8));
#endif  // FIV_CAIRO_RGBA128F

	// In Wuffs, /doc/note/pixel-formats.md declares "memory order", which,
	// for our purposes, means big endian, and BGRA results in 32-bit ARGB
	// on most machines.
	//
	// XXX: WUFFS_BASE__PIXEL_FORMAT__ARGB_PREMUL is not expressible, only RGBA.
	// Wuffs doesn't support big-endian architectures at all, we might want to
	// fall back to spng in such cases, or do a second conversion.
	uint32_t wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL;

	// CAIRO_FORMAT_ARGB32: "The 32-bit quantities are stored native-endian.
	// Pre-multiplied alpha is used." CAIRO_FORMAT_RGB{24,30} are analogous.
	ctx.cairo_format = CAIRO_FORMAT_ARGB32;

#ifdef FIV_CAIRO_RGBA128F
	if (ctx.expand_16_float) {
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE;
		ctx.cairo_format = CAIRO_FORMAT_RGBA128F;
	} else
#endif  // FIV_CAIRO_RGBA128F
	if (ctx.pack_16_10) {
		// TODO(p): Make Wuffs support A2RGB30 as a destination format;
		// in general, 16-bit depth swizzlers are stubbed.
		// See also wuffs_base__pixel_swizzler__prepare__*().
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE;
		ctx.cairo_format = CAIRO_FORMAT_RGB30;
	} else if (opaque) {
		// BGRX doesn't have as wide swizzler support, namely in GIF.
		// Moreover, follower frames may still be partly transparent.
		// Therefore, we choose to keep "wuffs_format" intact.
		ctx.cairo_format = CAIRO_FORMAT_RGB24;
	} else if (!ctx.target) {
		wuffs_format = WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL;
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
		if (ioctx->first_frame_only)
			break;

	// Wrap the chain around, since our caller receives only one pointer.
	if (ctx.result)
		ctx.result->frame_previous = ctx.result_tail;

fail:
	free(ctx.workbuf.ptr);
	g_clear_pointer(&ctx.meta_exif, g_bytes_unref);
	g_clear_pointer(&ctx.meta_iccp, g_bytes_unref);
	g_clear_pointer(&ctx.meta_xmp, g_bytes_unref);
	g_clear_pointer(&ctx.source, fiv_io_profile_free);
	return ctx.result;
}

static FivIoImage *
open_wuffs_using(wuffs_base__image_decoder *(*allocate)(),
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	wuffs_base__image_decoder *dec = allocate();
	if (!dec) {
		set_error(error, "memory allocation failed or internal error");
		return NULL;
	}

	FivIoImage *image =
		open_wuffs(dec, wuffs_base__ptr_u8__reader((uint8_t *) data, len, TRUE),
			ctx, error);
	free(dec);
	return image;
}

// --- Wuffs for PNG thumbnails ------------------------------------------------

static bool
pull_metadata_kvp(wuffs_png__decoder *dec, wuffs_base__io_buffer *src,
	GHashTable *texts, gchar **key, GError **error)
{
	wuffs_base__more_information minfo = {};
	GBytes *bytes = NULL;
	if (!(bytes = pull_metadata(
			wuffs_png__decoder__upcast_as__wuffs_base__image_decoder(dec),
			src, &minfo, error)))
		return false;

	switch (wuffs_base__more_information__metadata__fourcc(&minfo)) {
	case WUFFS_BASE__FOURCC__KVPK:
		g_assert(*key == NULL);
		*key = g_strndup(
			g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
		break;
	case WUFFS_BASE__FOURCC__KVPV:
		g_assert(*key != NULL);
		g_hash_table_insert(texts, *key, g_strndup(
			g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes)));
		*key = NULL;
	}

	g_bytes_unref(bytes);
	return true;
}

// An uncomplicated variant of fiv_io_open(), might be up for refactoring.
FivIoImage *
fiv_io_open_png_thumbnail(const char *path, GError **error)
{
	wuffs_png__decoder dec = {};
	wuffs_base__status status = wuffs_png__decoder__initialize(
		&dec, sizeof dec, WUFFS_VERSION, WUFFS_INITIALIZE__ALREADY_ZEROED);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		return NULL;
	}

	gchar *data = NULL;
	gsize len = 0;
	if (!g_file_get_contents(path, &data, &len, error))
		return NULL;

	wuffs_base__io_buffer src =
		wuffs_base__ptr_u8__reader((uint8_t *) data, len, TRUE);
	wuffs_png__decoder__set_report_metadata(
		&dec, WUFFS_BASE__FOURCC__KVP, true);

	wuffs_base__image_config cfg = {};
	wuffs_base__slice_u8 workbuf = {};
	FivIoImage *image = NULL;
	bool success = false;

	GHashTable *texts =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	gchar *key = NULL;
	while (true) {
		status = wuffs_png__decoder__decode_image_config(&dec, &cfg, &src);
		if (wuffs_base__status__is_ok(&status))
			break;

		if (status.repr != wuffs_base__note__metadata_reported) {
			set_error(error, wuffs_base__status__message(&status));
			goto fail;
		}
		if (!pull_metadata_kvp(&dec, &src, texts, &key, error))
			goto fail;
	}

	g_assert(key == NULL);

	uint32_t width = wuffs_base__pixel_config__width(&cfg.pixcfg);
	uint32_t height = wuffs_base__pixel_config__height(&cfg.pixcfg);
	if (width > INT16_MAX || height > INT16_MAX) {
		set_error(error, "image dimensions overflow");
		goto fail;
	}

	wuffs_base__pixel_config__set(&cfg.pixcfg,
		WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
		WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

	uint64_t workbuf_len_max_incl =
		wuffs_png__decoder__workbuf_len(&dec).max_incl;
	if (workbuf_len_max_incl) {
		workbuf = wuffs_base__malloc_slice_u8(malloc, workbuf_len_max_incl);
		if (!workbuf.ptr) {
			set_error(error, "failed to allocate a work buffer");
			goto fail;
		}
	}

	image = fiv_io_image_new(
		wuffs_base__image_config__first_frame_is_opaque(&cfg)
			? CAIRO_FORMAT_RGB24
			: CAIRO_FORMAT_ARGB32,
		width, height);
	if (!image) {
		set_error(error, "image allocation failure");
		goto fail;
	}

	wuffs_base__pixel_buffer pb = {};
	status = wuffs_base__pixel_buffer__set_from_slice(&pb, &cfg.pixcfg,
		wuffs_base__make_slice_u8(image->data, image->stride * image->height));
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		goto fail;
	}

	status = wuffs_png__decoder__decode_frame(&dec, &pb, &src,
		WUFFS_BASE__PIXEL_BLEND__SRC, workbuf, NULL);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		goto fail;
	}

	// The specification does not say where the required metadata should be,
	// it could very well be broken up into two parts.
	wuffs_base__frame_config fc = {};
	while (true) {
		// Not interested in APNG, might even throw an error in that case.
		status = wuffs_png__decoder__decode_frame_config(&dec, &fc, &src);
		if (status.repr == wuffs_base__note__end_of_data ||
			wuffs_base__status__is_ok(&status))
			break;

		if (status.repr != wuffs_base__note__metadata_reported) {
			set_error(error, wuffs_base__status__message(&status));
			goto fail;
		}
		if (!pull_metadata_kvp(&dec, &src, texts, &key, error))
			goto fail;
	}

	g_assert(key == NULL);

	image->text = g_hash_table_ref(texts);
	success = true;

fail:
	if (!success)
		g_clear_pointer(&image, fiv_io_image_unref);

	free(workbuf.ptr);
	g_free(data);
	g_hash_table_unref(texts);
	return image;
}

// --- Multi-Picture Format ----------------------------------------------------

static uint32_t
parse_mpf_mpentry(const uint8_t *p, const struct tiffer *T)
{
	uint32_t attrs = T->un->u32(p);
	uint32_t offset = T->un->u32(p + 8);

	enum {
		TypeBaselineMPPrimaryImage = 0x030000,
		TypeLargeThumbnailVGA = 0x010001,
		TypeLargeThumbnailFullHD = 0x010002,
		TypeMultiFrameImagePanorama = 0x020001,
		TypeMultiFrameImageDisparity = 0x020002,
		TypeMultiFrameImageMultiAngle = 0x020003,
		TypeUndefined = 0x000000,
	};
	switch (attrs & 0xFFFFFF) {
	case TypeLargeThumbnailVGA:
	case TypeLargeThumbnailFullHD:
		// Wasted cycles.
	case TypeUndefined:
		// Apple uses this for HDR and depth maps (same and lower resolution).
		// TODO(p): It would be nice to be able to view them.
		return 0;
	}

	// Don't report non-JPEGs, even though they're unlikely.
	if (((attrs >> 24) & 0x7) != 0)
		return 0;

	return offset;
}

static uint32_t *
parse_mpf_index_entries(const struct tiffer *T, struct tiffer_entry *entry)
{
	uint32_t count = entry->remaining_count / 16;
	uint32_t *offsets = g_malloc0_n(sizeof *offsets, count + 1), *out = offsets;
	for (uint32_t i = 0; i < count; i++) {
		// 5.2.3.3.3. Individual Image Data Offset
		uint32_t offset = parse_mpf_mpentry(entry->p + i * 16, T);
		if (offset)
			*out++ = offset;
	}
	return offsets;
}

static uint32_t *
parse_mpf_index_ifd(struct tiffer *T)
{
	struct tiffer_entry entry = {};
	while (tiffer_next_entry(T, &entry)) {
		// 5.2.3.3. MP Entry
		if (entry.tag == MPF_MPEntry && entry.type == TIFFER_UNDEFINED &&
			!(entry.remaining_count % 16)) {
			return parse_mpf_index_entries(T, &entry);
		}
	}
	return NULL;
}

static bool
parse_mpf(
	GPtrArray *individuals, const uint8_t *mpf, size_t len, size_t total_len)
{
	struct tiffer T;
	if (!tiffer_init(&T, mpf, len) || !tiffer_next_ifd(&T))
		return false;

	// First image: IFD0 is Index IFD, any IFD1 is Attribute IFD.
	// Other images: IFD0 is Attribute IFD, there is no Index IFD.
	uint32_t *offsets = parse_mpf_index_ifd(&T);
	if (offsets) {
		for (const uint32_t *o = offsets; *o; o++)
			if (*o <= total_len)
				g_ptr_array_add(individuals, (gpointer) mpf + *o);
		free(offsets);
	}
	return true;
}

// --- JPEG --------------------------------------------------------------------

struct exif_profile {
	double whitepoint[2];               ///< TIFF_WhitePoint
	double primaries[6];                ///< TIFF_PrimaryChromaticities
	enum Exif_ColorSpace colorspace;    ///< Exif_ColorSpace
	double gamma;                       ///< Exif_Gamma

	bool have_whitepoint;
	bool have_primaries;
	bool have_colorspace;
	bool have_gamma;
};

static bool
parse_exif_profile_reals(
	const struct tiffer *T, struct tiffer_entry *entry, double *out)
{
	while (tiffer_real(T, entry, out++))
		if (!tiffer_next_value(entry))
			return false;
	return true;
}

static void
parse_exif_profile_subifd(
	struct exif_profile *params, const struct tiffer *T, uint32_t offset)
{
	struct tiffer subT = {};
	if (!tiffer_subifd(T, offset, &subT))
		return;

	struct tiffer_entry entry = {};
	while (tiffer_next_entry(&subT, &entry)) {
		int64_t value = 0;
		if (G_UNLIKELY(entry.tag == Exif_ColorSpace) &&
			entry.type == TIFFER_SHORT && entry.remaining_count == 1 &&
			tiffer_integer(&subT, &entry, &value)) {
			params->have_colorspace = true;
			params->colorspace = value;
		} else if (G_UNLIKELY(entry.tag == Exif_Gamma) &&
			entry.type == TIFFER_RATIONAL && entry.remaining_count == 1 &&
			tiffer_real(&subT, &entry, &params->gamma)) {
			params->have_gamma = true;
		}
	}
}

static FivIoProfile *
parse_exif_profile(FivIoCmm *cmm, const void *data, size_t len)
{
	struct tiffer T = {};
	if (!tiffer_init(&T, (const uint8_t *) data, len) || !tiffer_next_ifd(&T))
		return NULL;

	struct exif_profile params = {};
	struct tiffer_entry entry = {};
	while (tiffer_next_entry(&T, &entry)) {
		int64_t offset = 0;
		if (G_UNLIKELY(entry.tag == TIFF_ExifIFDPointer) &&
			entry.type == TIFFER_LONG && entry.remaining_count == 1 &&
			tiffer_integer(&T, &entry, &offset) &&
			offset >= 0 && offset <= UINT32_MAX) {
			parse_exif_profile_subifd(&params, &T, offset);
		} else if (G_UNLIKELY(entry.tag == TIFF_WhitePoint) &&
			entry.type == TIFFER_RATIONAL &&
			entry.remaining_count == G_N_ELEMENTS(params.whitepoint)) {
			params.have_whitepoint =
				parse_exif_profile_reals(&T, &entry, params.whitepoint);
		} else if (G_UNLIKELY(entry.tag == TIFF_PrimaryChromaticities) &&
			entry.type == TIFFER_RATIONAL &&
			entry.remaining_count == G_N_ELEMENTS(params.primaries)) {
			params.have_primaries =
				parse_exif_profile_reals(&T, &entry, params.primaries);
		}
	}
	if (!params.have_colorspace)
		return NULL;

	// If sRGB is claimed, assume all parameters are standard.
	if (params.colorspace == Exif_ColorSpace_sRGB)
		return fiv_io_cmm_get_profile_sRGB(cmm);

	// AdobeRGB Nikon JPEGs provide all of these.
	if (params.colorspace != Exif_ColorSpace_Uncalibrated ||
		!params.have_gamma ||
		!params.have_whitepoint ||
		!params.have_primaries)
		return NULL;

	return fiv_io_cmm_get_profile_parametric(cmm,
		params.gamma, params.whitepoint, params.primaries);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct jpeg_metadata {
	GByteArray *exif;                   ///< Exif buffer or NULL
	GByteArray *icc;                    ///< ICC profile buffer or NULL
	GPtrArray *mpf;                     ///< Multi-Picture Format or NULL
	int width;                          ///< Image width
	int height;                         ///< Image height
};

static void
parse_jpeg_metadata(const char *data, size_t len, struct jpeg_metadata *meta)
{
	// Because the JPEG file format is simple, just do it manually.
	// See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
	enum {
		TEM = 0x01,
		SOF0 = 0xC0, SOF1, SOF2, SOF3, DHT, SOF5, SOF6, SOF7,
		JPG, SOF9, SOF10, SOF11, DAC, SOF13, SOF14, SOF15,
		RST0, RST1, RST2, RST3, RST4, RST5, RST6, RST7,
		SOI, EOI, SOS, DQT, DNL, DRI, DHP, EXP,
		APP0, APP1, APP2, APP3, APP4, APP5, APP6, APP7,
	};

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
		if (G_UNLIKELY((p += length) > end))
			break;

		switch (marker) {
		case SOF0:
		case SOF1:
		case SOF2:
		case SOF3:
		case SOF5:
		case SOF6:
		case SOF7:
		case SOF9:
		case SOF10:
		case SOF11:
		case SOF13:
		case SOF14:
		case SOF15:
			if (length >= 5) {
				meta->width = (payload[3] << 8) + payload[4];
				meta->height = (payload[1] << 8) + payload[2];
			}
		}

		// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf 4.7.2
		// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3
		// Not checking the padding byte is intentional.
		// XXX: Thumbnails may in practice overflow into follow-up segments.
		if (meta->exif && marker == APP1 && p - payload >= 6 &&
			!memcmp(payload, "Exif\0", 5) && !meta->exif->len) {
			payload += 6;
			g_byte_array_append(meta->exif, payload, p - payload);
		}

		// https://www.color.org/specification/ICC1v43_2010-12.pdf B.4
		if (meta->icc && marker == APP2 && p - payload >= 14 &&
			!memcmp(payload, "ICC_PROFILE\0", 12) && !icc_done &&
			payload[12] == ++icc_sequence && payload[13] >= payload[12]) {
			payload += 14;
			g_byte_array_append(meta->icc, payload, p - payload);
			icc_done = payload[-1] == icc_sequence;
		}

		// CIPA DC-007-2021 (Multi-Picture Format) 5.2
		// https://www.cipa.jp/e/std/std-sec.html
		if (meta->mpf && marker == APP2 && p - payload >= 8 &&
			!memcmp(payload, "MPF\0", 4) && !meta->mpf->len) {
			payload += 4;
			parse_mpf(meta->mpf, payload, p - payload, end - payload);
		}

		// TODO(p): Extract the main XMP segment.
	}

	if (meta->icc && !icc_done)
		g_byte_array_set_size(meta->icc, 0);
}

static FivIoImage *open_libjpeg_turbo(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error);

static void
load_jpeg_finalize(FivIoImage *image, bool cmyk,
	const FivIoOpenContext *ctx, const char *data, size_t len)
{
	struct jpeg_metadata meta = {
		.exif = g_byte_array_new(),
		.icc = g_byte_array_new(),
		.mpf = g_ptr_array_new(),
	};

	parse_jpeg_metadata(data, len, &meta);

	if (!ctx->first_frame_only) {
		// XXX: This is ugly, as it relies on just the first individual image
		// having any follow-up entries (as it should be).
		FivIoImage *image_tail = image;
		for (guint i = 0; i < meta.mpf->len; i++) {
			const char *jpeg = meta.mpf->pdata[i];
			GError *error = NULL;
			if (!try_append_page(
				open_libjpeg_turbo(jpeg, len - (jpeg - data), ctx, &error),
				&image, &image_tail)) {
				add_warning(ctx, "MPF image %d: %s", i + 2, error->message);
				g_error_free(error);
			}
		}
	}
	g_ptr_array_free(meta.mpf, TRUE);

	if (meta.exif->len)
		image->exif = g_byte_array_free_to_bytes(meta.exif);
	else
		g_byte_array_free(meta.exif, TRUE);

	GBytes *icc_profile = NULL;
	if (meta.icc->len)
		image->icc = icc_profile = g_byte_array_free_to_bytes(meta.icc);
	else
		g_byte_array_free(meta.icc, TRUE);

	FivIoProfile *source = NULL;
	if (icc_profile && ctx->cmm)
		source = fiv_io_cmm_get_profile(ctx->cmm,
			g_bytes_get_data(icc_profile, NULL), g_bytes_get_size(icc_profile));
	else if (image->exif && ctx->cmm)
		source = parse_exif_profile(ctx->cmm,
			g_bytes_get_data(image->exif, NULL), g_bytes_get_size(image->exif));

	if (cmyk)
		fiv_io_cmm_cmyk(ctx->cmm, image, source, ctx->screen_profile);
	else
		fiv_io_cmm_any(ctx->cmm, image, source, ctx->screen_profile);

	if (source)
		fiv_io_profile_free(source);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct libjpeg_error_mgr {
	struct jpeg_error_mgr pub;
	jmp_buf buf;
	GError **error;
	const FivIoOpenContext *ctx;
};

static void
libjpeg_error_exit(j_common_ptr cinfo)
{
	struct libjpeg_error_mgr *err = (struct libjpeg_error_mgr *) cinfo->err;
	char buf[JMSG_LENGTH_MAX] = "";
	(*cinfo->err->format_message)(cinfo, buf);
	set_error(err->error, buf);
	longjmp(err->buf, 1);
}

static void
libjpeg_output_message(j_common_ptr cinfo)
{
	struct libjpeg_error_mgr *err = (struct libjpeg_error_mgr *) cinfo->err;
	char buf[JMSG_LENGTH_MAX] = "";
	(*cinfo->err->format_message)(cinfo, buf);
	add_warning(err->ctx, "%s", buf);
}

static FivIoImage *
load_libjpeg_turbo(const char *data, gsize len, const FivIoOpenContext *ctx,
	void (*loop)(struct jpeg_decompress_struct *, JSAMPARRAY), GError **error)
{
	FivIoImage *volatile image = NULL;

	struct libjpeg_error_mgr jerr = {.error = error, .ctx = ctx};
	struct jpeg_decompress_struct cinfo = {.err = jpeg_std_error(&jerr.pub)};
	jerr.pub.error_exit = libjpeg_error_exit;
	jerr.pub.output_message = libjpeg_output_message;
	if (setjmp(jerr.buf)) {
		g_clear_pointer(&image, fiv_io_image_unref);
		jpeg_destroy_decompress(&cinfo);
		return NULL;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (const unsigned char *) data, len);
	(void) jpeg_read_header(&cinfo, true);
	// TODO(p): With newer libjpeg-turbo, if cinfo.data_precision is 12 or 16,
	// try to load it with higher precision.

	bool use_cmyk = cinfo.jpeg_color_space == JCS_CMYK ||
		cinfo.jpeg_color_space == JCS_YCCK;
	if (use_cmyk)
		cinfo.out_color_space = JCS_CMYK;
	else if (G_BYTE_ORDER == G_BIG_ENDIAN)
		cinfo.out_color_space = JCS_EXT_XRGB;
	else
		cinfo.out_color_space = JCS_EXT_BGRX;

	jpeg_calc_output_dimensions(&cinfo);
	int width = cinfo.output_width;
	int height = cinfo.output_height;

	// The limit of Cairo/pixman is 32767. but JPEG can go as high as 65535.
	// Prevent Cairo from throwing an error, and make use of libjpeg's scaling.
	// gdk-pixbuf circumvents this check, producing unrenderable surfaces.
	const int max = 32767;

	int nfs = 0;
	tjscalingfactor *fs = tjGetScalingFactors(&nfs), f = {0, 1};
	if (fs && (width > max || height > max)) {
		for (int i = 0; i < nfs; i++) {
			if (TJSCALED(width, fs[i]) <= max &&
				TJSCALED(height, fs[i]) <= max &&
				fs[i].num * f.denom > f.num * fs[i].denom)
				f = fs[i];
		}

		add_warning(ctx,
			"the image is too large, and had to be scaled by %d/%d",
			f.num, f.denom);
		width = TJSCALED(width, f);
		height = TJSCALED(height, f);
		cinfo.scale_num = f.num;
		cinfo.scale_denom = f.denom;
	}

	image = fiv_io_image_new(CAIRO_FORMAT_RGB24, width, height);
	if (!image) {
		set_error(error, "image allocation failure");
		longjmp(jerr.buf, 1);
	}

	JSAMPARRAY lines = (*cinfo.mem->alloc_small)(
		(j_common_ptr) &cinfo, JPOOL_IMAGE, sizeof *lines * height);
	for (int i = 0; i < height; i++)
		lines[i] = image->data + i * image->stride;

	// Slightly unfortunate generalization.
	loop(&cinfo, lines);

	load_jpeg_finalize(image, use_cmyk, ctx, data, len);
	jpeg_destroy_decompress(&cinfo);
	return image;
}

static void
load_libjpeg_simple(
	struct jpeg_decompress_struct *cinfo, JSAMPARRAY lines)
{
	(void) jpeg_start_decompress(cinfo);
	while (cinfo->output_scanline < cinfo->output_height)
		(void) jpeg_read_scanlines(cinfo, lines + cinfo->output_scanline,
			cinfo->output_height - cinfo->output_scanline);
	(void) jpeg_finish_decompress(cinfo);
}

#ifdef HAVE_JPEG_QS

static void
load_libjpeg_enhanced(
	struct jpeg_decompress_struct *cinfo, JSAMPARRAY lines)
{
	// Go for the maximum quality setting.
	jpegqs_control_t opts = {
		.flags = JPEGQS_DIAGONALS | JPEGQS_JOINT_YUV,
		.threads = g_get_num_processors(),
		.niter = 3,
	};

	// Waiting for https://github.com/ilyakurdyukov/jpeg-quantsmooth/issues/28
#if LIBJPEG_TURBO_VERSION_NUMBER < 2001090
	opts.flags |= JPEGQS_UPSAMPLE_UV;
#endif

	(void) jpegqs_start_decompress(cinfo, &opts);
	while (cinfo->output_scanline < cinfo->output_height)
		(void) jpeg_read_scanlines(cinfo, lines + cinfo->output_scanline,
			cinfo->output_height - cinfo->output_scanline);
	(void) jpegqs_finish_decompress(cinfo);
}

#else
#define load_libjpeg_enhanced load_libjpeg_simple
#endif

static FivIoImage *
open_libjpeg_turbo(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	return load_libjpeg_turbo(data, len, ctx,
		ctx->enhance ? load_libjpeg_enhanced : load_libjpeg_simple,
		error);
}

// --- WebP --------------------------------------------------------------------

static const char *
load_libwebp_error(VP8StatusCode err)
{
	switch (err) {
	case VP8_STATUS_OK:
		return "OK";
	case VP8_STATUS_OUT_OF_MEMORY:
		return "out of memory";
	case VP8_STATUS_INVALID_PARAM:
		return "invalid parameter";
	case VP8_STATUS_BITSTREAM_ERROR:
		return "bitstream error";
	case VP8_STATUS_UNSUPPORTED_FEATURE:
		return "unsupported feature";
	case VP8_STATUS_SUSPENDED:
		return "suspended";
	case VP8_STATUS_USER_ABORT:
		return "user abort";
	case VP8_STATUS_NOT_ENOUGH_DATA:
		return "not enough data";
	default:
		return "general failure";
	}
}

static FivIoImage *
load_libwebp_nonanimated(WebPDecoderConfig *config, const WebPData *wd,
	const FivIoOpenContext *ctx, GError **error)
{
	FivIoImage *image = fiv_io_image_new(
		config->input.has_alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
		config->input.width, config->input.height);
	if (!image) {
		set_error(error, "image allocation failure");
		return NULL;
	}

	config->options.use_threads = true;

	config->output.width = config->input.width;
	config->output.height = config->input.height;
	config->output.is_external_memory = true;
	config->output.u.RGBA.rgba = image->data;
	config->output.u.RGBA.stride = image->stride;
	config->output.u.RGBA.size = config->output.u.RGBA.stride * image->height;

	bool premultiply = !ctx->screen_profile;
	if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
		config->output.colorspace = premultiply ? MODE_bgrA : MODE_BGRA;
	else
		config->output.colorspace = premultiply ? MODE_Argb : MODE_ARGB;

	WebPIDecoder *idec = WebPIDecode(NULL, 0, config);
	if (!idec) {
		set_error(error, "WebP decoding error");
		fiv_io_image_unref(image);
		return NULL;
	}

	VP8StatusCode err = WebPIUpdate(idec, wd->bytes, wd->size);
	int x = 0, y = 0, w = 0, h = 0;
	(void) WebPIDecodedArea(idec, &x, &y, &w, &h);
	WebPIDelete(idec);
	if (err == VP8_STATUS_OK)
		return image;

	if (err != VP8_STATUS_SUSPENDED) {
		g_set_error(error, FIV_IO_ERROR, FIV_IO_ERROR_OPEN, "%s: %s",
			"WebP decoding error", load_libwebp_error(err));
		fiv_io_image_unref(image);
		return NULL;
	}

	add_warning(ctx, "image file is truncated");
	if (config->input.has_alpha)
		return image;

	// Always use transparent black, rather than opaque black.
	image->format = CAIRO_FORMAT_ARGB32;
	return image;
}

static FivIoImage *
load_libwebp_frame(WebPAnimDecoder *dec, const WebPAnimInfo *info,
	int *last_timestamp, GError **error)
{
	uint8_t *buf = NULL;
	int timestamp = 0;
	if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) {
		set_error(error, "WebP decoding error");
		return NULL;
	}

	uint64_t area = info->canvas_width * info->canvas_height;
	FivIoImage *image = fiv_io_image_new(CAIRO_FORMAT_RGB24,
		info->canvas_width, info->canvas_height);
	if (!image) {
		set_error(error, "image allocation failure");
		return NULL;
	}

	uint32_t *dst = (uint32_t *) image->data;
	if (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
		memcpy(dst, buf, area * sizeof *dst);
	} else {
		const uint32_t *src = (const uint32_t *) buf;
		for (uint64_t i = 0; i < area; i++) {
			uint32_t value = *src++;
			*dst++ = GUINT32_FROM_LE(value);
		}
	}

	// info->bgcolor is not reliable.
	for (const uint32_t *p = dst, *end = dst + area; p < end; p++)
		if ((~*p & 0xff000000)) {
			image->format = CAIRO_FORMAT_ARGB32;
			break;
		}

	// This API is confusing and awkward.
	image->frame_duration = timestamp - *last_timestamp;
	*last_timestamp = timestamp;
	return image;
}

static FivIoImage *
load_libwebp_animated(
	const WebPData *wd, const FivIoOpenContext *ctx, GError **error)
{
	bool premultiply = !ctx->screen_profile;
	WebPAnimDecoderOptions options = {};
	WebPAnimDecoderOptionsInit(&options);
	options.use_threads = true;
	options.color_mode = premultiply ? MODE_bgrA : MODE_BGRA;

	WebPAnimInfo info = {};
	WebPAnimDecoder *dec = WebPAnimDecoderNew(wd, &options);
	WebPAnimDecoderGetInfo(dec, &info);

	FivIoImage *frames = NULL, *frames_tail = NULL;
	if (info.canvas_width > INT_MAX || info.canvas_height > INT_MAX) {
		set_error(error, "image dimensions overflow");
		goto fail;
	}

	int last_timestamp = 0;
	while (WebPAnimDecoderHasMoreFrames(dec)) {
		FivIoImage *image =
			load_libwebp_frame(dec, &info, &last_timestamp, error);
		if (!image) {
			g_clear_pointer(&frames, fiv_io_image_unref);
			goto fail;
		}

		if (frames_tail)
			frames_tail->frame_next = image;
		else
			frames = image;

		image->frame_previous = frames_tail;
		frames_tail = image;
	}

	if (frames) {
		frames->frame_previous = frames_tail;
	} else {
		set_error(error, "the animation has no frames");
		g_clear_pointer(&frames, fiv_io_image_unref);
	}

fail:
	WebPAnimDecoderDelete(dec);
	return frames;
}

static FivIoImage *
open_libwebp(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	// It is wholly zero-initialized by libwebp.
	WebPDecoderConfig config = {};
	if (!WebPInitDecoderConfig(&config)) {
		set_error(error, "libwebp version mismatch");
		return NULL;
	}

	// TODO(p): Differentiate between a bad WebP, and not a WebP.
	VP8StatusCode err = 0;
	WebPData wd = {.bytes = (const uint8_t *) data, .size = len};
	if ((err = WebPGetFeatures(wd.bytes, wd.size, &config.input))) {
		g_set_error(error, FIV_IO_ERROR, FIV_IO_ERROR_OPEN,
			"%s: %s", "WebP decoding error", load_libwebp_error(err));
		return NULL;
	}

	FivIoImage *result = config.input.has_animation
		? load_libwebp_animated(&wd, ctx, error)
		: load_libwebp_nonanimated(&config, &wd, ctx, error);
	if (!result)
		goto fail;

	// Of course everything has to use a different abstraction.
	WebPDemuxState state = WEBP_DEMUX_PARSE_ERROR;
	WebPDemuxer *demux = WebPDemuxPartial(&wd, &state);
	if (!demux) {
		add_warning(ctx, "demux failure while reading metadata");
		goto fail;
	}

	// Releasing the demux chunk iterator is actually a no-op.
	WebPChunkIterator chunk_iter = {};
	uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
	if ((flags & EXIF_FLAG) &&
		WebPDemuxGetChunk(demux, "EXIF", 1, &chunk_iter)) {
		result->exif =
			g_bytes_new(chunk_iter.chunk.bytes, chunk_iter.chunk.size);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if ((flags & ICCP_FLAG) &&
		WebPDemuxGetChunk(demux, "ICCP", 1, &chunk_iter)) {
		result->icc =
			g_bytes_new(chunk_iter.chunk.bytes, chunk_iter.chunk.size);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if ((flags & XMP_FLAG) &&
		WebPDemuxGetChunk(demux, "XMP ", 1, &chunk_iter)) {
		result->xmp =
			g_bytes_new(chunk_iter.chunk.bytes, chunk_iter.chunk.size);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if (WebPDemuxGetChunk(demux, "THUM", 1, &chunk_iter)) {
		result->thum =
			g_bytes_new(chunk_iter.chunk.bytes, chunk_iter.chunk.size);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if (flags & ANIMATION_FLAG)
		result->loops = WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT);

	WebPDemuxDelete(demux);
	if (ctx->screen_profile)
		fiv_io_cmm_argb32_premultiply_page(
			ctx->cmm, result, ctx->screen_profile);

fail:
	WebPFreeDecBuffer(&config.output);
	return result;
}

// --- TIFF/EP + DNG -----------------------------------------------------------
// In Nikon NEF files, which claim to be TIFF/EP-compatible, IFD0 is a tiny
// uncompressed thumbnail with SubIFDs that, aside from raw sensor data,
// typically contain a nearly full-size JPEG preview.
//
// LibRaw takes too long a time to render something that will never be as good
// as that large preview--e.g., due to exposure correction or denoising.
// While since version 0.21.0 the library provides an API that would allow us
// to extract the JPEG, a little bit of custom processing won't hurt either.
// TODO(p): Though it can also extract thumbnails from many more formats,
// so maybe keep this code as a fallback for old or missing LibRaw.
//
// Note that libtiff can only read the horrible IFD0 thumbnail.
// (TIFFSetSubDirectory() requires an ImageLength tag that's missing from JPEG
// SubIFDs, and TIFFReadCustomDirectory() takes a privately defined struct that
// may not be omitted.)

static bool
tiffer_find(const struct tiffer *self, uint16_t tag, struct tiffer_entry *entry)
{
	// Note that we could employ binary search, because tags must be ordered:
	//  - TIFF 6.0: Sort Order
	//  - ISO/DIS 12234-2: 4.1.2, 5.1
	//  - CIPA DC-007-2009 (Multi-Picture Format): 5.2.3., 5.2.4.
	//  - CIPA DC-008-2019 (Exif 2.32): 4.6.2.
	// However, it doesn't seem to warrant the ugly code.
	struct tiffer T = *self;
	while (tiffer_next_entry(&T, entry)) {
		if (entry->tag == tag)
			return true;
	}
	*entry = (struct tiffer_entry) {};
	return false;
}

static bool
tiffer_find_integer(const struct tiffer *self, uint16_t tag, int64_t *i)
{
	struct tiffer_entry entry = {};
	return tiffer_find(self, tag, &entry) && tiffer_integer(self, &entry, i);
}

// In case of failure, an entry with a zero "remaining_count" is returned.
static struct tiffer_entry
tiff_ep_subifds_init(const struct tiffer *T)
{
	struct tiffer_entry entry = {};
	(void) tiffer_find(T, TIFF_SubIFDs, &entry);
	return entry;
}

static bool
tiff_ep_subifds_next(
	const struct tiffer *T, struct tiffer_entry *subifds, struct tiffer *subT)
{
	// XXX: Except for a zero "remaining_count", all conditions are errors,
	// and should perhaps be reported.
	int64_t offset = 0;
	if (!tiffer_integer(T, subifds, &offset) ||
		offset < 0 || offset > UINT32_MAX || !tiffer_subifd(T, offset, subT))
		return false;

	(void) tiffer_next_value(subifds);
	return true;
}

static bool
tiff_ep_find_main(const struct tiffer *T, struct tiffer *outputT)
{
	// This is a mandatory field.
	int64_t type = 0;
	if (!tiffer_find_integer(T, TIFF_NewSubfileType, &type))
		return false;

	// This is the main image.
	// (See DNG rather than ISO/DIS 12234-2 for values.)
	if (type == 0) {
		*outputT = *T;
		return true;
	}

	struct tiffer_entry subifds = tiff_ep_subifds_init(T);
	struct tiffer subT = {};
	while (tiff_ep_subifds_next(T, &subifds, &subT))
		if (tiff_ep_find_main(&subT, outputT))
			return true;
	return false;
}

struct tiff_ep_jpeg {
	const uint8_t *jpeg;                ///< JPEG data stream
	size_t jpeg_length;                 ///< JPEG data stream length
	int64_t pixels;                     ///< Number of pixels in the JPEG
};

static void
tiff_ep_find_jpeg_evaluate(const struct tiffer *T, struct tiff_ep_jpeg *out)
{
	// This is a mandatory field.
	int64_t compression = 0;
	if (!tiffer_find_integer(T, TIFF_Compression, &compression))
		return;

	uint16_t tag_pointer = 0, tag_length = 0;
	switch (compression) {
		// This is how Exif specifies it, which doesn't follow TIFF 6.0.
	case TIFF_Compression_JPEG:
		tag_pointer = TIFF_JPEGInterchangeFormat;
		tag_length = TIFF_JPEGInterchangeFormatLength;
		break;
		// Theoretically, there may be more strips, but this is not expected.
	case TIFF_Compression_JPEGDatastream:
		tag_pointer = TIFF_StripOffsets;
		tag_length = TIFF_StripByteCounts;
		break;
	default:
		return;
	}

	int64_t ipointer = 0, ilength = 0;
	if (!tiffer_find_integer(T, tag_pointer, &ipointer) || ipointer <= 0 ||
		!tiffer_find_integer(T, tag_length, &ilength) || ilength <= 0 ||
		ipointer > T->end - T->begin ||
		T->end - T->begin - ipointer < ilength)
		return;

	// Note that to get the largest JPEG,
	// we don't need to descend into Exif thumbnails.
	// TODO(p): Consider DNG 1.2.0.0 PreviewColorSpace.
	// But first, try to find some real-world files with it.
	const uint8_t *jpeg = T->begin + ipointer;
	size_t jpeg_length = ilength;

	struct jpeg_metadata meta = {};
	parse_jpeg_metadata((const char *) jpeg, jpeg_length, &meta);
	int64_t pixels = meta.width * meta.height;
	if (pixels > out->pixels) {
		out->jpeg = jpeg;
		out->jpeg_length = jpeg_length;
		out->pixels = pixels;
	}
}

static bool
tiff_ep_find_jpeg(const struct tiffer *T, struct tiff_ep_jpeg *out)
{
	// This is a mandatory field.
	int64_t type = 0;
	if (!tiffer_find_integer(T, TIFF_NewSubfileType, &type))
		return false;

	// This is a thumbnail of the main image.
	// (See DNG rather than ISO/DIS 12234-2 for values.)
	if (type == 1)
		tiff_ep_find_jpeg_evaluate(T, out);

	struct tiffer_entry subifds = tiff_ep_subifds_init(T);
	struct tiffer subT = {};
	while (tiff_ep_subifds_next(T, &subifds, &subT))
		if (!tiff_ep_find_jpeg(&subT, out))
			return false;
	return true;
}

static FivIoImage *
load_tiff_ep(
	const struct tiffer *T, const FivIoOpenContext *ctx, GError **error)
{
	// ISO/DIS 12234-2 is a fuck-up that says this should be in "IFD0",
	// but it might have intended to say "all top-level IFDs".
	// The DNG specification shares the same problem.
	//
	// In any case, chained TIFFs are relatively rare.
	struct tiffer_entry entry = {};
	bool is_tiffep = tiffer_find(T, TIFF_TIFF_EPStandardID, &entry) &&
		entry.type == TIFFER_BYTE && entry.remaining_count == 4 &&
		entry.p[0] == 1 && !entry.p[1] && !entry.p[2] && !entry.p[3];

	// Apple ProRAW, e.g., does not claim TIFF/EP compatibility,
	// but we should still be able to make sense of it.
	bool is_supported_dng = tiffer_find(T, TIFF_DNGBackwardVersion, &entry) &&
		entry.type == TIFFER_BYTE && entry.remaining_count == 4 &&
		entry.p[0] == 1 && entry.p[1] <= 6 && !entry.p[2] && !entry.p[3];
	if (!is_tiffep && !is_supported_dng) {
		set_error(error, "not a supported TIFF/EP or DNG image");
		return NULL;
	}

	struct tiffer fullT = {};
	if (!tiff_ep_find_main(T, &fullT)) {
		set_error(error, "could not find a main image");
		return NULL;
	}

	int64_t width = 0, height = 0;
	if (!tiffer_find_integer(&fullT, TIFF_ImageWidth, &width) ||
		!tiffer_find_integer(&fullT, TIFF_ImageLength, &height) ||
		width <= 0 || height <= 0) {
		set_error(error, "missing or invalid main image dimensions");
		return NULL;
	}

	struct tiff_ep_jpeg out = {};
	if (!tiff_ep_find_jpeg(T, &out)) {
		set_error(error, "error looking for a full-size JPEG preview");
		return NULL;
	}

	// Nikon NEFs seem to generally have a preview above 99 percent,
	// (though some of them may not even reach 50 percent).
	// Be a bit more generous than that with our crop tolerance.
	// TODO(p): Also take into account DNG DefaultCropSize, if present.
	if (out.pixels / ((double) width * height) < 0.95) {
		set_error(error, "could not find a large enough JPEG preview");
		return NULL;
	}

	FivIoImage *image = open_libjpeg_turbo(
		(const char *) out.jpeg, out.jpeg_length, ctx, error);
	if (!image)
		return NULL;

	// Note that Exif may override this later in fiv_io_open_from_data().
	// TODO(p): Try to use the Orientation field nearest to the target IFD.
	// IFD0 just happens to be fine for Nikon NEF.
	int64_t orientation = 0;
	if (tiffer_find_integer(T, TIFF_Orientation, &orientation) &&
		orientation >= 1 && orientation <= 8) {
		image->orientation = orientation;
	}

	// XXX: AdobeRGB Nikon NEFs can only be distinguished by a ColorSpace tag
	// from within their MakerNote.
	return image;
}

static FivIoImage *
open_tiff_ep(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	// -Wunused-function, we might want to give this its own compile unit.
	(void) tiffer_real;

	struct tiffer T = {};
	if (!tiffer_init(&T, (const uint8_t *) data, len)) {
		set_error(error, "not a TIFF file");
		return NULL;
	}

	FivIoImage *result = NULL, *result_tail = NULL;
	while (tiffer_next_ifd(&T)) {
		if (!try_append_page(
				load_tiff_ep(&T, ctx, error), &result, &result_tail)) {
			g_clear_pointer(&result, fiv_io_image_unref);
			return NULL;
		}
		if (ctx->first_frame_only)
			break;

		// TODO(p): Try to adjust tiffer so that this isn't necessary.
		struct tiffer_entry dummy = {};
		while (tiffer_next_entry(&T, &dummy))
			;
	}
	return result;
}

// --- Optional dependencies ---------------------------------------------------

#ifdef HAVE_LIBRAW  // ---------------------------------------------------------

static FivIoImage *
load_libraw(libraw_data_t *iprc, GError **error)
{
	int err = 0;
	if ((err = libraw_unpack(iprc))) {
		set_error(error, libraw_strerror(err));
		return NULL;
	}

#if 0
	// TODO(p): I'm not sure when this is necessary or useful yet.
	if ((err = libraw_adjust_sizes_info_only(iprc))) {
		set_error(error, libraw_strerror(err));
		return NULL;
	}
#endif

	// TODO(p): Documentation says I should look at the code and do it myself.
	if ((err = libraw_dcraw_process(iprc))) {
		set_error(error, libraw_strerror(err));
		return NULL;
	}

	// FIXME: This is shittily written to iterate over the range of
	// idata.colors, and will be naturally slow.
	libraw_processed_image_t *image = libraw_dcraw_make_mem_image(iprc, &err);
	if (!image) {
		set_error(error, libraw_strerror(err));
		return NULL;
	}

	// This should have been transformed, and kept, respectively.
	if (image->colors != 3 || image->bits != 8) {
		set_error(error, "unexpected number of colours, or bit depth");
		libraw_dcraw_clear_mem(image);
		return NULL;
	}

	FivIoImage *I =
		fiv_io_image_new(CAIRO_FORMAT_RGB24, image->width, image->height);
	if (!I) {
		set_error(error, "image allocation failure");
		libraw_dcraw_clear_mem(image);
		return NULL;
	}

	uint32_t *pixels = (uint32_t *) I->data;
	unsigned char *p = image->data;
	for (ushort y = 0; y < image->height; y++) {
		for (ushort x = 0; x < image->width; x++) {
			*pixels++ = 0xff000000 | (uint32_t) p[0] << 16 |
				(uint32_t) p[1] << 8 | (uint32_t) p[2];
			p += 3;
		}
	}

	libraw_dcraw_clear_mem(image);
	return I;
}

static FivIoImage *
open_libraw(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	// https://github.com/LibRaw/LibRaw/issues/418
	libraw_data_t *iprc = libraw_init(
		LIBRAW_OPIONS_NO_MEMERR_CALLBACK | LIBRAW_OPIONS_NO_DATAERR_CALLBACK);
	if (!iprc) {
		set_error(error, "failed to obtain a LibRaw handle");
		return NULL;
	}

	// TODO(p): Check if we need to set anything for autorotation (sizes.flip).
	iprc->params.use_camera_wb = 1;
	iprc->params.output_color = 1;  // sRGB, TODO(p): Is this used?
	iprc->params.output_bps = 8;    // This should be the default value.

	int err = 0;
	FivIoImage *result = NULL, *result_tail = NULL;
	if ((err = libraw_open_buffer(iprc, (const void *) data, len))) {
		set_error(error, libraw_strerror(err));
		goto out;
	}
	if (!try_append_page(load_libraw(iprc, error), &result, &result_tail) ||
		ctx->first_frame_only)
		goto out;

	for (unsigned i = 1; i < iprc->idata.raw_count; i++) {
		iprc->rawparams.shot_select = i;

		// This library is terrible, we need to start again.
		if ((err = libraw_open_buffer(iprc, (const void *) data, len))) {
			set_error(error, libraw_strerror(err));
			g_clear_pointer(&result, fiv_io_image_unref);
			goto out;
		}
		if (!try_append_page(load_libraw(iprc, error), &result, &result_tail)) {
			g_clear_pointer(&result, fiv_io_image_unref);
			goto out;
		}
	}

out:
	libraw_close(iprc);
	return fiv_io_cmm_finish(ctx->cmm, result, ctx->screen_profile);
}

#endif  // HAVE_LIBRAW ---------------------------------------------------------
#ifdef HAVE_RESVG  // ----------------------------------------------------------

typedef struct {
	FivIoRenderClosure parent;
	resvg_render_tree *tree;            ///< Loaded resvg tree
	double width;                       ///< Normal width
	double height;                      ///< Normal height
} FivIoRenderClosureResvg;

static void
load_resvg_destroy(FivIoRenderClosure *closure)
{
	FivIoRenderClosureResvg *self = (void *) closure;
	resvg_tree_destroy(self->tree);
	g_free(self);
}

static FivIoImage *
load_resvg_render_internal(FivIoRenderClosureResvg *self, double scale,
	FivIoCmm *cmm, FivIoProfile *target, GError **error)
{
	double w = ceil(self->width * scale), h = ceil(self->height * scale);
	if (w > SHRT_MAX || h > SHRT_MAX) {
		set_error(error, "image dimensions overflow");
		return NULL;
	}

	FivIoImage *image = fiv_io_image_new(CAIRO_FORMAT_ARGB32, w, h);
	if (!image) {
		set_error(error, "image allocation failure");
		return NULL;
	}

	uint32_t *pixels = (uint32_t *) image->data;
#if RESVG_MAJOR_VERSION == 0 && RESVG_MINOR_VERSION < 33
	resvg_fit_to fit_to = {
		scale == 1 ? RESVG_FIT_TO_TYPE_ORIGINAL : RESVG_FIT_TO_TYPE_ZOOM,
		scale};
	resvg_render(self->tree, fit_to, resvg_transform_identity(),
		image->width, image->height, (char *) pixels);
#else
	resvg_render(self->tree, (resvg_transform) {.a = scale, .d = scale},
		image->width, image->height, (char *) pixels);
#endif

	for (int i = 0; i < w * h; i++) {
		uint32_t rgba = g_ntohl(pixels[i]);
		pixels[i] = rgba << 24 | rgba >> 8;
	}
	return fiv_io_cmm_finish(cmm, image, target);
}

static FivIoImage *
load_resvg_render(FivIoRenderClosure *closure,
	FivIoCmm *cmm, FivIoProfile *target, double scale)
{
	FivIoRenderClosureResvg *self = (FivIoRenderClosureResvg *) closure;
	return load_resvg_render_internal(self, scale, cmm, target, NULL);
}

static const char *
load_resvg_error(int err)
{
	switch (err) {
	case RESVG_ERROR_NOT_AN_UTF8_STR:
		return "not a UTF-8 string";
	case RESVG_ERROR_FILE_OPEN_FAILED:
		return "I/O failure";
	case RESVG_ERROR_MALFORMED_GZIP:
		return "malformed gzip";
	case RESVG_ERROR_ELEMENTS_LIMIT_REACHED:
		return "element limit reached";
	case RESVG_ERROR_INVALID_SIZE:
		return "invalid or unspecified image size";
	case RESVG_ERROR_PARSING_FAILED:
		return "parsing failed";
	default:
		return "general failure";
	}
}

static FivIoImage *
open_resvg(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	GFile *file = g_file_new_for_uri(ctx->uri);
	GFile *base_file = g_file_get_parent(file);
	g_object_unref(file);

	resvg_options *opt = resvg_options_create();
	resvg_options_load_system_fonts(opt);
	if (base_file)
		resvg_options_set_resources_dir(opt, g_file_peek_path(base_file));
	if (ctx->screen_dpi)
		resvg_options_set_dpi(opt, ctx->screen_dpi);

	resvg_render_tree *tree = NULL;
	int err = resvg_parse_tree_from_data(data, len, opt, &tree);
	resvg_options_destroy(opt);
	g_clear_object(&base_file);
	if (err != RESVG_OK) {
		set_error(error, load_resvg_error(err));
		return NULL;
	}

	// TODO(p): See if there is a situation for resvg_get_image_viewbox().
	resvg_size size = resvg_get_image_size(tree);

	FivIoRenderClosureResvg *closure = g_malloc0(sizeof *closure);
	closure->parent.render = load_resvg_render;
	closure->parent.destroy = load_resvg_destroy;
	closure->tree = tree;
	closure->width = size.width;
	closure->height = size.height;

	FivIoImage *image = load_resvg_render_internal(
		closure, 1., ctx->cmm, ctx->screen_profile, error);
	if (!image) {
		load_resvg_destroy(&closure->parent);
		return NULL;
	}

	image->render = &closure->parent;
	return image;
}

#endif  // HAVE_RESVG ----------------------------------------------------------
#ifdef HAVE_LIBRSVG  // --------------------------------------------------------

typedef struct {
	FivIoRenderClosure parent;
	RsvgHandle *handle;                 ///< Loaded rsvg handle
	double width;                       ///< Normal width
	double height;                      ///< Normal height
} FivIoRenderClosureLibrsvg;

static void
load_librsvg_destroy(FivIoRenderClosure *closure)
{
	FivIoRenderClosureLibrsvg *self = (void *) closure;
	g_object_unref(self->handle);
	g_free(self);
}

static FivIoImage *
load_librsvg_render_internal(FivIoRenderClosureLibrsvg *self, double scale,
	FivIoCmm *cmm, FivIoProfile *target, GError **error)
{
	RsvgRectangle viewport = {.x = 0, .y = 0,
		.width = self->width * scale, .height = self->height * scale};
	FivIoImage *image = fiv_io_image_new(
		CAIRO_FORMAT_ARGB32, ceil(viewport.width), ceil(viewport.height));
	if (!image) {
		set_error(error, "image allocation failure");
		return NULL;
	}

	cairo_surface_t *surface = fiv_io_image_to_surface_noref(image);
	cairo_t *cr = cairo_create(surface);
	cairo_surface_destroy(surface);
	gboolean success =
		rsvg_handle_render_document(self->handle, cr, &viewport, error);
	cairo_status_t status = cairo_status(cr);
	cairo_destroy(cr);
	if (!success) {
		fiv_io_image_unref(image);
		return NULL;
	}
	if (status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(status));
		fiv_io_image_unref(image);
		return NULL;
	}
	return fiv_io_cmm_finish(cmm, image, target);
}

static FivIoImage *
load_librsvg_render(FivIoRenderClosure *closure,
	FivIoCmm *cmm, FivIoProfile *target, double scale)
{
	FivIoRenderClosureLibrsvg *self = (FivIoRenderClosureLibrsvg *) closure;
	return load_librsvg_render_internal(self, scale, cmm, target, NULL);
}

static FivIoImage *
open_librsvg(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	GFile *base_file = g_file_new_for_uri(ctx->uri);
	GInputStream *is = g_memory_input_stream_new_from_data(data, len, NULL);
	RsvgHandle *handle = rsvg_handle_new_from_stream_sync(
		is, base_file, RSVG_HANDLE_FLAG_KEEP_IMAGE_DATA, NULL, error);
	g_object_unref(base_file);
	g_object_unref(is);
	if (!handle)
		return NULL;

	rsvg_handle_set_dpi(handle, ctx->screen_dpi);

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
		rsvg_handle_get_intrinsic_dimensions(
			handle, NULL, NULL, NULL, NULL, &has_viewport, &viewbox);
		if (!has_viewport) {
			set_error(error, "cannot compute pixel dimensions");
			g_object_unref(handle);
			return NULL;
		}

		w = viewbox.width;
		h = viewbox.height;
	}

	FivIoRenderClosureLibrsvg *closure = g_malloc0(sizeof *closure);
	closure->parent.render = load_librsvg_render;
	closure->parent.destroy = load_librsvg_destroy;
	closure->handle = handle;
	closure->width = w;
	closure->height = h;

	// librsvg rasterizes filters, so rendering to a recording Cairo surface
	// has been abandoned.
	FivIoImage *image = load_librsvg_render_internal(
		closure, 1., ctx->cmm, ctx->screen_profile, error);
	if (!image) {
		load_librsvg_destroy(&closure->parent);
		return NULL;
	}

	image->render = &closure->parent;
	return image;
}

#endif  // HAVE_LIBRSVG --------------------------------------------------------
#ifdef HAVE_XCURSOR  //---------------------------------------------------------

// fmemopen is part of POSIX-1.2008, this exercise is technically unnecessary.
// libXcursor checks for EOF rather than -1, it may eat your hamster.
struct fiv_io_xcursor {
	XcursorFile parent;
	unsigned char *data;
	long position, len;
};

static int
fiv_io_xcursor_read(XcursorFile *file, unsigned char *buf, int len)
{
	struct fiv_io_xcursor *fix = (struct fiv_io_xcursor *) file;
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
fiv_io_xcursor_write(G_GNUC_UNUSED XcursorFile *file,
	G_GNUC_UNUSED unsigned char *buf, G_GNUC_UNUSED int len)
{
	errno = EBADF;
	return -1;
}

static int
fiv_io_xcursor_seek(XcursorFile *file, long offset, int whence)
{
	struct fiv_io_xcursor *fix = (struct fiv_io_xcursor *) file;
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

static const XcursorFile fiv_io_xcursor_adaptor = {
	.closure = NULL,
	.read    = fiv_io_xcursor_read,
	.write   = fiv_io_xcursor_write,
	.seek    = fiv_io_xcursor_seek,
};

static FivIoImage *
open_xcursor(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	if (len > G_MAXLONG) {
		set_error(error, "size overflow");
		return NULL;
	}

	struct fiv_io_xcursor file = {
		.parent = fiv_io_xcursor_adaptor,
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
	FivIoImage *pages = NULL, *frames_head = NULL, *frames_tail = NULL;

	// XXX: Assuming that all "nominal sizes" have the same dimensions.
	XcursorDim last_nominal = -1;
	for (int i = 0; i < images->nimage; i++) {
		XcursorImage *image = images->images[i];

		FivIoImage *I =
			fiv_io_image_new(CAIRO_FORMAT_ARGB32, image->width, image->height);
		if (!I) {
			add_warning(ctx, "%s", "image allocation failure");
			break;
		}

		// The library automatically byte swaps in _XcursorReadImage().
		memcpy(I->data, image->pixels, I->stride * I->height);
		I->frame_duration = image->delay;

		if (pages && image->size == last_nominal) {
			I->frame_previous = frames_tail;
			frames_tail->frame_next = I;
		} else if (frames_head) {
			frames_head->frame_previous = frames_tail;

			frames_head->page_next = I;
			I->page_previous = frames_head;
			frames_head = I;
		} else {
			pages = frames_head = I;
		}

		frames_tail = I;
		last_nominal = image->size;
	}
	XcursorImagesDestroy(images);
	if (!pages)
		return NULL;

	// Wrap around animations in the last page.
	frames_head->frame_previous = frames_tail;

	// Do not bother doing colour correction, there is no correct rendering.
	return pages;
}

#endif  // HAVE_XCURSOR --------------------------------------------------------
#ifdef HAVE_LIBHEIF  //---------------------------------------------------------

static FivIoImage *
load_libheif_image(struct heif_image_handle *handle, GError **error)
{
	FivIoImage *I = NULL;
	int has_alpha = heif_image_handle_has_alpha_channel(handle);
	int bit_depth = heif_image_handle_get_luma_bits_per_pixel(handle);
	if (bit_depth < 0) {
		set_error(error, "undefined bit depth");
		goto fail;
	}

	// Setting `convert_hdr_to_8bit` seems to be a no-op for RGBA32/64.
	struct heif_decoding_options *opts = heif_decoding_options_alloc();

	// TODO(p): We can get 16-bit depth, in reality most likely 10-bit.
	struct heif_image *image = NULL;
	struct heif_error err = heif_decode_image(handle, &image,
		heif_colorspace_RGB, heif_chroma_interleaved_RGBA, opts);
	if (err.code != heif_error_Ok) {
		set_error(error, err.message);
		goto fail_decode;
	}

	int w = heif_image_get_width(image, heif_channel_interleaved);
	int h = heif_image_get_height(image, heif_channel_interleaved);

	I = fiv_io_image_new(
		has_alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, w, h);
	if (!I) {
		set_error(error, "image allocation failure");
		goto fail_process;
	}

	// As of writing, the library is using 16-byte alignment, unlike Cairo.
	int src_stride = 0;
	const uint8_t *src = heif_image_get_plane_readonly(
		image, heif_channel_interleaved, &src_stride);
	for (int y = 0; y < h; y++) {
		uint32_t *dstp = (uint32_t *) (I->data + I->stride * y);
		const uint32_t *srcp = (const uint32_t *) (src + src_stride * y);
		for (int x = 0; x < w; x++) {
			uint32_t rgba = g_ntohl(srcp[x]);
			*dstp++ = rgba << 24 | rgba >> 8;
		}
	}

	// TODO(p): Test real behaviour on real transparent images.
	if (has_alpha && !heif_image_handle_is_premultiplied_alpha(handle))
		fiv_io_premultiply_argb32(I);

	heif_item_id exif_id = 0;
	if (heif_image_handle_get_list_of_metadata_block_IDs(
			handle, "Exif", &exif_id, 1)) {
		size_t exif_len = heif_image_handle_get_metadata_size(handle, exif_id);
		void *exif = g_malloc0(exif_len);
		err = heif_image_handle_get_metadata(handle, exif_id, exif);
		if (err.code) {
			g_warning("%s", err.message);
			g_free(exif);
		} else {
			I->exif = g_bytes_new_take(exif, exif_len);
		}
	}

	// https://loc.gov/preservation/digital/formats/fdd/fdd000526.shtml#factors
	if (heif_image_handle_get_color_profile_type(handle) ==
		heif_color_profile_type_prof) {
		size_t icc_len = heif_image_handle_get_raw_color_profile_size(handle);
		void *icc = g_malloc0(icc_len);
		err = heif_image_handle_get_raw_color_profile(handle, icc);
		if (err.code) {
			g_warning("%s", err.message);
			g_free(icc);
		} else {
			I->icc = g_bytes_new_take(icc, icc_len);
		}
	}

fail_process:
	heif_image_release(image);
fail_decode:
	heif_decoding_options_free(opts);
fail:
	return I;
}

static void
load_libheif_aux_images(const FivIoOpenContext *ioctx,
	struct heif_image_handle *top,
	FivIoImage **result, FivIoImage **result_tail)
{
	// Include the depth image, we have no special processing for it now.
	int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA;

	int n = heif_image_handle_get_number_of_auxiliary_images(top, filter);
	heif_item_id *ids = g_malloc0_n(n, sizeof *ids);
	n = heif_image_handle_get_list_of_auxiliary_image_IDs(top, filter, ids, n);
	for (int i = 0; i < n; i++) {
		struct heif_image_handle *handle = NULL;
		struct heif_error err =
			heif_image_handle_get_auxiliary_image_handle(top, ids[i], &handle);
		if (err.code != heif_error_Ok) {
			add_warning(ioctx, "%s", err.message);
			continue;
		}

		GError *e = NULL;
		if (!try_append_page(
				load_libheif_image(handle, &e), result, result_tail)) {
			add_warning(ioctx, "%s", e->message);
			g_error_free(e);
		}

		heif_image_handle_release(handle);
	}

	g_free(ids);
}

static FivIoImage *
open_libheif(
	const char *data, gsize len, const FivIoOpenContext *ioctx, GError **error)
{
	// libheif will throw C++ exceptions on allocation failures.
	// The library is generally awful through and through.
	struct heif_context *ctx = heif_context_alloc();
	FivIoImage *result = NULL, *result_tail = NULL;

	struct heif_error err;
	err = heif_context_read_from_memory_without_copy(ctx, data, len, NULL);
	if (err.code != heif_error_Ok) {
		set_error(error, err.message);
		goto fail_read;
	}

	int n = heif_context_get_number_of_top_level_images(ctx);
	heif_item_id *ids = g_malloc0_n(n, sizeof *ids);
	n = heif_context_get_list_of_top_level_image_IDs(ctx, ids, n);
	for (int i = 0; i < n; i++) {
		struct heif_image_handle *handle = NULL;
		err = heif_context_get_image_handle(ctx, ids[i], &handle);
		if (err.code != heif_error_Ok) {
			add_warning(ioctx, "%s", err.message);
			continue;
		}

		GError *e = NULL;
		if (!try_append_page(
				load_libheif_image(handle, &e), &result, &result_tail)) {
			add_warning(ioctx, "%s", e->message);
			g_error_free(e);
		}

		// TODO(p): Possibly add thumbnail images as well.
		load_libheif_aux_images(ioctx, handle, &result, &result_tail);
		heif_image_handle_release(handle);
	}
	if (!result) {
		g_clear_pointer(&result, fiv_io_image_unref);
		set_error(error, "empty or unsupported image");
	}

	g_free(ids);
fail_read:
	heif_context_free(ctx);
	return fiv_io_cmm_finish(ioctx->cmm, result, ioctx->screen_profile);
}

#endif  // HAVE_LIBHEIF --------------------------------------------------------
#ifdef HAVE_LIBTIFF  //---------------------------------------------------------

struct fiv_io_tiff {
	const FivIoOpenContext *ctx;
	unsigned char *data;
	gchar *error;

	// No, libtiff, the offset is not supposed to be unsigned (also see:
	// man 0p sys_types.h), but at least it's fewer cases for us to care about.
	toff_t position, len;
};

static tsize_t
fiv_io_tiff_read(thandle_t h, tdata_t buf, tsize_t len)
{
	struct fiv_io_tiff *io = h;
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
fiv_io_tiff_write(G_GNUC_UNUSED thandle_t h,
	G_GNUC_UNUSED tdata_t buf, G_GNUC_UNUSED tsize_t len)
{
	errno = EBADF;
	return -1;
}

static toff_t
fiv_io_tiff_seek(thandle_t h, toff_t offset, int whence)
{
	struct fiv_io_tiff *io = h;
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
fiv_io_tiff_close(G_GNUC_UNUSED thandle_t h)
{
	return 0;
}

static toff_t
fiv_io_tiff_size(thandle_t h)
{
	return ((struct fiv_io_tiff *) h)->len;
}

static void
fiv_io_tiff_error(
	thandle_t h, const char *module, const char *format, va_list ap)
{
	struct fiv_io_tiff *io = h;
	gchar *message = g_strdup_vprintf(format, ap);
	if (io->error)
		// I'm not sure if two errors can ever come in a succession,
		// but make sure to log them in any case.
		add_warning(io->ctx, "%s: %s", module, message);
	else
		io->error = g_strconcat(module, ": ", message, NULL);
	g_free(message);
}

static void
fiv_io_tiff_warning(G_GNUC_UNUSED thandle_t h,
	const char *module, const char *format, va_list ap)
{
	gchar *message = g_strdup_vprintf(format, ap);
	g_debug("tiff: %s: %s", module, message);
	g_free(message);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static FivIoImage *
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

	FivIoImage *I = NULL;
	if (image.width > G_MAXINT || image.height >= G_MAXINT ||
		G_MAXUINT32 / image.width < image.height) {
		set_error(error, "image dimensions too large");
		goto fail;
	}

	I = fiv_io_image_new(image.alpha != EXTRASAMPLE_UNSPECIFIED
			? CAIRO_FORMAT_ARGB32
			: CAIRO_FORMAT_RGB24,
		image.width, image.height);
	if (!I) {
		set_error(error, "image allocation failure");
		goto fail;
	}

	image.req_orientation = ORIENTATION_LEFTTOP;
	uint32_t *raster = (uint32_t *) I->data;
	if (!TIFFRGBAImageGet(&image, raster, image.width, image.height)) {
		g_clear_pointer(&I, fiv_io_image_unref);
		goto fail;
	}

	// Needs to be byte-swapped from ABGR to alpha-premultiplied ARGB for Cairo.
	for (uint64_t i = image.width * image.height; i--; ) {
		uint32_t pixel = raster[i];
		raster[i] = TIFFGetA(pixel) << 24 | TIFFGetR(pixel) << 16 |
			TIFFGetG(pixel) << 8 | TIFFGetB(pixel);
	}
	// It seems that neither GIMP nor Photoshop use unassociated alpha.
	if (image.alpha == EXTRASAMPLE_UNASSALPHA)
		fiv_io_premultiply_argb32(I);

	// XXX: The whole file is essentially an Exif, any ideas?

	// TODO(p): TIFF has a number of fields that an ICC profile can be
	// constructed from--it's not a good idea to blindly default to sRGB
	// if we don't find an ICC profile.
	const uint32_t meta_len = 0;
	const void *meta = NULL;
	if (TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &meta_len, &meta))
		I->icc = g_bytes_new(meta, meta_len);
	if (TIFFGetField(tiff, TIFFTAG_XMLPACKET, &meta_len, &meta))
		I->xmp = g_bytes_new(meta, meta_len);

	// Don't ask. The API is high, alright, I'm just not sure about the level.
	uint16_t orientation = 0;
	if (TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation)) {
		if (orientation == 5 || orientation == 7)
			I->orientation = 5;
		if (orientation == 6 || orientation == 8)
			I->orientation = 7;
	}

fail:
	TIFFRGBAImageEnd(&image);
	// TODO(p): It's possible to implement ClipPath easily with Cairo.
	return I;
}

static FivIoImage *
open_libtiff(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	// Both kinds of handlers are called, redirect everything.
	TIFFErrorHandler eh = TIFFSetErrorHandler(NULL);
	TIFFErrorHandler wh = TIFFSetWarningHandler(NULL);
	TIFFErrorHandlerExt ehe = TIFFSetErrorHandlerExt(fiv_io_tiff_error);
	TIFFErrorHandlerExt whe = TIFFSetWarningHandlerExt(fiv_io_tiff_warning);
	struct fiv_io_tiff h = {
		.ctx = ctx,
		.data = (unsigned char *) data,
		.position = 0,
		.len = len,
	};

	FivIoImage *result = NULL, *result_tail = NULL;
	TIFF *tiff = TIFFClientOpen(ctx->uri, "rm" /* Avoid mmap. */, &h,
		fiv_io_tiff_read, fiv_io_tiff_write, fiv_io_tiff_seek,
		fiv_io_tiff_close, fiv_io_tiff_size, NULL, NULL);
	if (!tiff)
		goto fail;

	do {
		// We inform about unsupported directories, but do not fail on them.
		GError *err = NULL;
		if (!try_append_page(
				load_libtiff_directory(tiff, &err), &result, &result_tail) &&
			err) {
			add_warning(ctx, "%s", err->message);
			g_error_free(err);
		}
	} while (TIFFReadDirectory(tiff));
	TIFFClose(tiff);

fail:
	if (h.error) {
		g_clear_pointer(&result, fiv_io_image_unref);
		set_error(error, h.error);
		g_free(h.error);
	} else if (!result) {
		set_error(error, "empty or unsupported image");
	}

	TIFFSetErrorHandlerExt(ehe);
	TIFFSetWarningHandlerExt(whe);
	TIFFSetErrorHandler(eh);
	TIFFSetWarningHandler(wh);
	return fiv_io_cmm_finish(ctx->cmm, result, ctx->screen_profile);
}

#endif  // HAVE_LIBTIFF --------------------------------------------------------
#ifdef HAVE_GDKPIXBUF  // ------------------------------------------------------

static FivIoImage *
load_gdkpixbuf_argb32_unpremultiplied(GdkPixbuf *pixbuf)
{
	int w = gdk_pixbuf_get_width(pixbuf);
	int h = gdk_pixbuf_get_height(pixbuf);
	FivIoImage *image = fiv_io_image_new(CAIRO_FORMAT_ARGB32, w, h);
	if (!image)
		return NULL;

	guint length = 0;
	guchar *src = gdk_pixbuf_get_pixels_with_length(pixbuf, &length);
	int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
	uint32_t *dst = (uint32_t *) image->data;
	for (int y = 0; y < h; y++) {
		const guchar *p = src + y * src_stride;
		for (int x = 0; x < w; x++) {
			*dst++ = (uint32_t) p[3] << 24 | p[0] << 16 | p[1] << 8 | p[2];
			p += 4;
		}
	}
	return image;
}

static FivIoImage *
open_gdkpixbuf(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	// gdk-pixbuf controls the playback itself, there is no reliable method of
	// extracting individual frames (due to loops).
	GInputStream *is = g_memory_input_stream_new_from_data(data, len, NULL);
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(is, NULL, error);
	g_object_unref(is);
	if (!pixbuf)
		return NULL;

	bool custom_argb32 = ctx->screen_profile &&
		gdk_pixbuf_get_has_alpha(pixbuf) &&
		gdk_pixbuf_get_colorspace(pixbuf) == GDK_COLORSPACE_RGB &&
		gdk_pixbuf_get_n_channels(pixbuf) == 4 &&
		gdk_pixbuf_get_bits_per_sample(pixbuf) == 8;

	FivIoImage *image = NULL;
	if (custom_argb32) {
		image = load_gdkpixbuf_argb32_unpremultiplied(pixbuf);
	} else if ((image = fiv_io_image_new(CAIRO_FORMAT_ARGB32,
			gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf)))) {
		// TODO(p): Ideally, don't go through Cairo at all.
		cairo_surface_t *surface = fiv_io_image_to_surface_noref(image);
		cairo_t *cr = cairo_create(surface);
		cairo_surface_destroy(surface);

		// Don't depend on GDK being initialized, to speed up thumbnailing
		// (calling gdk_cairo_surface_create_from_pixbuf() would).
		gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);

		// If the source was opaque, so will be the destination.
		if (cairo_pattern_get_surface(cairo_get_source(cr), &surface) ==
			CAIRO_STATUS_SUCCESS) {
			if (cairo_surface_get_content(surface) == CAIRO_CONTENT_COLOR)
				image->format = CAIRO_FORMAT_RGB24;
		}
		cairo_destroy(cr);
	}

	if (!image) {
		set_error(error, "image allocation failure");
		g_object_unref(pixbuf);
		return NULL;
	}

	const char *orientation = gdk_pixbuf_get_option(pixbuf, "orientation");
	if (orientation && strlen(orientation) == 1) {
		int n = *orientation - '0';
		if (n >= 1 && n <= 8)
			image->orientation = n;
	}

	const char *icc_profile = gdk_pixbuf_get_option(pixbuf, "icc-profile");
	if (icc_profile) {
		gsize out_len = 0;
		guchar *raw = g_base64_decode(icc_profile, &out_len);
		if (raw)
			image->icc = g_bytes_new_take(raw, out_len);
	}

	g_object_unref(pixbuf);
	if (custom_argb32)
		fiv_io_cmm_argb32_premultiply_page(
			ctx->cmm, image, ctx->screen_profile);
	else
		image = fiv_io_cmm_finish(ctx->cmm, image, ctx->screen_profile);
	return image;
}

#endif  // HAVE_GDKPIXBUF ------------------------------------------------------

FivIoImage *
fiv_io_open(const FivIoOpenContext *ctx, GError **error)
{
	// TODO(p): Don't always load everything into memory, test type first,
	// so that we can reject non-pictures early.  Wuffs only needs the first
	// 17 bytes to make a guess right now.
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
	GFile *file = g_file_new_for_uri(ctx->uri);

	gchar *data = NULL;
	gsize len = 0;
	gboolean success =
		g_file_load_contents(file, NULL, &data, &len, NULL, error);
	g_object_unref(file);
	if (!success)
		return NULL;

	FivIoImage *image = fiv_io_open_from_data(data, len, ctx, error);
	g_free(data);
	return image;
}

FivIoImage *
fiv_io_open_from_data(
	const char *data, size_t len, const FivIoOpenContext *ctx, GError **error)
{
	wuffs_base__slice_u8 prefix =
		wuffs_base__make_slice_u8((uint8_t *) data, len);

	FivIoImage *image = NULL;
	switch (wuffs_base__magic_number_guess_fourcc(prefix, true /* closed */)) {
	case WUFFS_BASE__FOURCC__BMP:
		// Note that BMP can redirect into another format,
		// which is so far unsupported here.
		image = open_wuffs_using(
			wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			ctx, error);
		break;
	case WUFFS_BASE__FOURCC__GIF:
		image = open_wuffs_using(
			wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			ctx, error);
		break;
	case WUFFS_BASE__FOURCC__PNG:
		image = open_wuffs_using(
			wuffs_png__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			ctx, error);
		break;
	case WUFFS_BASE__FOURCC__TGA:
		image = open_wuffs_using(
			wuffs_tga__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			ctx, error);
		break;
	case WUFFS_BASE__FOURCC__JPEG:
		image = open_libjpeg_turbo(data, len, ctx, error);
		break;
	case WUFFS_BASE__FOURCC__WEBP:
		image = open_libwebp(data, len, ctx, error);
		break;
	default:
		// Try to extract full-size previews from TIFF/EP-compatible raws,
		// but allow for running the full render.
#ifdef HAVE_LIBRAW  // ---------------------------------------------------------
		if (!ctx->enhance) {
#endif  // HAVE_LIBRAW ---------------------------------------------------------
		if ((image = open_tiff_ep(data, len, ctx, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#ifdef HAVE_LIBRAW  // ---------------------------------------------------------
		}
		if ((image = open_libraw(data, len, ctx, error)))
			break;

		// TODO(p): We should try to pass actual processing errors through,
		// notably only continue with LIBRAW_FILE_UNSUPPORTED.
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_LIBRAW ---------------------------------------------------------
#ifdef HAVE_RESVG  // ----------------------------------------------------------
		if ((image = open_resvg(data, len, ctx, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_RESVG ----------------------------------------------------------
#ifdef HAVE_LIBRSVG  // --------------------------------------------------------
		if ((image = open_librsvg(data, len, ctx, error)))
			break;

		// XXX: It doesn't look like librsvg can return sensible errors.
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_LIBRSVG --------------------------------------------------------
#ifdef HAVE_XCURSOR  //---------------------------------------------------------
		if ((image = open_xcursor(data, len, ctx, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_XCURSOR --------------------------------------------------------
#ifdef HAVE_LIBHEIF  //---------------------------------------------------------
		if ((image = open_libheif(data, len, ctx, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_LIBHEIF --------------------------------------------------------
#ifdef HAVE_LIBTIFF  //---------------------------------------------------------
		// This needs to be positioned after LibRaw.
		if ((image = open_libtiff(data, len, ctx, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_LIBTIFF --------------------------------------------------------

		set_error(error, "unsupported file type");
	}

#ifdef HAVE_GDKPIXBUF  // ------------------------------------------------------
	// This is used as a last resort, the rest above is special-cased.
	if (!image) {
		GError *err = NULL;
		if ((image = open_gdkpixbuf(data, len, ctx, &err))) {
			g_clear_error(error);
		} else if (!err) {
			// Contrary to documentation, this is a possible outcome (libheif).
		} else if (err->code == GDK_PIXBUF_ERROR_UNKNOWN_TYPE) {
			g_error_free(err);
		} else {
			g_clear_error(error);
			g_propagate_error(error, err);
		}
	}
#endif  // HAVE_GDKPIXBUF ------------------------------------------------------

	// gdk-pixbuf only gives out this single field--cater to its limitations,
	// since we'd really like to have it.
	// TODO(p): The Exif orientation should be ignored in JPEG-XL at minimum.
	gsize exif_len = 0;
	gconstpointer exif_data = NULL;
	if (image && image->exif &&
		(exif_data = g_bytes_get_data(image->exif, &exif_len))) {
		image->orientation = fiv_io_exif_orientation(exif_data, exif_len);
	}
	return image;
}

// --- Thumbnail passing utilities ---------------------------------------------

typedef struct {
	guint64 user_data;
	int width, height, stride, format;
} CairoHeader;

void
fiv_io_serialize_to_stdout(cairo_surface_t *surface, guint64 user_data)
{
	if (!surface || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
		return;

#ifdef G_OS_UNIX
	// Common courtesy, this is never what the user wants.
	if (isatty(fileno(stdout)))
		return;
#endif

	CairoHeader h = {
		.user_data = user_data,
		.width = cairo_image_surface_get_width(surface),
		.height = cairo_image_surface_get_height(surface),
		.stride = cairo_image_surface_get_stride(surface),
		.format = cairo_image_surface_get_format(surface),
	};

	// Cairo lets pixman initialize image surfaces.
	// pixman allocates stride * height, not omitting those trailing bytes.
	const unsigned char *data = cairo_image_surface_get_data(surface);
	if (fwrite(&h, sizeof h, 1, stdout) == 1)
		fwrite(data, 1, h.stride * h.height, stdout);
}

cairo_surface_t *
fiv_io_deserialize(GBytes *bytes, guint64 *user_data)
{
	CairoHeader h = {};
	GByteArray *array = g_bytes_unref_to_array(bytes);
	if (array->len < sizeof h) {
		g_byte_array_unref(array);
		return NULL;
	}

	h = *(CairoHeader *) array->data;
	if (h.width < 1 || h.height < 1 || h.stride < h.width ||
		G_MAXSIZE / (gsize) h.stride < (gsize) h.height ||
		array->len - sizeof h < (gsize) h.stride * (gsize) h.height) {
		g_byte_array_unref(array);
		return NULL;
	}

	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		array->data + sizeof h, h.format, h.width, h.height, h.stride);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		g_byte_array_unref(array);
		return NULL;
	}

	static cairo_user_data_key_t key;
	cairo_surface_set_user_data(
		surface, &key, array, (cairo_destroy_func_t) g_byte_array_unref);
	*user_data = h.user_data;
	return surface;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static cairo_status_t
write_to_byte_array(
	void *closure, const unsigned char *data, unsigned int length)
{
	g_byte_array_append(closure, data, length);
	return CAIRO_STATUS_SUCCESS;
}

GBytes *
fiv_io_serialize_for_search(cairo_surface_t *surface, GError **error)
{
	g_return_val_if_fail(
		surface && cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE,
		NULL);

	cairo_format_t format = cairo_image_surface_get_format(surface);
	if (format == CAIRO_FORMAT_ARGB32) {
		const uint32_t *data =
			(const uint32_t *) cairo_image_surface_get_data(surface);

		bool all_solid = true;
		for (size_t len = cairo_image_surface_get_width(surface) *
			cairo_image_surface_get_height(surface); len--; ) {
			if ((data[len] >> 24) != 0xFF)
				all_solid = false;
		}
		if (all_solid)
			format = CAIRO_FORMAT_RGB24;
	}

	if (format != CAIRO_FORMAT_RGB24) {
#if CAIRO_HAS_PNG_FUNCTIONS
		GByteArray *ba = g_byte_array_new();
		cairo_status_t status =
			cairo_surface_write_to_png_stream(surface, write_to_byte_array, ba);
		if (status == CAIRO_STATUS_SUCCESS)
			return g_byte_array_free_to_bytes(ba);
		g_byte_array_unref(ba);
#endif

		// Last resort: remove transparency by painting over black.
		cairo_surface_t *converted =
			cairo_image_surface_create(CAIRO_FORMAT_RGB24,
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface));
		cairo_t *cr = cairo_create(converted);
		cairo_set_source_surface(cr, surface, 0, 0);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_paint(cr);
		cairo_destroy(cr);
		GBytes *result = fiv_io_serialize_for_search(converted, error);
		cairo_surface_destroy(converted);
		return result;
	}

	tjhandle enc = tjInitCompress();
	if (!enc) {
		set_error(error, tjGetErrorStr2(enc));
		return NULL;
	}

	unsigned char *jpeg = NULL;
	unsigned long length = 0;
	if (tjCompress2(enc, cairo_image_surface_get_data(surface),
			cairo_image_surface_get_width(surface),
			cairo_image_surface_get_stride(surface),
			cairo_image_surface_get_height(surface),
			(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TJPF_BGRX : TJPF_XRGB),
			&jpeg, &length, TJSAMP_444, 90, 0)) {
		set_error(error, tjGetErrorStr2(enc));
		tjFree(jpeg);
		tjDestroy(enc);
		return NULL;
	}

	tjDestroy(enc);
	return g_bytes_new_with_free_func(
		jpeg, length, (GDestroyNotify) tjFree, jpeg);
}

// --- Export ------------------------------------------------------------------

unsigned char *
fiv_io_encode_webp(
	FivIoImage *image, const WebPConfig *config, size_t *len)
{
	if (image->format != CAIRO_FORMAT_ARGB32 &&
		image->format != CAIRO_FORMAT_RGB24) {
		FivIoImage *converted =
			fiv_io_image_new(CAIRO_FORMAT_ARGB32, image->width, image->height);

		cairo_surface_t *surface = fiv_io_image_to_surface_noref(converted);
		cairo_t *cr = cairo_create(surface);
		cairo_surface_destroy(surface);

		surface = fiv_io_image_to_surface_noref(image);
		cairo_set_source_surface(cr, surface, 0, 0);
		cairo_surface_destroy(surface);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);
		cairo_destroy(cr);
		image = converted;
	} else {
		image = fiv_io_image_ref(image);
	}

	WebPMemoryWriter writer = {};
	WebPMemoryWriterInit(&writer);
	WebPPicture picture = {};
	if (!WebPPictureInit(&picture))
		goto fail;

	picture.use_argb = true;
	picture.width = image->width;
	picture.height = image->height;
	if (!WebPPictureAlloc(&picture))
		goto fail;

	// Cairo uses a similar internal format, so we should be able to
	// copy it over and fix up the minor differences.
	// This is written to be easy to follow rather than fast.
	if (picture.argb_stride != (int) image->width ||
		picture.argb_stride * sizeof *picture.argb != image->stride ||
		UINT32_MAX / picture.argb_stride < image->height)
		goto fail_compatibility;

	uint32_t *argb =
		memcpy(picture.argb, image->data, image->stride * image->height);
	if (image->format == CAIRO_FORMAT_ARGB32)
		for (int i = image->height * picture.argb_stride; i-- > 0; argb++)
			*argb = wuffs_base__color_u32_argb_premul__as__color_u32_argb_nonpremul(*argb);
	else
		for (int i = image->height * picture.argb_stride; i-- > 0; argb++)
			*argb |= 0xFF000000;

	// TODO(p): Prevent or propagate VP8_ENC_ERROR_BAD_DIMENSION.
	picture.writer = WebPMemoryWrite;
	picture.custom_ptr = &writer;
	if (!WebPEncode(config, &picture))
		g_debug("WebPEncode: %d\n", picture.error_code);

fail_compatibility:
	WebPPictureFree(&picture);
fail:
	fiv_io_image_unref(image);
	*len = writer.size;
	return writer.mem;
}

static WebPData
encode_lossless_webp(FivIoImage *image)
{
	WebPData bitstream = {};
	WebPConfig config = {};
	if (!WebPConfigInit(&config) || !WebPConfigLosslessPreset(&config, 6))
		return bitstream;

	config.thread_level = true;
	if (!WebPValidateConfig(&config))
		return bitstream;

	bitstream.bytes = fiv_io_encode_webp(image, &config, &bitstream.size);
	return bitstream;
}

static gboolean
encode_webp_image(WebPMux *mux, FivIoImage *frame)
{
	WebPData bitstream = encode_lossless_webp(frame);
	gboolean ok = WebPMuxSetImage(mux, &bitstream, true) == WEBP_MUX_OK;
	WebPDataClear(&bitstream);
	return ok;
}

static gboolean
encode_webp_animation(WebPMux *mux, FivIoImage *page)
{
	gboolean ok = TRUE;
	for (FivIoImage *frame = page; ok && frame; frame = frame->frame_next) {
		WebPMuxFrameInfo info = {
			.bitstream = encode_lossless_webp(frame),
			.duration = frame->frame_duration,
			.id = WEBP_CHUNK_ANMF,
			.dispose_method = WEBP_MUX_DISPOSE_NONE,
			.blend_method = WEBP_MUX_NO_BLEND,
		};
		ok = WebPMuxPushFrame(mux, &info, true) == WEBP_MUX_OK;
		WebPDataClear(&info.bitstream);
	}
	WebPMuxAnimParams params = {
		.bgcolor = 0x00000000,  // BGRA, curiously.
		.loop_count = page->loops,
	};
	return ok && WebPMuxSetAnimationParams(mux, &params) == WEBP_MUX_OK;
}

static gboolean
set_metadata(WebPMux *mux, const char *fourcc, GBytes *data)
{
	if (!data)
		return TRUE;

	gsize len = 0;
	gconstpointer p = g_bytes_get_data(data, &len);
	return WebPMuxSetChunk(mux, fourcc, &(WebPData) {.bytes = p, .size = len},
		false) == WEBP_MUX_OK;
}

gboolean
fiv_io_save(FivIoImage *page, FivIoImage *frame, FivIoProfile *target,
	const char *path, GError **error)
{
	g_return_val_if_fail(page != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	gboolean ok = TRUE;
	WebPMux *mux = WebPMuxNew();
	if (frame)
		ok = encode_webp_image(mux, frame);
	else if (!page->frame_next)
		ok = encode_webp_image(mux, page);
	else
		ok = encode_webp_animation(mux, page);

	ok = ok && set_metadata(mux, "EXIF", page->exif);
	ok = ok && set_metadata(mux, "ICCP", page->icc);
	ok = ok && set_metadata(mux, "XMP ", page->xmp);

	GBytes *iccp = NULL;
	if (ok && target && (iccp = fiv_io_profile_to_bytes(target)))
		ok = set_metadata(mux, "ICCP", iccp);

	WebPData assembled = {};
	WebPDataInit(&assembled);
	if (!(ok = ok && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK))
		set_error(error, "encoding failed");
	else
		ok = g_file_set_contents(
			path, (const gchar *) assembled.bytes, assembled.size, error);

	if (iccp)
		g_bytes_unref(iccp);

	WebPMuxDelete(mux);
	WebPDataClear(&assembled);
	return ok;
}

// --- Metadata ----------------------------------------------------------------

void
fiv_io_orientation_dimensions(
	const FivIoImage *image, FivIoOrientation orientation, double *w, double *h)
{
	switch (orientation) {
	case FivIoOrientation90:
	case FivIoOrientationMirror90:
	case FivIoOrientation270:
	case FivIoOrientationMirror270:
		*w = image->height;
		*h = image->width;
		break;
	default:
		*w = image->width;
		*h = image->height;
	}
}

cairo_matrix_t
fiv_io_orientation_apply(const FivIoImage *image,
	FivIoOrientation orientation, double *width, double *height)
{
	fiv_io_orientation_dimensions(image, orientation, width, height);
	return fiv_io_orientation_matrix(orientation, *width, *height);
}

cairo_matrix_t
fiv_io_orientation_matrix(
	FivIoOrientation orientation, double width, double height)
{
	cairo_matrix_t matrix = {};
	cairo_matrix_init_identity(&matrix);
	switch (orientation) {
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

FivIoOrientation
fiv_io_exif_orientation(const guint8 *tiff, gsize len)
{
	// The "Orientation" tag/field is part of Baseline TIFF 6.0 (1992),
	// it just so happens that Exif is derived from this format.
	// There is no other meaningful placement for this than right in IFD0,
	// describing the main image.
	struct tiffer T = {};
	if (!tiffer_init(&T, tiff, len) || !tiffer_next_ifd(&T))
		return FivIoOrientationUnknown;

	struct tiffer_entry entry = {};
	while (tiffer_next_entry(&T, &entry)) {
		int64_t orientation = 0;
		if (G_UNLIKELY(entry.tag == TIFF_Orientation) &&
			entry.type == TIFFER_SHORT && entry.remaining_count == 1 &&
			tiffer_integer(&T, &entry, &orientation) &&
			orientation >= 1 && orientation <= 8)
			return orientation;
	}
	return FivIoOrientationUnknown;
}

gboolean
fiv_io_save_metadata(const FivIoImage *page, const char *path, GError **error)
{
	g_return_val_if_fail(page != NULL, FALSE);

	FILE *fp = fopen(path, "wb");
	if (!fp) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			"%s: %s", path, g_strerror(errno));
		return FALSE;
	}

	// This does not constitute a valid JPEG codestream--it's a TEM marker
	// (standalone) with trailing nonsense.
	fprintf(fp, "\xFF\001Exiv2");

	gsize len = 0;
	gconstpointer p = NULL;

	// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3
	// I don't care if Exiv2 supports it this way.
	if (page->exif && (p = g_bytes_get_data(page->exif, &len))) {
		while (len) {
			gsize chunk = MIN(len, 0xFFFF - 2 - 6);
			uint8_t header[10] = "\xFF\xE1\000\000Exif\000\000";
			header[2] = (chunk + 2 + 6) >> 8;
			header[3] = (chunk + 2 + 6);

			fwrite(header, 1, sizeof header, fp);
			fwrite(p, 1, chunk, fp);

			len -= chunk;
			p += chunk;
		}
	}

	// https://www.color.org/specification/ICC1v43_2010-12.pdf B.4
	if (page->icc && (p = g_bytes_get_data(page->icc, &len))) {
		gsize limit = 0xFFFF - 2 - 12;
		uint8_t current = 0, total = (len + limit - 1) / limit;
		while (len) {
			gsize chunk = MIN(len, limit);
			uint8_t header[18] = "\xFF\xE2\000\000ICC_PROFILE\000\000\000";
			header[2] = (chunk + 2 + 12 + 2) >> 8;
			header[3] = (chunk + 2 + 12 + 2);
			header[16] = ++current;
			header[17] = total;

			fwrite(header, 1, sizeof header, fp);
			fwrite(p, 1, chunk, fp);

			len -= chunk;
			p += chunk;
		}
	}

	// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3
	// If the main segment overflows, then it's a sign of bad luck,
	// because 1.1.3.1 is way too complex.
	if (page->xmp && (p = g_bytes_get_data(page->xmp, &len))) {
		while (len) {
			gsize chunk = MIN(len, 0xFFFF - 2 - 29);
			uint8_t header[33] =
				"\xFF\xE1\000\000http://ns.adobe.com/xap/1.0/\000";
			header[2] = (chunk + 2 + 29) >> 8;
			header[3] = (chunk + 2 + 29);

			fwrite(header, 1, sizeof header, fp);
			fwrite(p, 1, chunk, fp);
			break;
		}
	}

	fprintf(fp, "\xFF\xD9");
	if (ferror(fp)) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			"%s: %s", path, g_strerror(errno));
		fclose(fp);
		return FALSE;
	}
	if (fclose(fp)) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			"%s: %s", path, g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

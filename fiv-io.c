//
// fiv-io.c: image operations
//
// Copyright (c) 2021 - 2023, Přemysl Eric Janouch <p@janouch.name>
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

#include <cairo.h>
#include <glib.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#ifdef HAVE_JPEG_QS
#include <setjmp.h>
#include <stdio.h>

#include <jpeglib.h>
#include <libjpegqs.h>
#endif  // HAVE_JPEG_QS

// Colour management must be handled before RGB conversions.
#ifdef HAVE_LCMS2
#include <lcms2.h>
#endif  // HAVE_LCMS2

#ifdef HAVE_LIBRAW
#include <libraw.h>
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 21, 0)
#define LIBRAW_OPIONS_NO_MEMERR_CALLBACK 0
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
#include "wuffs-mirror-release-c/release/c/wuffs-v0.3.c"

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

static bool
try_append_page(cairo_surface_t *surface, cairo_surface_t **result,
	cairo_surface_t **result_tail)
{
	if (!surface)
		return false;

	if (*result) {
		cairo_surface_set_user_data(*result_tail, &fiv_io_key_page_next,
			surface, (cairo_destroy_func_t) cairo_surface_destroy);
		cairo_surface_set_user_data(
			surface, &fiv_io_key_page_previous, *result_tail, NULL);
		*result_tail = surface;
	} else {
		*result = *result_tail = surface;
	}
	return true;
}

// --- Colour management -------------------------------------------------------

FivIoProfile
fiv_io_profile_new(const void *data, size_t len)
{
#ifdef HAVE_LCMS2
	return cmsOpenProfileFromMem(data, len);
#else
	(void) data;
	(void) len;
	return NULL;
#endif
}

FivIoProfile
fiv_io_profile_new_sRGB(void)
{
#ifdef HAVE_LCMS2
	return cmsCreate_sRGBProfile();
#else
	return NULL;
#endif
}

FivIoProfile
fiv_io_profile_new_sRGB_gamma(double gamma)
{
#ifdef HAVE_LCMS2
	// TODO(p): Make sure to use the library in a thread-safe manner.
	cmsContext context = NULL;

	static const cmsCIExyY D65 = {0.3127, 0.3290, 1.0};
	static const cmsCIExyYTRIPLE primaries = {
		{0.6400, 0.3300, 1.0}, {0.3000, 0.6000, 1.0}, {0.1500, 0.0600, 1.0}};
	cmsToneCurve *curve = cmsBuildGamma(context, gamma);
	if (!curve)
		return NULL;

	cmsHPROFILE profile = cmsCreateRGBProfileTHR(
		context, &D65, &primaries, (cmsToneCurve *[3]){curve, curve, curve});
	cmsFreeToneCurve(curve);
	return profile;
#else
	(void) gamma;
	return NULL;
#endif
}

static FivIoProfile
fiv_io_profile_new_from_bytes(GBytes *bytes)
{
	gsize len = 0;
	gconstpointer p = g_bytes_get_data(bytes, &len);
	return fiv_io_profile_new(p, len);
}

static GBytes *
fiv_io_profile_to_bytes(FivIoProfile profile)
{
#ifdef HAVE_LCMS2
	cmsUInt32Number len = 0;
	(void) cmsSaveProfileToMem(profile, NULL, &len);
	gchar *data = g_malloc0(len);
	if (!cmsSaveProfileToMem(profile, data, &len)) {
		g_free(data);
		return NULL;
	}
	return g_bytes_new_take(data, len);
#else
	(void) profile;
	return NULL;
#endif
}

void
fiv_io_profile_free(FivIoProfile self)
{
#ifdef HAVE_LCMS2
	cmsCloseProfile(self);
#else
	(void) self;
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// TODO(p): In general, try to use CAIRO_FORMAT_RGB30 or CAIRO_FORMAT_RGBA128F.
#define FIV_IO_LCMS2_ARGB32 \
	(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_8 : TYPE_ARGB_8)
#define FIV_IO_LCMS2_4X16LE \
	(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_16 : TYPE_BGRA_16_SE)

// CAIRO_STRIDE_ALIGNMENT is 4 bytes, so there will be no padding with
// ARGB/BGRA/XRGB/BGRX.
static void
trivial_cmyk_to_host_byte_order_argb(unsigned char *p, int len)
{
	// This CMYK handling has been seen in gdk-pixbuf/JPEG, GIMP/JPEG, skcms.
	// It will typically produce horribly oversaturated results.
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
fiv_io_profile_cmyk(
	cairo_surface_t *surface, FivIoProfile source, FivIoProfile target)
{
	unsigned char *data = cairo_image_surface_get_data(surface);
	int w = cairo_image_surface_get_width(surface);
	int h = cairo_image_surface_get_height(surface);

#ifndef HAVE_LCMS2
	(void) source;
	(void) target;
#else
	cmsHTRANSFORM transform = NULL;
	if (source && target) {
		transform = cmsCreateTransform(source, TYPE_CMYK_8_REV, target,
			FIV_IO_LCMS2_ARGB32, INTENT_PERCEPTUAL, 0);
	}
	if (transform) {
		cmsDoTransform(transform, data, data, w * h);
		cmsDeleteTransform(transform);
		return;
	}
#endif
	trivial_cmyk_to_host_byte_order_argb(data, w * h);
}

static void
fiv_io_profile_xrgb32_direct(unsigned char *data, int w, int h,
	FivIoProfile source, FivIoProfile target)
{
#ifndef HAVE_LCMS2
	(void) data;
	(void) w;
	(void) h;
	(void) source;
	(void) target;
#else
	// TODO(p): We should make this optional.
	cmsHPROFILE src_fallback = NULL;
	if (target && !source)
		source = src_fallback = cmsCreate_sRGBProfile();

	cmsHTRANSFORM transform = NULL;
	if (source && target) {
		transform = cmsCreateTransform(source, FIV_IO_LCMS2_ARGB32, target,
			FIV_IO_LCMS2_ARGB32, INTENT_PERCEPTUAL, 0);
	}
	if (transform) {
		cmsDoTransform(transform, data, data, w * h);
		cmsDeleteTransform(transform);
	}
	if (src_fallback)
		cmsCloseProfile(src_fallback);
#endif
}

static void
fiv_io_profile_xrgb32(
	cairo_surface_t *surface, FivIoProfile source, FivIoProfile target)
{
	unsigned char *data = cairo_image_surface_get_data(surface);
	int w = cairo_image_surface_get_width(surface);
	int h = cairo_image_surface_get_height(surface);
	fiv_io_profile_xrgb32_direct(data, w, h, source, target);
}

static void
fiv_io_profile_4x16le_direct(
	unsigned char *data, int w, int h, FivIoProfile source, FivIoProfile target)
{
#ifndef HAVE_LCMS2
	(void) data;
	(void) w;
	(void) h;
	(void) source;
	(void) target;
#else
	// TODO(p): We should make this optional.
	cmsHPROFILE src_fallback = NULL;
	if (target && !source)
		source = src_fallback = cmsCreate_sRGBProfile();

	cmsHTRANSFORM transform = NULL;
	if (source && target) {
		transform = cmsCreateTransform(source, FIV_IO_LCMS2_4X16LE, target,
			FIV_IO_LCMS2_4X16LE, INTENT_PERCEPTUAL, 0);
	}
	if (transform) {
		cmsDoTransform(transform, data, data, w * h);
		cmsDeleteTransform(transform);
	}
	if (src_fallback)
		cmsCloseProfile(src_fallback);
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
fiv_io_profile_xrgb32_page(cairo_surface_t *page, FivIoProfile target)
{
	GBytes *bytes = NULL;
	FivIoProfile source = NULL;
	if ((bytes = cairo_surface_get_user_data(page, &fiv_io_key_icc)))
		source = fiv_io_profile_new_from_bytes(bytes);

	// TODO(p): All animations need to be composited in a linear colour space.
	for (cairo_surface_t *frame = page; frame != NULL;
		frame = cairo_surface_get_user_data(frame, &fiv_io_key_frame_next))
		fiv_io_profile_xrgb32(frame, source, target);

	if (source)
		fiv_io_profile_free(source);
}

// TODO(p): Offer better integration, upgrade the bit depth if appropriate.
static cairo_surface_t *
fiv_io_profile_finalize(cairo_surface_t *image, FivIoProfile target)
{
	if (!image || !target)
		return image;

	for (cairo_surface_t *page = image; page != NULL;
		page = cairo_surface_get_user_data(page, &fiv_io_key_page_next)) {
		// TODO(p): 1. un/premultiply ARGB, 2. do colour management
		// early enough, so that no avoidable increase of quantization error
		// occurs beforehands, and also for correct alpha compositing.
		// FIXME: This assumes that if the first frame is opaque, they all are.
		if (cairo_image_surface_get_format(page) == CAIRO_FORMAT_RGB24)
			fiv_io_profile_xrgb32_page(page, target);
	}
	return image;
}

static void
fiv_io_premultiply_argb32(cairo_surface_t *surface)
{
	int w = cairo_image_surface_get_width(surface);
	int h = cairo_image_surface_get_height(surface);
	unsigned char *data = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	if (cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
		return;

	for (int y = 0; y < h; y++) {
		uint32_t *dstp = (uint32_t *) (data + stride * y);
		for (int x = 0; x < w; x++) {
			uint32_t argb = dstp[x], a = argb >> 24;
			dstp[x] = a << 24 |
				PREMULTIPLY8(a, 0xFF & (argb >> 16)) << 16 |
				PREMULTIPLY8(a, 0xFF & (argb >>  8)) <<  8 |
				PREMULTIPLY8(a, 0xFF &  argb);
		}
	}
}

static void
fiv_io_premultiply_argb32_page(cairo_surface_t *page)
{
	for (cairo_surface_t *frame = page; frame != NULL;
		frame = cairo_surface_get_user_data(frame, &fiv_io_key_frame_next))
		fiv_io_premultiply_argb32(frame);
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

	FivIoProfile target;                ///< Target device profile, if any
	FivIoProfile source;                ///< Source colour profile, if any

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

	// TODO(p): Maybe pre-clear with
	// wuffs_base__frame_config__background_color(&fc).

	// Wuffs' test/data/animated-red-blue.gif, e.g., needs this handling.
	cairo_format_t decode_format = ctx->cairo_format;
	if (wuffs_base__frame_config__index(&fc) > 0 &&
		wuffs_base__pixel_config__pixel_format(&ctx->cfg.pixcfg).repr ==
			WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL)
		decode_format = CAIRO_FORMAT_ARGB32;

	unsigned char *targetbuf = NULL;
	cairo_surface_t *surface =
		cairo_image_surface_create(decode_format, ctx->width, ctx->height);
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
	if (ctx->expand_16_float || ctx->pack_16_10) {
		uint32_t targetbuf_size = ctx->height * ctx->width * 8;
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

		// The PNG decoder, at minimum, will flush any pixel data upon
		// finding out that the input is truncated, so accept whatever we get.
	}

	if (ctx->target) {
		if (ctx->expand_16_float || ctx->pack_16_10) {
			fiv_io_profile_4x16le_direct(
				targetbuf, ctx->width, ctx->height, ctx->source, ctx->target);
			// The first one premultiplies below, the second doesn't need to.
		} else {
			fiv_io_profile_xrgb32_direct(surface_data, ctx->width, ctx->height,
				ctx->source, ctx->target);
			fiv_io_premultiply_argb32(surface);
		}
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

		cairo_set_source_surface(cr, surface, 0, 0);
		cairo_paint(cr);
		cairo_destroy(cr);
		cairo_surface_destroy(surface);
		surface = canvas;
	}

	if (ctx->meta_exif)
		cairo_surface_set_user_data(surface, &fiv_io_key_exif,
			g_bytes_ref(ctx->meta_exif), (cairo_destroy_func_t) g_bytes_unref);
	if (ctx->meta_iccp)
		cairo_surface_set_user_data(surface, &fiv_io_key_icc,
			g_bytes_ref(ctx->meta_iccp), (cairo_destroy_func_t) g_bytes_unref);
	if (ctx->meta_xmp)
		cairo_surface_set_user_data(surface, &fiv_io_key_xmp,
			g_bytes_ref(ctx->meta_xmp), (cairo_destroy_func_t) g_bytes_unref);

	cairo_surface_set_user_data(surface, &fiv_io_key_loops,
		(void *) (uintptr_t) wuffs_base__image_decoder__num_animation_loops(
			ctx->dec), NULL);
	cairo_surface_set_user_data(surface, &fiv_io_key_frame_duration,
		(void *) (intptr_t) (wuffs_base__frame_config__duration(&fc) /
			WUFFS_BASE__FLICKS_PER_MILLISECOND), NULL);

	cairo_surface_set_user_data(
		surface, &fiv_io_key_frame_previous, ctx->result_tail, NULL);
	if (ctx->result_tail)
		cairo_surface_set_user_data(ctx->result_tail, &fiv_io_key_frame_next,
			surface, (cairo_destroy_func_t) cairo_surface_destroy);
	else
		ctx->result = surface;

	ctx->result_tail = surface;
	ctx->last_fc = fc;
	g_free(targetbuf);
	return wuffs_base__status__is_ok(&status);

fail:
	cairo_surface_destroy(surface);
	g_clear_pointer(&ctx->result, cairo_surface_destroy);
	ctx->result_tail = NULL;
	g_free(targetbuf);
	return false;
}

// https://github.com/google/wuffs/blob/main/example/gifplayer/gifplayer.c
// is pure C, and a good reference. I can't use the auxiliary libraries,
// since they depend on C++, which is undesirable.
static cairo_surface_t *
open_wuffs(wuffs_base__image_decoder *dec, wuffs_base__io_buffer src,
	const FivIoOpenContext *ioctx, GError **error)
{
	struct load_wuffs_frame_context ctx = {
		.dec = dec, .src = &src, .target = ioctx->screen_profile};

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
			ctx.source = fiv_io_profile_new_from_bytes(ctx.meta_iccp);
		else if (isfinite(gamma) && gamma > 0)
			ctx.source = fiv_io_profile_new_sRGB_gamma(gamma);
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
		cairo_surface_set_user_data(
			ctx.result, &fiv_io_key_frame_previous, ctx.result_tail, NULL);

fail:
	free(ctx.workbuf.ptr);
	g_clear_pointer(&ctx.meta_exif, g_bytes_unref);
	g_clear_pointer(&ctx.meta_iccp, g_bytes_unref);
	g_clear_pointer(&ctx.meta_xmp, g_bytes_unref);
	g_clear_pointer(&ctx.source, fiv_io_profile_free);
	return ctx.result;
}

static cairo_surface_t *
open_wuffs_using(wuffs_base__image_decoder *(*allocate)(),
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	wuffs_base__image_decoder *dec = allocate();
	if (!dec) {
		set_error(error, "memory allocation failed or internal error");
		return NULL;
	}

	cairo_surface_t *surface =
		open_wuffs(dec, wuffs_base__ptr_u8__reader((uint8_t *) data, len, TRUE),
			ctx, error);
	free(dec);
	return surface;
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
cairo_surface_t *
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
	cairo_surface_t *surface = NULL;
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

	surface = cairo_image_surface_create(
		wuffs_base__image_config__first_frame_is_opaque(&cfg)
			? CAIRO_FORMAT_RGB24
			: CAIRO_FORMAT_ARGB32,
		width, height);

	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		goto fail;
	}

	wuffs_base__pixel_buffer pb = {};
	status = wuffs_base__pixel_buffer__set_from_slice(&pb, &cfg.pixcfg,
		wuffs_base__make_slice_u8(cairo_image_surface_get_data(surface),
			cairo_image_surface_get_stride(surface) *
				cairo_image_surface_get_height(surface)));
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

	cairo_surface_mark_dirty(surface);
	cairo_surface_set_user_data(surface, &fiv_io_key_text,
		g_hash_table_ref(texts), (cairo_destroy_func_t) g_hash_table_unref);
	success = true;

fail:
	if (!success)
		g_clear_pointer(&surface, cairo_surface_destroy);

	free(workbuf.ptr);
	g_free(data);
	g_hash_table_unref(texts);
	return surface;
}

// --- JPEG --------------------------------------------------------------------

static GBytes *
parse_jpeg_metadata(cairo_surface_t *surface, const char *data, gsize len)
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
		if (G_UNLIKELY((p += length) > end))
			break;

		// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf 4.7.2
		// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3
		// Not checking the padding byte is intentional.
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

		// TODO(p): Extract the main XMP segment.
	}

	if (exif->len)
		cairo_surface_set_user_data(surface, &fiv_io_key_exif,
			g_byte_array_free_to_bytes(exif),
			(cairo_destroy_func_t) g_bytes_unref);
	else
		g_byte_array_free(exif, TRUE);

	GBytes *icc_profile = NULL;
	if (icc_done)
		cairo_surface_set_user_data(surface, &fiv_io_key_icc,
			(icc_profile = g_byte_array_free_to_bytes(icc)),
			(cairo_destroy_func_t) g_bytes_unref);
	else
		g_byte_array_free(icc, TRUE);
	return icc_profile;
}

static void
load_jpeg_finalize(cairo_surface_t *surface, bool cmyk,
	FivIoProfile destination, const char *data, size_t len)
{
	GBytes *icc_profile = parse_jpeg_metadata(surface, data, len);
	FivIoProfile source = NULL;
	if (icc_profile)
		source = fiv_io_profile_new(
			g_bytes_get_data(icc_profile, NULL), g_bytes_get_size(icc_profile));

	if (cmyk)
		fiv_io_profile_cmyk(surface, source, destination);
	else
		fiv_io_profile_xrgb32(surface, source, destination);

	if (source)
		fiv_io_profile_free(source);

	// Pixel data has been written, need to let Cairo know.
	cairo_surface_mark_dirty(surface);
}

static cairo_surface_t *
open_libjpeg_turbo(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	// Note that there doesn't seem to be much of a point in using this
	// simplified API anymore, because JPEG-QS needs the original libjpeg API.
	// It's just more or less duplicated code which won't compile with
	// the slow version of the library.
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

	bool use_cmyk = colorspace == TJCS_CMYK || colorspace == TJCS_YCCK;
	int pixel_format = use_cmyk
		? TJPF_CMYK
		: (G_BYTE_ORDER == G_LITTLE_ENDIAN ? TJPF_BGRX : TJPF_XRGB);

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
	}

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
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
		if (tjGetErrorCode(dec) == TJERR_WARNING) {
			add_warning(ctx, "%s", tjGetErrorStr2(dec));
		} else {
			set_error(error, tjGetErrorStr2(dec));
			cairo_surface_destroy(surface);
			tjDestroy(dec);
			return NULL;
		}
	}

	load_jpeg_finalize(surface, use_cmyk, ctx->screen_profile, data, len);
	tjDestroy(dec);
	return surface;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#ifdef HAVE_JPEG_QS

struct libjpeg_error_mgr {
	struct jpeg_error_mgr pub;
	jmp_buf buf;
	GError **error;
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

static cairo_surface_t *
open_libjpeg_enhanced(
	const char *data, gsize len, const FivIoOpenContext *ctx, GError **error)
{
	cairo_surface_t *volatile surface = NULL;

	struct libjpeg_error_mgr jerr = {.error = error};
	struct jpeg_decompress_struct cinfo = {.err = jpeg_std_error(&jerr.pub)};
	jerr.pub.error_exit = libjpeg_error_exit;
	if (setjmp(jerr.buf)) {
		g_clear_pointer(&surface, cairo_surface_destroy);
		jpeg_destroy_decompress(&cinfo);
		return NULL;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (const unsigned char *) data, len);
	(void) jpeg_read_header(&cinfo, true);

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

	surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		longjmp(jerr.buf, 1);
	}

	unsigned char *surface_data = cairo_image_surface_get_data(surface);
	int surface_stride = cairo_image_surface_get_stride(surface);
	JSAMPARRAY lines = (*cinfo.mem->alloc_small)(
		(j_common_ptr) &cinfo, JPOOL_IMAGE, sizeof *lines * height);
	for (int i = 0; i < height; i++)
		lines[i] = surface_data + i * surface_stride;

	// Go for the maximum quality setting.
	jpegqs_control_t opts = {
		.flags = JPEGQS_DIAGONALS | JPEGQS_JOINT_YUV | JPEGQS_UPSAMPLE_UV,
		.threads = g_get_num_processors(),
		.niter = 3,
	};

	(void) jpegqs_start_decompress(&cinfo, &opts);
	while (cinfo.output_scanline < cinfo.output_height)
		(void) jpeg_read_scanlines(&cinfo, lines + cinfo.output_scanline,
			cinfo.output_height - cinfo.output_scanline);
	if (cinfo.out_color_space == JCS_CMYK)
		trivial_cmyk_to_host_byte_order_argb(
			surface_data, cinfo.output_width * cinfo.output_height);
	(void) jpegqs_finish_decompress(&cinfo);

	load_jpeg_finalize(surface, use_cmyk, ctx->screen_profile, data, len);
	jpeg_destroy_decompress(&cinfo);
	return surface;
}

#else
#define open_libjpeg_enhanced open_libjpeg_turbo
#endif

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

static cairo_surface_t *
load_libwebp_nonanimated(WebPDecoderConfig *config, const WebPData *wd,
	const FivIoOpenContext *ctx, GError **error)
{
	cairo_surface_t *surface = cairo_image_surface_create(
		config->input.has_alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
		config->input.width, config->input.height);
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
		return NULL;
	}

	config->options.use_threads = true;

	config->output.width = config->input.width;
	config->output.height = config->input.height;
	config->output.is_external_memory = true;
	config->output.u.RGBA.rgba = cairo_image_surface_get_data(surface);
	config->output.u.RGBA.stride = cairo_image_surface_get_stride(surface);
	config->output.u.RGBA.size =
		config->output.u.RGBA.stride * cairo_image_surface_get_height(surface);

	bool premultiply = !ctx->screen_profile;
	if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
		config->output.colorspace = premultiply ? MODE_bgrA : MODE_BGRA;
	else
		config->output.colorspace = premultiply ? MODE_Argb : MODE_ARGB;

	WebPIDecoder *idec = WebPIDecode(NULL, 0, config);
	if (!idec) {
		set_error(error, "WebP decoding error");
		cairo_surface_destroy(surface);
		return NULL;
	}

	VP8StatusCode err = WebPIUpdate(idec, wd->bytes, wd->size);
	cairo_surface_mark_dirty(surface);
	int x = 0, y = 0, w = 0, h = 0;
	(void) WebPIDecodedArea(idec, &x, &y, &w, &h);
	WebPIDelete(idec);
	if (err == VP8_STATUS_OK)
		return surface;

	if (err != VP8_STATUS_SUSPENDED) {
		g_set_error(error, FIV_IO_ERROR, FIV_IO_ERROR_OPEN, "%s: %s",
			"WebP decoding error", load_libwebp_error(err));
		cairo_surface_destroy(surface);
		return NULL;
	}

	add_warning(ctx, "image file is truncated");
	if (config->input.has_alpha)
		return surface;

	// Always use transparent black, rather than opaque black.
	cairo_surface_t *masked = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, config->input.width, config->input.height);
	cairo_t *cr = cairo_create(masked);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	return masked;
}

static cairo_surface_t *
load_libwebp_frame(WebPAnimDecoder *dec, const WebPAnimInfo *info,
	int *last_timestamp, GError **error)
{
	uint8_t *buf = NULL;
	int timestamp = 0;
	if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) {
		set_error(error, "WebP decoding error");
		return NULL;
	}

	bool is_opaque = (info->bgcolor & 0xFF) == 0xFF;
	uint64_t area = info->canvas_width * info->canvas_height;
	cairo_surface_t *surface = cairo_image_surface_create(
		is_opaque ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32,
		info->canvas_width, info->canvas_height);

	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
		return NULL;
	}

	uint32_t *dst = (uint32_t *) cairo_image_surface_get_data(surface);
	if (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
		memcpy(dst, buf, area * sizeof *dst);
	} else {
		uint32_t *src = (uint32_t *) buf;
		for (uint64_t i = 0; i < area; i++)
			*dst++ = GUINT32_FROM_LE(*src++);
	}

	cairo_surface_mark_dirty(surface);

	// This API is confusing and awkward.
	cairo_surface_set_user_data(surface, &fiv_io_key_frame_duration,
		(void *) (intptr_t) (timestamp - *last_timestamp), NULL);
	*last_timestamp = timestamp;
	return surface;
}

static cairo_surface_t *
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

	cairo_surface_t *frames = NULL, *frames_tail = NULL;
	if (info.canvas_width > INT_MAX || info.canvas_height > INT_MAX) {
		set_error(error, "image dimensions overflow");
		goto fail;
	}

	int last_timestamp = 0;
	while (WebPAnimDecoderHasMoreFrames(dec)) {
		cairo_surface_t *surface =
			load_libwebp_frame(dec, &info, &last_timestamp, error);
		if (!surface) {
			g_clear_pointer(&frames, cairo_surface_destroy);
			goto fail;
		}

		if (frames_tail)
			cairo_surface_set_user_data(frames_tail, &fiv_io_key_frame_next,
				surface, (cairo_destroy_func_t) cairo_surface_destroy);
		else
			frames = surface;

		cairo_surface_set_user_data(
			surface, &fiv_io_key_frame_previous, frames_tail, NULL);
		frames_tail = surface;
	}

	if (frames) {
		cairo_surface_set_user_data(
			frames, &fiv_io_key_frame_previous, frames_tail, NULL);
	} else {
		set_error(error, "the animation has no frames");
		g_clear_pointer(&frames, cairo_surface_destroy);
	}

fail:
	WebPAnimDecoderDelete(dec);
	return frames;
}

static cairo_surface_t *
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

	cairo_surface_t *result = config.input.has_animation
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
	// TODO(p): Avoid copy-pasting the chunk transfer code.
	WebPChunkIterator chunk_iter = {};
	uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
	if ((flags & EXIF_FLAG) &&
		WebPDemuxGetChunk(demux, "EXIF", 1, &chunk_iter)) {
		cairo_surface_set_user_data(result, &fiv_io_key_exif,
			g_bytes_new(chunk_iter.chunk.bytes, chunk_iter.chunk.size),
			(cairo_destroy_func_t) g_bytes_unref);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if ((flags & ICCP_FLAG) &&
		WebPDemuxGetChunk(demux, "ICCP", 1, &chunk_iter)) {
		cairo_surface_set_user_data(result, &fiv_io_key_icc,
			g_bytes_new(chunk_iter.chunk.bytes, chunk_iter.chunk.size),
			(cairo_destroy_func_t) g_bytes_unref);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if ((flags & XMP_FLAG) &&
		WebPDemuxGetChunk(demux, "XMP ", 1, &chunk_iter)) {
		cairo_surface_set_user_data(result, &fiv_io_key_xmp,
			g_bytes_new(chunk_iter.chunk.bytes, chunk_iter.chunk.size),
			(cairo_destroy_func_t) g_bytes_unref);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if (WebPDemuxGetChunk(demux, "THUM", 1, &chunk_iter)) {
		cairo_surface_set_user_data(result, &fiv_io_key_thum,
			g_bytes_new(chunk_iter.chunk.bytes, chunk_iter.chunk.size),
			(cairo_destroy_func_t) g_bytes_unref);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if (flags & ANIMATION_FLAG) {
		cairo_surface_set_user_data(result, &fiv_io_key_loops,
			(void *) (uintptr_t) WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT),
			NULL);
	}

	WebPDemuxDelete(demux);
	if (ctx->screen_profile) {
		fiv_io_profile_xrgb32_page(result, ctx->screen_profile);
		fiv_io_premultiply_argb32_page(result);
	}

fail:
	WebPFreeDecBuffer(&config.output);
	return result;
}

// --- Optional dependencies ---------------------------------------------------

#ifdef HAVE_LIBRAW  // ---------------------------------------------------------

static cairo_surface_t *
open_libraw(const char *data, gsize len, GError **error)
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
		cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
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
#ifdef HAVE_RESVG  // ----------------------------------------------------------

typedef struct {
	FivIoRenderClosure parent;
	resvg_render_tree *tree;            ///< Loaded resvg tree
	double width;                       ///< Normal width
	double height;                      ///< Normal height
} FivIoRenderClosureResvg;

static void
load_resvg_destroy(void *closure)
{
	FivIoRenderClosureResvg *self = closure;
	resvg_tree_destroy(self->tree);
	g_free(self);
}

static cairo_surface_t *
load_resvg_render_internal(
	FivIoRenderClosureResvg *self, double scale, GError **error)
{
	double w = ceil(self->width * scale), h = ceil(self->height * scale);
	if (w > SHRT_MAX || h > SHRT_MAX) {
		set_error(error, "image dimensions overflow");
		return NULL;
	}

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
		return NULL;
	}

	uint32_t *pixels = (uint32_t *) cairo_image_surface_get_data(surface);
	resvg_fit_to fit_to = {
		scale == 1 ? RESVG_FIT_TO_TYPE_ORIGINAL : RESVG_FIT_TO_TYPE_ZOOM,
		scale};
	resvg_render(self->tree, fit_to, resvg_transform_identity(),
		cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface), (char *) pixels);

	// TODO(p): Also apply colour management, we'll need to un-premultiply.
	for (int i = 0; i < w * h; i++) {
		uint32_t rgba = g_ntohl(pixels[i]);
		pixels[i] = rgba << 24 | rgba >> 8;
	}

	cairo_surface_mark_dirty(surface);
	return surface;
}

static cairo_surface_t *
load_resvg_render(FivIoRenderClosure *closure, double scale)
{
	FivIoRenderClosureResvg *self = (FivIoRenderClosureResvg *) closure;
	return load_resvg_render_internal(self, scale, NULL);
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

static cairo_surface_t *
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
	closure->tree = tree;
	closure->width = size.width;
	closure->height = size.height;

	cairo_surface_t *surface = load_resvg_render_internal(closure, 1., error);
	if (!surface) {
		load_resvg_destroy(closure);
		return NULL;
	}

	cairo_surface_set_user_data(
		surface, &fiv_io_key_render, closure, load_resvg_destroy);
	return surface;
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
load_librsvg_destroy(void *closure)
{
	FivIoRenderClosureLibrsvg *self = closure;
	g_object_unref(self->handle);
	g_free(self);
}

static cairo_surface_t *
load_librsvg_render(FivIoRenderClosure *closure, double scale)
{
	FivIoRenderClosureLibrsvg *self = (FivIoRenderClosureLibrsvg *) closure;
	RsvgRectangle viewport = {.x = 0, .y = 0,
		.width = self->width * scale, .height = self->height * scale};
	cairo_surface_t *surface = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, ceil(viewport.width), ceil(viewport.height));

	GError *error = NULL;
	cairo_t *cr = cairo_create(surface);
	(void) rsvg_handle_render_document(self->handle, cr, &viewport, &error);
	cairo_destroy(cr);
	if (error) {
		g_debug("%s", error->message);
		g_error_free(error);
		cairo_surface_destroy(surface);
		return NULL;
	}

	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		g_debug("%s", cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
		return NULL;
	}
	return surface;
}

static cairo_surface_t *
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

	// librsvg rasterizes filters, so this method isn't fully appropriate.
	// It might be worth removing altogether.
	cairo_rectangle_t extents = {
		.x = 0, .y = 0, .width = ceil(w), .height = ceil(h)};
	cairo_surface_t *surface =
		cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, &extents);

	cairo_t *cr = cairo_create(surface);
	RsvgRectangle viewport = {.x = 0, .y = 0, .width = w, .height = h};
	if (!rsvg_handle_render_document(handle, cr, &viewport, error)) {
		cairo_surface_destroy(surface);
		cairo_destroy(cr);
		g_object_unref(handle);
		return NULL;
	}

	cairo_destroy(cr);

	FivIoRenderClosureLibrsvg *closure = g_malloc0(sizeof *closure);
	closure->parent.render = load_librsvg_render;
	closure->handle = handle;
	closure->width = w;
	closure->height = h;
	cairo_surface_set_user_data(
		surface, &fiv_io_key_render, closure, load_librsvg_destroy);
	return surface;
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

static cairo_surface_t *
open_xcursor(const char *data, gsize len, GError **error)
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
	cairo_surface_t *pages = NULL, *frames_head = NULL, *frames_tail = NULL;

	// XXX: Assuming that all "nominal sizes" have the same dimensions.
	XcursorDim last_nominal = -1;
	for (int i = 0; i < images->nimage; i++) {
		XcursorImage *image = images->images[i];

		// The library automatically byte swaps in _XcursorReadImage().
		cairo_surface_t *surface = cairo_image_surface_create_for_data(
			(unsigned char *) image->pixels, CAIRO_FORMAT_ARGB32,
			image->width, image->height, image->width * sizeof *image->pixels);
		cairo_surface_set_user_data(surface, &fiv_io_key_frame_duration,
			(void *) (intptr_t) image->delay, NULL);

		if (pages && image->size == last_nominal) {
			cairo_surface_set_user_data(
				surface, &fiv_io_key_frame_previous, frames_tail, NULL);
			cairo_surface_set_user_data(frames_tail, &fiv_io_key_frame_next,
				surface, (cairo_destroy_func_t) cairo_surface_destroy);
		} else if (frames_head) {
			cairo_surface_set_user_data(
				frames_head, &fiv_io_key_frame_previous, frames_tail, NULL);

			cairo_surface_set_user_data(frames_head, &fiv_io_key_page_next,
				surface, (cairo_destroy_func_t) cairo_surface_destroy);
			cairo_surface_set_user_data(
				surface, &fiv_io_key_page_previous, frames_head, NULL);
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
		frames_head, &fiv_io_key_frame_previous, frames_tail, NULL);

	// There is no need to copy data, assign it to the surface.
	static cairo_user_data_key_t key = {};
	cairo_surface_set_user_data(
		pages, &key, images, (cairo_destroy_func_t) XcursorImagesDestroy);

	// Do not bother doing colour correction, there is no correct rendering.
	return pages;
}

#endif  // HAVE_XCURSOR --------------------------------------------------------
#ifdef HAVE_LIBHEIF  //---------------------------------------------------------

static cairo_surface_t *
load_libheif_image(struct heif_image_handle *handle, GError **error)
{
	cairo_surface_t *surface = NULL;
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

	surface = cairo_image_surface_create(
		has_alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, w, h);
	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
		surface = NULL;
		goto fail_process;
	}

	// As of writing, the library is using 16-byte alignment, unlike Cairo.
	int src_stride = 0;
	const uint8_t *src = heif_image_get_plane_readonly(
		image, heif_channel_interleaved, &src_stride);
	int dst_stride = cairo_image_surface_get_stride(surface);
	const uint8_t *dst = cairo_image_surface_get_data(surface);

	for (int y = 0; y < h; y++) {
		uint32_t *dstp = (uint32_t *) (dst + dst_stride * y);
		const uint32_t *srcp = (const uint32_t *) (src + src_stride * y);
		for (int x = 0; x < w; x++) {
			uint32_t rgba = g_ntohl(srcp[x]);
			*dstp++ = rgba << 24 | rgba >> 8;
		}
	}

	// TODO(p): Test real behaviour on real transparent images.
	if (has_alpha && !heif_image_handle_is_premultiplied_alpha(handle))
		fiv_io_premultiply_argb32(surface);

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
			cairo_surface_set_user_data(surface, &fiv_io_key_exif,
				g_bytes_new_take(exif, exif_len),
				(cairo_destroy_func_t) g_bytes_unref);
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
			cairo_surface_set_user_data(surface, &fiv_io_key_icc,
				g_bytes_new_take(icc, icc_len),
				(cairo_destroy_func_t) g_bytes_unref);
		}
	}

	cairo_surface_mark_dirty(surface);

fail_process:
	heif_image_release(image);
fail_decode:
	heif_decoding_options_free(opts);
fail:
	return surface;
}

static void
load_libheif_aux_images(const FivIoOpenContext *ioctx,
	struct heif_image_handle *top, cairo_surface_t **result,
	cairo_surface_t **result_tail)
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

static cairo_surface_t *
open_libheif(
	const char *data, gsize len, const FivIoOpenContext *ioctx, GError **error)
{
	// libheif will throw C++ exceptions on allocation failures.
	// The library is generally awful through and through.
	struct heif_context *ctx = heif_context_alloc();
	cairo_surface_t *result = NULL, *result_tail = NULL;

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
		g_clear_pointer(&result, cairo_surface_destroy);
		set_error(error, "empty or unsupported image");
	}

	g_free(ids);
fail_read:
	heif_context_free(ctx);
	return fiv_io_profile_finalize(result, ioctx->screen_profile);
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

	surface = cairo_image_surface_create(image.alpha != EXTRASAMPLE_UNSPECIFIED
			? CAIRO_FORMAT_ARGB32
			: CAIRO_FORMAT_RGB24,
		image.width, image.height);

	image.req_orientation = ORIENTATION_LEFTTOP;
	uint32_t *raster = (uint32_t *) cairo_image_surface_get_data(surface);
	if (!TIFFRGBAImageGet(&image, raster, image.width, image.height)) {
		g_clear_pointer(&surface, cairo_surface_destroy);
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
		fiv_io_premultiply_argb32(surface);

	cairo_surface_mark_dirty(surface);
	// XXX: The whole file is essentially an Exif, any ideas?

	const uint32_t meta_len = 0;
	const void *meta = NULL;
	if (TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &meta_len, &meta)) {
		cairo_surface_set_user_data(surface, &fiv_io_key_icc,
			g_bytes_new(meta, meta_len), (cairo_destroy_func_t) g_bytes_unref);
	}
	if (TIFFGetField(tiff, TIFFTAG_XMLPACKET, &meta_len, &meta)) {
		cairo_surface_set_user_data(surface, &fiv_io_key_xmp,
			g_bytes_new(meta, meta_len), (cairo_destroy_func_t) g_bytes_unref);
	}

	// Don't ask. The API is high, alright, I'm just not sure about the level.
	uint16_t orientation = 0;
	if (TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation)) {
		if (orientation == 5 || orientation == 7)
			cairo_surface_set_user_data(
				surface, &fiv_io_key_orientation, (void *) (uintptr_t) 5, NULL);
		if (orientation == 6 || orientation == 8)
			cairo_surface_set_user_data(
				surface, &fiv_io_key_orientation, (void *) (uintptr_t) 7, NULL);
	}

fail:
	TIFFRGBAImageEnd(&image);
	// TODO(p): It's possible to implement ClipPath easily with Cairo.
	return surface;
}

static cairo_surface_t *
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

	cairo_surface_t *result = NULL, *result_tail = NULL;
	TIFF *tiff = TIFFClientOpen(ctx->uri, "rm" /* Avoid mmap. */, &h,
		fiv_io_tiff_read, fiv_io_tiff_write, fiv_io_tiff_seek,
		fiv_io_tiff_close, fiv_io_tiff_size, NULL, NULL);
	if (!tiff)
		goto fail;

	// In Nikon NEF files, IFD0 is a tiny uncompressed thumbnail with SubIFDs--
	// two of them JPEGs, the remaining one is raw. libtiff cannot read either
	// of those better versions.
	//
	// TODO(p): If NewSubfileType is ReducedImage, and it has SubIFDs compressed
	// as old JPEG (6), decode JPEGInterchangeFormat/JPEGInterchangeFormatLength
	// with libjpeg-turbo and insert them as the starting pages.
	//
	// This is not possible with libtiff directly, because TIFFSetSubDirectory()
	// requires an ImageLength tag that's missing, and TIFFReadCustomDirectory()
	// takes a privately defined struct that cannot be omitted.
	//
	// TODO(p): Samsung Android DNGs also claim to be TIFF/EP, but use a smaller
	// uncompressed YCbCr image. Apple ProRAW uses the new JPEG Compression (7),
	// with a weird Orientation. It also uses that value for its raw data.
	uint32_t subtype = 0;
	uint16_t subifd_count = 0;
	const uint64_t *subifd_offsets = NULL;
	if (TIFFGetField(tiff, TIFFTAG_SUBFILETYPE, &subtype) &&
		(subtype & FILETYPE_REDUCEDIMAGE) &&
		TIFFGetField(tiff, TIFFTAG_SUBIFD, &subifd_count, &subifd_offsets) &&
		subifd_count > 0 && subifd_offsets) {
	}

	do {
		// We inform about unsupported directories, but do not fail on them.
		GError *err = NULL;
		if (!try_append_page(
				load_libtiff_directory(tiff, &err), &result, &result_tail)) {
			add_warning(ctx, "%s", err->message);
			g_error_free(err);
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

	// TODO(p): Colour management even for un/associated alpha channels.
	// Note that TIFF has a number of fields that an ICC profile can be
	// constructed from--it's not a good idea to blindly assume sRGB.
	return fiv_io_profile_finalize(result, ctx->screen_profile);
}

#endif  // HAVE_LIBTIFF --------------------------------------------------------
#ifdef HAVE_GDKPIXBUF  // ------------------------------------------------------

static cairo_surface_t *
load_gdkpixbuf_argb32_unpremultiplied(GdkPixbuf *pixbuf)
{
	int w = gdk_pixbuf_get_width(pixbuf);
	int h = gdk_pixbuf_get_height(pixbuf);
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);

	guint length = 0;
	guchar *src = gdk_pixbuf_get_pixels_with_length(pixbuf, &length);
	int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
	uint32_t *dst = (uint32_t *) cairo_image_surface_get_data(surface);
	for (int y = 0; y < h; y++) {
		const guchar *p = src + y * src_stride;
		for (int x = 0; x < w; x++) {
			*dst++ = (uint32_t) p[3] << 24 | p[0] << 16 | p[1] << 8 | p[2];
			p += 4;
		}
	}
	cairo_surface_mark_dirty(surface);
	return surface;
}

static cairo_surface_t *
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

	cairo_surface_t *surface = NULL;
	if (custom_argb32)
		surface = load_gdkpixbuf_argb32_unpremultiplied(pixbuf);
	else
		surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, NULL);

	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		cairo_surface_destroy(surface);
		g_object_unref(pixbuf);
		return NULL;
	}

	const char *orientation = gdk_pixbuf_get_option(pixbuf, "orientation");
	if (orientation && strlen(orientation) == 1) {
		int n = *orientation - '0';
		if (n >= 1 && n <= 8)
			cairo_surface_set_user_data(
				surface, &fiv_io_key_orientation, (void *) (uintptr_t) n, NULL);
	}

	const char *icc_profile = gdk_pixbuf_get_option(pixbuf, "icc-profile");
	if (icc_profile) {
		gsize out_len = 0;
		guchar *raw = g_base64_decode(icc_profile, &out_len);
		if (raw) {
			cairo_surface_set_user_data(surface, &fiv_io_key_icc,
				g_bytes_new_take(raw, out_len),
				(cairo_destroy_func_t) g_bytes_unref);
		}
	}

	g_object_unref(pixbuf);
	if (custom_argb32) {
		fiv_io_profile_xrgb32_page(surface, ctx->screen_profile);
		fiv_io_premultiply_argb32_page(surface);
	} else {
		surface = fiv_io_profile_finalize(surface, ctx->screen_profile);
	}
	return surface;
}

#endif  // HAVE_GDKPIXBUF ------------------------------------------------------

// TODO(p): Check that all cairo_surface_set_user_data() calls succeed.
cairo_user_data_key_t fiv_io_key_exif;
cairo_user_data_key_t fiv_io_key_orientation;
cairo_user_data_key_t fiv_io_key_icc;
cairo_user_data_key_t fiv_io_key_xmp;
cairo_user_data_key_t fiv_io_key_thum;
cairo_user_data_key_t fiv_io_key_text;

cairo_user_data_key_t fiv_io_key_frame_next;
cairo_user_data_key_t fiv_io_key_frame_previous;
cairo_user_data_key_t fiv_io_key_frame_duration;
cairo_user_data_key_t fiv_io_key_loops;

cairo_user_data_key_t fiv_io_key_page_next;
cairo_user_data_key_t fiv_io_key_page_previous;

cairo_user_data_key_t fiv_io_key_render;

cairo_surface_t *
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

	cairo_surface_t *surface = fiv_io_open_from_data(data, len, ctx, error);
	g_free(data);
	return surface;
}

cairo_surface_t *
fiv_io_open_from_data(
	const char *data, size_t len, const FivIoOpenContext *ctx, GError **error)
{
	wuffs_base__slice_u8 prefix =
		wuffs_base__make_slice_u8((uint8_t *) data, len);

	cairo_surface_t *surface = NULL;
	switch (wuffs_base__magic_number_guess_fourcc(prefix, true /* closed */)) {
	case WUFFS_BASE__FOURCC__BMP:
		// Note that BMP can redirect into another format,
		// which is so far unsupported here.
		surface = open_wuffs_using(
			wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			ctx, error);
		break;
	case WUFFS_BASE__FOURCC__GIF:
		surface = open_wuffs_using(
			wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			ctx, error);
		break;
	case WUFFS_BASE__FOURCC__PNG:
		surface = open_wuffs_using(
			wuffs_png__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			ctx, error);
		break;
	case WUFFS_BASE__FOURCC__TGA:
		surface = open_wuffs_using(
			wuffs_tga__decoder__alloc_as__wuffs_base__image_decoder, data, len,
			ctx, error);
		break;
	case WUFFS_BASE__FOURCC__JPEG:
		surface = ctx->enhance
			? open_libjpeg_enhanced(data, len, ctx, error)
			: open_libjpeg_turbo(data, len, ctx, error);
		break;
	case WUFFS_BASE__FOURCC__WEBP:
		surface = open_libwebp(data, len, ctx, error);
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
#ifdef HAVE_RESVG  // ----------------------------------------------------------
		if ((surface = open_resvg(data, len, ctx, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_RESVG ----------------------------------------------------------
#ifdef HAVE_LIBRSVG  // --------------------------------------------------------
		if ((surface = open_librsvg(data, len, ctx, error)))
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
#ifdef HAVE_LIBHEIF  //---------------------------------------------------------
		if ((surface = open_libheif(data, len, ctx, error)))
			break;
		if (error) {
			g_debug("%s", (*error)->message);
			g_clear_error(error);
		}
#endif  // HAVE_LIBHEIF --------------------------------------------------------
#ifdef HAVE_LIBTIFF  //---------------------------------------------------------
		// This needs to be positioned after LibRaw.
		if ((surface = open_libtiff(data, len, ctx, error)))
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
	if (!surface) {
		GError *err = NULL;
		if ((surface = open_gdkpixbuf(data, len, ctx, &err))) {
			g_clear_error(error);
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
	GBytes *exif = NULL;
	gsize exif_len = 0;
	gconstpointer exif_data = NULL;
	if (surface &&
		(exif = cairo_surface_get_user_data(surface, &fiv_io_key_exif)) &&
		(exif_data = g_bytes_get_data(exif, &exif_len))) {
		cairo_surface_set_user_data(surface, &fiv_io_key_orientation,
			(void *) (uintptr_t) fiv_io_exif_orientation(exif_data, exif_len),
			NULL);
	}
	return surface;
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

// --- Filesystem --------------------------------------------------------------

#include "xdg.h"

static GPtrArray *
model_entry_array_new(void)
{
	return g_ptr_array_new_with_free_func(g_rc_box_release);
}

struct _FivIoModel {
	GObject parent_instance;
	GPatternSpec **supported_patterns;

	GFile *directory;                   ///< Currently loaded directory
	GFileMonitor *monitor;              ///< "directory" monitoring
	GPtrArray *subdirs;                 ///< "directory" contents
	GPtrArray *files;                   ///< "directory" contents

	FivIoModelSort sort_field;          ///< How to sort
	gboolean sort_descending;           ///< Whether to sort in reverse
	gboolean filtering;                 ///< Only show non-hidden, supported
};

G_DEFINE_TYPE(FivIoModel, fiv_io_model, G_TYPE_OBJECT)

enum {
	PROP_FILTERING = 1,
	PROP_SORT_FIELD,
	PROP_SORT_DESCENDING,
	N_PROPERTIES
};

static GParamSpec *model_properties[N_PROPERTIES];

enum {
	FILES_CHANGED,
	SUBDIRECTORIES_CHANGED,
	LAST_SIGNAL,
};

// Globals are, sadly, the canonical way of storing signal numbers.
static guint model_signals[LAST_SIGNAL];

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
model_supports(FivIoModel *self, const char *filename)
{
	gchar *utf8 = g_filename_to_utf8(filename, -1, NULL, NULL, NULL);
	if (!utf8)
		return FALSE;

	gchar *lc = g_utf8_strdown(utf8, -1);
	gsize lc_length = strlen(lc);
	gchar *reversed = g_utf8_strreverse(lc, lc_length);
	g_free(utf8);

	// fnmatch() uses the /locale encoding/, and isn't present on Windows.
	// TODO(p): Consider using g_file_info_get_display_name() for direct UTF-8.
	gboolean result = FALSE;
	for (GPatternSpec **p = self->supported_patterns; *p; p++)
		if ((result = g_pattern_spec_match(*p, lc_length, lc, reversed)))
			break;

	g_free(lc);
	g_free(reversed);
	return result;
}

static inline int
model_compare_entries(FivIoModel *self,
	const FivIoModelEntry *entry1, GFile *file1,
	const FivIoModelEntry *entry2, GFile *file2)
{
	if (g_file_has_prefix(file1, file2))
		return +1;
	if (g_file_has_prefix(file2, file1))
		return -1;

	int result = 0;
	switch (self->sort_field) {
	case FIV_IO_MODEL_SORT_MTIME:
		result -= entry1->mtime_msec < entry2->mtime_msec;
		result += entry1->mtime_msec > entry2->mtime_msec;
		if (result != 0)
			break;

		// Fall-through
	case FIV_IO_MODEL_SORT_NAME:
	case FIV_IO_MODEL_SORT_COUNT:
		result = strcmp(entry1->collate_key, entry2->collate_key);
	}
	return self->sort_descending ? -result : +result;
}

static gint
model_compare(gconstpointer a, gconstpointer b, gpointer user_data)
{
	const FivIoModelEntry *entry1 = *(const FivIoModelEntry **) a;
	const FivIoModelEntry *entry2 = *(const FivIoModelEntry **) b;
	GFile *file1 = g_file_new_for_uri(entry1->uri);
	GFile *file2 = g_file_new_for_uri(entry2->uri);
	int result = model_compare_entries(user_data, entry1, file1, entry2, file2);
	g_object_unref(file1);
	g_object_unref(file2);
	return result;
}

static size_t
model_strsize(const char *string)
{
	if (!string)
		return 0;

	return strlen(string) + 1;
}

static char *
model_strappend(char **p, const char *string, size_t size)
{
	if (!string)
		return NULL;

	char *destination = memcpy(*p, string, size);
	*p += size;
	return destination;
}

static FivIoModelEntry *
model_entry_new(GFile *file, GFileInfo *info)
{
	gchar *uri = g_file_get_uri(file);
	const gchar *target_uri = g_file_info_get_attribute_string(
		info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
	const gchar *display_name = g_file_info_get_display_name(info);

	// TODO(p): Make it possible to use g_utf8_collate_key() instead,
	// which does not use natural sorting.
	gchar *parse_name = g_file_get_parse_name(file);
	gchar *collate_key = g_utf8_collate_key_for_filename(parse_name, -1);
	g_free(parse_name);

	// The entries are immutable. Packing them into the structure
	// should help memory usage as well as performance.
	size_t size_uri          = model_strsize(uri);
	size_t size_target_uri   = model_strsize(target_uri);
	size_t size_display_name = model_strsize(display_name);
	size_t size_collate_key  = model_strsize(collate_key);

	FivIoModelEntry *entry = g_rc_box_alloc0(sizeof *entry +
		size_uri +
		size_target_uri +
		size_display_name +
		size_collate_key);

	gchar *p = (gchar *) entry + sizeof *entry;
	entry->uri          = model_strappend(&p, uri, size_uri);
	entry->target_uri   = model_strappend(&p, target_uri, size_target_uri);
	entry->display_name = model_strappend(&p, display_name, size_display_name);
	entry->collate_key  = model_strappend(&p, collate_key, size_collate_key);

	entry->filesize = (guint64) g_file_info_get_size(info);

	GDateTime *mtime = g_file_info_get_modification_date_time(info);
	if (mtime) {
		entry->mtime_msec = g_date_time_to_unix(mtime) * 1000 +
			g_date_time_get_microsecond(mtime) / 1000;
		g_date_time_unref(mtime);
	}

	g_free(uri);
	g_free(collate_key);
	return entry;
}

static gboolean
model_reload_to(FivIoModel *self, GFile *directory,
	GPtrArray *subdirs, GPtrArray *files, GError **error)
{
	if (subdirs)
		g_ptr_array_set_size(subdirs, 0);
	if (files)
		g_ptr_array_set_size(files, 0);

	GFileEnumerator *enumerator = g_file_enumerate_children(directory,
		G_FILE_ATTRIBUTE_STANDARD_TYPE ","
		G_FILE_ATTRIBUTE_STANDARD_NAME ","
		G_FILE_ATTRIBUTE_STANDARD_SIZE ","
		G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
		G_FILE_ATTRIBUTE_STANDARD_TARGET_URI ","
		G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
		G_FILE_ATTRIBUTE_TIME_MODIFIED ","
		G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
		G_FILE_QUERY_INFO_NONE, NULL, error);
	if (!enumerator)
		return FALSE;

	GFileInfo *info = NULL;
	GFile *child = NULL;
	GError *e = NULL;
	while (TRUE) {
		if (!g_file_enumerator_iterate(enumerator, &info, &child, NULL, &e) &&
			e) {
			g_warning("%s", e->message);
			g_clear_error(&e);
			continue;
		}
		if (!info)
			break;
		if (self->filtering && g_file_info_get_is_hidden(info))
			continue;

		GPtrArray *target = NULL;
		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
			target = subdirs;
		else if (!self->filtering ||
			model_supports(self, g_file_info_get_name(info)))
			target = files;

		if (target)
			g_ptr_array_add(target, model_entry_new(child, info));
	}
	g_object_unref(enumerator);

	if (subdirs)
		g_ptr_array_sort_with_data(subdirs, model_compare, self);
	if (files)
		g_ptr_array_sort_with_data(files, model_compare, self);
	return TRUE;
}

static gboolean
model_reload(FivIoModel *self, GError **error)
{
	// Note that this will clear all entries on failure.
	gboolean result = model_reload_to(
		self, self->directory, self->subdirs, self->files, error);

	g_signal_emit(self, model_signals[FILES_CHANGED], 0);
	g_signal_emit(self, model_signals[SUBDIRECTORIES_CHANGED], 0);
	return result;
}

static void
model_resort(FivIoModel *self)
{
	g_ptr_array_sort_with_data(self->subdirs, model_compare, self);
	g_ptr_array_sort_with_data(self->files, model_compare, self);

	g_signal_emit(self, model_signals[FILES_CHANGED], 0);
	g_signal_emit(self, model_signals[SUBDIRECTORIES_CHANGED], 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// This would be more efficient iteratively, but it's not that important.
static GFile *
model_last_deep_subdirectory(FivIoModel *self, GFile *directory)
{
	GFile *result = NULL;
	GPtrArray *subdirs = model_entry_array_new();
	if (!model_reload_to(self, directory, subdirs, NULL, NULL))
		goto out;

	if (subdirs->len) {
		FivIoModelEntry *entry = g_ptr_array_index(subdirs, subdirs->len - 1);
		GFile *last = g_file_new_for_uri(entry->uri);
		result = model_last_deep_subdirectory(self, last);
		g_object_unref(last);
	} else {
		result = g_object_ref(directory);
	}

out:
	g_ptr_array_free(subdirs, TRUE);
	return result;
}

GFile *
fiv_io_model_get_previous_directory(FivIoModel *self)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(self), NULL);

	GFile *parent_directory = g_file_get_parent(self->directory);
	if (!parent_directory)
		return NULL;

	GFile *result = NULL;
	GPtrArray *subdirs = model_entry_array_new();
	if (!model_reload_to(self, parent_directory, subdirs, NULL, NULL))
		goto out;

	for (gsize i = 0; i < subdirs->len; i++) {
		FivIoModelEntry *entry = g_ptr_array_index(subdirs, i);
		GFile *file = g_file_new_for_uri(entry->uri);
		if (g_file_equal(file, self->directory)) {
			g_object_unref(file);
			break;
		}

		g_clear_object(&result);
		result = file;
	}
	if (result) {
		GFile *last = model_last_deep_subdirectory(self, result);
		g_object_unref(result);
		result = last;
	} else {
		result = g_object_ref(parent_directory);
	}

out:
	g_object_unref(parent_directory);
	g_ptr_array_free(subdirs, TRUE);
	return result;
}

// This would be more efficient iteratively, but it's not that important.
static GFile *
model_next_directory_within_parents(FivIoModel *self, GFile *directory)
{
	GFile *parent_directory = g_file_get_parent(directory);
	if (!parent_directory)
		return NULL;

	GFile *result = NULL;
	GPtrArray *subdirs = model_entry_array_new();
	if (!model_reload_to(self, parent_directory, subdirs, NULL, NULL))
		goto out;

	gboolean found_self = FALSE;
	for (gsize i = 0; i < subdirs->len; i++) {
		FivIoModelEntry *entry = g_ptr_array_index(subdirs, i);
		result = g_file_new_for_uri(entry->uri);
		if (found_self)
			goto out;

		found_self = g_file_equal(result, directory);
		g_clear_object(&result);
	}
	if (!result)
		result = model_next_directory_within_parents(self, parent_directory);

out:
	g_object_unref(parent_directory);
	g_ptr_array_free(subdirs, TRUE);
	return result;
}

GFile *
fiv_io_model_get_next_directory(FivIoModel *self)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(self), NULL);

	if (self->subdirs->len) {
		FivIoModelEntry *entry = g_ptr_array_index(self->subdirs, 0);
		return g_file_new_for_uri(entry->uri);
	}

	return model_next_directory_within_parents(self, self->directory);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
fiv_io_model_finalize(GObject *gobject)
{
	FivIoModel *self = FIV_IO_MODEL(gobject);
	for (GPatternSpec **p = self->supported_patterns; *p; p++)
		g_pattern_spec_free(*p);
	g_free(self->supported_patterns);

	g_clear_object(&self->directory);
	g_clear_object(&self->monitor);
	g_ptr_array_free(self->subdirs, TRUE);
	g_ptr_array_free(self->files, TRUE);

	G_OBJECT_CLASS(fiv_io_model_parent_class)->finalize(gobject);
}

static void
fiv_io_model_get_property(
	GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	FivIoModel *self = FIV_IO_MODEL(object);
	switch (property_id) {
	case PROP_FILTERING:
		g_value_set_boolean(value, self->filtering);
		break;
	case PROP_SORT_FIELD:
		g_value_set_int(value, self->sort_field);
		break;
	case PROP_SORT_DESCENDING:
		g_value_set_boolean(value, self->sort_descending);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
fiv_io_model_set_property(
	GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	FivIoModel *self = FIV_IO_MODEL(object);
	switch (property_id) {
	case PROP_FILTERING:
		if (self->filtering != g_value_get_boolean(value)) {
			self->filtering = !self->filtering;
			g_object_notify_by_pspec(object, model_properties[property_id]);
			(void) model_reload(self, NULL /* error */);
		}
		break;
	case PROP_SORT_FIELD:
		if ((int) self->sort_field != g_value_get_int(value)) {
			self->sort_field = g_value_get_int(value);
			g_object_notify_by_pspec(object, model_properties[property_id]);
			model_resort(self);
		}
		break;
	case PROP_SORT_DESCENDING:
		if (self->sort_descending != g_value_get_boolean(value)) {
			self->sort_descending = !self->sort_descending;
			g_object_notify_by_pspec(object, model_properties[property_id]);
			model_resort(self);
		}
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
fiv_io_model_class_init(FivIoModelClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = fiv_io_model_get_property;
	object_class->set_property = fiv_io_model_set_property;
	object_class->finalize = fiv_io_model_finalize;

	model_properties[PROP_FILTERING] = g_param_spec_boolean(
		"filtering", "Filtering", "Only show non-hidden, supported entries",
		TRUE, G_PARAM_READWRITE);
	// TODO(p): GObject enumerations are annoying, but this should be one.
	model_properties[PROP_SORT_FIELD] = g_param_spec_int(
		"sort-field", "Sort field", "Sort order",
		FIV_IO_MODEL_SORT_MIN, FIV_IO_MODEL_SORT_MAX,
		FIV_IO_MODEL_SORT_NAME, G_PARAM_READWRITE);
	model_properties[PROP_SORT_DESCENDING] = g_param_spec_boolean(
		"sort-descending", "Sort descending", "Use reverse sort order",
		FALSE, G_PARAM_READWRITE);
	g_object_class_install_properties(
		object_class, N_PROPERTIES, model_properties);

	// TODO(p): Arguments something like: index, added, removed.
	model_signals[FILES_CHANGED] =
		g_signal_new("files-changed", G_TYPE_FROM_CLASS(klass), 0, 0,
			NULL, NULL, NULL, G_TYPE_NONE, 0);
	model_signals[SUBDIRECTORIES_CHANGED] =
		g_signal_new("subdirectories-changed", G_TYPE_FROM_CLASS(klass), 0, 0,
			NULL, NULL, NULL, G_TYPE_NONE, 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
fiv_io_model_init(FivIoModel *self)
{
	self->filtering = TRUE;

	char **types = fiv_io_all_supported_media_types();
	char **globs = extract_mime_globs((const char **) types);
	g_strfreev(types);

	gsize n = g_strv_length(globs);
	self->supported_patterns =
		g_malloc0_n(n + 1, sizeof *self->supported_patterns);
	while (n--)
		self->supported_patterns[n] = g_pattern_spec_new(globs[n]);
	g_strfreev(globs);

	self->files = model_entry_array_new();
	self->subdirs = model_entry_array_new();
}

gboolean
fiv_io_model_open(FivIoModel *self, GFile *directory, GError **error)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(self), FALSE);
	g_return_val_if_fail(G_IS_FILE(directory), FALSE);

	g_clear_object(&self->directory);
	g_clear_object(&self->monitor);
	self->directory = g_object_ref(directory);

	// TODO(p): Process the ::changed signal.
	self->monitor = g_file_monitor_directory(
		directory, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL /* error */);
	return model_reload(self, error);
}

GFile *
fiv_io_model_get_location(FivIoModel *self)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(self), NULL);
	return self->directory;
}

FivIoModelEntry *const *
fiv_io_model_get_files(FivIoModel *self, gsize *len)
{
	*len = self->files->len;
	return (FivIoModelEntry *const *) self->files->pdata;
}

FivIoModelEntry *const *
fiv_io_model_get_subdirs(FivIoModel *self, gsize *len)
{
	*len = self->subdirs->len;
	return (FivIoModelEntry *const *) self->subdirs->pdata;
}

// --- Export ------------------------------------------------------------------

unsigned char *
fiv_io_encode_webp(
	cairo_surface_t *surface, const WebPConfig *config, size_t *len)
{
	cairo_format_t format = cairo_image_surface_get_format(surface);
	int w = cairo_image_surface_get_width(surface);
	int h = cairo_image_surface_get_height(surface);
	if (format != CAIRO_FORMAT_ARGB32 &&
		format != CAIRO_FORMAT_RGB24) {
		cairo_surface_t *converted =
			cairo_image_surface_create((format = CAIRO_FORMAT_ARGB32), w, h);
		cairo_t *cr = cairo_create(converted);
		cairo_set_source_surface(cr, surface, 0, 0);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);
		cairo_destroy(cr);
		surface = converted;
	} else {
		surface = cairo_surface_reference(surface);
	}

	WebPMemoryWriter writer = {};
	WebPMemoryWriterInit(&writer);
	WebPPicture picture = {};
	if (!WebPPictureInit(&picture))
		goto fail;

	picture.use_argb = true;
	picture.width = w;
	picture.height = h;
	if (!WebPPictureAlloc(&picture))
		goto fail;

	// Cairo uses a similar internal format, so we should be able to
	// copy it over and fix up the minor differences.
	// This is written to be easy to follow rather than fast.
	int stride = cairo_image_surface_get_stride(surface);
	if (picture.argb_stride != w ||
		picture.argb_stride * (int) sizeof *picture.argb != stride ||
		INT_MAX / picture.argb_stride < h)
		goto fail_compatibility;

	uint32_t *argb =
		memcpy(picture.argb, cairo_image_surface_get_data(surface), stride * h);
	if (format == CAIRO_FORMAT_ARGB32)
		for (int i = h * picture.argb_stride; i-- > 0; argb++)
			*argb = wuffs_base__color_u32_argb_premul__as__color_u32_argb_nonpremul(*argb);
	else
		for (int i = h * picture.argb_stride; i-- > 0; argb++)
			*argb |= 0xFF000000;

	// TODO(p): Prevent or propagate VP8_ENC_ERROR_BAD_DIMENSION.
	picture.writer = WebPMemoryWrite;
	picture.custom_ptr = &writer;
	if (!WebPEncode(config, &picture))
		g_debug("WebPEncode: %d\n", picture.error_code);

fail_compatibility:
	WebPPictureFree(&picture);
fail:
	cairo_surface_destroy(surface);
	*len = writer.size;
	return writer.mem;
}

static WebPData
encode_lossless_webp(cairo_surface_t *surface)
{
	WebPData bitstream = {};
	WebPConfig config = {};
	if (!WebPConfigInit(&config) || !WebPConfigLosslessPreset(&config, 6))
		return bitstream;

	config.thread_level = true;
	if (!WebPValidateConfig(&config))
		return bitstream;

	bitstream.bytes = fiv_io_encode_webp(surface, &config, &bitstream.size);
	return bitstream;
}

static gboolean
encode_webp_image(WebPMux *mux, cairo_surface_t *frame)
{
	WebPData bitstream = encode_lossless_webp(frame);
	gboolean ok = WebPMuxSetImage(mux, &bitstream, true) == WEBP_MUX_OK;
	WebPDataClear(&bitstream);
	return ok;
}

static gboolean
encode_webp_animation(WebPMux *mux, cairo_surface_t *page)
{
	gboolean ok = TRUE;
	for (cairo_surface_t *frame = page; ok && frame; frame =
			cairo_surface_get_user_data(frame, &fiv_io_key_frame_next)) {
		WebPMuxFrameInfo info = {
			.bitstream = encode_lossless_webp(frame),
			.duration = (intptr_t) cairo_surface_get_user_data(
				frame, &fiv_io_key_frame_duration),
			.id = WEBP_CHUNK_ANMF,
			.dispose_method = WEBP_MUX_DISPOSE_NONE,
			.blend_method = WEBP_MUX_NO_BLEND,
		};
		ok = WebPMuxPushFrame(mux, &info, true) == WEBP_MUX_OK;
		WebPDataClear(&info.bitstream);
	}
	WebPMuxAnimParams params = {
		.bgcolor = 0x00000000,  // BGRA, curiously.
		.loop_count = (uintptr_t)
			cairo_surface_get_user_data(page, &fiv_io_key_loops),
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
fiv_io_save(cairo_surface_t *page, cairo_surface_t *frame, FivIoProfile target,
	const char *path, GError **error)
{
	g_return_val_if_fail(page != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	gboolean ok = TRUE;
	WebPMux *mux = WebPMuxNew();
	if (frame)
		ok = encode_webp_image(mux, frame);
	else if (!cairo_surface_get_user_data(page, &fiv_io_key_frame_next))
		ok = encode_webp_image(mux, page);
	else
		ok = encode_webp_animation(mux, page);

	ok = ok && set_metadata(mux, "EXIF",
		cairo_surface_get_user_data(page, &fiv_io_key_exif));
	ok = ok && set_metadata(mux, "ICCP",
		cairo_surface_get_user_data(page, &fiv_io_key_icc));
	ok = ok && set_metadata(mux, "XMP ",
		cairo_surface_get_user_data(page, &fiv_io_key_xmp));

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
fiv_io_orientation_dimensions(cairo_surface_t *surface,
	FivIoOrientation orientation, double *w, double *h)
{
	cairo_rectangle_t extents = {};
	switch (cairo_surface_get_type(surface)) {
	case CAIRO_SURFACE_TYPE_IMAGE:
		extents.width = cairo_image_surface_get_width(surface);
		extents.height = cairo_image_surface_get_height(surface);
		break;
	case CAIRO_SURFACE_TYPE_RECORDING:
		if (!cairo_recording_surface_get_extents(surface, &extents))
			cairo_recording_surface_ink_extents(surface,
				&extents.x, &extents.y, &extents.width, &extents.height);
		break;
	default:
		g_assert_not_reached();
	}

	switch (orientation) {
	case FivIoOrientation90:
	case FivIoOrientationMirror90:
	case FivIoOrientation270:
	case FivIoOrientationMirror270:
		*w = extents.height;
		*h = extents.width;
		break;
	default:
		*w = extents.width;
		*h = extents.height;
	}
}

cairo_matrix_t
fiv_io_orientation_apply(cairo_surface_t *surface,
	FivIoOrientation orientation, double *width, double *height)
{
	fiv_io_orientation_dimensions(surface, orientation, width, height);

	cairo_matrix_t matrix = {};
	cairo_matrix_init_identity(&matrix);
	switch (orientation) {
	case FivIoOrientation90:
		cairo_matrix_rotate(&matrix, -M_PI_2);
		cairo_matrix_translate(&matrix, -*width, 0);
		break;
	case FivIoOrientation180:
		cairo_matrix_scale(&matrix, -1, -1);
		cairo_matrix_translate(&matrix, -*width, -*height);
		break;
	case FivIoOrientation270:
		cairo_matrix_rotate(&matrix, +M_PI_2);
		cairo_matrix_translate(&matrix, 0, -*height);
		break;
	case FivIoOrientationMirror0:
		cairo_matrix_scale(&matrix, -1, +1);
		cairo_matrix_translate(&matrix, -*width, 0);
		break;
	case FivIoOrientationMirror90:
		cairo_matrix_rotate(&matrix, +M_PI_2);
		cairo_matrix_scale(&matrix, -1, +1);
		cairo_matrix_translate(&matrix, -*width, -*height);
		break;
	case FivIoOrientationMirror180:
		cairo_matrix_scale(&matrix, +1, -1);
		cairo_matrix_translate(&matrix, 0, -*height);
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
		return FivIoOrientationUnknown;
	} else if (!memcmp(tiff, le, sizeof le)) {
		u16 = wuffs_base__peek_u16le__no_bounds_check;
		u32 = wuffs_base__peek_u32le__no_bounds_check;
	} else if (!memcmp(tiff, be, sizeof be)) {
		u16 = wuffs_base__peek_u16be__no_bounds_check;
		u32 = wuffs_base__peek_u32be__no_bounds_check;
	} else {
		return FivIoOrientationUnknown;
	}

	const uint8_t *ifd0 = tiff + u32(tiff + 4);
	if (ifd0 + 2 > end)
		return FivIoOrientationUnknown;

	uint16_t fields = u16(ifd0);
	enum { BYTE = 1, ASCII, SHORT, LONG, RATIONAL,
		SBYTE, UNDEFINED, SSHORT, SLONG, SRATIONAL, FLOAT, DOUBLE };
	enum { Orientation = 274 };
	for (const guint8 *p = ifd0 + 2; fields-- && p + 12 <= end; p += 12) {
		uint16_t tag = u16(p), type = u16(p + 2), value16 = u16(p + 8);
		uint32_t count = u32(p + 4);
		if (G_UNLIKELY(tag == Orientation && type == SHORT && count == 1 &&
			value16 >= 1 && value16 <= 8))
			return value16;
	}
	return FivIoOrientationUnknown;
}

gboolean
fiv_io_save_metadata(cairo_surface_t *page, const char *path, GError **error)
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

	GBytes *data = NULL;
	gsize len = 0;
	gconstpointer p = NULL;

	// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3
	// I don't care if Exiv2 supports it this way.
	if ((data = cairo_surface_get_user_data(page, &fiv_io_key_exif)) &&
		(p = g_bytes_get_data(data, &len))) {
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
	if ((data = cairo_surface_get_user_data(page, &fiv_io_key_icc)) &&
		(p = g_bytes_get_data(data, &len))) {
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
	if ((data = cairo_surface_get_user_data(page, &fiv_io_key_xmp)) &&
		(p = g_bytes_get_data(data, &len))) {
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

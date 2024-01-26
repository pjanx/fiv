//
// fiv-profile.c: colour management
//
// Copyright (c) 2024, Přemysl Eric Janouch <p@janouch.name>
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

#include <glib.h>
#include <stdbool.h>

#include "fiv-io.h"

// Colour management must be handled before RGB conversions.
#ifdef HAVE_LCMS2
#include <lcms2.h>
#endif  // HAVE_LCMS2
#ifdef HAVE_LCMS2_FAST_FLOAT
#include <lcms2_fast_float.h>
#endif  // HAVE_LCMS2_FAST_FLOAT

// https://github.com/mm2/Little-CMS/issues/430
static bool g_broken_cms_premul;

void
fiv_io_profile_init(void)
{
	// TODO(p): Use Little CMS with contexts instead.
#ifdef HAVE_LCMS2_FAST_FLOAT
	if (cmsPluginTHR(NULL, cmsFastFloatExtensions()))
		g_broken_cms_premul = LCMS_VERSION <= 2160;
#endif  // HAVE_LCMS2_FAST_FLOAT
}

FivIoProfile *
fiv_io_profile_new(const void *data, size_t len)
{
#ifdef HAVE_LCMS2
	return cmsOpenProfileFromMemTHR(NULL, data, len);
#else
	(void) data;
	(void) len;
	return NULL;
#endif
}

FivIoProfile *
fiv_io_profile_new_sRGB(void)
{
#ifdef HAVE_LCMS2
	return cmsCreate_sRGBProfileTHR(NULL);
#else
	return NULL;
#endif
}

FivIoProfile *
fiv_io_profile_new_parametric(
	double gamma, double whitepoint[2], double primaries[6])
{
#ifdef HAVE_LCMS2
	// TODO(p): Make sure to use the library in a thread-safe manner.
	cmsContext context = NULL;

	const cmsCIExyY cmsWP = {whitepoint[0], whitepoint[1], 1.0};
	const cmsCIExyYTRIPLE cmsP = {
		{primaries[0], primaries[1], 1.0},
		{primaries[2], primaries[3], 1.0},
		{primaries[4], primaries[5], 1.0},
	};

	cmsToneCurve *curve = cmsBuildGamma(context, gamma);
	if (!curve)
		return NULL;

	cmsHPROFILE profile = cmsCreateRGBProfileTHR(
		context, &cmsWP, &cmsP, (cmsToneCurve *[3]){curve, curve, curve});
	cmsFreeToneCurve(curve);
	return profile;
#else
	(void) gamma;
	(void) whitepoint;
	(void) primaries;
	return NULL;
#endif
}

FivIoProfile *
fiv_io_profile_new_sRGB_gamma(double gamma)
{
	return fiv_io_profile_new_parametric(gamma,
		(double[2]){0.3127, 0.3290},
		(double[6]){0.6400, 0.3300, 0.3000, 0.6000, 0.1500, 0.0600});
}

FivIoProfile *
fiv_io_profile_new_from_bytes(GBytes *bytes)
{
	gsize len = 0;
	gconstpointer p = g_bytes_get_data(bytes, &len);
	return fiv_io_profile_new(p, len);
}

GBytes *
fiv_io_profile_to_bytes(FivIoProfile *profile)
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
fiv_io_profile_free(FivIoProfile *self)
{
#ifdef HAVE_LCMS2
	cmsCloseProfile(self);
#else
	(void) self;
#endif
}

// --- Image loading -----------------------------------------------------------

// TODO(p): In general, try to use CAIRO_FORMAT_RGB30 or CAIRO_FORMAT_RGBA128F.
#ifndef HAVE_LCMS2
#define FIV_IO_PROFILE_ARGB32 0
#define FIV_IO_PROFILE_4X16LE 0
#else
#define FIV_IO_PROFILE_ARGB32 \
	(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_8 : TYPE_ARGB_8)
#define FIV_IO_PROFILE_4X16LE \
	(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_16 : TYPE_BGRA_16_SE)
#endif

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

void
fiv_io_profile_cmyk(
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
#ifndef HAVE_LCMS2
	(void) source;
	(void) target;
#else
	cmsHTRANSFORM transform = NULL;
	if (source && target) {
		transform = cmsCreateTransformTHR(NULL, source, TYPE_CMYK_8_REV, target,
			FIV_IO_PROFILE_ARGB32, INTENT_PERCEPTUAL, 0);
	}
	if (transform) {
		cmsDoTransform(
			transform, image->data, image->data, image->width * image->height);
		cmsDeleteTransform(transform);
		return;
	}
#endif
	trivial_cmyk_to_host_byte_order_argb(
		image->data, image->width * image->height);
}

static bool
fiv_io_profile_rgb_direct(unsigned char *data, int w, int h,
	FivIoProfile *source, FivIoProfile *target,
	uint32_t source_format, uint32_t target_format)
{
#ifndef HAVE_LCMS2
	(void) data;
	(void) w;
	(void) h;
	(void) source;
	(void) source_format;
	(void) target;
	(void) target_format;
	return false;
#else
	// TODO(p): We should make this optional.
	cmsHPROFILE src_fallback = NULL;
	if (target && !source)
		source = src_fallback = cmsCreate_sRGBProfileTHR(NULL);

	cmsHTRANSFORM transform = NULL;
	if (source && target) {
		transform = cmsCreateTransformTHR(NULL,
			source, source_format, target, target_format, INTENT_PERCEPTUAL, 0);
	}
	if (transform) {
		cmsDoTransform(transform, data, data, w * h);
		cmsDeleteTransform(transform);
	}
	if (src_fallback)
		cmsCloseProfile(src_fallback);
	return transform != NULL;
#endif
}

static void
fiv_io_profile_xrgb32(
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	fiv_io_profile_rgb_direct(image->data, image->width, image->height,
		source, target, FIV_IO_PROFILE_ARGB32, FIV_IO_PROFILE_ARGB32);
}

void
fiv_io_profile_4x16le_direct(unsigned char *data,
	int w, int h, FivIoProfile *source, FivIoProfile *target)
{
	fiv_io_profile_rgb_direct(data, w, h, source, target,
		FIV_IO_PROFILE_4X16LE, FIV_IO_PROFILE_4X16LE);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void
fiv_io_profile_page(FivIoImage *page, FivIoProfile *target,
	void (*frame_cb) (FivIoImage *, FivIoProfile *, FivIoProfile *))
{
	FivIoProfile *source = NULL;
	if (page->icc)
		source = fiv_io_profile_new_from_bytes(page->icc);

	// TODO(p): All animations need to be composited in a linear colour space.
	for (FivIoImage *frame = page; frame != NULL; frame = frame->frame_next)
		frame_cb(frame, source, target);

	if (source)
		fiv_io_profile_free(source);
}

// From libwebp, verified to exactly match [x * a / 255].
#define PREMULTIPLY8(a, x) (((uint32_t) (x) * (uint32_t) (a) * 32897U) >> 23)

void
fiv_io_premultiply_argb32(FivIoImage *image)
{
	if (image->format != CAIRO_FORMAT_ARGB32)
		return;

	for (uint32_t y = 0; y < image->height; y++) {
		uint32_t *dstp = (uint32_t *) (image->data + image->stride * y);
		for (uint32_t x = 0; x < image->width; x++) {
			uint32_t argb = dstp[x], a = argb >> 24;
			dstp[x] = a << 24 |
				PREMULTIPLY8(a, 0xFF & (argb >> 16)) << 16 |
				PREMULTIPLY8(a, 0xFF & (argb >>  8)) <<  8 |
				PREMULTIPLY8(a, 0xFF &  argb);
		}
	}
}

#if defined HAVE_LCMS2 && LCMS_VERSION >= 2130

#define FIV_IO_PROFILE_ARGB32_PREMUL \
	(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_8_PREMUL : TYPE_ARGB_8_PREMUL)

static void
fiv_io_profile_argb32(FivIoImage *image,
	FivIoProfile *source, FivIoProfile *target)
{
	g_return_if_fail(image->format == CAIRO_FORMAT_ARGB32);

	// TODO: With g_no_cms_premultiplication,
	// this probably also needs to be wrapped in un-premultiplication.
	fiv_io_profile_rgb_direct(image->data, image->width, image->height,
		source, target,
		FIV_IO_PROFILE_ARGB32_PREMUL, FIV_IO_PROFILE_ARGB32_PREMUL);
}

void
fiv_io_profile_argb32_premultiply(
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	if (image->format != CAIRO_FORMAT_ARGB32) {
		fiv_io_profile_xrgb32(image, source, target);
	} else if (g_broken_cms_premul) {
		fiv_io_profile_xrgb32(image, source, target);
		fiv_io_premultiply_argb32(image);
	} else if (!fiv_io_profile_rgb_direct(image->data,
			image->width, image->height, source, target,
			FIV_IO_PROFILE_ARGB32, FIV_IO_PROFILE_ARGB32_PREMUL)) {
		g_debug("failed to create a premultiplying transform");
		fiv_io_premultiply_argb32(image);
	}
}

#else  // ! HAVE_LCMS2 || LCMS_VERSION < 2130

// TODO(p): Unpremultiply, transform, repremultiply. Or require lcms2>=2.13.
#define fiv_io_profile_argb32(surface, source, target)

static void
fiv_io_profile_argb32_premultiply(
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	fiv_io_profile_xrgb32(image, source, target);
	fiv_io_premultiply_argb32(image);
}

#endif  // ! HAVE_LCMS2 || LCMS_VERSION < 2130

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void
fiv_io_profile_any(
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	// TODO(p): Ensure we do colour management early enough, so that
	// no avoidable increase of quantization error occurs beforehands,
	// and also for correct alpha compositing.
	switch (image->format) {
	break; case CAIRO_FORMAT_RGB24:
		fiv_io_profile_xrgb32(image, source, target);
	break; case CAIRO_FORMAT_ARGB32:
		fiv_io_profile_argb32(image, source, target);
	break; default:
		g_debug("CM attempted on an unsupported surface format");
	}
}

// TODO(p): Offer better integration, upgrade the bit depth if appropriate.
FivIoImage *
fiv_io_profile_finalize(FivIoImage *image, FivIoProfile *target)
{
	if (!target)
		return image;

	for (FivIoImage *page = image; page != NULL; page = page->page_next)
		fiv_io_profile_page(page, target, fiv_io_profile_any);
	return image;
}

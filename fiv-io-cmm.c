//
// fiv-io-cmm.c: colour management
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
// TODO(p): Make it also possible to use Skia's skcms.
#ifdef HAVE_LCMS2
#include <lcms2.h>
#endif  // HAVE_LCMS2
#ifdef HAVE_LCMS2_FAST_FLOAT
#include <lcms2_fast_float.h>
#endif  // HAVE_LCMS2_FAST_FLOAT

// --- CMM-independent transforms ----------------------------------------------

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

// --- Profiles ----------------------------------------------------------------
#ifdef HAVE_LCMS2

struct _FivIoProfile {
	FivIoCmm *cmm;
	cmsHPROFILE profile;
};

GBytes *
fiv_io_profile_to_bytes(FivIoProfile *profile)
{
	cmsUInt32Number len = 0;
	(void) cmsSaveProfileToMem(profile, NULL, &len);
	gchar *data = g_malloc0(len);
	if (!cmsSaveProfileToMem(profile, data, &len)) {
		g_free(data);
		return NULL;
	}
	return g_bytes_new_take(data, len);
}

static FivIoProfile *
fiv_io_profile_new(FivIoCmm *cmm, cmsHPROFILE profile)
{
	FivIoProfile *self = g_new0(FivIoProfile, 1);
	self->cmm = g_object_ref(cmm);
	self->profile = profile;
	return self;
}

void
fiv_io_profile_free(FivIoProfile *self)
{
	cmsCloseProfile(self->profile);
	g_clear_object(&self->cmm);
	g_free(self);
}

#else  // ! HAVE_LCMS2

GBytes *fiv_io_profile_to_bytes(FivIoProfile *) { return NULL; }
void fiv_io_profile_free(FivIoProfile *) {}

#endif  // ! HAVE_LCMS2
// --- Contexts ----------------------------------------------------------------
#ifdef HAVE_LCMS2

struct _FivIoCmm {
	GObject parent_instance;
	cmsContext context;

	// https://github.com/mm2/Little-CMS/issues/430
	gboolean broken_premul;
};

G_DEFINE_TYPE(FivIoCmm, fiv_io_cmm, G_TYPE_OBJECT)

static void
fiv_io_cmm_finalize(GObject *gobject)
{
	FivIoCmm *self = FIV_IO_CMM(gobject);
	cmsDeleteContext(self->context);

	G_OBJECT_CLASS(fiv_io_cmm_parent_class)->finalize(gobject);
}

static void
fiv_io_cmm_class_init(FivIoCmmClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fiv_io_cmm_finalize;
}

static void
fiv_io_cmm_init(FivIoCmm *self)
{
	self->context = cmsCreateContext(NULL, self);
#ifdef HAVE_LCMS2_FAST_FLOAT
	if (cmsPluginTHR(self->context, cmsFastFloatExtensions()))
		self->broken_premul = LCMS_VERSION <= 2160;
#endif  // HAVE_LCMS2_FAST_FLOAT
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FivIoCmm *
fiv_io_cmm_get_default(void)
{
	static gsize initialization_value = 0;
	static FivIoCmm *default_ = NULL;
	if (g_once_init_enter(&initialization_value)) {
		gsize setup_value = 1;
		default_ = g_object_new(FIV_TYPE_IO_CMM, NULL);
		g_once_init_leave(&initialization_value, setup_value);
	}
	return default_;
}

FivIoProfile *
fiv_io_cmm_get_profile(FivIoCmm *self, const void *data, size_t len)
{
	g_return_val_if_fail(self != NULL, NULL);

	return fiv_io_profile_new(self,
		cmsOpenProfileFromMemTHR(self->context, data, len));
}

FivIoProfile *
fiv_io_cmm_get_profile_sRGB(FivIoCmm *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return fiv_io_profile_new(self,
		cmsCreate_sRGBProfileTHR(self->context));
}

FivIoProfile *
fiv_io_cmm_get_profile_parametric(FivIoCmm *self,
	double gamma, double whitepoint[2], double primaries[6])
{
	g_return_val_if_fail(self != NULL, NULL);

	const cmsCIExyY cmsWP = {whitepoint[0], whitepoint[1], 1.0};
	const cmsCIExyYTRIPLE cmsP = {
		{primaries[0], primaries[1], 1.0},
		{primaries[2], primaries[3], 1.0},
		{primaries[4], primaries[5], 1.0},
	};

	cmsToneCurve *curve = cmsBuildGamma(self->context, gamma);
	if (!curve)
		return NULL;

	cmsHPROFILE profile = cmsCreateRGBProfileTHR(self->context,
		&cmsWP, &cmsP, (cmsToneCurve *[3]){curve, curve, curve});
	cmsFreeToneCurve(curve);
	return fiv_io_profile_new(self, profile);
}

#else  // ! HAVE_LCMS2

FivIoCmm *
fiv_io_cmm_get_default()
{
	return NULL;
}

FivIoProfile *
fiv_io_cmm_get_profile(FivIoCmm *, const void *, size_t)
{
	return NULL;
}

FivIoProfile *
fiv_io_cmm_get_profile_sRGB(FivIoCmm *)
{
	return NULL;
}

FivIoProfile *
fiv_io_cmm_get_profile_parametric(FivIoCmm *, double, double[2], double[6])
{
	return NULL;
}

#endif  // ! HAVE_LCMS2
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FivIoProfile *
fiv_io_cmm_get_profile_sRGB_gamma(FivIoCmm *self, double gamma)
{
	return fiv_io_cmm_get_profile_parametric(self, gamma,
		(double[2]){0.3127, 0.3290},
		(double[6]){0.6400, 0.3300, 0.3000, 0.6000, 0.1500, 0.0600});
}

FivIoProfile *
fiv_io_cmm_get_profile_from_bytes(FivIoCmm *self, GBytes *bytes)
{
	gsize len = 0;
	gconstpointer p = g_bytes_get_data(bytes, &len);
	return fiv_io_cmm_get_profile(self, p, len);
}

// --- Image loading -----------------------------------------------------------
#ifdef HAVE_LCMS2

// TODO(p): In general, try to use CAIRO_FORMAT_RGB30 or CAIRO_FORMAT_RGBA128F.
#define FIV_IO_PROFILE_ARGB32 \
	(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_8 : TYPE_ARGB_8)
#define FIV_IO_PROFILE_4X16LE \
	(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_16 : TYPE_BGRA_16_SE)

void
fiv_io_cmm_cmyk(FivIoCmm *self,
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	g_return_if_fail(target == NULL || self != NULL);

	cmsHTRANSFORM transform = NULL;
	if (source && target) {
		transform = cmsCreateTransformTHR(self->context,
			source->profile, TYPE_CMYK_8_REV,
			target->profile, FIV_IO_PROFILE_ARGB32, INTENT_PERCEPTUAL, 0);
	}
	if (transform) {
		cmsDoTransform(
			transform, image->data, image->data, image->width * image->height);
		cmsDeleteTransform(transform);
		return;
	}
	trivial_cmyk_to_host_byte_order_argb(
		image->data, image->width * image->height);
}

static bool
fiv_io_cmm_rgb_direct(FivIoCmm *self, unsigned char *data, int w, int h,
	FivIoProfile *source, FivIoProfile *target,
	uint32_t source_format, uint32_t target_format)
{
	g_return_val_if_fail(target == NULL || self != NULL, false);

	// TODO(p): We should make this optional.
	FivIoProfile *src_fallback = NULL;
	if (target && !source)
		source = src_fallback = fiv_io_cmm_get_profile_sRGB(self);

	cmsHTRANSFORM transform = NULL;
	if (source && target) {
		transform = cmsCreateTransformTHR(self->context,
			source->profile, source_format,
			target->profile, target_format, INTENT_PERCEPTUAL, 0);
	}
	if (transform) {
		cmsDoTransform(transform, data, data, w * h);
		cmsDeleteTransform(transform);
	}
	if (src_fallback)
		fiv_io_profile_free(src_fallback);
	return transform != NULL;
}

static void
fiv_io_cmm_xrgb32(FivIoCmm *self,
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	fiv_io_cmm_rgb_direct(self, image->data, image->width, image->height,
		source, target, FIV_IO_PROFILE_ARGB32, FIV_IO_PROFILE_ARGB32);
}

void
fiv_io_cmm_4x16le_direct(FivIoCmm *self, unsigned char *data,
	int w, int h, FivIoProfile *source, FivIoProfile *target)
{
	fiv_io_cmm_rgb_direct(self, data, w, h, source, target,
		FIV_IO_PROFILE_4X16LE, FIV_IO_PROFILE_4X16LE);
}

#else  // ! HAVE_LCMS2

void
fiv_io_cmm_cmyk(FivIoCmm *, FivIoImage *image, FivIoProfile *, FivIoProfile *)
{
	trivial_cmyk_to_host_byte_order_argb(
		image->data, image->width * image->height);
}

static void
fiv_io_cmm_xrgb32(FivIoCmm *, FivIoImage *, FivIoProfile *, FivIoProfile *)
{
}

void
fiv_io_cmm_4x16le_direct(
	FivIoCmm *, unsigned char *, int, int, FivIoProfile *, FivIoProfile *)
{
}

#endif  // ! HAVE_LCMS2
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
#if defined HAVE_LCMS2 && LCMS_VERSION >= 2130

#define FIV_IO_PROFILE_ARGB32_PREMUL \
	(G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_8_PREMUL : TYPE_ARGB_8_PREMUL)

static void
fiv_io_cmm_argb32(FivIoCmm *self, FivIoImage *image,
	FivIoProfile *source, FivIoProfile *target)
{
	g_return_if_fail(image->format == CAIRO_FORMAT_ARGB32);

	// TODO: With self->broken_premul,
	// this probably also needs to be wrapped in un-premultiplication.
	fiv_io_cmm_rgb_direct(self, image->data, image->width, image->height,
		source, target,
		FIV_IO_PROFILE_ARGB32_PREMUL, FIV_IO_PROFILE_ARGB32_PREMUL);
}

void
fiv_io_cmm_argb32_premultiply(FivIoCmm *self,
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	g_return_if_fail(target == NULL || self != NULL);

	if (image->format != CAIRO_FORMAT_ARGB32) {
		fiv_io_cmm_xrgb32(self, image, source, target);
	} else if (!target || self->broken_premul) {
		fiv_io_cmm_xrgb32(self, image, source, target);
		fiv_io_premultiply_argb32(image);
	} else if (!fiv_io_cmm_rgb_direct(self, image->data,
			image->width, image->height, source, target,
			FIV_IO_PROFILE_ARGB32, FIV_IO_PROFILE_ARGB32_PREMUL)) {
		g_debug("failed to create a premultiplying transform");
		fiv_io_premultiply_argb32(image);
	}
}

#else  // ! HAVE_LCMS2 || LCMS_VERSION < 2130

static void
fiv_io_cmm_argb32(G_GNUC_UNUSED FivIoCmm *self, G_GNUC_UNUSED FivIoImage *image,
	G_GNUC_UNUSED FivIoProfile *source, G_GNUC_UNUSED FivIoProfile *target)
{
	// TODO(p): Unpremultiply, transform, repremultiply. Or require lcms2>=2.13.
}

void
fiv_io_cmm_argb32_premultiply(FivIoCmm *self,
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	fiv_io_cmm_xrgb32(self, image, source, target);
	fiv_io_premultiply_argb32(image);
}

#endif  // ! HAVE_LCMS2 || LCMS_VERSION < 2130
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void
fiv_io_cmm_page(FivIoCmm *self, FivIoImage *page, FivIoProfile *target,
	void (*frame_cb) (FivIoCmm *, FivIoImage *, FivIoProfile *, FivIoProfile *))
{
	FivIoProfile *source = NULL;
	if (page->icc)
		source = fiv_io_cmm_get_profile_from_bytes(self, page->icc);

	// TODO(p): All animations need to be composited in a linear colour space.
	for (FivIoImage *frame = page; frame != NULL; frame = frame->frame_next)
		frame_cb(self, frame, source, target);

	if (source)
		fiv_io_profile_free(source);
}

void
fiv_io_cmm_any(FivIoCmm *self,
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target)
{
	// TODO(p): Ensure we do colour management early enough, so that
	// no avoidable increase of quantization error occurs beforehands,
	// and also for correct alpha compositing.
	switch (image->format) {
	break; case CAIRO_FORMAT_RGB24:
		fiv_io_cmm_xrgb32(self, image, source, target);
	break; case CAIRO_FORMAT_ARGB32:
		fiv_io_cmm_argb32(self, image, source, target);
	break; default:
		g_debug("CM attempted on an unsupported surface format");
	}
}

// TODO(p): Offer better integration, upgrade the bit depth if appropriate.
FivIoImage *
fiv_io_cmm_finish(FivIoCmm *self, FivIoImage *image, FivIoProfile *target)
{
	if (!target)
		return image;

	for (FivIoImage *page = image; page != NULL; page = page->page_next)
		fiv_io_cmm_page(self, page, target, fiv_io_cmm_any);
	return image;
}

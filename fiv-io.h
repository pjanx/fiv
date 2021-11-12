//
// fiv-io.h: image operations
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

#pragma once

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>
#include <webp/encode.h>  // WebPConfig

typedef enum _FivIoOrientation FivIoOrientation;
typedef struct _FivIoRenderClosure FivIoRenderClosure;
typedef struct _FivIoImage FivIoImage;
typedef struct _FivIoProfile FivIoProfile;

// --- Colour management -------------------------------------------------------
// Note that without a CMM, all FivIoCmm and FivIoProfile will be returned NULL.

GBytes *fiv_io_profile_to_bytes(FivIoProfile *profile);
void fiv_io_profile_free(FivIoProfile *self);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define FIV_TYPE_IO_CMM (fiv_io_cmm_get_type())
G_DECLARE_FINAL_TYPE(FivIoCmm, fiv_io_cmm, FIV, IO_CMM, GObject)

FivIoCmm *fiv_io_cmm_get_default(void);

FivIoProfile *fiv_io_cmm_get_profile(
	FivIoCmm *self, const void *data, size_t len);
FivIoProfile *fiv_io_cmm_get_profile_from_bytes(FivIoCmm *self, GBytes *bytes);
FivIoProfile *fiv_io_cmm_get_profile_sRGB(FivIoCmm *self);
FivIoProfile *fiv_io_cmm_get_profile_sRGB_gamma(FivIoCmm *self, double gamma);
FivIoProfile *fiv_io_cmm_get_profile_parametric(
	FivIoCmm *self, double gamma, double whitepoint[2], double primaries[6]);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void fiv_io_premultiply_argb32(FivIoImage *image);

void fiv_io_cmm_cmyk(FivIoCmm *self,
    FivIoImage *image, FivIoProfile *source, FivIoProfile *target);
void fiv_io_cmm_4x16le_direct(FivIoCmm *self, unsigned char *data,
    int w, int h, FivIoProfile *source, FivIoProfile *target);

void fiv_io_cmm_argb32_premultiply(FivIoCmm *self,
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target);
#define fiv_io_cmm_argb32_premultiply_page(cmm, page, target) \
	fiv_io_cmm_page((cmm), (page), (target), fiv_io_cmm_argb32_premultiply)

void fiv_io_cmm_page(FivIoCmm *self, FivIoImage *page, FivIoProfile *target,
	void (*frame_cb) (FivIoCmm *,
		FivIoImage *, FivIoProfile *, FivIoProfile *));
void fiv_io_cmm_any(FivIoCmm *self,
	FivIoImage *image, FivIoProfile *source, FivIoProfile *target);
FivIoImage *fiv_io_cmm_finish(FivIoCmm *self,
	FivIoImage *image, FivIoProfile *target);

// --- Loading -----------------------------------------------------------------

extern const char *fiv_io_supported_media_types[];

gchar **fiv_io_all_supported_media_types(void);

// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf Table 6
enum _FivIoOrientation {
	FivIoOrientationUnknown   = 0,
	FivIoOrientation0         = 1,
	FivIoOrientationMirror0   = 2,
	FivIoOrientation180       = 3,
	FivIoOrientationMirror180 = 4,
	FivIoOrientationMirror270 = 5,
	FivIoOrientation90        = 6,
	FivIoOrientationMirror90  = 7,
	FivIoOrientation270       = 8
};

// TODO(p): Maybe make FivIoProfile a referencable type,
// then loaders could store it in their closures.
struct _FivIoRenderClosure {
	/// The rendering is allowed to fail, returning NULL.
	FivIoImage *(*render)(
		FivIoRenderClosure *, FivIoCmm *, FivIoProfile *, double scale);
	void (*destroy)(FivIoRenderClosure *);
};

// Metadata are typically attached to all Cairo surfaces in an animation.

struct _FivIoImage {
	uint8_t *data;                      ///< Raw image data
	cairo_format_t format;              ///< Data format
	uint32_t width;                     ///< Width of the image in pixels
	uint32_t stride;                    ///< Row stride in bytes
	uint32_t height;                    ///< Height of the image in pixels

	FivIoOrientation orientation;       ///< Orientation to use for display

	GBytes *exif;                       ///< Raw Exif/TIFF segment
	GBytes *icc;                        ///< Raw ICC profile data
	GBytes *xmp;                        ///< Raw XMP data
	GBytes *thum;                       ///< WebP THUM chunk, for our thumbnails

	/// GHashTable with key-value pairs from PNG's tEXt, zTXt, iTXt chunks.
	/// Currently only read by fiv_io_open_png_thumbnail().
	GHashTable *text;

	/// A FivIoRenderClosure for parametrized re-rendering of vector formats.
	/// This is attached at the page level.
	FivIoRenderClosure *render;

	/// The first frame of the next page, in a chain.
	/// There is no wrap-around.
	FivIoImage *page_next;

	/// The first frame of the previous page, in a chain.
	/// There is no wrap-around. This is a weak pointer.
	FivIoImage *page_previous;

	/// The next frame in a sequence, in a chain, pre-composited.
	/// There is no wrap-around.
	FivIoImage *frame_next;

	/// The previous frame in a sequence, in a chain, pre-composited.
	/// This is a weak pointer that wraps around,
	/// and needn't be present for static images.
	FivIoImage *frame_previous;

	/// Frame duration in milliseconds.
	int64_t frame_duration;

	/// How many times to repeat the animation, or zero for +inf.
	uint64_t loops;
};

FivIoImage *fiv_io_image_ref(FivIoImage *image);
void fiv_io_image_unref(FivIoImage *image);

/// Analogous to cairo_image_surface_create(). May return NULL.
FivIoImage *fiv_io_image_new(
	cairo_format_t format, uint32_t width, uint32_t height);

/// Return a new Cairo image surface referencing the same data as the image,
/// eating the reference to it.
cairo_surface_t *fiv_io_image_to_surface(FivIoImage *image);

/// Return a new Cairo image surface referencing the same data as the image,
/// without eating the image's reference.
cairo_surface_t *fiv_io_image_to_surface_noref(const FivIoImage *image);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

typedef struct {
	const char *uri;                    ///< Source URI
	FivIoCmm *cmm;                      ///< Colour management module or NULL
	FivIoProfile *screen_profile;       ///< Target colour space or NULL
	int screen_dpi;                     ///< Target DPI
	gboolean enhance;                   ///< Enhance JPEG (currently)
	gboolean first_frame_only;          ///< Only interested in the 1st frame
	GPtrArray *warnings;                ///< String vector for non-fatal errors
} FivIoOpenContext;

FivIoImage *fiv_io_open(const FivIoOpenContext *ctx, GError **error);
FivIoImage *fiv_io_open_from_data(
	const char *data, size_t len, const FivIoOpenContext *ctx, GError **error);

FivIoImage *fiv_io_open_png_thumbnail(const char *path, GError **error);

// --- Metadata ----------------------------------------------------------------

/// Returns a rendering matrix for an image (user space to pattern space),
/// and its target dimensions.
cairo_matrix_t fiv_io_orientation_apply(const FivIoImage *image,
	FivIoOrientation orientation, double *width, double *height);
cairo_matrix_t fiv_io_orientation_matrix(
	FivIoOrientation orientation, double width, double height);
void fiv_io_orientation_dimensions(const FivIoImage *image,
	FivIoOrientation orientation, double *width, double *height);

/// Extracts the orientation field from Exif, if there's any.
FivIoOrientation fiv_io_exif_orientation(const guint8 *exif, gsize len);

/// Save metadata attached by this module in Exiv2 format.
gboolean fiv_io_save_metadata(
	const FivIoImage *page, const char *path, GError **error);

// --- Thumbnail passing utilities ---------------------------------------------

enum { FIV_IO_SERIALIZE_LOW_QUALITY = 1 << 0 };

void fiv_io_serialize_to_stdout(cairo_surface_t *surface, guint64 user_data);
cairo_surface_t *fiv_io_deserialize(GBytes *bytes, guint64 *user_data);

GBytes *fiv_io_serialize_for_search(cairo_surface_t *surface, GError **error);

// --- Export ------------------------------------------------------------------

/// Encodes an image as a WebP bitstream, following the configuration.
/// The result needs to be freed using WebPFree/WebPDataClear().
unsigned char *fiv_io_encode_webp(
	FivIoImage *image, const WebPConfig *config, size_t *len);

/// Saves the page as a lossless WebP still picture or animation.
/// If no exact frame is specified, this potentially creates an animation.
gboolean fiv_io_save(FivIoImage *page, FivIoImage *frame,
	FivIoProfile *target, const char *path, GError **error);

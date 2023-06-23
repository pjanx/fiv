//
// fiv-io.h: image operations
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

#pragma once

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>
#include <webp/encode.h>  // WebPConfig

// --- Colour management -------------------------------------------------------

// TODO(p): Make it also possible to use Skia's skcms.
typedef void *FivIoProfile;
FivIoProfile fiv_io_profile_new(const void *data, size_t len);
FivIoProfile fiv_io_profile_new_sRGB(void);
void fiv_io_profile_free(FivIoProfile self);

// From libwebp, verified to exactly match [x * a / 255].
#define PREMULTIPLY8(a, x) (((uint32_t) (x) * (uint32_t) (a) * 32897U) >> 23)

// --- Metadata ----------------------------------------------------------------

// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf Table 6
typedef enum _FivIoOrientation {
	FivIoOrientationUnknown   = 0,
	FivIoOrientation0         = 1,
	FivIoOrientationMirror0   = 2,
	FivIoOrientation180       = 3,
	FivIoOrientationMirror180 = 4,
	FivIoOrientationMirror270 = 5,
	FivIoOrientation90        = 6,
	FivIoOrientationMirror90  = 7,
	FivIoOrientation270       = 8
} FivIoOrientation;

/// Returns a rendering matrix for a surface (user space to pattern space),
/// and its target dimensions.
cairo_matrix_t fiv_io_orientation_apply(cairo_surface_t *surface,
	FivIoOrientation orientation, double *width, double *height);
void fiv_io_orientation_dimensions(cairo_surface_t *surface,
	FivIoOrientation orientation, double *width, double *height);

/// Extracts the orientation field from Exif, if there's any.
FivIoOrientation fiv_io_exif_orientation(const guint8 *exif, gsize len);

/// Save metadata attached by this module in Exiv2 format.
gboolean fiv_io_save_metadata(
	cairo_surface_t *page, const char *path, GError **error);

// --- Loading -----------------------------------------------------------------

extern const char *fiv_io_supported_media_types[];

gchar **fiv_io_all_supported_media_types(void);

typedef struct _FivIoRenderClosure {
	/// The rendering is allowed to fail, returning NULL.
	cairo_surface_t *(*render)(struct _FivIoRenderClosure *, double scale);
	void (*destroy)(struct _FivIoRenderClosure *);
} FivIoRenderClosure;

typedef struct _FivIoImage {
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

	/// A FivIoRenderClosure for parametrized re-rendering of vector formats.
	/// This is attached at the page level.
	FivIoRenderClosure *render;

	/// The first frame of the next page, in a chain.
	/// There is no wrap-around.
	struct _FivIoImage *page_next;

	/// The first frame of the previous page, in a chain.
	/// There is no wrap-around. This is a weak pointer.
	struct _FivIoImage *page_previous;

	/// The next frame in a sequence, in a chain, pre-composited.
	/// There is no wrap-around.
	struct _FivIoImage *frame_next;

	/// The previous frame in a sequence, in a chain, pre-composited.
	/// This is a weak pointer that wraps around,
	/// and needn't be present for static images.
	struct _FivIoImage *frame_previous;

	/// Frame duration in milliseconds.
	int64_t frame_duration;

	/// How many times to repeat the animation, or zero for +inf.
	uint64_t loops;
} FivIoImage;

FivIoImage *fiv_io_image_ref(FivIoImage *image);
void fiv_io_image_unref(FivIoImage *image);

/// Return a new Cairo image surface referencing the same data as the image,
/// eating the reference to it.
cairo_surface_t *fiv_io_image_to_surface(FivIoImage *image);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Userdata are typically attached to all Cairo surfaces in an animation.

/// GBytes with plain Exif/TIFF data.
extern cairo_user_data_key_t fiv_io_key_exif;
/// FivIoOrientation, as a uintptr_t.
extern cairo_user_data_key_t fiv_io_key_orientation;
/// GBytes with plain ICC profile data.
extern cairo_user_data_key_t fiv_io_key_icc;
/// GBytes with plain XMP data.
extern cairo_user_data_key_t fiv_io_key_xmp;
/// GBytes with a WebP's THUM chunk, used for our thumbnails.
extern cairo_user_data_key_t fiv_io_key_thum;

/// GHashTable with key-value pairs from PNG's tEXt, zTXt, iTXt chunks.
/// Currently only read by fiv_io_open_png_thumbnail().
extern cairo_user_data_key_t fiv_io_key_text;

/// The next frame in a sequence, as a surface, in a chain, pre-composited.
/// There is no wrap-around.
extern cairo_user_data_key_t fiv_io_key_frame_next;
/// The previous frame in a sequence, as a surface, in a chain, pre-composited.
/// This is a weak pointer that wraps around, and needn't be present
/// for static images.
extern cairo_user_data_key_t fiv_io_key_frame_previous;
/// Frame duration in milliseconds as an intptr_t.
extern cairo_user_data_key_t fiv_io_key_frame_duration;
/// How many times to repeat the animation, or zero for +inf, as a uintptr_t.
extern cairo_user_data_key_t fiv_io_key_loops;

/// The first frame of the next page, as a surface, in a chain.
/// There is no wrap-around.
extern cairo_user_data_key_t fiv_io_key_page_next;
/// The first frame of the previous page, as a surface, in a chain.
/// There is no wrap-around. This is a weak pointer.
extern cairo_user_data_key_t fiv_io_key_page_previous;

/// A FivIoRenderClosure for parametrized re-rendering of vector formats.
/// This is attached at the page level.
/// The rendered image will not have this key.
extern cairo_user_data_key_t fiv_io_key_render;

typedef struct {
	const char *uri;                    ///< Source URI
	FivIoProfile screen_profile;        ///< Target colour space or NULL
	int screen_dpi;                     ///< Target DPI
	gboolean enhance;                   ///< Enhance JPEG (currently)
	gboolean first_frame_only;          ///< Only interested in the 1st frame
	GPtrArray *warnings;                ///< String vector for non-fatal errors
} FivIoOpenContext;

FivIoImage *fiv_io_open(const FivIoOpenContext *ctx, GError **error);
FivIoImage *fiv_io_open_from_data(
	const char *data, size_t len, const FivIoOpenContext *ctx, GError **error);

cairo_surface_t *fiv_io_open_png_thumbnail(const char *path, GError **error);

// --- Thumbnail passing utilities ---------------------------------------------

enum { FIV_IO_SERIALIZE_LOW_QUALITY = 1 << 0 };

void fiv_io_serialize_to_stdout(cairo_surface_t *surface, guint64 user_data);
cairo_surface_t *fiv_io_deserialize(GBytes *bytes, guint64 *user_data);

GBytes *fiv_io_serialize_for_search(cairo_surface_t *surface, GError **error);

// --- Export ------------------------------------------------------------------

/// Encodes a Cairo surface as a WebP bitstream, following the configuration.
/// The result needs to be freed using WebPFree/WebPDataClear().
unsigned char *fiv_io_encode_webp(
	cairo_surface_t *surface, const WebPConfig *config, size_t *len);

/// Saves the page as a lossless WebP still picture or animation.
/// If no exact frame is specified, this potentially creates an animation.
gboolean fiv_io_save(cairo_surface_t *page, cairo_surface_t *frame,
	FivIoProfile target, const char *path, GError **error);

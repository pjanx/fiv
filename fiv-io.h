//
// fiv-io.h: image operations
//
// Copyright (c) 2021 - 2022, Přemysl Eric Janouch <p@janouch.name>
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

// TODO(p): Make it possible to use Skia's skcms,
// which also supports premultiplied alpha.
// NOTE: Little CMS 2.13 will support premultiplied alpha in 2022.
typedef void *FivIoProfile;
FivIoProfile fiv_io_profile_new(const void *data, size_t len);
FivIoProfile fiv_io_profile_new_sRGB(void);
void fiv_io_profile_free(FivIoProfile self);

// From libwebp, verified to exactly match [x * a / 255].
#define PREMULTIPLY8(a, x) (((uint32_t) (x) * (uint32_t) (a) * 32897U) >> 23)

// --- Loading -----------------------------------------------------------------

extern const char *fiv_io_supported_media_types[];

gchar **fiv_io_all_supported_media_types(void);

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

typedef struct _FivIoRenderClosure {
	/// The rendering is allowed to fail, returning NULL.
	cairo_surface_t *(*render)(struct _FivIoRenderClosure *, double scale);
} FivIoRenderClosure;

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

cairo_surface_t *fiv_io_open(const FivIoOpenContext *ctx, GError **error);
cairo_surface_t *fiv_io_open_from_data(
	const char *data, size_t len, const FivIoOpenContext *ctx, GError **error);
cairo_surface_t *fiv_io_open_png_thumbnail(const char *path, GError **error);

// --- Thumbnail passing utilities ---------------------------------------------

enum { FIV_IO_SERIALIZE_LOW_QUALITY = 1 << 0 };

void fiv_io_serialize_to_stdout(cairo_surface_t *surface, guint64 user_data);
cairo_surface_t *fiv_io_deserialize(GBytes *bytes, guint64 *user_data);

// --- Filesystem --------------------------------------------------------------

typedef enum _FivIoModelSort {
	FIV_IO_MODEL_SORT_NAME,
	FIV_IO_MODEL_SORT_MTIME,
	FIV_IO_MODEL_SORT_COUNT,

	FIV_IO_MODEL_SORT_MIN = 0,
	FIV_IO_MODEL_SORT_MAX = FIV_IO_MODEL_SORT_COUNT - 1
} FivIoModelSort;

#define FIV_TYPE_IO_MODEL (fiv_io_model_get_type())
G_DECLARE_FINAL_TYPE(FivIoModel, fiv_io_model, FIV, IO_MODEL, GObject)

/// Loads a directory. Clears itself even on failure.
gboolean fiv_io_model_open(FivIoModel *self, GFile *directory, GError **error);

/// Returns the current location as a GFile.
/// There is no ownership transfer, and the object may be NULL.
GFile *fiv_io_model_get_location(FivIoModel *self);

typedef struct {
	gchar *uri;                         ///< GIO URI
	gchar *collate_key;                 ///< Collate key for the filename
	gint64 mtime_msec;                  ///< Modification time in milliseconds
} FivIoModelEntry;

const FivIoModelEntry *fiv_io_model_get_files(FivIoModel *self, gsize *len);
const FivIoModelEntry *fiv_io_model_get_subdirs(FivIoModel *self, gsize *len);

// --- Export ------------------------------------------------------------------

/// Encodes a Cairo surface as a WebP bitstream, following the configuration.
/// The result needs to be freed using WebPFree/WebPDataClear().
unsigned char *fiv_io_encode_webp(
	cairo_surface_t *surface, const WebPConfig *config, size_t *len);

/// Saves the page as a lossless WebP still picture or animation.
/// If no exact frame is specified, this potentially creates an animation.
gboolean fiv_io_save(cairo_surface_t *page, cairo_surface_t *frame,
	FivIoProfile target, const char *path, GError **error);

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

/// Returns a rendering matrix for a surface, and its target dimensions.
cairo_matrix_t fiv_io_orientation_apply(cairo_surface_t *surface,
	FivIoOrientation orientation, double *width, double *height);
void fiv_io_orientation_dimensions(cairo_surface_t *surface,
	FivIoOrientation orientation, double *width, double *height);

/// Extracts the orientation field from Exif, if there's any.
FivIoOrientation fiv_io_exif_orientation(const guint8 *exif, gsize len);

/// Save metadata attached by this module in Exiv2 format.
gboolean fiv_io_save_metadata(
	cairo_surface_t *page, const char *path, GError **error);

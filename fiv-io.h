//
// fiv-io.h: image operations
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

#pragma once

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>

// --- Colour management -------------------------------------------------------

// TODO(p): Make it possible to use Skia's skcms,
// which also supports premultiplied alpha.
// NOTE: Little CMS will probably start supporting premultiplied alpha in 2022.
typedef void *FivIoProfile;
FivIoProfile fiv_io_profile_new(const void *data, size_t len);
FivIoProfile fiv_io_profile_new_sRGB(void);
void fiv_io_profile_free(FivIoProfile self);

// --- Loading -----------------------------------------------------------------

extern const char *fiv_io_supported_media_types[];

char **fiv_io_all_supported_media_types(void);

// Userdata are typically attached to all Cairo surfaces in an animation.

/// GBytes with plain Exif/TIFF data.
extern cairo_user_data_key_t fiv_io_key_exif;
/// FivIoOrientation, as a uintptr_t.
extern cairo_user_data_key_t fiv_io_key_orientation;
/// GBytes with plain ICC profile data.
extern cairo_user_data_key_t fiv_io_key_icc;
/// GBytes with plain XMP data.
extern cairo_user_data_key_t fiv_io_key_xmp;

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

/// If non-NULL, indicates a thumbnail of insufficient quality.
extern cairo_user_data_key_t fiv_io_key_thumbnail_lq;

cairo_surface_t *fiv_io_open(
	const gchar *path, FivIoProfile profile, gboolean enhance, GError **error);
cairo_surface_t *fiv_io_open_from_data(const char *data, size_t len,
	const gchar *path, FivIoProfile profile, gboolean enhance, GError **error);

int fiv_io_filecmp(GFile *f1, GFile *f2);

// --- Export ------------------------------------------------------------------

/// Saves the page as a lossless WebP still picture or animation.
/// If no exact frame is specified, this potentially creates an animation.
gboolean fiv_io_save(cairo_surface_t *page, cairo_surface_t *frame,
	FivIoProfile target, const gchar *path, GError **error);

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

FivIoOrientation fiv_io_exif_orientation(const guint8 *exif, gsize len);

/// Save metadata attached by this module in Exiv2 format.
gboolean fiv_io_save_metadata(
	cairo_surface_t *page, const gchar *path, GError **error);

// --- Thumbnails --------------------------------------------------------------

// And this is how you avoid glib-mkenums.
typedef enum _FivIoThumbnailSize {
#define FIV_IO_THUMBNAIL_SIZES(XX) \
	XX(SMALL,  128, "normal") \
	XX(NORMAL, 256, "large") \
	XX(LARGE,  512, "x-large") \
	XX(HUGE,  1024, "xx-large")
#define XX(name, value, dir) FIV_IO_THUMBNAIL_SIZE_ ## name,
	FIV_IO_THUMBNAIL_SIZES(XX)
#undef XX
	FIV_IO_THUMBNAIL_SIZE_COUNT,

	FIV_IO_THUMBNAIL_SIZE_MIN = 0,
	FIV_IO_THUMBNAIL_SIZE_MAX = FIV_IO_THUMBNAIL_SIZE_COUNT - 1
} FivIoThumbnailSize;

GType fiv_io_thumbnail_size_get_type(void) G_GNUC_CONST;
#define FIV_TYPE_IO_THUMBNAIL_SIZE (fiv_io_thumbnail_size_get_type())

typedef struct _FivIoThumbnailSizeInfo {
	int size;                           ///< Nominal size in pixels
	const char *thumbnail_spec_name;    ///< thumbnail-spec directory name
} FivIoThumbnailSizeInfo;

extern FivIoThumbnailSizeInfo
	fiv_io_thumbnail_sizes[FIV_IO_THUMBNAIL_SIZE_COUNT];

/// Returns this user's root thumbnail directory.
gchar *fiv_io_get_thumbnail_root(void);

/// Generates wide thumbnails of up to the specified size, saves them in cache.
gboolean fiv_io_produce_thumbnail(
	GFile *target, FivIoThumbnailSize size, GError **error);

/// Retrieves a thumbnail of the most appropriate quality and resolution
/// for the target file.
cairo_surface_t *fiv_io_lookup_thumbnail(
	GFile *target, FivIoThumbnailSize size);

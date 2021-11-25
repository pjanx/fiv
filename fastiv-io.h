//
// fastiv-io.h: image loaders
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
#include <glib.h>
#include <gio/gio.h>

extern const char *fastiv_io_supported_media_types[];

char **fastiv_io_all_supported_media_types(void);

// Userdata are typically attached to all Cairo surfaces in an animation.

/// GBytes with plain Exif data.
extern cairo_user_data_key_t fastiv_io_key_exif;
/// GBytes with plain ICC profile data.
extern cairo_user_data_key_t fastiv_io_key_icc;

/// The next frame in a sequence, as a surface, in a chain, pre-composited.
/// There is no wrap-around.
extern cairo_user_data_key_t fastiv_io_key_frame_next;
/// Frame duration in milliseconds as an intptr_t.
extern cairo_user_data_key_t fastiv_io_key_frame_duration;
/// How many times to repeat the animation, or zero for +inf, as a uintptr_t.
extern cairo_user_data_key_t fastiv_io_key_loops;

cairo_surface_t *fastiv_io_open(const gchar *path, GError **error);
cairo_surface_t *fastiv_io_open_from_data(
	const char *data, size_t len, const gchar *path, GError **error);

int fastiv_io_filecmp(GFile *f1, GFile *f2);

// --- Metadata ----------------------------------------------------------------

// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf Table 6
typedef enum _FastivIoOrientation {
	FastivIoOrientationUnknown   = 0,
	FastivIoOrientation0         = 1,
	FastivIoOrientationMirror0   = 2,
	FastivIoOrientation180       = 3,
	FastivIoOrientationMirror180 = 4,
	FastivIoOrientationMirror90  = 5,
	FastivIoOrientation90        = 6,
	FastivIoOrientationMirror270 = 7,
	FastivIoOrientation270       = 8
} FastivIoOrientation;

FastivIoOrientation fastiv_io_exif_orientation(const guint8 *exif, gsize len);

// --- Thumbnails --------------------------------------------------------------

// And this is how you avoid glib-mkenums.
typedef enum _FastivIoThumbnailSize {
#define FASTIV_IO_THUMBNAIL_SIZES(XX) \
	XX(SMALL,  128, "normal") \
	XX(NORMAL, 256, "large") \
	XX(LARGE,  512, "x-large") \
	XX(HUGE,  1024, "xx-large")
#define XX(name, value, dir) FASTIV_IO_THUMBNAIL_SIZE_ ## name,
	FASTIV_IO_THUMBNAIL_SIZES(XX)
#undef XX
	FASTIV_IO_THUMBNAIL_SIZE_COUNT,

	FASTIV_IO_THUMBNAIL_SIZE_MIN = 0,
	FASTIV_IO_THUMBNAIL_SIZE_MAX = FASTIV_IO_THUMBNAIL_SIZE_COUNT - 1
} FastivIoThumbnailSize;

GType fastiv_io_thumbnail_size_get_type(void) G_GNUC_CONST;
#define FASTIV_TYPE_IO_THUMBNAIL_SIZE (fastiv_io_thumbnail_size_get_type())

typedef struct _FastivIoThumbnailSizeInfo {
	int size;                           ///< Nominal size in pixels
	const char *thumbnail_spec_name;    ///< thumbnail-spec directory name
} FastivIoThumbnailSizeInfo;

extern FastivIoThumbnailSizeInfo
	fastiv_io_thumbnail_sizes[FASTIV_IO_THUMBNAIL_SIZE_COUNT];

cairo_surface_t *fastiv_io_lookup_thumbnail(
	const gchar *target, FastivIoThumbnailSize size);

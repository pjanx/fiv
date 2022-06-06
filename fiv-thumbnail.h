//
// fiv-thumbnail.h: thumbnail management
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

// And this is how you avoid glib-mkenums.
typedef enum _FivThumbnailSize {
#define FIV_THUMBNAIL_SIZES(XX) \
	XX(SMALL,  128, "normal") \
	XX(NORMAL, 256, "large") \
	XX(LARGE,  512, "x-large") \
	XX(HUGE,  1024, "xx-large")
#define XX(name, value, dir) FIV_THUMBNAIL_SIZE_ ## name,
	FIV_THUMBNAIL_SIZES(XX)
#undef XX
	FIV_THUMBNAIL_SIZE_COUNT,

	FIV_THUMBNAIL_SIZE_MIN = 0,
	FIV_THUMBNAIL_SIZE_MAX = FIV_THUMBNAIL_SIZE_COUNT - 1
} FivThumbnailSize;

GType fiv_thumbnail_size_get_type(void) G_GNUC_CONST;
#define FIV_TYPE_THUMBNAIL_SIZE (fiv_thumbnail_size_get_type())

typedef struct _FivThumbnailSizeInfo {
	int size;                           ///< Nominal size in pixels
	const char *thumbnail_spec_name;    ///< thumbnail-spec directory name
} FivThumbnailSizeInfo;

enum { FIV_THUMBNAIL_WIDE_COEFFICIENT = 2 };

extern FivThumbnailSizeInfo fiv_thumbnail_sizes[FIV_THUMBNAIL_SIZE_COUNT];

/// If non-NULL, indicates a thumbnail of insufficient quality.
extern cairo_user_data_key_t fiv_thumbnail_key_lq;

/// Returns this user's root thumbnail directory.
gchar *fiv_thumbnail_get_root(void);

/// Generates wide thumbnails of up to the specified size, saves them in cache.
/// Returns the surface used for the maximum size (if the pointer was NULL).
gboolean fiv_thumbnail_produce(GFile *target, FivThumbnailSize max_size,
	cairo_surface_t **max_size_surface, GError **error);

/// Retrieves a thumbnail of the most appropriate quality and resolution
/// for the target file.
cairo_surface_t *fiv_thumbnail_lookup(
	char *uri, gint64 mtime_msec, FivThumbnailSize size);

/// Invalidate the wide thumbnail cache. May write to standard streams.
void fiv_thumbnail_invalidate(void);

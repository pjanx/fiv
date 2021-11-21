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

// And this is how you avoid glib-mkenums.
typedef enum _FastivIoThumbnailSize {
#define FASTIV_IO_THUMBNAIL_SIZES(XX) \
	XX(SMALL,  128, "normal") \
	XX(NORMAL, 256, "large") \
	XX(LARGE,  512, "x-large") \
	XX(HUGE,  1024, "xx-large")
#define XX(name, value, dir) FASTIV_IO_THUMBNAIL_SIZE_ ## name = value,
	FASTIV_IO_THUMBNAIL_SIZES(XX)
#undef XX
	FASTIV_IO_THUMBNAIL_SIZE_MIN = FASTIV_IO_THUMBNAIL_SIZE_SMALL,
	FASTIV_IO_THUMBNAIL_SIZE_MAX = FASTIV_IO_THUMBNAIL_SIZE_HUGE
} FastivIoThumbnailSize;

GType fastiv_io_thumbnail_size_get_type(void) G_GNUC_CONST;
#define FASTIV_TYPE_IO_THUMBNAIL_SIZE (fastiv_io_thumbnail_size_get_type())

typedef struct _FastivIoThumbnailSizeInfo {
	FastivIoThumbnailSize size;         ///< Nominal size in pixels
	const char *directory_name;         ///< thumbnail-spec directory name
} FastivIoThumbnailSizeInfo;

// The array is null-terminated.
extern FastivIoThumbnailSizeInfo fastiv_io_thumbnail_sizes[];

cairo_surface_t *fastiv_io_open(const gchar *path, GError **error);
cairo_surface_t *fastiv_io_open_from_data(
	const char *data, size_t len, const gchar *path, GError **error);
cairo_surface_t *fastiv_io_lookup_thumbnail(
	const gchar *target, FastivIoThumbnailSize size);

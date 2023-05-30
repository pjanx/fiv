//
// fiv-io-model.h: filesystem
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

#include <gio/gio.h>
#include <glib.h>

// Avoid glib-mkenums.
typedef enum _FivIoModelSort {
#define FIV_IO_MODEL_SORTS(XX) \
	XX(NAME) \
	XX(MTIME)
#define XX(name) FIV_IO_MODEL_SORT_ ## name,
	FIV_IO_MODEL_SORTS(XX)
#undef XX
	FIV_IO_MODEL_SORT_COUNT
} FivIoModelSort;

GType fiv_io_model_sort_get_type(void) G_GNUC_CONST;
#define FIV_TYPE_IO_MODEL_SORT (fiv_io_model_sort_get_type())

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

typedef struct {
	const char *uri;                    ///< GIO URI
	const char *target_uri;             ///< GIO URI for any target
	const char *display_name;           ///< Label for the file
	const char *collate_key;            ///< Collate key for the filename
	guint64 filesize;                   ///< Filesize in bytes
	gint64 mtime_msec;                  ///< Modification time in milliseconds
} FivIoModelEntry;

GType fiv_io_model_entry_get_type(void) G_GNUC_CONST;
#define FIV_TYPE_IO_MODEL_ENTRY (fiv_io_model_entry_get_type())

FivIoModelEntry *fiv_io_model_entry_ref(FivIoModelEntry *self);
void fiv_io_model_entry_unref(FivIoModelEntry *self);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define FIV_TYPE_IO_MODEL (fiv_io_model_get_type())
G_DECLARE_FINAL_TYPE(FivIoModel, fiv_io_model, FIV, IO_MODEL, GObject)

/// Loads a directory. Clears itself even on failure.
gboolean fiv_io_model_open(FivIoModel *self, GFile *directory, GError **error);

/// Returns the current location as a GFile.
/// There is no ownership transfer, and the object may be NULL.
GFile *fiv_io_model_get_location(FivIoModel *self);

/// Returns the previous VFS directory in order, or NULL.
GFile *fiv_io_model_get_previous_directory(FivIoModel *self);
/// Returns the next VFS directory in order, or NULL.
GFile *fiv_io_model_get_next_directory(FivIoModel *self);

FivIoModelEntry *const *fiv_io_model_get_files(FivIoModel *self, gsize *len);
FivIoModelEntry *const *fiv_io_model_get_subdirs(FivIoModel *self, gsize *len);

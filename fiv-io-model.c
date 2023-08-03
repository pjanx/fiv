//
// fiv-io-model.c: filesystem
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

#include "fiv-io.h"
#include "fiv-io-model.h"
#include "xdg.h"

GType
fiv_io_model_sort_get_type(void)
{
	static gsize guard;
	if (g_once_init_enter(&guard)) {
#define XX(name) {FIV_IO_MODEL_SORT_ ## name, \
	"FIV_IO_MODEL_SORT_" #name, #name},
		static const GEnumValue values[] = {FIV_IO_MODEL_SORTS(XX) {}};
#undef XX
		GType type = g_enum_register_static(
			g_intern_static_string("FivIoModelSort"), values);
		g_once_init_leave(&guard, type);
	}
	return guard;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

G_DEFINE_BOXED_TYPE(FivIoModelEntry, fiv_io_model_entry,
	fiv_io_model_entry_ref, fiv_io_model_entry_unref)

FivIoModelEntry *
fiv_io_model_entry_ref(FivIoModelEntry *self)
{
	return g_rc_box_acquire(self);
}

void
fiv_io_model_entry_unref(FivIoModelEntry *self)
{
	g_rc_box_release(self);
}

static size_t
entry_strsize(const char *string)
{
	if (!string)
		return 0;

	return strlen(string) + 1;
}

static char *
entry_strappend(char **p, const char *string, size_t size)
{
	if (!string)
		return NULL;

	char *destination = memcpy(*p, string, size);
	*p += size;
	return destination;
}

// See model_load_attributes for a (superset of a) list of required attributes.
static FivIoModelEntry *
entry_new(GFile *file, GFileInfo *info)
{
	gchar *uri = g_file_get_uri(file);
	const gchar *target_uri = g_file_info_get_attribute_string(
		info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
	const gchar *display_name = g_file_info_get_display_name(info);

	// TODO(p): Make it possible to use g_utf8_collate_key() instead,
	// which does not use natural sorting.
	gchar *parse_name = g_file_get_parse_name(file);
	gchar *collate_key = g_utf8_collate_key_for_filename(parse_name, -1);
	g_free(parse_name);

	// The entries are immutable. Packing them into the structure
	// should help memory usage as well as performance.
	size_t size_uri          = entry_strsize(uri);
	size_t size_target_uri   = entry_strsize(target_uri);
	size_t size_display_name = entry_strsize(display_name);
	size_t size_collate_key  = entry_strsize(collate_key);

	FivIoModelEntry *entry = g_rc_box_alloc0(sizeof *entry +
		size_uri +
		size_target_uri +
		size_display_name +
		size_collate_key);

	gchar *p = (gchar *) entry + sizeof *entry;
	entry->uri          = entry_strappend(&p, uri, size_uri);
	entry->target_uri   = entry_strappend(&p, target_uri, size_target_uri);
	entry->display_name = entry_strappend(&p, display_name, size_display_name);
	entry->collate_key  = entry_strappend(&p, collate_key, size_collate_key);

	entry->filesize = (guint64) g_file_info_get_size(info);

	GDateTime *mtime = g_file_info_get_modification_date_time(info);
	if (mtime) {
		entry->mtime_msec = g_date_time_to_unix(mtime) * 1000 +
			g_date_time_get_microsecond(mtime) / 1000;
		g_date_time_unref(mtime);
	}

	g_free(uri);
	g_free(collate_key);
	return entry;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct _FivIoModel {
	GObject parent_instance;
	GPatternSpec **supported_patterns;

	GFile *directory;                   ///< Currently loaded directory
	GFileMonitor *monitor;              ///< "directory" monitoring
	GPtrArray *subdirs;                 ///< "directory" contents
	GPtrArray *files;                   ///< "directory" contents

	FivIoModelSort sort_field;          ///< How to sort
	gboolean sort_descending;           ///< Whether to sort in reverse
	gboolean filtering;                 ///< Only show non-hidden, supported
};

G_DEFINE_TYPE(FivIoModel, fiv_io_model, G_TYPE_OBJECT)

enum {
	PROP_FILTERING = 1,
	PROP_SORT_FIELD,
	PROP_SORT_DESCENDING,
	N_PROPERTIES
};

static GParamSpec *model_properties[N_PROPERTIES];

enum {
	RELOADED,
	FILES_CHANGED,
	SUBDIRECTORIES_CHANGED,
	LAST_SIGNAL,
};

// Globals are, sadly, the canonical way of storing signal numbers.
static guint model_signals[LAST_SIGNAL];

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static GPtrArray *
model_entry_array_new(void)
{
	return g_ptr_array_new_with_free_func(
		(GDestroyNotify) fiv_io_model_entry_unref);
}

#if !GLIB_CHECK_VERSION(2, 70, 0)
#define g_pattern_spec_match g_pattern_match
#endif

static gboolean
model_supports(FivIoModel *self, const char *filename)
{
	gchar *utf8 = g_filename_to_utf8(filename, -1, NULL, NULL, NULL);
	if (!utf8)
		return FALSE;

	gchar *lc = g_utf8_strdown(utf8, -1);
	gsize lc_length = strlen(lc);
	gchar *reversed = g_utf8_strreverse(lc, lc_length);
	g_free(utf8);

	// fnmatch() uses the /locale encoding/, and isn't present on Windows.
	// TODO(p): Consider using g_file_info_get_display_name() for direct UTF-8.
	gboolean result = FALSE;
	for (GPatternSpec **p = self->supported_patterns; *p; p++)
		if ((result = g_pattern_spec_match(*p, lc_length, lc, reversed)))
			break;

	g_free(lc);
	g_free(reversed);
	return result;
}

static inline int
model_compare_entries(FivIoModel *self,
	const FivIoModelEntry *entry1, GFile *file1,
	const FivIoModelEntry *entry2, GFile *file2)
{
	if (g_file_has_prefix(file1, file2))
		return +1;
	if (g_file_has_prefix(file2, file1))
		return -1;

	int result = 0;
	switch (self->sort_field) {
	case FIV_IO_MODEL_SORT_MTIME:
		result -= entry1->mtime_msec < entry2->mtime_msec;
		result += entry1->mtime_msec > entry2->mtime_msec;
		if (result != 0)
			break;

		// Fall-through
	case FIV_IO_MODEL_SORT_NAME:
	case FIV_IO_MODEL_SORT_COUNT:
		result = strcmp(entry1->collate_key, entry2->collate_key);
	}
	return self->sort_descending ? -result : +result;
}

static gint
model_compare(gconstpointer a, gconstpointer b, gpointer user_data)
{
	const FivIoModelEntry *entry1 = *(const FivIoModelEntry **) a;
	const FivIoModelEntry *entry2 = *(const FivIoModelEntry **) b;
	GFile *file1 = g_file_new_for_uri(entry1->uri);
	GFile *file2 = g_file_new_for_uri(entry2->uri);
	int result = model_compare_entries(user_data, entry1, file1, entry2, file2);
	g_object_unref(file1);
	g_object_unref(file2);
	return result;
}

static const char *model_load_attributes =
	G_FILE_ATTRIBUTE_STANDARD_TYPE ","
	G_FILE_ATTRIBUTE_STANDARD_NAME ","
	G_FILE_ATTRIBUTE_STANDARD_SIZE ","
	G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
	G_FILE_ATTRIBUTE_STANDARD_TARGET_URI ","
	G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
	G_FILE_ATTRIBUTE_TIME_MODIFIED ","
	G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC;

static GPtrArray *
model_decide_placement(
	FivIoModel *self, GFileInfo *info, GPtrArray *subdirs, GPtrArray *files)
{
	if (self->filtering && g_file_info_get_is_hidden(info))
		return NULL;
	if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
		return subdirs;
	if (!self->filtering ||
		model_supports(self, g_file_info_get_name(info)))
		return files;
	return NULL;
}

static gboolean
model_reload_to(FivIoModel *self, GFile *directory,
	GPtrArray *subdirs, GPtrArray *files, GError **error)
{
	if (subdirs)
		g_ptr_array_set_size(subdirs, 0);
	if (files)
		g_ptr_array_set_size(files, 0);

	GFileEnumerator *enumerator = g_file_enumerate_children(
		directory, model_load_attributes, G_FILE_QUERY_INFO_NONE, NULL, error);
	if (!enumerator)
		return FALSE;

	GFileInfo *info = NULL;
	GFile *child = NULL;
	GError *e = NULL;
	while (TRUE) {
		if (!g_file_enumerator_iterate(enumerator, &info, &child, NULL, &e) &&
			e) {
			g_warning("%s", e->message);
			g_clear_error(&e);
			continue;
		}
		if (!info)
			break;

		GPtrArray *target =
			model_decide_placement(self, info, subdirs, files);
		if (target)
			g_ptr_array_add(target, entry_new(child, info));
	}
	g_object_unref(enumerator);

	if (subdirs)
		g_ptr_array_sort_with_data(subdirs, model_compare, self);
	if (files)
		g_ptr_array_sort_with_data(files, model_compare, self);
	return TRUE;
}

static gboolean
model_reload(FivIoModel *self, GError **error)
{
	// Note that this will clear all entries on failure.
	gboolean result = model_reload_to(
		self, self->directory, self->subdirs, self->files, error);
	g_signal_emit(self, model_signals[RELOADED], 0);
	return result;
}

static void
model_resort(FivIoModel *self)
{
	g_ptr_array_sort_with_data(self->subdirs, model_compare, self);
	g_ptr_array_sort_with_data(self->files, model_compare, self);
	g_signal_emit(self, model_signals[RELOADED], 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gint
model_find(const GPtrArray *target, GFile *file, FivIoModelEntry **entry)
{
	for (guint i = 0; i < target->len; i++) {
		FivIoModelEntry *e = target->pdata[i];
		GFile *f = g_file_new_for_uri(e->uri);
		gboolean match = g_file_equal(f, file);
		g_object_unref(f);
		if (match) {
			*entry = e;
			return i;
		}
	}
	return -1;
}

enum monitor_event {
	MONITOR_NONE,
	MONITOR_CHANGING,
	MONITOR_RENAMING,
	MONITOR_REMOVING,
	MONITOR_ADDING
};

static void
monitor_apply(enum monitor_event event, GPtrArray *target, int index,
	FivIoModelEntry *new_entry)
{
	g_return_if_fail(event != MONITOR_CHANGING || index >= 0);

	if (event == MONITOR_RENAMING && index < 0)
		// The file used to be filtered out but isn't anymore.
		event = MONITOR_ADDING;
	else if (!new_entry && index >= 0)
		// The file wasn't filtered out but now it is.
		event = MONITOR_REMOVING;

	if (event == MONITOR_CHANGING) {
		fiv_io_model_entry_unref(target->pdata[index]);
		target->pdata[index] = fiv_io_model_entry_ref(new_entry);
	}
	if (event == MONITOR_REMOVING || event == MONITOR_RENAMING)
		g_ptr_array_remove_index(target, index);
	if (event == MONITOR_RENAMING || event == MONITOR_ADDING)
		g_ptr_array_add(target, fiv_io_model_entry_ref(new_entry));
}

static void
on_monitor_changed(G_GNUC_UNUSED GFileMonitor *monitor, GFile *file,
	GFile *other_file, GFileMonitorEvent event_type, gpointer user_data)
{
	FivIoModel *self = user_data;

	FivIoModelEntry *old_entry = NULL;
	gint files_index = model_find(self->files, file, &old_entry);
	gint subdirs_index = model_find(self->subdirs, file, &old_entry);

	enum monitor_event event = MONITOR_NONE;
	GFile *new_entry_file = NULL;
	switch (event_type) {
	case G_FILE_MONITOR_EVENT_CHANGED:
	case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
		event = MONITOR_CHANGING;
		new_entry_file = file;
		break;
	case G_FILE_MONITOR_EVENT_RENAMED:
		event = MONITOR_RENAMING;
		new_entry_file = other_file;
		break;
	case G_FILE_MONITOR_EVENT_DELETED:
	case G_FILE_MONITOR_EVENT_MOVED_OUT:
		event = MONITOR_REMOVING;
		break;
	case G_FILE_MONITOR_EVENT_CREATED:
	case G_FILE_MONITOR_EVENT_MOVED_IN:
		event = MONITOR_ADDING;
		old_entry = NULL;
		new_entry_file = file;
		break;

	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
		// TODO(p): Figure out if we can't make use of _CHANGES_DONE_HINT.
	case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
	case G_FILE_MONITOR_EVENT_UNMOUNTED:
		// TODO(p): Figure out how to handle _UNMOUNTED sensibly.
	case G_FILE_MONITOR_EVENT_MOVED:
		return;
	}

	FivIoModelEntry *new_entry = NULL;
	GPtrArray *new_target = NULL;
	if (new_entry_file) {
		GError *error = NULL;
		GFileInfo *info = g_file_query_info(new_entry_file,
			model_load_attributes, G_FILE_QUERY_INFO_NONE, NULL, &error);
		if (error) {
			g_debug("monitor: %s", error->message);
			g_error_free(error);
			goto run;
		}

		if ((new_target =
			model_decide_placement(self, info, self->subdirs, self->files)))
			new_entry = entry_new(new_entry_file, info);
		g_object_unref(info);

		if ((files_index != -1 && new_target == self->subdirs) ||
			(subdirs_index != -1 && new_target == self->files)) {
			g_debug("monitor: ignoring transfer between files and subdirs");
			goto out;
		}
	}

run:
	// Keep a reference alive so that signal handlers see the new arrays.
	if (old_entry)
		fiv_io_model_entry_ref(old_entry);

	if (files_index != -1 || new_target == self->files) {
		monitor_apply(event, self->files, files_index, new_entry);
		g_signal_emit(self, model_signals[FILES_CHANGED],
			0, old_entry, new_entry);
	}
	if (subdirs_index != -1 || new_target == self->subdirs) {
		monitor_apply(event, self->subdirs, subdirs_index, new_entry);
		g_signal_emit(self, model_signals[SUBDIRECTORIES_CHANGED],
			0, old_entry, new_entry);
	}

	// NOTE: It would make sense to do
	//   g_ptr_array_sort_with_data(self->{files,subdirs}, model_compare, self);
	// but then the iteration behaviour of fiv.c would differ from what's shown
	// in the browser. Perhaps we need to use an index-based, fully-synchronized
	// interface similar to GListModel::items-changed.

	if (old_entry)
		fiv_io_model_entry_unref(old_entry);
out:
	if (new_entry)
		fiv_io_model_entry_unref(new_entry);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// This would be more efficient iteratively, but it's not that important.
static GFile *
model_last_deep_subdirectory(FivIoModel *self, GFile *directory)
{
	GFile *result = NULL;
	GPtrArray *subdirs = model_entry_array_new();
	if (!model_reload_to(self, directory, subdirs, NULL, NULL))
		goto out;

	if (subdirs->len) {
		FivIoModelEntry *entry = g_ptr_array_index(subdirs, subdirs->len - 1);
		GFile *last = g_file_new_for_uri(entry->uri);
		result = model_last_deep_subdirectory(self, last);
		g_object_unref(last);
	} else {
		result = g_object_ref(directory);
	}

out:
	g_ptr_array_free(subdirs, TRUE);
	return result;
}

GFile *
fiv_io_model_get_previous_directory(FivIoModel *self)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(self), NULL);

	GFile *parent_directory = g_file_get_parent(self->directory);
	if (!parent_directory)
		return NULL;

	GFile *result = NULL;
	GPtrArray *subdirs = model_entry_array_new();
	if (!model_reload_to(self, parent_directory, subdirs, NULL, NULL))
		goto out;

	for (gsize i = 0; i < subdirs->len; i++) {
		FivIoModelEntry *entry = g_ptr_array_index(subdirs, i);
		GFile *file = g_file_new_for_uri(entry->uri);
		if (g_file_equal(file, self->directory)) {
			g_object_unref(file);
			break;
		}

		g_clear_object(&result);
		result = file;
	}
	if (result) {
		GFile *last = model_last_deep_subdirectory(self, result);
		g_object_unref(result);
		result = last;
	} else {
		result = g_object_ref(parent_directory);
	}

out:
	g_object_unref(parent_directory);
	g_ptr_array_free(subdirs, TRUE);
	return result;
}

// This would be more efficient iteratively, but it's not that important.
static GFile *
model_next_directory_within_parents(FivIoModel *self, GFile *directory)
{
	GFile *parent_directory = g_file_get_parent(directory);
	if (!parent_directory)
		return NULL;

	GFile *result = NULL;
	GPtrArray *subdirs = model_entry_array_new();
	if (!model_reload_to(self, parent_directory, subdirs, NULL, NULL))
		goto out;

	gboolean found_self = FALSE;
	for (gsize i = 0; i < subdirs->len; i++) {
		FivIoModelEntry *entry = g_ptr_array_index(subdirs, i);
		result = g_file_new_for_uri(entry->uri);
		if (found_self)
			goto out;

		found_self = g_file_equal(result, directory);
		g_clear_object(&result);
	}
	if (!result)
		result = model_next_directory_within_parents(self, parent_directory);

out:
	g_object_unref(parent_directory);
	g_ptr_array_free(subdirs, TRUE);
	return result;
}

GFile *
fiv_io_model_get_next_directory(FivIoModel *self)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(self), NULL);

	if (self->subdirs->len) {
		FivIoModelEntry *entry = g_ptr_array_index(self->subdirs, 0);
		return g_file_new_for_uri(entry->uri);
	}

	return model_next_directory_within_parents(self, self->directory);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
fiv_io_model_finalize(GObject *gobject)
{
	FivIoModel *self = FIV_IO_MODEL(gobject);
	for (GPatternSpec **p = self->supported_patterns; *p; p++)
		g_pattern_spec_free(*p);
	g_free(self->supported_patterns);

	g_clear_object(&self->directory);
	g_clear_object(&self->monitor);
	g_ptr_array_free(self->subdirs, TRUE);
	g_ptr_array_free(self->files, TRUE);

	G_OBJECT_CLASS(fiv_io_model_parent_class)->finalize(gobject);
}

static void
fiv_io_model_get_property(
	GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	FivIoModel *self = FIV_IO_MODEL(object);
	switch (property_id) {
	case PROP_FILTERING:
		g_value_set_boolean(value, self->filtering);
		break;
	case PROP_SORT_FIELD:
		g_value_set_enum(value, self->sort_field);
		break;
	case PROP_SORT_DESCENDING:
		g_value_set_boolean(value, self->sort_descending);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
fiv_io_model_set_property(
	GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	FivIoModel *self = FIV_IO_MODEL(object);
	switch (property_id) {
	case PROP_FILTERING:
		if (self->filtering != g_value_get_boolean(value)) {
			self->filtering = !self->filtering;
			g_object_notify_by_pspec(object, model_properties[property_id]);
			(void) model_reload(self, NULL /* error */);
		}
		break;
	case PROP_SORT_FIELD:
		if ((int) self->sort_field != g_value_get_enum(value)) {
			self->sort_field = g_value_get_enum(value);
			g_object_notify_by_pspec(object, model_properties[property_id]);
			model_resort(self);
		}
		break;
	case PROP_SORT_DESCENDING:
		if (self->sort_descending != g_value_get_boolean(value)) {
			self->sort_descending = !self->sort_descending;
			g_object_notify_by_pspec(object, model_properties[property_id]);
			model_resort(self);
		}
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
fiv_io_model_class_init(FivIoModelClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = fiv_io_model_get_property;
	object_class->set_property = fiv_io_model_set_property;
	object_class->finalize = fiv_io_model_finalize;

	model_properties[PROP_FILTERING] = g_param_spec_boolean(
		"filtering", "Filtering", "Only show non-hidden, supported entries",
		TRUE, G_PARAM_READWRITE);
	model_properties[PROP_SORT_FIELD] = g_param_spec_enum(
		"sort-field", "Sort field", "Sort order",
		FIV_TYPE_IO_MODEL_SORT, FIV_IO_MODEL_SORT_NAME, G_PARAM_READWRITE);
	model_properties[PROP_SORT_DESCENDING] = g_param_spec_boolean(
		"sort-descending", "Sort descending", "Use reverse sort order",
		FALSE, G_PARAM_READWRITE);
	g_object_class_install_properties(
		object_class, N_PROPERTIES, model_properties);

	// All entries might have changed.
	model_signals[RELOADED] =
		g_signal_new("reloaded", G_TYPE_FROM_CLASS(klass), 0, 0,
			NULL, NULL, NULL, G_TYPE_NONE, 0);

	model_signals[FILES_CHANGED] =
		g_signal_new("files-changed", G_TYPE_FROM_CLASS(klass), 0, 0,
			NULL, NULL, NULL,
			G_TYPE_NONE, 2, FIV_TYPE_IO_MODEL_ENTRY, FIV_TYPE_IO_MODEL_ENTRY);
	model_signals[SUBDIRECTORIES_CHANGED] =
		g_signal_new("subdirectories-changed", G_TYPE_FROM_CLASS(klass), 0, 0,
			NULL, NULL, NULL,
			G_TYPE_NONE, 2, FIV_TYPE_IO_MODEL_ENTRY, FIV_TYPE_IO_MODEL_ENTRY);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
fiv_io_model_init(FivIoModel *self)
{
	self->filtering = TRUE;

	char **types = fiv_io_all_supported_media_types();
	char **globs = extract_mime_globs((const char **) types);
	g_strfreev(types);

	gsize n = g_strv_length(globs);
	self->supported_patterns =
		g_malloc0_n(n + 1, sizeof *self->supported_patterns);
	while (n--)
		self->supported_patterns[n] = g_pattern_spec_new(globs[n]);
	g_strfreev(globs);

	self->files = model_entry_array_new();
	self->subdirs = model_entry_array_new();
}

gboolean
fiv_io_model_open(FivIoModel *self, GFile *directory, GError **error)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(self), FALSE);
	g_return_val_if_fail(G_IS_FILE(directory), FALSE);

	g_clear_object(&self->directory);
	g_clear_object(&self->monitor);
	self->directory = g_object_ref(directory);

	GError *e = NULL;
	if ((self->monitor = g_file_monitor_directory(
			directory, G_FILE_MONITOR_WATCH_MOVES, NULL, &e))) {
		g_signal_connect(self->monitor, "changed",
			G_CALLBACK(on_monitor_changed), self);
	} else {
		g_debug("directory monitoring failed: %s", e->message);
		g_error_free(e);
	}
	return model_reload(self, error);
}

GFile *
fiv_io_model_get_location(FivIoModel *self)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(self), NULL);
	return self->directory;
}

FivIoModelEntry *const *
fiv_io_model_get_files(FivIoModel *self, gsize *len)
{
	*len = self->files->len;
	return (FivIoModelEntry *const *) self->files->pdata;
}

FivIoModelEntry *const *
fiv_io_model_get_subdirs(FivIoModel *self, gsize *len)
{
	*len = self->subdirs->len;
	return (FivIoModelEntry *const *) self->subdirs->pdata;
}

//
// fiv-collection.c: GVfs extension for grouping arbitrary files together
//
// Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
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

#include <gio/gio.h>

#include "fiv-collection.h"

static struct {
	GFile **files;
	gsize files_len;
} g;

gboolean
fiv_collection_uri_matches(const char *uri)
{
	static const char prefix[] = FIV_COLLECTION_SCHEME ":";
	return !g_ascii_strncasecmp(uri, prefix, sizeof prefix - 1);
}

GFile **
fiv_collection_get_contents(gsize *len)
{
	*len = g.files_len;
	return g.files;
}

void
fiv_collection_reload(gchar **uris)
{
	if (g.files) {
		for (gsize i = 0; i < g.files_len; i++)
			g_object_unref(g.files[i]);
		g_free(g.files);
	}

	g.files_len = g_strv_length(uris);
	g.files = g_malloc0_n(g.files_len + 1, sizeof *g.files);
	for (gsize i = 0; i < g.files_len; i++)
		g.files[i] = g_file_new_for_uri(uris[i]);
}

// --- Declarations ------------------------------------------------------------

#define FIV_TYPE_COLLECTION_FILE (fiv_collection_file_get_type())
G_DECLARE_FINAL_TYPE(
	FivCollectionFile, fiv_collection_file, FIV, COLLECTION_FILE, GObject)

struct _FivCollectionFile {
	GObject parent_instance;

	gint index;                         ///< Original index into g.files, or -1
	GFile *target;                      ///< The wrapped file, or NULL for root
	gchar *subpath;                     ///< Any subpath, rooted at the target
};

#define FIV_TYPE_COLLECTION_ENUMERATOR (fiv_collection_enumerator_get_type())
G_DECLARE_FINAL_TYPE(FivCollectionEnumerator, fiv_collection_enumerator, FIV,
	COLLECTION_ENUMERATOR, GFileEnumerator)

struct _FivCollectionEnumerator {
	GFileEnumerator parent_instance;

	gchar *attributes;                  ///< Attributes to look up
	gsize index;                        ///< Root: index into g.files
	GFileEnumerator *subenumerator;     ///< Non-root: a wrapped enumerator
};

// --- Enumerator --------------------------------------------------------------

G_DEFINE_TYPE(
	FivCollectionEnumerator, fiv_collection_enumerator, G_TYPE_FILE_ENUMERATOR)

static void
fiv_collection_enumerator_finalize(GObject *object)
{
	FivCollectionEnumerator *self = FIV_COLLECTION_ENUMERATOR(object);
	g_free(self->attributes);
	g_clear_object(&self->subenumerator);
}

static GFileInfo *
fiv_collection_enumerator_next_file(GFileEnumerator *enumerator,
	GCancellable *cancellable, GError **error)
{
	FivCollectionEnumerator *self = FIV_COLLECTION_ENUMERATOR(enumerator);
	if (self->subenumerator) {
		GFileInfo *info = g_file_enumerator_next_file(
			self->subenumerator, cancellable, error);
		if (!info)
			return NULL;

		// TODO(p): Consider discarding certain classes of attributes
		// from the results (adjusting "attributes" is generally unreliable).
		GFile *target = g_file_enumerator_get_child(self->subenumerator, info);
		gchar *target_uri = g_file_get_uri(target);
		g_object_unref(target);
		g_file_info_set_attribute_string(
			info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, target_uri);
		g_free(target_uri);
		return info;
	}

	if (self->index >= g.files_len)
		return NULL;

	FivCollectionFile *file = g_object_new(FIV_TYPE_COLLECTION_FILE, NULL);
	file->index = self->index;
	file->target = g_object_ref(g.files[self->index++]);

	GFileInfo *info = g_file_query_info(G_FILE(file), self->attributes,
		G_FILE_QUERY_INFO_NONE, cancellable, error);
	g_object_unref(file);
	return info;
}

static gboolean
fiv_collection_enumerator_close(
	GFileEnumerator *enumerator, GCancellable *cancellable, GError **error)
{
	FivCollectionEnumerator *self = FIV_COLLECTION_ENUMERATOR(enumerator);
	if (self->subenumerator)
		return g_file_enumerator_close(self->subenumerator, cancellable, error);
	return TRUE;
}

static void
fiv_collection_enumerator_class_init(FivCollectionEnumeratorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fiv_collection_enumerator_finalize;

	GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS(klass);
	enumerator_class->next_file = fiv_collection_enumerator_next_file;
	enumerator_class->close_fn = fiv_collection_enumerator_close;
}

static void
fiv_collection_enumerator_init(G_GNUC_UNUSED FivCollectionEnumerator *self)
{
}

// --- Proxying GFile implementation -------------------------------------------

static void fiv_collection_file_file_iface_init(GFileIface *iface);

G_DEFINE_TYPE_WITH_CODE(FivCollectionFile, fiv_collection_file, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(G_TYPE_FILE, fiv_collection_file_file_iface_init))

static void
fiv_collection_file_finalize(GObject *object)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(object);
	if (self->target)
		g_object_unref(self->target);
	g_free(self->subpath);
}

static GFile *
fiv_collection_file_dup(GFile *file)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	FivCollectionFile *new = g_object_new(FIV_TYPE_COLLECTION_FILE, NULL);
	if (self->target)
		new->target = g_object_ref(self->target);
	new->subpath = g_strdup(self->subpath);
	return G_FILE(new);
}

static guint
fiv_collection_file_hash(GFile *file)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	guint hash = g_int_hash(&self->index);
	if (self->target)
		hash ^= g_file_hash(self->target);
	if (self->subpath)
		hash ^= g_str_hash(self->subpath);
	return hash;
}

static gboolean
fiv_collection_file_equal(GFile *file1, GFile *file2)
{
	FivCollectionFile *cf1 = FIV_COLLECTION_FILE(file1);
	FivCollectionFile *cf2 = FIV_COLLECTION_FILE(file2);
	return cf1->index == cf2->index && cf1->target == cf2->target &&
		!g_strcmp0(cf1->subpath, cf2->subpath);
}

static gboolean
fiv_collection_file_is_native(G_GNUC_UNUSED GFile *file)
{
	return FALSE;
}

static gboolean
fiv_collection_file_has_uri_scheme(
	G_GNUC_UNUSED GFile *file, const char *uri_scheme)
{
	return !g_ascii_strcasecmp(uri_scheme, FIV_COLLECTION_SCHEME);
}

static char *
fiv_collection_file_get_uri_scheme(G_GNUC_UNUSED GFile *file)
{
	return g_strdup(FIV_COLLECTION_SCHEME);
}

static char *
get_prefixed_name(FivCollectionFile *self, const char *name)
{
	return g_strdup_printf("%d. %s", self->index + 1, name);
}

static char *
get_target_basename(FivCollectionFile *self)
{
	g_return_val_if_fail(self->target != NULL, g_strdup(""));

	// The "http" scheme doesn't behave nicely, make something up if needed.
	// Foreign roots likewise need to be fixed up for our needs.
	gchar *basename = g_file_get_basename(self->target);
	if (!basename || *basename == '/') {
		g_free(basename);
		basename = g_file_get_uri_scheme(self->target);
	}

	gchar *name = get_prefixed_name(self, basename);
	g_free(basename);
	return name;
}

static char *
fiv_collection_file_get_basename(GFile *file)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	if (!self->target)
		return g_strdup("/");
	if (self->subpath)
		return g_path_get_basename(self->subpath);
	return get_target_basename(self);
}

static char *
fiv_collection_file_get_path(G_GNUC_UNUSED GFile *file)
{
	// This doesn't seem to be worth implementing (for compatible targets).
	return NULL;
}

static char *
get_unescaped_uri(FivCollectionFile *self)
{
	GString *unescaped = g_string_new(FIV_COLLECTION_SCHEME ":/");
	if (!self->target)
		return g_string_free(unescaped, FALSE);

	gchar *basename = get_target_basename(self);
	g_string_append(unescaped, basename);
	g_free(basename);
	if (self->subpath)
		g_string_append(g_string_append(unescaped, "/"), self->subpath);
	return g_string_free(unescaped, FALSE);
}

static char *
fiv_collection_file_get_uri(GFile *file)
{
	gchar *unescaped = get_unescaped_uri(FIV_COLLECTION_FILE(file));
	gchar *uri = g_uri_escape_string(
		unescaped, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
	g_free(unescaped);
	return uri;
}

static char *
fiv_collection_file_get_parse_name(GFile *file)
{
	gchar *unescaped = get_unescaped_uri(FIV_COLLECTION_FILE(file));
	gchar *parse_name = g_uri_escape_string(
		unescaped, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH " ", TRUE);
	g_free(unescaped);
	return parse_name;
}

static GFile *
fiv_collection_file_get_parent(GFile *file)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	if (!self->target)
		return NULL;

	FivCollectionFile *new = g_object_new(FIV_TYPE_COLLECTION_FILE, NULL);
	if (self->subpath) {
		new->index = self->index;
		new->target = g_object_ref(self->target);
		if (strchr(self->subpath, '/'))
			new->subpath = g_path_get_dirname(self->subpath);
	}
	return G_FILE(new);
}

static gboolean
fiv_collection_file_prefix_matches(GFile *prefix, GFile *file)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	FivCollectionFile *parent = FIV_COLLECTION_FILE(prefix);

	// The root has no parents.
	if (!self->target)
		return FALSE;

	// The root prefixes everything that is not the root.
	if (!parent->target)
		return TRUE;

	if (self->index != parent->index || !self->subpath)
		return FALSE;
	if (!parent->subpath)
		return TRUE;

	return g_str_has_prefix(self->subpath, parent->subpath) &&
		self->subpath[strlen(parent->subpath)] == '/';
}

// This virtual method seems to be intended for local files only,
// and documentation claims that the result is in filesystem encoding.
// For us, paths are mostly opaque strings of arbitrary encoding, however.
static char *
fiv_collection_file_get_relative_path(GFile *parent, GFile *descendant)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(descendant);
	FivCollectionFile *prefix = FIV_COLLECTION_FILE(parent);
	if (!fiv_collection_file_prefix_matches(parent, descendant))
		return NULL;

	g_assert((!prefix->target && self->target) ||
		(prefix->target && self->target && self->subpath));

	if (!prefix->target) {
		gchar *basename = get_target_basename(self);
		gchar *path = g_build_path("/", basename, self->subpath, NULL);
		g_free(basename);
		return path;
	}

	return prefix->subpath
		? g_strdup(self->subpath + strlen(prefix->subpath) + 1)
		: g_strdup(self->subpath);
}

static GFile *
get_file_for_path(const char *path)
{
	// Skip all initial slashes, making the result relative to the root.
	if (!*(path += strspn(path, "/")))
		return g_object_new(FIV_TYPE_COLLECTION_FILE, NULL);

	char *end = NULL;
	guint64 i = g_ascii_strtoull(path, &end, 10);
	if (i <= 0 || i > g.files_len || *end != '.')
		return g_file_new_for_uri("");

	FivCollectionFile *new = g_object_new(FIV_TYPE_COLLECTION_FILE, NULL);
	new->index = --i;
	new->target = g_object_ref(g.files[i]);

	const char *subpath = strchr(path, '/');
	if (subpath && subpath[1])
		new->subpath = g_strdup(++subpath);
	return G_FILE(new);
}

static GFile *
fiv_collection_file_resolve_relative_path(
	GFile *file, const char *relative_path)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	if (!self->target)
		return get_file_for_path(relative_path);

	gchar *basename = get_target_basename(self);
	gchar *root = g_build_path("/", "/", basename, self->subpath, NULL);
	g_free(basename);
	gchar *canonicalized = g_canonicalize_filename(relative_path, root);
	GFile *result = get_file_for_path(canonicalized);
	g_free(canonicalized);
	return result;
}

static GFile *
get_target_subpathed(FivCollectionFile *self)
{
	return self->subpath
		? g_file_resolve_relative_path(self->target, self->subpath)
		: g_object_ref(self->target);
}

static GFile *
fiv_collection_file_get_child_for_display_name(
	GFile *file, const char *display_name, GError **error)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	if (!self->target)
		return get_file_for_path(display_name);

	// Implementations often redirect to g_file_resolve_relative_path().
	// We don't want to go up (and possibly receive a "/" basename),
	// nor do we want to skip path elements.
	// TODO(p): This should still be implementable, via URI inspection.
	if (strchr(display_name, '/')) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Display name must not contain path separators");
		return NULL;
	}

	GFile *intermediate = get_target_subpathed(self);
	GFile *resolved =
		g_file_get_child_for_display_name(intermediate, display_name, error);
	g_object_unref(intermediate);
	if (!resolved)
		return NULL;

	// Try to retrieve the display name converted to whatever insanity
	// the target might have chosen to encode its paths with.
	gchar *converted = g_file_get_basename(resolved);
	g_object_unref(resolved);

	FivCollectionFile *new = g_object_new(FIV_TYPE_COLLECTION_FILE, NULL);
	new->index = self->index;
	new->target = g_object_ref(self->target);
	new->subpath = self->subpath
		? g_build_path("/", self->subpath, converted, NULL)
		: g_strdup(converted);
	g_free(converted);
	return G_FILE(new);
}

static GFileEnumerator *
fiv_collection_file_enumerate_children(GFile *file, const char *attributes,
	GFileQueryInfoFlags flags, GCancellable *cancellable, GError **error)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	FivCollectionEnumerator *enumerator = g_object_new(
		FIV_TYPE_COLLECTION_ENUMERATOR, "container", file, NULL);
	enumerator->attributes = g_strdup(attributes);
	if (self->target) {
		GFile *intermediate = get_target_subpathed(self);
		enumerator->subenumerator = g_file_enumerate_children(
			intermediate, enumerator->attributes, flags, cancellable, error);
		g_object_unref(intermediate);
	}
	return G_FILE_ENUMERATOR(enumerator);
}

// TODO(p): Implement async variants of this proxying method.
static GFileInfo *
fiv_collection_file_query_info(GFile *file, const char *attributes,
	GFileQueryInfoFlags flags, GCancellable *cancellable,
	G_GNUC_UNUSED GError **error)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	GError *e = NULL;
	if (!self->target) {
		GFileInfo *info = g_file_info_new();
		g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
		g_file_info_set_name(info, "/");
		g_file_info_set_display_name(info, "Collection");

		GIcon *icon = g_icon_new_for_string("shapes-symbolic", NULL);
		if (icon) {
			g_file_info_set_symbolic_icon(info, icon);
			g_object_unref(icon);
		} else {
			g_warning("%s", e->message);
			g_error_free(e);
		}
		return info;
	}

	// The "http" scheme doesn't behave nicely, make something up if needed.
	GFile *intermediate = get_target_subpathed(self);
	GFileInfo *info =
		g_file_query_info(intermediate, attributes, flags, cancellable, &e);
	if (!info) {
		g_warning("%s", e->message);
		g_error_free(e);

		info = g_file_info_new();
		g_file_info_set_file_type(info, G_FILE_TYPE_REGULAR);
		gchar *basename = g_file_get_basename(intermediate);
		g_file_info_set_name(info, basename);

		// The display name is "guaranteed to always be set" when queried,
		// which is up to implementations.
		gchar *safe = g_utf8_make_valid(basename, -1);
		g_free(basename);
		g_file_info_set_display_name(info, safe);
		g_free(safe);
	}

	gchar *target_uri = g_file_get_uri(intermediate);
	g_file_info_set_attribute_string(
		info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, target_uri);
	g_free(target_uri);
	g_object_unref(intermediate);

	// Ensure all basenames that might have been set have the numeric prefix.
	const char *name = NULL;
	if (!self->subpath) {
		// Always set this, because various schemes may not do so themselves,
		// which then troubles GFileEnumerator.
		gchar *basename = get_target_basename(self);
		g_file_info_set_name(info, basename);
		g_free(basename);

		if (g_file_info_has_attribute(
				info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME) &&
			(name = g_file_info_get_display_name(info))) {
			gchar *prefixed = get_prefixed_name(self, name);
			g_file_info_set_display_name(info, prefixed);
			g_free(prefixed);
		}
		if (g_file_info_has_attribute(
				info, G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME) &&
			(name = g_file_info_get_edit_name(info))) {
			gchar *prefixed = get_prefixed_name(self, name);
			g_file_info_set_edit_name(info, prefixed);
			g_free(prefixed);
		}
	}
	return info;
}

static GFileInfo *
fiv_collection_file_query_filesystem_info(G_GNUC_UNUSED GFile *file,
	G_GNUC_UNUSED const char *attributes,
	G_GNUC_UNUSED GCancellable *cancellable, G_GNUC_UNUSED GError **error)
{
	GFileInfo *info = g_file_info_new();
	GFileAttributeMatcher *matcher = g_file_attribute_matcher_new(attributes);
	if (g_file_attribute_matcher_matches(
			matcher, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE)) {
		g_file_info_set_attribute_string(
			info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, FIV_COLLECTION_SCHEME);
	}
	if (g_file_attribute_matcher_matches(
			matcher, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY)) {
		g_file_info_set_attribute_boolean(
			info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
	}

	g_file_attribute_matcher_unref(matcher);
	return info;
}

static GFile *
fiv_collection_file_set_display_name(G_GNUC_UNUSED GFile *file,
	G_GNUC_UNUSED const char *display_name,
	G_GNUC_UNUSED GCancellable *cancellable, GError **error)
{
	g_set_error_literal(
		error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Operation not supported");
	return NULL;
}

static GFileInputStream *
fiv_collection_file_read(GFile *file, GCancellable *cancellable, GError **error)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	if (!self->target) {
		g_set_error_literal(
			error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY, "Is a directory");
		return NULL;
	}

	GFile *intermediate = get_target_subpathed(self);
	GFileInputStream *stream = g_file_read(intermediate, cancellable, error);
	g_object_unref(intermediate);
	return stream;
}

static void
on_read(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GFile *intermediate = G_FILE(source_object);
	GTask *task = G_TASK(user_data);
	GError *error = NULL;
	GFileInputStream *result = g_file_read_finish(intermediate, res, &error);
	if (result)
		g_task_return_pointer(task, result, g_object_unref);
	else
		g_task_return_error(task, error);
	g_object_unref(task);
}

static void
fiv_collection_file_read_async(GFile *file, int io_priority,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	FivCollectionFile *self = FIV_COLLECTION_FILE(file);
	GTask *task = g_task_new(file, cancellable, callback, user_data);
	g_task_set_name(task, __func__);
	g_task_set_priority(task, io_priority);
	if (!self->target) {
		g_task_return_new_error(
			task, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY, "Is a directory");
		g_object_unref(task);
		return;
	}

	GFile *intermediate = get_target_subpathed(self);
	g_file_read_async(intermediate, io_priority, cancellable, on_read, task);
	g_object_unref(intermediate);
}

static GFileInputStream *
fiv_collection_file_read_finish(
	G_GNUC_UNUSED GFile *file, GAsyncResult *res, GError **error)
{
	return g_task_propagate_pointer(G_TASK(res), error);
}

static void
fiv_collection_file_file_iface_init(GFileIface *iface)
{
	// Required methods that would segfault if unimplemented.
	iface->dup = fiv_collection_file_dup;
	iface->hash = fiv_collection_file_hash;
	iface->equal = fiv_collection_file_equal;
	iface->is_native = fiv_collection_file_is_native;
	iface->has_uri_scheme = fiv_collection_file_has_uri_scheme;
	iface->get_uri_scheme = fiv_collection_file_get_uri_scheme;
	iface->get_basename = fiv_collection_file_get_basename;
	iface->get_path = fiv_collection_file_get_path;
	iface->get_uri = fiv_collection_file_get_uri;
	iface->get_parse_name = fiv_collection_file_get_parse_name;
	iface->get_parent = fiv_collection_file_get_parent;
	iface->prefix_matches = fiv_collection_file_prefix_matches;
	iface->get_relative_path = fiv_collection_file_get_relative_path;
	iface->resolve_relative_path = fiv_collection_file_resolve_relative_path;
	iface->get_child_for_display_name =
		fiv_collection_file_get_child_for_display_name;
	iface->set_display_name = fiv_collection_file_set_display_name;

	// Optional methods.
	iface->enumerate_children = fiv_collection_file_enumerate_children;
	iface->query_info = fiv_collection_file_query_info;
	iface->query_filesystem_info = fiv_collection_file_query_filesystem_info;
	iface->read_fn = fiv_collection_file_read;
	iface->read_async = fiv_collection_file_read_async;
	iface->read_finish = fiv_collection_file_read_finish;

	iface->supports_thread_contexts = TRUE;
}

static void
fiv_collection_file_class_init(FivCollectionFileClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fiv_collection_file_finalize;
}

static void
fiv_collection_file_init(FivCollectionFile *self)
{
	self->index = -1;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static GFile *
get_file_for_uri(G_GNUC_UNUSED GVfs *vfs, const char *identifier,
	G_GNUC_UNUSED gpointer user_data)
{
	static const char prefix[] = FIV_COLLECTION_SCHEME ":";
	const char *path = identifier + sizeof prefix - 1;
	if (!g_str_has_prefix(identifier, prefix))
		return NULL;

	// Specifying the authority is not supported.
	if (g_str_has_prefix(path, "//"))
		return NULL;

	// Otherwise, it needs to look like an absolute path.
	if (!g_str_has_prefix(path, "/"))
		return NULL;

	// TODO(p): Figure out what to do about queries and fragments.
	// GDummyFile carries them across level, which seems rather arbitrary.
	const char *trailing = strpbrk(path, "?#");
	gchar *unescaped = g_uri_unescape_segment(path, trailing, "/");
	if (!unescaped)
		return NULL;

	GFile *result = get_file_for_path(unescaped);
	g_free(unescaped);
	return result;
}

static GFile *
parse_name(GVfs *vfs, const char *identifier, gpointer user_data)
{
	// get_file_for_uri() already parses a superset of URIs.
	return get_file_for_uri(vfs, identifier, user_data);
}

void
fiv_collection_register(void)
{
	GVfs *vfs = g_vfs_get_default();
	if (!g_vfs_register_uri_scheme(vfs, FIV_COLLECTION_SCHEME,
			get_file_for_uri, NULL, NULL, parse_name, NULL, NULL))
		g_warning(FIV_COLLECTION_SCHEME " scheme registration failed");
}

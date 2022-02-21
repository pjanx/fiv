//
// fiv-thumbnail.c: thumbnail management
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

#include "config.h"

#include <glib/gstdio.h>
#include <spng.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <errno.h>
#include <math.h>
#include <stdbool.h>

#include "fiv-io.h"
#include "fiv-thumbnail.h"
#include "xdg.h"

#ifndef __linux__
#define st_mtim st_mtimespec
#endif  // ! __linux__

// TODO(p): Consider merging back with fiv-io.
#define FIV_THUMBNAIL_ERROR fiv_thumbnail_error_quark()

G_DEFINE_QUARK(fiv-thumbnail-error-quark, fiv_thumbnail_error)

enum FivThumbnailError {
	FIV_THUMBNAIL_ERROR_IO
};

static void
set_error(GError **error, const char *message)
{
	g_set_error_literal(
		error, FIV_THUMBNAIL_ERROR, FIV_THUMBNAIL_ERROR_IO, message);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

GType
fiv_thumbnail_size_get_type(void)
{
	static gsize guard;
	if (g_once_init_enter(&guard)) {
#define XX(name, value, dir) {FIV_THUMBNAIL_SIZE_ ## name, \
	"FIV_THUMBNAIL_SIZE_" #name, #name},
		static const GEnumValue values[] = {FIV_THUMBNAIL_SIZES(XX) {}};
#undef XX
		GType type = g_enum_register_static(
			g_intern_static_string("FivThumbnailSize"), values);
		g_once_init_leave(&guard, type);
	}
	return guard;
}

#define XX(name, value, dir) {value, dir},
FivThumbnailSizeInfo
	fiv_thumbnail_sizes[FIV_THUMBNAIL_SIZE_COUNT] = {
		FIV_THUMBNAIL_SIZES(XX)};
#undef XX

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define THUMB_URI             "Thumb::URI"
#define THUMB_MTIME           "Thumb::MTime"
#define THUMB_SIZE            "Thumb::Size"
#define THUMB_IMAGE_WIDTH     "Thumb::Image::Width"
#define THUMB_IMAGE_HEIGHT    "Thumb::Image::Height"
#define THUMB_COLORSPACE      "Thumb::ColorSpace"
#define THUMB_COLORSPACE_SRGB "sRGB"

cairo_user_data_key_t fiv_thumbnail_key_lq;

static void
mark_thumbnail_lq(cairo_surface_t *surface)
{
	cairo_surface_set_user_data(
		surface, &fiv_thumbnail_key_lq, (void *) (intptr_t) 1, NULL);
}

gchar *
fiv_thumbnail_get_root(void)
{
	gchar *cache_dir = get_xdg_home_dir("XDG_CACHE_HOME", ".cache");
	gchar *thumbnails_dir = g_build_filename(cache_dir, "thumbnails", NULL);
	g_free(cache_dir);
	return thumbnails_dir;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// In principle similar to rescale_thumbnail() from fiv-browser.c.
static cairo_surface_t *
adjust_thumbnail(cairo_surface_t *thumbnail, double row_height)
{
	// Hardcode orientation.
	FivIoOrientation orientation = (uintptr_t) cairo_surface_get_user_data(
		thumbnail, &fiv_io_key_orientation);

	double w = 0, h = 0;
	cairo_matrix_t matrix =
		fiv_io_orientation_apply(thumbnail, orientation, &w, &h);

	double scale_x = 1;
	double scale_y = 1;
	if (w > FIV_THUMBNAIL_WIDE_COEFFICIENT * h) {
		scale_x = FIV_THUMBNAIL_WIDE_COEFFICIENT * row_height / w;
		scale_y = round(scale_x * h) / h;
	} else {
		scale_y = row_height / h;
		scale_x = round(scale_y * w) / w;
	}

	// Vector images should not have orientation, this should handle them all.
	FivIoRenderClosure *closure =
		cairo_surface_get_user_data(thumbnail, &fiv_io_key_render);
	if (closure && orientation <= FivIoOrientation0) {
		// This API doesn't accept non-uniform scaling; prefer a vertical fit.
		cairo_surface_t *scaled = closure->render(closure, scale_y);
		if (scaled)
			return scaled;
	}

	// This will be CAIRO_FORMAT_INVALID with non-image surfaces, which is fine.
	cairo_format_t format = cairo_image_surface_get_format(thumbnail);
	if (format != CAIRO_FORMAT_INVALID &&
		orientation <= FivIoOrientation0 && scale_x == 1 && scale_y == 1)
		return cairo_surface_reference(thumbnail);

	int projected_width = round(scale_x * w);
	int projected_height = round(scale_y * h);
	cairo_surface_t *scaled = cairo_image_surface_create(
		(format == CAIRO_FORMAT_RGB24 || format == CAIRO_FORMAT_RGB30)
			? CAIRO_FORMAT_RGB24
			: CAIRO_FORMAT_ARGB32,
		projected_width, projected_height);

	cairo_t *cr = cairo_create(scaled);
	cairo_scale(cr, scale_x, scale_y);

	cairo_set_source_surface(cr, thumbnail, 0, 0);
	cairo_pattern_t *pattern = cairo_get_source(cr);
	// CAIRO_FILTER_BEST, for some reason, works bad with CAIRO_FORMAT_RGB30.
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
	cairo_pattern_set_matrix(pattern, &matrix);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);
	return scaled;
}

static WebPData
encode_thumbnail(cairo_surface_t *surface)
{
	WebPData bitstream = {};
	WebPConfig config = {};
	if (!WebPConfigInit(&config) || !WebPConfigLosslessPreset(&config, 6))
		return bitstream;

	config.near_lossless = 95;
	config.thread_level = true;
	if (!WebPValidateConfig(&config))
		return bitstream;

	bitstream.bytes = fiv_io_encode_webp(surface, &config, &bitstream.size);
	return bitstream;
}

static void
save_thumbnail(cairo_surface_t *thumbnail, const char *path, GString *thum)
{
	WebPMux *mux = WebPMuxNew();
	WebPData bitstream = encode_thumbnail(thumbnail);
	gboolean ok = WebPMuxSetImage(mux, &bitstream, true) == WEBP_MUX_OK;
	WebPDataClear(&bitstream);

	WebPData data = {.bytes = (const uint8_t *) thum->str, .size = thum->len};
	ok = ok && WebPMuxSetChunk(mux, "THUM", &data, false) == WEBP_MUX_OK;

	WebPData assembled = {};
	WebPDataInit(&assembled);
	ok = ok && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK;
	WebPMuxDelete(mux);
	if (!ok) {
		g_warning("thumbnail encoding failed");
		return;
	}

	GError *e = NULL;
	while (!g_file_set_contents(
		path, (const gchar *) assembled.bytes, assembled.size, &e)) {
		bool missing_parents =
			e->domain == G_FILE_ERROR && e->code == G_FILE_ERROR_NOENT;
		g_debug("%s: %s", path, e->message);
		g_clear_error(&e);
		if (!missing_parents)
			break;

		gchar *dirname = g_path_get_dirname(path);
		int err = g_mkdir_with_parents(dirname, 0755);
		if (err)
			g_debug("%s: %s", dirname, g_strerror(errno));

		g_free(dirname);
		if (err)
			break;
	}

	// It would be possible to create square thumbnails as well,
	// but it seems like wasted effort.
	WebPDataClear(&assembled);
}

static cairo_surface_t *
render(GFile *target, GBytes *data, gboolean *color_managed, GError **error)
{
	FivIoOpenContext ctx = {
		.uri = g_file_get_uri(target),
		.screen_profile = fiv_io_profile_new_sRGB(),
		.screen_dpi = 96,
		.first_frame_only = TRUE,
		// Only using this array as a redirect.
		.warnings = g_ptr_array_new_with_free_func(g_free),
	};

	cairo_surface_t *surface = fiv_io_open_from_data(
		g_bytes_get_data(data, NULL), g_bytes_get_size(data), &ctx, error);
	g_free((gchar *) ctx.uri);
	g_ptr_array_free(ctx.warnings, TRUE);
	if ((*color_managed = !!ctx.screen_profile))
		fiv_io_profile_free(ctx.screen_profile);
	g_bytes_unref(data);
	return surface;
}

static gboolean
produce_fallback(GFile *target, FivThumbnailSize size,
	cairo_surface_t **surface, GError **error)
{
	goffset filesize = 0;
	GFileInfo *info = g_file_query_info(target,
		G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (info) {
		filesize = g_file_info_get_size(info);
		g_object_unref(info);
	}

	// TODO(p): Try to be a bit more intelligent about this.
	// For example, we can employ magic checks.
	if (filesize > 10 << 20) {
		set_error(error, "oversize, not thumbnailing");
		return FALSE;
	}

	GBytes *data = g_file_load_bytes(target, NULL, NULL, error);
	if (!data)
		return FALSE;

	gboolean color_managed = FALSE;
	cairo_surface_t *result = render(target, data, &color_managed, error);
	if (!result)
		return FALSE;

	if (!*surface)
		*surface = adjust_thumbnail(result, fiv_thumbnail_sizes[size].size);
	cairo_surface_destroy(result);
	return TRUE;
}

gboolean
fiv_thumbnail_produce(GFile *target, FivThumbnailSize max_size,
	cairo_surface_t **max_size_surface, GError **error)
{
	g_return_val_if_fail(max_size >= FIV_THUMBNAIL_SIZE_MIN &&
		max_size <= FIV_THUMBNAIL_SIZE_MAX, FALSE);

	const gchar *path = g_file_peek_path(target);
	if (!path || !g_file_is_native(target) /* Don't save sftp://. */)
		return produce_fallback(target, max_size, max_size_surface, error);

	// Make the TOCTTOU issue favour unnecessary reloading.
	GStatBuf st = {};
	if (g_stat(path, &st)) {
		set_error(error, g_strerror(errno));
		return FALSE;
	}

	GError *e = NULL;
	GMappedFile *mf = g_mapped_file_new(path, FALSE, &e);
	if (!mf) {
		g_debug("%s: %s", path, e->message);
		g_error_free(e);
		return produce_fallback(target, max_size, max_size_surface, error);
	}

	gsize filesize = g_mapped_file_get_length(mf);
	gboolean color_managed = FALSE;
	cairo_surface_t *surface =
		render(target, g_mapped_file_get_bytes(mf), &color_managed, error);
	g_mapped_file_unref(mf);
	if (!surface)
		return FALSE;

	// Boilerplate copied from fiv_thumbnail_lookup().
	gchar *uri = g_file_get_uri(target);
	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	gchar *thumbnails_dir = fiv_thumbnail_get_root();

	GString *thum = g_string_new("");
	g_string_append_printf(
		thum, "%s%c%s%c", THUMB_URI, 0, uri, 0);
	g_string_append_printf(
		thum, "%s%c%ld%c", THUMB_MTIME, 0, (long) st.st_mtim.tv_sec, 0);
	g_string_append_printf(
		thum, "%s%c%ld%c", THUMB_SIZE, 0, (long) filesize, 0);

	if (cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE) {
		g_string_append_printf(thum, "%s%c%d%c", THUMB_IMAGE_WIDTH, 0,
			cairo_image_surface_get_width(surface), 0);
		g_string_append_printf(thum, "%s%c%d%c", THUMB_IMAGE_HEIGHT, 0,
			cairo_image_surface_get_height(surface), 0);
	}

	// Without a CMM, no conversion is attempted.
	if (color_managed) {
		g_string_append_printf(
			thum, "%s%c%s%c", THUMB_COLORSPACE, 0, THUMB_COLORSPACE_SRGB, 0);
	}

	for (int use = max_size; use >= FIV_THUMBNAIL_SIZE_MIN; use--) {
		cairo_surface_t *scaled =
			adjust_thumbnail(surface, fiv_thumbnail_sizes[use].size);
		gchar *path = g_strdup_printf("%s/wide-%s/%s.webp", thumbnails_dir,
			fiv_thumbnail_sizes[use].thumbnail_spec_name, sum);
		save_thumbnail(scaled, path, thum);
		g_free(path);

		if (!*max_size_surface)
			*max_size_surface = scaled;
		else
			cairo_surface_destroy(scaled);
	}

	g_string_free(thum, TRUE);

	g_free(thumbnails_dir);
	g_free(sum);
	g_free(uri);
	cairo_surface_destroy(surface);
	return TRUE;
}

static bool
check_wide_thumbnail_texts(GBytes *thum, const gchar *target, time_t mtime,
	bool *sRGB)
{
	gsize len = 0;
	const gchar *s = g_bytes_get_data(thum, &len), *end = s + len;

	// Similar to PNG below, but we're following our own specification.
	const gchar *key = NULL, *nul = NULL;
	bool have_uri = false, have_mtime = false;
	for (; (nul = memchr(s, '\0', end - s)); s = ++nul) {
		if (!key) {
			key = s;
			continue;
		} else if (!strcmp(key, THUMB_URI)) {
			have_uri = true;
			if (strcmp(target, s))
				return false;
		} else if (!strcmp(key, THUMB_MTIME)) {
			have_mtime = true;
			if (atol(s) != mtime)
				return false;
		} else if (!strcmp(key, THUMB_COLORSPACE))
			*sRGB = !strcmp(s, THUMB_COLORSPACE_SRGB);

		key = NULL;
	}
	return have_uri && have_mtime;
}

static cairo_surface_t *
read_wide_thumbnail(
	const gchar *path, const gchar *uri, time_t mtime, GError **error)
{
	gchar *thumbnail_uri = g_filename_to_uri(path, NULL, error);
	if (!thumbnail_uri)
		return NULL;

	cairo_surface_t *surface =
		fiv_io_open(&(FivIoOpenContext){.uri = thumbnail_uri}, error);
	g_free(thumbnail_uri);
	if (!surface)
		return NULL;

	bool sRGB = false;
	GBytes *thum = cairo_surface_get_user_data(surface, &fiv_io_key_thum);
	if (!thum) {
		set_error(error, "not a thumbnail");
	} else if (!check_wide_thumbnail_texts(thum, uri, mtime, &sRGB)) {
		set_error(error, "mismatch");
	} else {
		// TODO(p): Add a function or a non-valueless define to check
		// for CMM presence, then remove this ifdef.
#ifdef HAVE_LCMS2
		if (!sRGB)
			mark_thumbnail_lq(surface);
#endif  // HAVE_LCMS2
		return surface;
	}

	cairo_surface_destroy(surface);
	return NULL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int  // tri-state
check_spng_thumbnail_texts(struct spng_text *texts, uint32_t texts_len,
	const gchar *target, time_t mtime)
{
	// May contain Thumb::Image::Width Thumb::Image::Height,
	// but those aren't interesting currently (would be for fast previews).
	bool need_uri = true, need_mtime = true;
	for (uint32_t i = 0; i < texts_len; i++) {
		struct spng_text *text = texts + i;
		if (!strcmp(text->keyword, THUMB_URI)) {
			need_uri = false;
			if (strcmp(target, text->text))
				return false;
		}
		if (!strcmp(text->keyword, THUMB_MTIME)) {
			need_mtime = false;
			if (atol(text->text) != mtime)
				return false;
		}
	}
	return need_uri || need_mtime ? -1 : true;
}

static int  // tri-state
check_spng_thumbnail(spng_ctx *ctx, const gchar *target, time_t mtime, int *err)
{
	uint32_t texts_len = 0;
	if ((*err = spng_get_text(ctx, NULL, &texts_len)))
		return false;

	int result = false;
	struct spng_text *texts = g_malloc0_n(texts_len, sizeof *texts);
	if (!(*err = spng_get_text(ctx, texts, &texts_len)))
		result = check_spng_thumbnail_texts(texts, texts_len, target, mtime);
	g_free(texts);
	return result;
}

static cairo_surface_t *
read_spng_thumbnail(
	const gchar *path, const gchar *uri, time_t mtime, GError **error)
{
	FILE *fp;
	cairo_surface_t *result = NULL;
	if (!(fp = fopen(path, "rb"))) {
		set_error(error, g_strerror(errno));
		return NULL;
	}

	errno = 0;
	spng_ctx *ctx = spng_ctx_new(0);
	if (!ctx) {
		set_error(error, g_strerror(errno));
		goto fail_init;
	}

	int err;
	size_t size = 0;
	if ((err = spng_set_png_file(ctx, fp)) ||
		(err = spng_set_image_limits(ctx, INT16_MAX, INT16_MAX)) ||
		(err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &size))) {
		set_error(error, spng_strerror(err));
		goto fail;
	}
	if (check_spng_thumbnail(ctx, uri, mtime, &err) == false) {
		set_error(error, err ? spng_strerror(err) : "mismatch");
		goto fail;
	}

	struct spng_ihdr ihdr = {};
	struct spng_trns trns = {};
	spng_get_ihdr(ctx, &ihdr);
	bool may_be_translucent = !spng_get_trns(ctx, &trns) ||
		ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA ||
		ihdr.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;

	cairo_surface_t *surface = cairo_image_surface_create(
		may_be_translucent ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
		ihdr.width, ihdr.height);

	cairo_status_t surface_status = cairo_surface_status(surface);
	if (surface_status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(surface_status));
		goto fail_data;
	}

	uint32_t *data = (uint32_t *) cairo_image_surface_get_data(surface);
	g_assert((size_t) cairo_image_surface_get_stride(surface) *
		cairo_image_surface_get_height(surface) == size);

	cairo_surface_flush(surface);
	if ((err = spng_decode_image(ctx, data, size, SPNG_FMT_RGBA8,
		SPNG_DECODE_TRNS | SPNG_DECODE_GAMMA))) {
		set_error(error, spng_strerror(err));
		goto fail_data;
	}

	// The specification does not say where the required metadata should be,
	// it could very well be broken up into two parts.
	if (check_spng_thumbnail(ctx, uri, mtime, &err) != true) {
		set_error(
			error, err ? spng_strerror(err) : "mismatch or not a thumbnail");
		goto fail_data;
	}

	// pixman can be mildly abused to do this operation, but it won't be faster.
	if (may_be_translucent) {
		for (size_t i = size / sizeof *data; i--; ) {
			const uint8_t *unit = (const uint8_t *) &data[i];
			uint32_t a = unit[3],
				b = PREMULTIPLY8(a, unit[2]),
				g = PREMULTIPLY8(a, unit[1]),
				r = PREMULTIPLY8(a, unit[0]);
			data[i] = a << 24 | r << 16 | g << 8 | b;
		}
	} else {
		for (size_t i = size / sizeof *data; i--; ) {
			uint32_t rgba = g_ntohl(data[i]);
			data[i] = rgba << 24 | rgba >> 8;
		}
	}

	cairo_surface_mark_dirty((result = surface));

fail_data:
	if (!result)
		cairo_surface_destroy(surface);
fail:
	spng_ctx_free(ctx);
fail_init:
	fclose(fp);
	return result;
}

cairo_surface_t *
fiv_thumbnail_lookup(GFile *target, FivThumbnailSize size)
{
	g_return_val_if_fail(size >= FIV_THUMBNAIL_SIZE_MIN &&
		size <= FIV_THUMBNAIL_SIZE_MAX, NULL);

	// Local files only, at least for now.
	GStatBuf st = {};
	const gchar *path = g_file_peek_path(target);
	if (!path || g_stat(path, &st))
		return NULL;

	gchar *uri = g_file_get_uri(target);
	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	gchar *thumbnails_dir = fiv_thumbnail_get_root();

	// The lookup sequence is: nominal..max, then mirroring back to ..min.
	cairo_surface_t *result = NULL;
	GError *error = NULL;
	for (int i = 0; i < FIV_THUMBNAIL_SIZE_COUNT; i++) {
		FivThumbnailSize use = size + i;
		if (use > FIV_THUMBNAIL_SIZE_MAX)
			use = FIV_THUMBNAIL_SIZE_MAX - i;

		const char *name = fiv_thumbnail_sizes[use].thumbnail_spec_name;
		gchar *wide =
			g_strdup_printf("%s/wide-%s/%s.webp", thumbnails_dir, name, sum);
		result = read_wide_thumbnail(wide, uri, st.st_mtim.tv_sec, &error);
		if (error) {
			g_debug("%s: %s", wide, error->message);
			g_clear_error(&error);
		}
		g_free(wide);
		if (result) {
			// Higher up we can't distinguish images smaller than the thumbnail.
			// Also, try not to rescale the already rescaled.
			if (use != size)
				mark_thumbnail_lq(result);
			break;
		}

		gchar *path =
			g_strdup_printf("%s/%s/%s.png", thumbnails_dir, name, sum);
		result = read_spng_thumbnail(path, uri, st.st_mtim.tv_sec, &error);
		if (error) {
			g_debug("%s: %s", path, error->message);
			g_clear_error(&error);
		}
		g_free(path);
		if (result) {
			// Whatever produced it, we may be able to outclass it.
			mark_thumbnail_lq(result);
			break;
		}
	}

	// TODO(p): We can definitely extract embedded thumbnails, but it should be
	// done as a separate stage--the file may be stored on a slow device.

	g_free(thumbnails_dir);
	g_free(sum);
	g_free(uri);
	return result;
}

// --- Invalidation ------------------------------------------------------------

static void
print_error(GFile *file, GError *error)
{
	gchar *name = g_file_get_parse_name(file);
	g_printerr("%s: %s\n", name, error->message);
	g_free(name);
	g_error_free(error);
}

static gchar *
identify_wide_thumbnail(GMappedFile *mf, time_t *mtime, GError **error)
{
	WebPDemuxer *demux = WebPDemux(&(WebPData) {
		.bytes = (const uint8_t *) g_mapped_file_get_contents(mf),
		.size = g_mapped_file_get_length(mf),
	});
	if (!demux) {
		set_error(error, "demux failure while reading metadata");
		return NULL;
	}

	WebPChunkIterator chunk_iter = {};
	gchar *uri = NULL;
	if (!WebPDemuxGetChunk(demux, "THUM", 1, &chunk_iter)) {
		set_error(error, "missing THUM chunk");
		goto fail;
	}

	// Similar to check_wide_thumbnail_texts(), but with a different purpose.
	const char *p = (const char *) chunk_iter.chunk.bytes,
		*end = p + chunk_iter.chunk.size, *key = NULL, *nul = NULL;
	for (; (nul = memchr(p, '\0', end - p)); p = ++nul)
		if (key) {
			if (!strcmp(key, THUMB_URI) && !uri)
				uri = g_strdup(p);
			if (!strcmp(key, THUMB_MTIME))
				*mtime = atol(p);
			key = NULL;
		} else {
			key = p;
		}
	if (!uri)
		set_error(error, "missing target URI");

	WebPDemuxReleaseChunkIterator(&chunk_iter);
fail:
	WebPDemuxDelete(demux);
	return uri;
}

static void
check_wide_thumbnail(GFile *thumbnail, GError **error)
{
	// Not all errors are enough of a reason for us to delete something.
	GError *tolerable = NULL;
	const char *path = g_file_peek_path(thumbnail);
	GMappedFile *mf = g_mapped_file_new(path, FALSE, &tolerable);
	if (!mf) {
		print_error(thumbnail, tolerable);
		return;
	}

	time_t target_mtime = 0;
	gchar *target_uri = identify_wide_thumbnail(mf, &target_mtime, error);
	g_mapped_file_unref(mf);
	if (!target_uri)
		return;

	// This should not occur at all, we're being pedantic.
	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, target_uri, -1);
	gchar *expected_basename = g_strdup_printf("%s.webp", sum);
	gchar *basename = g_path_get_basename(path);
	gboolean match = !strcmp(basename, expected_basename);
	g_free(basename);
	g_free(expected_basename);
	g_free(sum);
	if (!match) {
		set_error(error, "URI checksum mismatch");
		g_free(target_uri);
		return;
	}

	GFile *target = g_file_new_for_uri(target_uri);
	g_free(target_uri);
	GFileInfo *info = g_file_query_info(target,
		G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
		G_FILE_QUERY_INFO_NONE, NULL, &tolerable);
	g_object_unref(target);
	if (g_error_matches(tolerable, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
		g_propagate_error(error, tolerable);
		return;
	} else if (tolerable) {
		print_error(thumbnail, tolerable);
		return;
	}

	GDateTime *mdatetime = g_file_info_get_modification_date_time(info);
	g_object_unref(info);
	if (!mdatetime) {
		set_error(&tolerable, "cannot retrieve file modification time");
		print_error(thumbnail, tolerable);
		return;
	}
	if (g_date_time_to_unix(mdatetime) != target_mtime)
		set_error(error, "mtime mismatch");
	g_date_time_unref(mdatetime);
}

static void
invalidate_wide_thumbnail(GFile *thumbnail)
{
	// It's possible to lift that restriction in the future,
	// but we need to codify how the modification time should be checked.
	const char *path = g_file_peek_path(thumbnail);
	if (!path) {
		print_error(thumbnail,
			g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
				"thumbnails are expected to be local files"));
		return;
	}

	// You cannot kill what you did not create.
	if (!g_str_has_suffix(path, ".webp"))
		return;

	GError *error = NULL;
	check_wide_thumbnail(thumbnail, &error);
	if (error) {
		g_debug("Deleting %s: %s", path, error->message);
		g_clear_error(&error);
		if (!g_file_delete(thumbnail, NULL, &error))
			print_error(thumbnail, error);
	}
}

static void
invalidate_wide_thumbnail_directory(GFile *directory)
{
	GError *error = NULL;
	GFileEnumerator *enumerator = g_file_enumerate_children(directory,
		G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
		G_FILE_QUERY_INFO_NONE, NULL, &error);
	if (!enumerator) {
		print_error(directory, error);
		return;
	}

	GFileInfo *info = NULL;
	GFile *child = NULL;
	while (g_file_enumerator_iterate(enumerator, &info, &child, NULL, &error) &&
		info != NULL) {
		if (g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR)
			invalidate_wide_thumbnail(child);
	}
	g_object_unref(enumerator);
	if (error)
		print_error(directory, error);
}

void
fiv_thumbnail_invalidate(void)
{
	gchar *thumbnails_dir = fiv_thumbnail_get_root();
	for (int i = 0; i < FIV_THUMBNAIL_SIZE_COUNT; i++) {
		const char *name = fiv_thumbnail_sizes[i].thumbnail_spec_name;
		gchar *dirname = g_strdup_printf("wide-%s", name);
		GFile *dir = g_file_new_build_filename(thumbnails_dir, dirname, NULL);
		g_free(dirname);
		invalidate_wide_thumbnail_directory(dir);
		g_object_unref(dir);
	}
	g_free(thumbnails_dir);
}

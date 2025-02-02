//
// fiv-thumbnail.c: thumbnail management
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

#include "config.h"

#include <glib/gstdio.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "fiv-io.h"
#include "fiv-thumbnail.h"
#include "xdg.h"

#ifdef HAVE_LIBRAW
#include <libraw.h>
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 21, 0)
#define LIBRAW_OPIONS_NO_MEMERR_CALLBACK 0
#endif
#endif  // HAVE_LIBRAW

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

static gchar *
fiv_thumbnail_get_root(void)
{
#ifdef G_OS_WIN32
	// We can do better than GLib with FOLDERID_InternetCache,
	// and we don't want to place .cache directly in the user's home.
	// TODO(p): Register this thumbnail path using the installer:
	// https://learn.microsoft.com/en-us/windows/win32/lwef/disk-cleanup
	gchar *cache_dir =
		g_build_filename(g_get_user_data_dir(), PROJECT_NAME, NULL);
#else
	gchar *cache_dir = get_xdg_home_dir("XDG_CACHE_HOME", ".cache");
#endif
	gchar *thumbnails_dir = g_build_filename(cache_dir, "thumbnails", NULL);
	g_free(cache_dir);
	return thumbnails_dir;
}

static gboolean
might_be_a_thumbnail(const char *path_or_uri)
{
	// It is generally difficult to discern case in/sensitivity of subpaths,
	// so err on the side of false positives.
	gchar *normalized = g_ascii_strdown(path_or_uri, -1);

	// The Windows path separator must be percent-encoded in URIs,
	// and the file scheme always uses forward slashes.
	if (G_DIR_SEPARATOR != '/')
		g_strdelimit(normalized, G_DIR_SEPARATOR_S, '/');

	gboolean matches = strstr(normalized, "/.cache/thumbnails/") != NULL;
	g_free(normalized);
	return matches;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static FivIoImage *
render(GFile *target, GBytes *data, gboolean *color_managed, GError **error)
{
	FivIoCmm *cmm = fiv_io_cmm_get_default();
	FivIoOpenContext ctx = {
		.uri = g_file_get_uri(target),
		// Remember to synchronize changes with adjust_thumbnail().
		.cmm = cmm,
		.screen_profile = fiv_io_cmm_get_profile_sRGB(cmm),
		.screen_dpi = 96,
		.first_frame_only = TRUE,
		// Only using this array as a redirect.
		.warnings = g_ptr_array_new_with_free_func(g_free),
	};

	FivIoImage *image = fiv_io_open_from_data(
		g_bytes_get_data(data, NULL), g_bytes_get_size(data), &ctx, error);
	g_free((gchar *) ctx.uri);
	g_ptr_array_free(ctx.warnings, TRUE);
	if ((*color_managed = !!ctx.screen_profile))
		fiv_io_profile_free(ctx.screen_profile);
	g_bytes_unref(data);
	return image;
}

// In principle similar to rescale_thumbnail() from fiv-browser.c.
static FivIoImage *
adjust_thumbnail(FivIoImage *thumbnail, double row_height)
{
	// Hardcode orientation.
	FivIoOrientation orientation = thumbnail->orientation;

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
	FivIoRenderClosure *closure = thumbnail->render;
	if (closure && orientation <= FivIoOrientation0) {
		// Remember to synchronize changes with render().
		FivIoCmm *cmm = fiv_io_cmm_get_default();
		FivIoProfile *screen_profile = fiv_io_cmm_get_profile_sRGB(cmm);
		// This API doesn't accept non-uniform scaling; prefer a vertical fit.
		FivIoImage *scaled =
			closure->render(closure, cmm, screen_profile, scale_y);
		if (screen_profile)
			fiv_io_profile_free(screen_profile);
		if (scaled)
			return scaled;
	}

	if (orientation <= FivIoOrientation0 && scale_x == 1 && scale_y == 1)
		return fiv_io_image_ref(thumbnail);

	cairo_format_t format = thumbnail->format;
	int projected_width = round(scale_x * w);
	int projected_height = round(scale_y * h);
	FivIoImage *scaled = fiv_io_image_new(
		(format == CAIRO_FORMAT_RGB24 || format == CAIRO_FORMAT_RGB30)
			? CAIRO_FORMAT_RGB24
			: CAIRO_FORMAT_ARGB32,
		projected_width, projected_height);
	if (!scaled) {
		g_warning("image allocation failure");
		return fiv_io_image_ref(thumbnail);
	}

	cairo_surface_t *surface = fiv_io_image_to_surface_noref(scaled);
	cairo_t *cr = cairo_create(surface);
	cairo_surface_destroy(surface);

	cairo_scale(cr, scale_x, scale_y);

	surface = fiv_io_image_to_surface_noref(thumbnail);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_surface_destroy(surface);

	cairo_pattern_t *pattern = cairo_get_source(cr);
	// CAIRO_FILTER_BEST, for some reason, works bad with CAIRO_FORMAT_RGB30.
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
	cairo_pattern_set_matrix(pattern, &matrix);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);

	// Note that this doesn't get triggered with oversize input surfaces,
	// even though nothing will be rendered.
	if (cairo_pattern_status(pattern) != CAIRO_STATUS_SUCCESS ||
		cairo_status(cr) != CAIRO_STATUS_SUCCESS)
		g_warning("thumbnail scaling failed");

	cairo_destroy(cr);
	return scaled;
}

static FivIoImage *
orient_thumbnail(FivIoImage *image)
{
	if (image->orientation <= FivIoOrientation0)
		return image;

	double w = 0, h = 0;
	cairo_matrix_t matrix =
		fiv_io_orientation_apply(image, image->orientation, &w, &h);
	FivIoImage *oriented = fiv_io_image_new(image->format, w, h);
	if (!oriented) {
		g_warning("image allocation failure");
		return image;
	}

	cairo_surface_t *surface = fiv_io_image_to_surface_noref(oriented);
	cairo_t *cr = cairo_create(surface);
	cairo_surface_destroy(surface);

	surface = fiv_io_image_to_surface(image);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_surface_destroy(surface);
	cairo_pattern_set_matrix(cairo_get_source(cr), &matrix);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);
	return oriented;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#ifdef HAVE_LIBRAW
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 21, 0)

static int
extract_libraw_compare(const void *a, const void *b)
{
	const libraw_thumbnail_item_t **t1 = (const libraw_thumbnail_item_t **) a;
	const libraw_thumbnail_item_t **t2 = (const libraw_thumbnail_item_t **) b;
	float p1 = (float) (*t1)->twidth * (*t1)->theight;
	float p2 = (float) (*t2)->twidth * (*t2)->theight;
	return (p2 < p1) - (p1 < p2);
}

static gboolean
extract_libraw_unpack(libraw_data_t *iprc, int *flip, GError **error)
{
	int count = iprc->thumbs_list.thumbcount;
	if (count <= 0) {
		set_error(error, "no thumbnails found");
		return FALSE;
	}

	// The old libraw_unpack_thumb() goes for the largest thumbnail,
	// but we currently want the smallest usable thumbnail. Order them.
	libraw_thumbnail_item_t **sorted = g_malloc_n(count, sizeof *sorted);
	for (int i = 0; i < count; i++)
		sorted[i] = &iprc->thumbs_list.thumblist[i];
	qsort(sorted, count, sizeof *sorted, extract_libraw_compare);

	// With the raw.pixls.us database, zero dimensions occur in two cases:
	//  - when thumbcount should really be 0,
	//  - with the last, huge JPEG thumbnail in CR3 raws.
	// The maintainer refuses to change anything about it (#589).
	int i = 0;
	while (i < count && (!sorted[i]->twidth || !sorted[i]->theight))
		i++;

	// Ignore thumbnails whose decoding is likely to be a waste of time.
	// XXX: This primarily targets the TIFF/EP shortcut code,
	// because decoding a thumbnail will always be /much/ quicker than a render.
	// TODO(p): Maybe don't mark raw image thumbnails as low-quality
	// if they're the right aspect ratio, and of sufficiently large size.
	// The only downsides to camera-provided thumbnails seem to be cropping,
	// and when they're decoded incorrectly. Also don't trust tflip.
	float output_pixels = (float) iprc->sizes.iwidth * iprc->sizes.iheight;
	// Note that the ratio may even be larger than 1, as seen with CR2 files.
	while (i < count &&
		(float) sorted[count - 1]->twidth * sorted[count - 1]->theight >
			output_pixels * 0.75)
		count--;

	// The smallest size thumbnail is very often forced to be 4:3,
	// and the remaining space is filled with black, looking quite wrong.
	// It isn't really possible to strip those borders, because many are JPEGs.
	//
	// Another reason to skip thumbnails of mismatching aspect ratios is
	// to avoid browser items from jumping around when low-quality thumbnails
	// get replaced with their final versions.
	//
	// Note that some of them actually have borders on all four sides
	// (Nikon/D50/DSC_5155.NEF, Nikon/D70/20170902_0047.NEF,
	// Nikon/D70s/RAW_NIKON_D70S.NEF), or even on just one side
	// (Leica/LEICA M MONOCHROM (Typ 246), Leica/M (Typ 240)).
	// Another interesting possibility is Sony/DSC-HX99/DSC00001.ARW,
	// where the correct-ratio thumbnail has borders but the main image doesn't.
	//
	// The problematic thumbnail is usually, but not always, sized 160x120,
	// and some of them may actually be fine.
	float output_ratio = (float) iprc->sizes.iwidth / iprc->sizes.iheight;
	while (i < count) {
		// XXX: tflip is less reliable than libraw_dcraw_make_mem_thumb()
		// and reading out Orientation from the resulting Exif.
		float ratio = sorted[i]->tflip == 5 || sorted[i]->tflip == 6
			? (float) sorted[i]->theight / sorted[i]->twidth
			: (float) sorted[i]->twidth / sorted[i]->theight;
		if (fabsf(ratio - output_ratio) < 0.05)
			break;
		i++;
	}

	// Avoid pink-tinted readouts of CR2 IFD2 (#590).
	//
	// This thumbnail can also have a black stripe on the left and the top,
	// which we should remove if using fixed LibRaw > 0.21.1.
	if (i < count && iprc->idata.maker_index == LIBRAW_CAMERAMAKER_Canon &&
		sorted[i]->tformat == LIBRAW_INTERNAL_THUMBNAIL_KODAK_THUMB)
		i++;

	bool found = i != count;
	if (found)
		i = sorted[i] - iprc->thumbs_list.thumblist;

	g_free(sorted);
	if (!found) {
		set_error(error, "no suitable thumbnails found");
		return FALSE;
	}

	int err = 0;
	if ((err = libraw_unpack_thumb_ex(iprc, i))) {
		set_error(error, libraw_strerror(err));
		return FALSE;
	}
	*flip = iprc->thumbs_list.thumblist[i].tflip;
	return TRUE;
}

#else  // LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 21, 0)

static gboolean
extract_libraw_unpack(libraw_data_t *iprc, int *flip, GError **error)
{
	int err = 0;
	if ((err = libraw_unpack_thumb(iprc))) {
		set_error(error, libraw_strerror(err));
		return FALSE;
	}

	// The main image's "flip" often matches up, but sometimes doesn't, e.g.:
	//  - Phase One/H 25/H25_Outdoor_.IIQ
	//  - Phase One/H 25/H25_IT8.7-2_Card.TIF
	*flip = iprc->sizes.flip;
	return TRUE;
}

#endif  // LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 21, 0)

// LibRaw does a weird permutation here, so follow the documentation,
// which assumes that mirrored orientations never happen.
static FivIoOrientation
extract_libraw_unflip(int flip)
{
	switch (flip) {
	break; case 0:
		return FivIoOrientation0;
	break; case 3:
		return FivIoOrientation180;
	break; case 5:
		return FivIoOrientation270;
	break; case 6:
		return FivIoOrientation90;
	break; default:
		return FivIoOrientationUnknown;
	}
}

static FivIoImage *
extract_libraw_bitmap(libraw_processed_image_t *image, int flip, GError **error)
{
	// Anything else is extremely rare.
	if (image->colors != 3 || image->bits != 8) {
		set_error(error, "unsupported bitmap thumbnail");
		return NULL;
	}

	FivIoImage *I = fiv_io_image_new(
		CAIRO_FORMAT_RGB24, image->width, image->height);
	if (!I) {
		set_error(error, "image allocation failure");
		return NULL;
	}

	guint32 *out = (guint32 *) I->data;
	const unsigned char *in = image->data;
	for (guint64 i = 0; i < (guint64) image->width * image->height; in += 3)
		out[i++] = in[0] << 16 | in[1] << 8 | in[2];

	I->orientation = extract_libraw_unflip(flip);
	return I;
}

static FivIoImage *
extract_libraw(GFile *target, GMappedFile *mf, GError **error)
{
	FivIoImage *I = NULL;
	libraw_data_t *iprc = libraw_init(
		LIBRAW_OPIONS_NO_MEMERR_CALLBACK | LIBRAW_OPIONS_NO_DATAERR_CALLBACK);
	if (!iprc) {
		set_error(error, "failed to obtain a LibRaw handle");
		return NULL;
	}

	int err = 0;
	if ((err = libraw_open_buffer(iprc, (void *) g_mapped_file_get_contents(mf),
			 g_mapped_file_get_length(mf)))) {
		set_error(error, libraw_strerror(err));
		goto fail;
	}
	if ((err = libraw_adjust_sizes_info_only(iprc))) {
		set_error(error, libraw_strerror(err));
		goto fail;
	}

	int flip = 0;
	if (!extract_libraw_unpack(iprc, &flip, error))
		goto fail;

	libraw_processed_image_t *image = libraw_dcraw_make_mem_thumb(iprc, &err);
	if (!image) {
		set_error(error, libraw_strerror(err));
		goto fail;
	}

	// Bitmap thumbnails generally need rotating, e.g.:
	//  - Hasselblad/H4D-50/2-9-2017_street_0012.fff
	//  - OnePlus/One/IMG_20150729_201116.dng (and more DNGs in general)
	//
	// JPEG thumbnails generally have the right rotation in their Exif, e.g.:
	//  - Canon/EOS-1Ds Mark II/RAW_CANON_1DSM2.CR2
	//  - Leica/C (Typ 112)/Leica_-_C_(Typ_112)-_3:2.RWL
	//  - Nikon/1 S2/RAW_NIKON_1S2.NEF
	//  - Panasonic/DMC-FZ18/RAW_PANASONIC_LUMIX_FZ18.RAW
	//  - Panasonic/DMC-FZ70/P1000836.RW2
	//  - Samsung/NX200/2013-05-08-194524__sam6589.srw
	//  - Sony/DSC-HX95/DSC00018.ARW
	// Note that LibRaw inserts its own Exif segment if it doesn't find one,
	// and this may differ from flip. It may also be wrong, as in:
	//  - Leaf/Aptus 22/L_003172.mos
	//
	// Some files are problematic and we won't bother with special-casing:
	//  - Nokia/Lumia 1020/RAW_NOKIA_LUMIA_1020.DNG (bitmap) has wrong color.
	//  - Ricoh/GXR/R0017428.DNG (JPEG) seems to be plainly invalid.
	switch (image->type) {
		gboolean dummy;
	case LIBRAW_IMAGE_JPEG:
		I = render(
			target, g_bytes_new(image->data, image->data_size), &dummy, error);
		break;
	case LIBRAW_IMAGE_BITMAP:
		I = extract_libraw_bitmap(image, flip, error);
		break;
	default:
		set_error(error, "unsupported embedded thumbnail");
	}

	libraw_dcraw_clear_mem(image);
fail:
	libraw_close(iprc);
	return I;
}

#endif  // HAVE_LIBRAW

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

cairo_surface_t *
fiv_thumbnail_extract(GFile *target, FivThumbnailSize max_size, GError **error)
{
	const char *path = g_file_peek_path(target);
	if (!path) {
		set_error(error, "thumbnails will only be extracted from local files");
		return NULL;
	}

	GMappedFile *mf = g_mapped_file_new(path, FALSE, error);
	if (!mf)
		return NULL;

	// In this case, g_mapped_file_get_contents() returns NULL, causing issues.
	if (!g_mapped_file_get_length(mf)) {
		set_error(error, "empty file");
		return NULL;
	}

	FivIoImage *image = NULL;
#ifdef HAVE_LIBRAW
	image = extract_libraw(target, mf, error);
#else  // ! HAVE_LIBRAW
	// TODO(p): Implement our own thumbnail extractors.
	set_error(error, "unsupported file");
#endif  // ! HAVE_LIBRAW
	g_mapped_file_unref(mf);

	if (!image)
		return NULL;
	if (max_size < FIV_THUMBNAIL_SIZE_MIN || max_size > FIV_THUMBNAIL_SIZE_MAX)
		return fiv_io_image_to_surface(orient_thumbnail(image));

	FivIoImage *result =
		adjust_thumbnail(image, fiv_thumbnail_sizes[max_size].size);
	fiv_io_image_unref(image);
	return fiv_io_image_to_surface(result);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static WebPData
encode_thumbnail(FivIoImage *image)
{
	WebPData bitstream = {};
	WebPConfig config = {};
	if (!WebPConfigInit(&config) || !WebPConfigLosslessPreset(&config, 6))
		return bitstream;

	config.near_lossless = 95;
	config.thread_level = true;
	if (!WebPValidateConfig(&config))
		return bitstream;

	bitstream.bytes = fiv_io_encode_webp(image, &config, &bitstream.size);
	return bitstream;
}

static void
save_thumbnail(FivIoImage *thumbnail, const char *path, GString *thum)
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

cairo_surface_t *
fiv_thumbnail_produce_for_search(
	GFile *target, FivThumbnailSize max_size, GError **error)
{
	g_return_val_if_fail(max_size >= FIV_THUMBNAIL_SIZE_MIN &&
		max_size <= FIV_THUMBNAIL_SIZE_MAX, NULL);

	GBytes *data = g_file_load_bytes(target, NULL, NULL, error);
	if (!data)
		return NULL;

	gboolean color_managed = FALSE;
	FivIoImage *image = render(target, data, &color_managed, error);
	if (!image)
		return NULL;

	// TODO(p): Might want to keep this a square.
	FivIoImage *result =
		adjust_thumbnail(image, fiv_thumbnail_sizes[max_size].size);
	fiv_io_image_unref(image);
	return fiv_io_image_to_surface(result);
}

static cairo_surface_t *
produce_fallback(GFile *target, FivThumbnailSize size, GError **error)
{
	// Note that this comes with a TOCTTOU problem.
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
		return NULL;
	}

	GBytes *data = g_file_load_bytes(target, NULL, NULL, error);
	if (!data)
		return NULL;

	gboolean color_managed = FALSE;
	FivIoImage *image = render(target, data, &color_managed, error);
	if (!image)
		return NULL;

	FivIoImage *result =
		adjust_thumbnail(image, fiv_thumbnail_sizes[size].size);
	fiv_io_image_unref(image);
	return fiv_io_image_to_surface(result);
}

cairo_surface_t *
fiv_thumbnail_produce(GFile *target, FivThumbnailSize max_size, GError **error)
{
	g_return_val_if_fail(max_size >= FIV_THUMBNAIL_SIZE_MIN &&
		max_size <= FIV_THUMBNAIL_SIZE_MAX, NULL);

	// Don't save thumbnails for FUSE mounts, such as sftp://.
	// Moreover, it doesn't make sense to save thumbnails of thumbnails.
	const gchar *path = g_file_peek_path(target);
	if (!path || !g_file_is_native(target) || might_be_a_thumbnail(path))
		return produce_fallback(target, max_size, error);

	// Make the TOCTTOU issue favour unnecessary reloading.
	GStatBuf st = {};
	if (g_stat(path, &st)) {
		set_error(error, g_strerror(errno));
		return NULL;
	}

	// TODO(p): Use open(O_RDONLY | O_NONBLOCK | _O_BINARY), fstat(),
	// g_mapped_file_new_from_fd(), and reset the non-blocking flag on the file.
	if (!S_ISREG(st.st_mode)) {
		set_error(error, "not a regular file");
		return NULL;
	}

	GError *e = NULL;
	GMappedFile *mf = g_mapped_file_new(path, FALSE, &e);
	if (!mf) {
		g_debug("%s: %s", path, e->message);
		g_error_free(e);
		return produce_fallback(target, max_size, error);
	}

	// In this case, g_mapped_file_get_bytes() has NULL data, causing issues.
	gsize filesize = g_mapped_file_get_length(mf);
	if (!filesize) {
		set_error(error, "empty file");
		return NULL;
	}

	gboolean color_managed = FALSE;
	FivIoImage *image =
		render(target, g_mapped_file_get_bytes(mf), &color_managed, error);
	g_mapped_file_unref(mf);
	if (!image)
		return NULL;

	// Boilerplate copied from fiv_thumbnail_lookup().
	gchar *uri = g_file_get_uri(target);
	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	gchar *thumbnails_dir = fiv_thumbnail_get_root();

	GString *thum = g_string_new("");
	g_string_append_printf(
		thum, "%s%c%s%c", THUMB_URI, 0, uri, 0);
	g_string_append_printf(
		thum, "%s%c%ld%c", THUMB_MTIME, 0, (long) st.st_mtime, 0);
	g_string_append_printf(
		thum, "%s%c%llu%c", THUMB_SIZE, 0, (unsigned long long) filesize, 0);

	g_string_append_printf(thum, "%s%c%u%c", THUMB_IMAGE_WIDTH, 0,
		(unsigned) image->width, 0);
	g_string_append_printf(thum, "%s%c%u%c", THUMB_IMAGE_HEIGHT, 0,
		(unsigned) image->height, 0);

	// Without a CMM, no conversion is attempted.
	if (color_managed) {
		g_string_append_printf(
			thum, "%s%c%s%c", THUMB_COLORSPACE, 0, THUMB_COLORSPACE_SRGB, 0);
	}

	FivIoImage *max_size_image = NULL;
	for (int use = max_size; use >= FIV_THUMBNAIL_SIZE_MIN; use--) {
		FivIoImage *scaled =
			adjust_thumbnail(image, fiv_thumbnail_sizes[use].size);
		gchar *path = g_strdup_printf("%s/wide-%s/%s.webp", thumbnails_dir,
			fiv_thumbnail_sizes[use].thumbnail_spec_name, sum);
		save_thumbnail(scaled, path, thum);
		g_free(path);

		if (!max_size_image)
			max_size_image = scaled;
		else
			fiv_io_image_unref(scaled);
	}

	g_string_free(thum, TRUE);

	g_free(thumbnails_dir);
	g_free(sum);
	g_free(uri);
	fiv_io_image_unref(image);
	return fiv_io_image_to_surface(max_size_image);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

typedef struct {
	const char *uri;                    ///< Target URI
	time_t mtime;                       ///< File modification time
	guint64 size;                       ///< File size
} Stat;

static bool
check_wide_thumbnail_texts(GBytes *thum, const Stat *st, bool *sRGB)
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
			if (strcmp(st->uri, s))
				return false;
		} else if (!strcmp(key, THUMB_MTIME)) {
			have_mtime = true;
			if (atol(s) != st->mtime)
				return false;
		} else if (!strcmp(key, THUMB_SIZE)) {
			if (strtoull(s, NULL, 10) != st->size)
				return false;
		} else if (!strcmp(key, THUMB_COLORSPACE))
			*sRGB = !strcmp(s, THUMB_COLORSPACE_SRGB);

		key = NULL;
	}
	return have_uri && have_mtime;
}

static cairo_surface_t *
read_wide_thumbnail(const char *path, const Stat *st, GError **error)
{
	gchar *thumbnail_uri = g_filename_to_uri(path, NULL, error);
	if (!thumbnail_uri)
		return NULL;

	FivIoImage *image =
		fiv_io_open(&(FivIoOpenContext){.uri = thumbnail_uri}, error);
	g_free(thumbnail_uri);
	if (!image)
		return NULL;

	bool sRGB = false;
	if (!image->thum) {
		g_clear_error(error);
		set_error(error, "not a thumbnail");
	} else if (!check_wide_thumbnail_texts(image->thum, st, &sRGB)) {
		g_clear_error(error);
		set_error(error, "mismatch");
	} else {
		// TODO(p): Add a function or a non-valueless define to check
		// for CMM presence, then remove this ifdef.
		cairo_surface_t *surface = fiv_io_image_to_surface(image);
#ifdef HAVE_LCMS2
		if (!sRGB)
			mark_thumbnail_lq(surface);
#endif  // HAVE_LCMS2
		return surface;
	}

	fiv_io_image_unref(image);
	return NULL;
}

static cairo_surface_t *
read_png_thumbnail(const char *path, const Stat *st, GError **error)
{
	FivIoImage *image = fiv_io_open_png_thumbnail(path, error);
	if (!image)
		return NULL;

	GHashTable *texts = image->text;
	if (!texts) {
		set_error(error, "not a thumbnail");
		fiv_io_image_unref(image);
		return NULL;
	}

	// May contain Thumb::Image::Width Thumb::Image::Height,
	// but those aren't interesting currently (would be for fast previews).
	const char *text_uri = g_hash_table_lookup(texts, THUMB_URI);
	const char *text_mtime = g_hash_table_lookup(texts, THUMB_MTIME);
	const char *text_size = g_hash_table_lookup(texts, THUMB_SIZE);
	if (!text_uri || strcmp(text_uri, st->uri) ||
		!text_mtime || atol(text_mtime) != st->mtime) {
		set_error(error, "mismatch or not a thumbnail");
		fiv_io_image_unref(image);
		return NULL;
	}
	if (text_size && strtoull(text_size, NULL, 10) != st->size) {
		set_error(error, "file size mismatch");
		fiv_io_image_unref(image);
		return NULL;
	}

	return fiv_io_image_to_surface(image);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

cairo_surface_t *
fiv_thumbnail_lookup(const char *uri, gint64 mtime_msec, guint64 filesize,
	FivThumbnailSize size)
{
	g_return_val_if_fail(size >= FIV_THUMBNAIL_SIZE_MIN &&
		size <= FIV_THUMBNAIL_SIZE_MAX, NULL);

	// Don't waste time looking up something that shouldn't exist--
	// thumbnail directories tend to get huge, and syscalls are expensive.
	if (might_be_a_thumbnail(uri))
		return NULL;

	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	gchar *thumbnails_dir = fiv_thumbnail_get_root();
	const Stat st = {.uri = uri, .mtime = mtime_msec / 1000, .size = filesize};

	// The lookup sequence is: nominal..max, then mirroring back to ..min.
	cairo_surface_t *result = NULL;
	GError *error = NULL;
	for (int i = 0; i < FIV_THUMBNAIL_SIZE_COUNT; i++) {
		FivThumbnailSize use = size + i;
		if (use > FIV_THUMBNAIL_SIZE_MAX)
			use = FIV_THUMBNAIL_SIZE_MAX - i;

		const char *name = fiv_thumbnail_sizes[use].thumbnail_spec_name;
		gchar *wide = g_strconcat(thumbnails_dir, G_DIR_SEPARATOR_S "wide-",
			name, G_DIR_SEPARATOR_S, sum, ".webp", NULL);
		result = read_wide_thumbnail(wide, &st, &error);
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

		gchar *path = g_strconcat(thumbnails_dir, G_DIR_SEPARATOR_S,
			name, G_DIR_SEPARATOR_S, sum, ".png", NULL);
		result = read_png_thumbnail(path, &st, &error);
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
identify_wide_thumbnail(GMappedFile *mf, Stat *st, GError **error)
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
				st->mtime = atol(p);
			if (!strcmp(key, THUMB_SIZE))
				st->size = strtoull(p, NULL, 10);
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
	GError *tolerable_error = NULL;
	const char *path = g_file_peek_path(thumbnail);
	GMappedFile *mf = g_mapped_file_new(path, FALSE, &tolerable_error);
	if (!mf) {
		print_error(thumbnail, tolerable_error);
		return;
	}

	// Note that we could enforce the presence of the size field in our spec.
	Stat target_st = {.uri = NULL, .mtime = 0, .size = G_MAXUINT64};
	gchar *target_uri = identify_wide_thumbnail(mf, &target_st, error);
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
		G_FILE_ATTRIBUTE_STANDARD_NAME ","
		G_FILE_ATTRIBUTE_STANDARD_SIZE ","
		G_FILE_ATTRIBUTE_TIME_MODIFIED,
		G_FILE_QUERY_INFO_NONE, NULL, &tolerable_error);
	g_object_unref(target);
	if (g_error_matches(tolerable_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
		g_propagate_error(error, tolerable_error);
		return;
	} else if (tolerable_error) {
		print_error(thumbnail, tolerable_error);
		return;
	}

	guint64 filesize = g_file_info_get_size(info);
	GDateTime *mdatetime = g_file_info_get_modification_date_time(info);
	g_object_unref(info);
	if (!mdatetime) {
		set_error(&tolerable_error, "cannot retrieve file modification time");
		print_error(thumbnail, tolerable_error);
		return;
	}
	if (g_date_time_to_unix(mdatetime) != target_st.mtime)
		set_error(error, "modification time mismatch");
	else if (target_st.size != G_MAXUINT64 && filesize != target_st.size)
		set_error(error, "file size mismatch");

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

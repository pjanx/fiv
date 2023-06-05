//
// rawinfo.c: acquire information about raw image files in JSON format
//
// Copyright (c) 2023, Přemysl Eric Janouch <p@janouch.name>
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

#include "info.h"

#include <jv.h>
#include <libraw.h>

#if LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 21, 0)
#error LibRaw 0.21.0 or newer is required.
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Raw image files ---------------------------------------------------------
// This is in principle similar to LibRaw's `raw-identify -v`,
// but the output is machine-processable.

static jv
parse_raw(jv o, const uint8_t *p, size_t len)
{
	libraw_data_t *iprc = libraw_init(LIBRAW_OPIONS_NO_DATAERR_CALLBACK);
	if (!iprc)
		return add_error(o, "failed to obtain a LibRaw handle");

	int err = 0;
	if ((err = libraw_open_buffer(iprc, p, len))) {
		libraw_close(iprc);
		return add_error(o, libraw_strerror(err));
	}

	// -> iprc->rawparams.shot_select
	o = jv_set(o, jv_string("count"), jv_number(iprc->idata.raw_count));

	o = jv_set(o, jv_string("width"), jv_number(iprc->sizes.width));
	o = jv_set(o, jv_string("height"), jv_number(iprc->sizes.height));
	o = jv_set(o, jv_string("flip"), jv_number(iprc->sizes.flip));
	o = jv_set(o, jv_string("pixel_aspect_ratio"),
		jv_number(iprc->sizes.pixel_aspect));

	if ((err = libraw_adjust_sizes_info_only(iprc))) {
		o = add_warning(o, libraw_strerror(err));
	} else {
		o = jv_set(
			o, jv_string("output_width"), jv_number(iprc->sizes.iwidth));
		o = jv_set(
			o, jv_string("output_height"), jv_number(iprc->sizes.iheight));
	}

	jv thumbnails = jv_array();
	for (int i = 0; i < iprc->thumbs_list.thumbcount; i++) {
		libraw_thumbnail_item_t *item = iprc->thumbs_list.thumblist + i;

		const char *format = "?";
		switch (item->tformat) {
		case LIBRAW_INTERNAL_THUMBNAIL_UNKNOWN:
			format = "unknown";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_KODAK_THUMB:
			format = "Kodak thumbnail";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_KODAK_YCBCR:
			format = "Kodak YCbCr";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_KODAK_RGB:
			format = "Kodak RGB";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_JPEG:
			format = "JPEG";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_LAYER:
			format = "layer";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_ROLLEI:
			format = "Rollei";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_PPM:
			format = "PPM";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_PPM16:
			format = "PPM16";
			break;
		case LIBRAW_INTERNAL_THUMBNAIL_X3F:
			format = "X3F";
			break;
		}

		jv to = JV_OBJECT(
			jv_string("width"), jv_number(item->twidth),
			jv_string("height"), jv_number(item->theight),
			jv_string("flip"), jv_number(item->tflip),
			jv_string("format"), jv_string(format));

		if (item->tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEG &&
			item->toffset > 0 &&
			(size_t) item->toffset + item->tlength <= len) {
			to = jv_set(to, jv_string("JPEG"),
				parse_jpeg(jv_object(), p + item->toffset, item->tlength));
		}

		thumbnails = jv_array_append(thumbnails, to);
	}

	libraw_close(iprc);
	return jv_set(o, jv_string("thumbnails"), thumbnails);
}

// --- I/O ---------------------------------------------------------------------

static jv
do_file(const char *filename, jv o)
{
	const char *err = NULL;
	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		err = strerror(errno);
		goto error;
	}

	uint8_t *data = NULL, buf[256 << 10];
	size_t n, len = 0;
	while ((n = fread(buf, sizeof *buf, sizeof buf / sizeof *buf, fp))) {
		data = realloc(data, len + n);
		memcpy(data + len, buf, n);
		len += n;
	}
	if (ferror(fp)) {
		err = strerror(errno);
		goto error_read;
	}

	o = parse_raw(o, data, len);

error_read:
	fclose(fp);
	free(data);
error:
	if (err)
		o = add_error(o, err);
	return o;
}

int
main(int argc, char *argv[])
{
	// XXX: Can't use `xargs -P0`, there's a risk of non-atomic writes.
	// Usage: find . -print0 | xargs -0 ./rawinfo
	for (int i = 1; i < argc; i++) {
		const char *filename = argv[i];

		jv o = jv_object();
		o = jv_object_set(o, jv_string("filename"), jv_string(filename));
		o = do_file(filename, o);
		jv_dumpf(o, stdout, 0 /* Might consider JV_PRINT_SORTED. */);
		fputc('\n', stdout);
	}
	return 0;
}

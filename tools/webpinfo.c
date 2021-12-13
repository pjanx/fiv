//
// webpinfo.c: acquire information about WebP files in JSON format
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

#include "info.h"

#include <jv.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- WebP --------------------------------------------------------------------
// https://github.com/webmproject/libwebp/blob/master/doc/webp-container-spec.txt
// https://github.com/webmproject/libwebp/blob/master/doc/webp-lossless-bitstream-spec.txt
// https://datatracker.ietf.org/doc/html/rfc6386

static jv
parse_webp(jv o, const uint8_t *p, size_t len)
{
	// libwebp won't let us simply iterate over all chunks, so handroll it.
	if (len < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WEBP", 4))
		return add_error(o, "not a WEBP file");

	// TODO(p): This can still be parseable.
	// TODO(p): Warn on trailing data.
	uint32_t size = unle.u32(p + 4);
	if (8 + size < len)
		return add_error(o, "truncated file");

	const uint8_t *end = p + 8 + size;
	p += 12;

	jv chunks = jv_array();
	while (p < end) {
		if (end - p < 8) {
			o = add_warning(o, "framing mismatch");
			printf("%ld", end - p);
			break;
		}

		uint32_t chunk_size = unle.u32(p + 4);
		uint32_t chunk_advance = (chunk_size + 1) & ~1;
		if (p + 8 + chunk_advance > end) {
			o = add_warning(o, "runaway chunk payload");
			break;
		}

		char fourcc[5] = "";
		memcpy(fourcc, p, 4);
		chunks = jv_array_append(chunks, jv_string(fourcc));
		p += 8;

		// TODO(p): Decode VP8 and VP8L chunk metadata.
		if (!strcmp(fourcc, "EXIF"))
			o = parse_exif(o, p, chunk_size);
		if (!strcmp(fourcc, "ICCP"))
			o = parse_icc(o, p, chunk_size);
		p += chunk_advance;
	}
	return jv_set(o, jv_string("chunks"), chunks);
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

	o = parse_webp(o, data, len);
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
	(void) parse_psir;

	// XXX: Can't use `xargs -P0`, there's a risk of non-atomic writes.
	// Usage: find . -iname *.png -print0 | xargs -0 ./pnginfo
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

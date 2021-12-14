//
// bmffinfo.c: acquire information about BMFF files in JSON format
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

// --- ISO/IEC base media file format ------------------------------------------
// ISO/IEC 14496-12:2015(E), used to be publicly available, now there's only:
// https://mpeg.chiariglione.org/standards/mpeg-4/iso-base-media-file-format/text-isoiec-14496-12-5th-edition
// but people have managed to archive the final version as well:
// https://b.goeswhere.com/ISO_IEC_14496-12_2015.pdf
//
// ISO/IEC 23008-12:2017 Information technology -
// High efficiency coding and media delivery in heterogeneous environments -
// Part 12: Image File Format + Cor 1:2020 Technical Corrigendum 1
// https://standards.iso.org/ittf/PubliclyAvailableStandards/

static jv
parse_bmff_box(jv o, const char *type, const uint8_t *data, size_t len)
{
	// TODO(p): Parse out "uuid"'s uint8_t[16] initial field, present as hex.
	// TODO(p): Parse out "ftyp" contents: 14496-12:2015 4.3
	// TODO(p): Parse out other important boxes: 14496-12:2015 8+
	return add_to_subarray(o, "boxes", jv_string(type));
}

static jv
parse_bmff(jv o, const uint8_t *p, size_t len)
{
	// 4.2 Object Structure--this box need not be present, nor at the beginning
	// TODO(p): What does `aligned(8)` mean? It's probably in bits.
	if (len < 8 || memcmp(p + 4, "ftyp", 4))
		return add_error(o, "not BMFF at all or unsupported");

	const uint8_t *end = p + len;
	while (p < end) {
		if (end - p < 8) {
			o = add_warning(o, "box framing mismatch");
			break;
		}

		char type[5] = "";
		memcpy(type, p + 4, 4);

		uint64_t box_size = u32be(p);
		const uint8_t *data = p + 8;
		if (box_size == 1) {
			if (end - p < 16) {
				o = add_warning(o, "unexpected EOF");
				break;
			}
			box_size = u64be(data);
			data += 8;
		} else if (!box_size)
			box_size = end - p;

		if (box_size > (uint64_t) (end - p)) {
			o = add_warning(o, "unexpected EOF");
			break;
		}

		size_t data_len = box_size - (data - p);
		o = parse_bmff_box(o, type, data, data_len);
		p += box_size;
	}
	return o;
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

	o = parse_bmff(o, data, len);
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
	(void) parse_icc;
	(void) parse_exif;
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

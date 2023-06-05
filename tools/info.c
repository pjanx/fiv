//
// info.c: acquire information about JPEG/TIFF/BMFF/WebP files in JSON format
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

static bool
detect_bmff(const uint8_t *p, size_t len)
{
	// 4.2 Object Structure--this box need not be present, nor at the beginning
	// TODO(p): What does `aligned(8)` mean? It's probably in bits.
	return len >= 8 && !memcmp(p + 4, "ftyp", 4);
}

static jv
parse_bmff(jv o, const uint8_t *p, size_t len)
{
	if (!detect_bmff(p, len))
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

// --- WebP --------------------------------------------------------------------
// libwebp won't let us simply iterate over all chunks, so handroll it.
//
// https://github.com/webmproject/libwebp/blob/master/doc/webp-container-spec.txt
// https://github.com/webmproject/libwebp/blob/master/doc/webp-lossless-bitstream-spec.txt
// https://datatracker.ietf.org/doc/html/rfc6386
//
// Pretty versions, hopefully not outdated:
// https://developers.google.com/speed/webp/docs/riff_container
// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification

static bool
detect_webp(const uint8_t *p, size_t len)
{
	return len >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4);
}

static jv
parse_webp_vp8(jv o, const uint8_t *p, size_t len)
{
	if (len < 10 || (p[0] & 1) != 0 /* key frame */ ||
		p[3] != 0x9d || p[4] != 0x01 || p[5] != 0x2a) {
		return add_warning(o, "invalid VP8 chunk");
	}

	o = jv_set(o, jv_string("width"), jv_number(u16le(p + 6) & 0x3fff));
	o = jv_set(o, jv_string("height"), jv_number(u16le(p + 8) & 0x3fff));
	return o;
}

static jv
parse_webp_vp8l(jv o, const uint8_t *p, size_t len)
{
	if (len < 5 || p[0] != 0x2f)
		return add_warning(o, "invalid VP8L chunk");

	// Reading LSB-first from a little endian value means reading in order.
	uint32_t header = u32le(p + 1);
	o = jv_set(o, jv_string("width"), jv_number((header & 0x3fff) + 1));
	header >>= 14;
	o = jv_set(o, jv_string("height"), jv_number((header & 0x3fff) + 1));
	header >>= 14;
	o = jv_set(o, jv_string("alpha_is_used"), jv_bool(header & 1));
	return o;
}

static jv
parse_webp_vp8x(jv o, const uint8_t *p, size_t len)
{
	if (len < 10)
		return add_warning(o, "invalid VP8X chunk");

	// Most of the fields in this chunk are duplicate or inferrable.
	// Probably not worth decoding or verifying.
	// TODO(p): For animations, we need to use the width and height from here.
	uint8_t flags = p[0];
	o = jv_set(o, jv_string("animation"), jv_bool((flags >> 1) & 1));
	return o;
}

static jv
parse_webp(jv o, const uint8_t *p, size_t len)
{
	if (!detect_webp(p, len))
		return add_error(o, "not a WEBP file");

	// TODO(p): This can still be parseable.
	// TODO(p): Warn on trailing data.
	uint32_t size = u32le(p + 4);
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

		uint32_t chunk_size = u32le(p + 4);
		uint32_t chunk_advance = (chunk_size + 1) & ~1;
		if (p + 8 + chunk_advance > end) {
			o = add_warning(o, "runaway chunk payload");
			break;
		}

		char fourcc[5] = "";
		memcpy(fourcc, p, 4);
		chunks = jv_array_append(chunks, jv_string(fourcc));
		p += 8;

		// TODO(p): Decode more chunks.
		if (!strcmp(fourcc, "VP8 "))
			o = parse_webp_vp8(o, p, chunk_size);
		if (!strcmp(fourcc, "VP8L"))
			o = parse_webp_vp8l(o, p, chunk_size);
		if (!strcmp(fourcc, "VP8X"))
			o = parse_webp_vp8x(o, p, chunk_size);
		if (!strcmp(fourcc, "EXIF"))
			o = parse_exif(o, p, chunk_size);
		if (!strcmp(fourcc, "ICCP"))
			o = parse_icc(o, p, chunk_size);
		p += chunk_advance;
	}
	return jv_set(o, jv_string("chunks"), chunks);
}

// --- I/O ---------------------------------------------------------------------

static struct {
	const char *name;
	bool (*detect) (const uint8_t *, size_t);
	jv (*parse) (jv, const uint8_t *, size_t);
} formats[] = {
	{"JPEG", detect_jpeg, parse_jpeg},
	{"TIFF", detect_tiff, parse_tiff},
	{"BMFF", detect_bmff, parse_bmff},
	{"WebP", detect_webp, parse_webp},
};

static jv
parse_any(jv o, const uint8_t *p, size_t len)
{
	// TODO(p): Also see if the file extension is appropriate.
	for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) {
		if (!formats[i].detect(p, len))
			continue;
		if (getenv("INFO_IDENTIFY"))
			o = jv_set(o, jv_string("format"), jv_string(formats[i].name));
		return formats[i].parse(o, p, len);
	}
	return add_error(o, "unsupported file format");
}

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

#if 0
	// Not sure if I want to ensure their existence...
	o = jv_object_set(o, jv_string("info"), jv_array());
	o = jv_object_set(o, jv_string("warnings"), jv_array());
#endif

	o = parse_any(o, data, len);
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
	// Usage: find . -print0 | xargs -0 ./info
	for (int i = 1; i < argc; i++) {
		const char *filename = argv[i];

		jv o = jv_object();
		o = jv_object_set(o, jv_string("filename"), jv_string(filename));
		o = do_file(filename, o);
		jv_dumpf(o, stdout, 0 /* JV_PRINT_SORTED would discard information. */);
		fputc('\n', stdout);
	}
	return 0;
}

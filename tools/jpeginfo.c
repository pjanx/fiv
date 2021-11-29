//
// jpeginfo.c: acquire information about JPEG files in JSON format
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

#include <jv.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

// --- JPEG --------------------------------------------------------------------
// See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf

enum {
	TEM = 0x01,
	SOF0 = 0xC0, SOF1, SOF2, SOF3,
	DHT = 0xC4,
	SOF5, SOF6, SOF7,
	JPG = 0xC8,
	SOF9, SOF10, SOF11,
	DAC = 0xCC,
	SOF13, SOF14, SOF15,

	RST0 = 0xD0, RST1, RST2, RST3, RST4, RST5, RST6, RST7,

	SOI = 0xD8,
	EOI = 0xD9,
	SOS = 0xDA,
	DQT = 0xDB,
	DNL = 0xDC,
	DRI = 0xDD,
	DHP = 0xDE,
	EXP = 0xDF,

	APP0 = 0xE0, APP1, APP2, APP3, APP4, APP5, APP6, APP7,
	APP8, APP9, APP10, APP11, APP12, APP13, APP14, APP15,

	JPG0 = 0xF0, JPG1, JPG2, JPG3, JPG4, JPG5, JPG6, JPG7,
	JPG8, JPG9, JPG10, JPG11, JPG12, JPG13,

	COM = 0xFE
};

// The rest is "RES (Reserved)", except for 0xFF (filler) and 0x00 (invalid).
static const char *marker_ids[0xFF] = {
	[TEM]   = "TEM",
	[SOF0]  = "SOF0",  [SOF1]  = "SOF1",  [SOF2]  = "SOF2",  [SOF3]  = "SOF3",
	[DHT]   = "DHT",   [SOF5]  = "SOF5",  [SOF6]  = "SOF6",  [SOF7]  = "SOF7",
	[JPG]   = "JPG",   [SOF9]  = "SOF9",  [SOF10] = "SOF10", [SOF11] = "SOF11",
	[DAC]   = "DAC",   [SOF13] = "SOF13", [SOF14] = "SOF14", [SOF15] = "SOF15",
	[RST0]  = "RST0",  [RST1]  = "RST1",  [RST2]  = "RST2",  [RST3]  = "RST3",
	[RST4]  = "RST4",  [RST5]  = "RST5",  [RST6]  = "RST6",  [RST7]  = "RST7",
	[SOI]   = "SOI",   [EOI]   = "EOI",   [SOS]   = "SOS",   [DQT]   = "DQT",
	[DNL]   = "DNL",   [DRI]   = "DRI",   [DHP]   = "DHP",   [EXP]   = "EXP",
	[APP0]  = "APP0",  [APP1]  = "APP1",  [APP2]  = "APP2",  [APP3]  = "APP3",
	[APP4]  = "APP4",  [APP5]  = "APP5",  [APP6]  = "APP6",  [APP7]  = "APP7",
	[APP8]  = "APP8",  [APP9]  = "APP9",  [APP10] = "APP10", [APP11] = "APP11",
	[APP12] = "APP12", [APP13] = "APP13", [APP14] = "APP14", [APP15] = "APP15",
	[JPG0]  = "JPG0",  [JPG1]  = "JPG1",  [JPG2]  = "JPG2",  [JPG3]  = "JPG3",
	[JPG4]  = "JPG4",  [JPG5]  = "JPG5",  [JPG6]  = "JPG6",  [JPG7]  = "JPG7",
	[JPG8]  = "JPG8",  [JPG9]  = "JPG9",  [JPG10] = "JPG10", [JPG11] = "JPG11",
	[JPG12] = "JPG12", [JPG13] = "JPG13", [COM]   = "COM"
};

/*
// The rest is "RES (Reserved)", except for 0xFF (filler) and 0x00 (invalid).
static const char *marker_descriptions[0xFF] = {
	[TEM]   = "For temporary private use in arithmetic coding",
	[SOF0]  = "Baseline DCT",
	[SOF1]  = "Extended sequential DCT",
	[SOF2]  = "Progressive DCT",
	[SOF3]  = "Lossless (sequential)",
	[DHT]   = "Define Huffman table(s)",
	[SOF5]  = "Differential sequential DCT",
	[SOF6]  = "Differential progressive DCT",
	[SOF7]  = "Differential lossless (sequential)",
	[JPG]   = "Reserved for JPEG extensions",
	[SOF9]  = "Extended sequential DCT",
	[SOF10] = "Progressive DCT",
	[SOF11] = "Lossless (sequential)",
	[DAC]   = "Define arithmetic coding conditioning(s)",
	[SOF13] = "Differential sequential DCT",
	[SOF14] = "Differential progressive DCT",
	[SOF15] = "Differential lossless (sequential)",
	[RST0]  = "Restart with module 8 count 0",
	[RST1]  = "Restart with module 8 count 1",
	[RST2]  = "Restart with module 8 count 2",
	[RST3]  = "Restart with module 8 count 3",
	[RST4]  = "Restart with module 8 count 4",
	[RST5]  = "Restart with module 8 count 5",
	[RST6]  = "Restart with module 8 count 6",
	[RST7]  = "Restart with module 8 count 7",
	[SOI]   = "Start of image",
	[EOI]   = "End of image",
	[SOS]   = "Start of scan",
	[DQT]   = "Define quantization table(s)",
	[DNL]   = "Define number of lines",
	[DRI]   = "Define restart interval",
	[DHP]   = "Define hierarchical progression",
	[EXP]   = "Expand reference component(s)",
	[APP0]  = "Reserved for application segments, 0",
	[APP1]  = "Reserved for application segments, 1",
	[APP2]  = "Reserved for application segments, 2",
	[APP3]  = "Reserved for application segments, 3",
	[APP4]  = "Reserved for application segments, 4",
	[APP5]  = "Reserved for application segments, 5",
	[APP6]  = "Reserved for application segments, 6",
	[APP7]  = "Reserved for application segments, 7",
	[APP8]  = "Reserved for application segments, 8",
	[APP9]  = "Reserved for application segments, 9",
	[APP10] = "Reserved for application segments, 10",
	[APP11] = "Reserved for application segments, 11",
	[APP12] = "Reserved for application segments, 12",
	[APP13] = "Reserved for application segments, 13",
	[APP14] = "Reserved for application segments, 14",
	[APP15] = "Reserved for application segments, 15",
	[JPG0]  = "Reserved for JPEG extensions, 0",
	[JPG1]  = "Reserved for JPEG extensions, 1",
	[JPG2]  = "Reserved for JPEG extensions, 2",
	[JPG3]  = "Reserved for JPEG extensions, 3",
	[JPG4]  = "Reserved for JPEG extensions, 4",
	[JPG5]  = "Reserved for JPEG extensions, 5",
	[JPG6]  = "Reserved for JPEG extensions, 6",
	[JPG7]  = "Reserved for JPEG extensions, 7",
	[JPG8]  = "Reserved for JPEG extensions, 8",
	[JPG9]  = "Reserved for JPEG extensions, 9",
	[JPG10] = "Reserved for JPEG extensions, 10",
	[JPG11] = "Reserved for JPEG extensions, 11",
	[JPG12] = "Reserved for JPEG extensions, 12",
	[JPG13] = "Reserved for JPEG extensions, 13",
	[COM]   = "Comment",
};
*/

// --- Analysis ----------------------------------------------------------------
// Because the JPEG file format is simple, just do it manually.

static jv
add_to_subarray(jv o, const char *key, jv value)
{
	// Invalid values are not allocated, and we use up any valid one.
	// Beware that jv_get() returns jv_null() rather than jv_invalid().
	jv a = jv_object_get(jv_copy(o), jv_string(key));
	return jv_set(o, jv_string(key),
		jv_is_valid(jv_copy(a)) ? jv_array_append(a, value) : JV_ARRAY(value));
}

static jv
add_warning(jv o, const char *message)
{
	return add_to_subarray(o, "warnings", jv_string(message));
}

static jv
add_error(jv o, const char *message)
{
	return jv_object_set(o, jv_string("error"), jv_string(message));
}

struct data {
	bool ended;
	char *exif, *icc;
	size_t exif_len, icc_len;
	int icc_sequence, icc_done;
};

static const uint8_t *
parse_marker(uint8_t marker, const uint8_t *p, const uint8_t *end,
	struct data *data, jv *o)
{
	// Suspected: MJPEG? Undetected format recursion, e.g., thumbnails?
	// Found: Random metadata! Multi-Picture Format!
	if ((data->ended = marker == EOI) && p != end)
		*o = add_warning(*o, "trailing data");

	// These markers stand alone, not starting a marker segment.
	switch (marker) {
	case RST0:
	case RST1:
	case RST2:
	case RST3:
	case RST4:
	case RST5:
	case RST6:
	case RST7:
		*o = add_warning(*o, "unexpected restart marker");
		// Fall-through
	case SOI:
	case EOI:
	case TEM:
		return p;
	}

	uint16_t length = p[0] << 8 | p[1];
	const uint8_t *payload = p + 2;
	if ((p += length) > end) {
		*o = add_error(*o, "runaway marker segment");
		return NULL;
	}

	switch (marker) {
	case SOF0:
	case SOF1:
	case SOF2:
	case SOF3:
	case SOF5:
	case SOF6:
	case SOF7:
	case SOF9:
	case SOF10:
	case SOF11:
	case SOF13:
	case SOF14:
	case SOF15:
	case DHP:  // B.2.2 and B.3.2.
		// As per B.2.5, Y can be zero, then there needs to be a DNL segment.
		*o = add_to_subarray(*o, "info", JV_OBJECT(
			jv_string("bits"), jv_number(payload[0]),
			jv_string("height"), jv_number(payload[1] << 8 | payload[2]),
			jv_string("width"), jv_number(payload[3] << 8 | payload[4]),
			jv_string("components"), jv_number(payload[5])
		));
		return p;
	}

	// See B.1.1.5, we can brute-force our way through the entropy-coded data.
	if (marker == SOS) {
		while (p + 2 <= end && (p[0] != 0xFF || p[1] < 0xC0 || p[1] > 0xFE ||
				(p[1] >= RST0 && p[1] <= RST7)))
			p++;
		return p;
	}

	// "The interpretation is left to the application."
	if (marker == COM) {
		int superascii = 0;
		char *buf = calloc(3, p - payload), *bufp = buf;
		for (const uint8_t *q = payload; q < p; q++) {
			if (*q < 128) {
				*bufp++ = *q;
			} else {
				superascii++;
				*bufp++ = 0xC0 | (*q >> 6);
				*bufp++ = 0x80 | (*q & 0x3F);
			}
		}
		*bufp++ = 0;
		*o = add_to_subarray(*o, "comments", jv_string(buf));
		free(buf);

		if (superascii)
			*o = add_warning(*o, "super-ASCII comments");
	}

	// These mostly contain an ASCII string header, following JPEG FIF:
	//
	// "Application-specific APP0 marker segments are identified
	//  by a zero terminated string which identifies the application
	//  (not 'JFIF' or 'JFXX')."
	if (marker >= APP0 && marker <= APP15) {
		const uint8_t *nul = memchr(payload, 0, p - payload);
		int unprintable = !nul;
		if (nul) {
			for (const uint8_t *q = payload; q < nul; q++)
				unprintable += *q < 32 || *q >= 127;
		}
		*o = add_to_subarray(*o, "apps",
			unprintable ? jv_null() : jv_string((const char *) payload));
	}

	// CIPA DC-007 (Multi-Picture Format)
	// http://fileformats.archiveteam.org/wiki/Multi-Picture_Format
	// TODO(p): Handle by properly skipping trailing data (use MPF offsets).

	// CIPA DC-006 (Stereo Still Image Format for Digital Cameras)
	// http://fileformats.archiveteam.org/wiki/Multi-Picture_Format
	// TODO(p): Handle by properly skipping trailing data (use Stim offsets).

	// https://www.w3.org/Graphics/JPEG/jfif3.pdf
	if (marker == APP0 && p - payload >= 14 && !memcmp(payload, "JFIF\0", 5)) {
		payload += 5;

		jv units = jv_number(payload[2]);
		switch (payload[2]) {
		break; case 0: units = jv_null();
		break; case 1: units = jv_string("DPI");
		break; case 2: units = jv_string("dots per cm");
		}

		// The rest is picture data.
		*o = add_to_subarray(*o, "JFIF", JV_OBJECT(
			jv_string("version"), jv_number(payload[0] * 100 + payload[1]),
			jv_string("units"), units,
			jv_string("density-x"), jv_number(payload[3] << 8 | payload[4]),
			jv_string("density-y"), jv_number(payload[5] << 8 | payload[6]),
			jv_string("thumbnail-w"), jv_number(payload[7]),
			jv_string("thumbnail-h"), jv_number(payload[8])
		));
	}

	// https://www.w3.org/Graphics/JPEG/jfif3.pdf
	if (marker == APP0 && p - payload >= 6 && !memcmp(payload, "JFXX\0", 5)) {
		payload += 5;

		jv extension = jv_number(payload[0]);
		switch (payload[0]) {
		break; case 0x10: extension = jv_string("JPEG thumbnail");
		break; case 0x11: extension = jv_string("Paletted thumbnail");
		break; case 0x13: extension = jv_string("RGB thumbnail");
		}

		// The rest is picture data.
		*o = add_to_subarray(*o, "JFXX",
			JV_OBJECT(jv_string("extension"), extension));
	}

	// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf 4.7.2
	if (marker == APP1 && p - payload >= 6 && !memcmp(payload, "Exif\0", 5)) {
		payload += 6;

		if (payload[-1] != 0)
			*o = add_warning(*o, "weirdly padded Exif header");

		if (data->exif) {
			*o = add_warning(*o, "multiple Exifs");
		} else {
			data->exif = realloc(data->exif, (data->exif_len = p - payload));
			memcpy(data->exif, payload, data->exif_len);
		}
	}

	// https://www.color.org/specification/ICC1v43_2010-12.pdf B.4
	if (marker == APP2 && p - payload >= 14 &&
		!memcmp(payload, "ICC_PROFILE\0", 12) && !data->icc_done &&
		payload[12] == ++data->icc_sequence && payload[13] >= payload[12]) {
		payload += 14;

		size_t icc_longer = data->icc_len + (p - payload);
		data->icc = realloc(data->icc, icc_longer);
		memcpy(data->icc + data->icc_len, payload, p - payload);
		data->icc_len = icc_longer;

		data->icc_done = payload[-1] == data->icc_sequence;
	}
	return p;
}

static jv
parse_jpeg(jv o, const uint8_t *p, size_t len)
{
	struct data data = {};
	const uint8_t *end = p + len;
	jv markers = jv_array();
	while (p) {
		// This is an expectable condition, use a simple warning.
		if (p + 2 > end) {
			if (!data.ended)
				o = add_warning(o, "unexpected EOF");
			break;
		}
		if (*p++ != 0xFF || *p == 0) {
			o = add_error(o, "no marker found where one was expected");
			break;
		}

		// Markers may be preceded by fill bytes.
		if (*p == 0xFF) {
			o = jv_object_set(o, jv_string("fillers"), jv_bool(true));
			continue;
		}

		uint8_t marker = *p++;
		markers = jv_array_append(markers,
			jv_string(marker_ids[marker] ? marker_ids[marker] : "RES"));
		p = parse_marker(marker, p, end, &data, &o);
	}

	if (data.exif) {
		// TODO(p): Decode.
		free(data.exif);
	}

	if (data.icc) {
		if (data.icc_done) {
			// TODO(p): Decode.
		} else {
			o = add_warning(o, "bad ICC profile sequence");
		}
		free(data.icc);
	}

	return jv_set(o, jv_string("markers"), markers);
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

	o = parse_jpeg(o, data, len);
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

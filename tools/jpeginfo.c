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

// --- Utilities ---------------------------------------------------------------

static uint32_t
u32be(const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint16_t
u16be(const uint8_t *p)
{
	return (uint16_t) p[0] << 8 | p[1];
}

static uint32_t
u32le(const uint8_t *p)
{
	return (uint32_t) p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0];
}

static uint16_t
u16le(const uint8_t *p)
{
	return (uint16_t) p[1] << 8 | p[0];
}

// --- TIFF --------------------------------------------------------------------
// TIFF Revision 6.0
// https://www.adobe.io/content/dam/udp/en/open/standards/tiff/TIFF6.pdf
//
// TIFF Technical Note 1: TIFF Trees
// https://download.osgeo.org/libtiff/old/TTN1.ps
//
// Adobe PageMaker 6.0 TIFF Technical Notes [includes TTN1]
// https://www.adobe.io/content/dam/udp/en/open/standards/tiff/TIFFPM6.pdf
//
// Exif Version 2.3
// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf
//
// libtiff is a mess, and the format is not particularly complicated.
// Exif libraries are senselessly copylefted.

static struct un {
	uint32_t (*u32) (const uint8_t *);
	uint16_t (*u16) (const uint8_t *);
} unbe = {u32be, u16be}, unle = {u32le, u16le};

struct tiffer {
	struct un *un;
	const uint8_t *begin, *p, *end;
	uint16_t remaining_fields;
};

static bool
tiffer_u32(struct tiffer *self, uint32_t *u)
{
	if (self->p + 4 > self->end)
		return false;
	*u = self->un->u32(self->p);
	self->p += 4;
	return true;
}

static bool
tiffer_u16(struct tiffer *self, uint16_t *u)
{
	if (self->p + 2 > self->end)
		return false;
	*u = self->un->u16(self->p);
	self->p += 2;
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
tiffer_init(struct tiffer *self, const uint8_t *tiff, size_t len)
{
	self->un = NULL;
	self->begin = self->p = tiff;
	self->end = tiff + len;
	self->remaining_fields = 0;

	const uint8_t
		le[4] = {'I', 'I', 42, 0},
		be[4] = {'M', 'M', 0, 42};

	if (tiff + 8 > self->end)
		return false;
	else if (!memcmp(tiff, le, sizeof le))
		self->un = &unle;
	else if (!memcmp(tiff, be, sizeof be))
		self->un = &unbe;
	else
		return false;

	self->p = tiff + 4;
	// The first IFD needs to be read by caller explicitly.
	return true;
}

/// Read the next IFD in a sequence.
static bool
tiffer_next_ifd(struct tiffer *self)
{
	// All fields from any previous IFD need to be read first.
	if (self->remaining_fields)
		return false;

	uint32_t ifd_offset = 0;
	if (!tiffer_u32(self, &ifd_offset))
		return false;

	// There is nothing more to read, this chain has terminated.
	if (!ifd_offset)
		return false;

	self->p = self->begin + ifd_offset;
	return tiffer_u16(self, &self->remaining_fields);
}

/// Initialize a derived TIFF reader for a subIFD at the given location.
static bool
tiffer_subifd(struct tiffer *self, uint32_t offset, struct tiffer *subreader)
{
	*subreader = *self;
	subreader->p = subreader->begin + offset;
	return tiffer_u16(subreader, &subreader->remaining_fields);
}

enum tiffer_type {
	BYTE = 1, ASCII, SHORT, LONG, RATIONAL,
	SBYTE, UNDEFINED, SSHORT, SLONG, SRATIONAL, FLOAT, DOUBLE,
	IFD  // This last type from TIFF Technical Note 1 isn't really used much.
};

static size_t
tiffer_value_size(enum tiffer_type type)
{
	switch (type) {
	case BYTE:
	case SBYTE:
	case ASCII:
	case UNDEFINED:
		return 1;
	case SHORT:
	case SSHORT:
		return 2;
	case LONG:
	case SLONG:
	case FLOAT:
	case IFD:
		return 4;
	case RATIONAL:
	case SRATIONAL:
	case DOUBLE:
		return 8;
	default:
		return 0;
	}
}

/// A lean iterator for values within entries.
struct tiffer_entry {
	uint16_t tag;
	enum tiffer_type type;
	// For {S,}BYTE, ASCII, UNDEFINED, use these fields directly.
	const uint8_t *p;
	uint32_t remaining_count;
};

static bool
tiffer_next_value(struct tiffer_entry *entry)
{
	if (!entry->remaining_count)
		return false;

	entry->p += tiffer_value_size(entry->type);
	entry->remaining_count--;
	return true;
}

static bool
tiffer_integer(
	const struct tiffer *self, const struct tiffer_entry *entry, int64_t *out)
{
	if (!entry->remaining_count)
		return false;

	// Somewhat excessively lenient, intended for display.
	switch (entry->type) {
	case BYTE:
	case ASCII:
	case UNDEFINED:
		*out = *entry->p;
		return true;
	case SBYTE:
		*out = (int8_t) *entry->p;
		return true;
	case SHORT:
		*out = self->un->u16(entry->p);
		return true;
	case SSHORT:
		*out = (int16_t) self->un->u16(entry->p);
		return true;
	case LONG:
	case IFD:
		*out = self->un->u32(entry->p);
		return true;
	case SLONG:
		*out = (int32_t) self->un->u32(entry->p);
		return true;
	default:
		return false;
	}
}

static bool
tiffer_rational(const struct tiffer *self, const struct tiffer_entry *entry,
	int64_t *numerator, int64_t *denominator)
{
	if (!entry->remaining_count)
		return false;

	// Somewhat excessively lenient, intended for display.
	switch (entry->type) {
	case RATIONAL:
		*numerator = self->un->u32(entry->p);
		*denominator = self->un->u32(entry->p + 4);
		return true;
	case SRATIONAL:
		*numerator = (int32_t) self->un->u32(entry->p);
		*denominator = (int32_t) self->un->u32(entry->p + 4);
		return true;
	default:
		if (tiffer_integer(self, entry, numerator)) {
			*denominator = 1;
			return true;
		}
		return false;
	}
}

static bool
tiffer_real(
	const struct tiffer *self, const struct tiffer_entry *entry, double *out)
{
	if (!entry->remaining_count)
		return false;

	// Somewhat excessively lenient, intended for display.
	switch (entry->type) {
		int64_t numerator, denominator;
	case FLOAT:
		*out = *(float *) entry->p;
		return true;
	case DOUBLE:
		*out = *(double *) entry->p;
		return true;
	default:
		if (tiffer_rational(self, entry, &numerator, &denominator)) {
			*out = (double) numerator / denominator;
			return true;
		}
		return false;
	}
}

static bool
tiffer_next_entry(struct tiffer *self, struct tiffer_entry *entry)
{
	if (!self->remaining_fields)
		return false;

	uint16_t type = entry->type = 0xFFFF;
	if (!tiffer_u16(self, &entry->tag) || !tiffer_u16(self, &type) ||
		!tiffer_u32(self, &entry->remaining_count))
		return false;

	// Short values may and will be inlined, rather than pointed to.
	size_t values_size = tiffer_value_size(type) * entry->remaining_count;
	uint32_t offset = 0;
	if (values_size <= sizeof offset) {
		entry->p = self->p;
		self->p += sizeof offset;
	} else if (tiffer_u32(self, &offset)) {
		entry->p = self->begin + offset;
	} else {
		return false;
	}

	// All entries are pre-checked not to overflow.
	if (entry->p + values_size > self->end)
		return false;

	// Setting it at the end may provide an indication while debugging.
	entry->type = type;
	self->remaining_fields--;
	return true;
}

// --- TIFF/Exif/MPF/* tags ----------------------------------------------------

struct tiff_value {
	const char *name;
	uint16_t value;
};

struct tiff_entry {
	const char *name;
	uint16_t tag;
	struct tiff_value *values;
};

static struct tiff_entry tiff_entries[] = {
	{"NewSubfileType", 254, NULL},
	{"SubfileType", 255, (struct tiff_value[]) {
		{"Full-resolution image data", 1},
		{"Reduced-resolution image data", 2},
		{"Page of a multi-page image", 3},
		{}
	}},
	{"ImageWidth", 256, NULL},
	{"ImageLength", 257, NULL},
	{"BitsPerSample", 258, NULL},
	{"Compression", 259, (struct tiff_value[]) {
		{"Uncompressed", 1},
		{"CCITT 1D", 2},
		{"Group 3 Fax", 3},
		{"Group 4 Fax", 4},
		{"LZW", 5},
		{"JPEG", 6},
		{"PackBits", 32773},
		{}
	}},
	{"PhotometricInterpretation", 262, (struct tiff_value[]) {
		{"WhiteIsZero", 0},
		{"BlackIsZero", 1},
		{"RGB", 2},
		{"RGB Palette", 3},
		{"Transparency mask", 4},
		{"CMYK", 5},
		{"YCbCr", 6},
		{"CIELab", 8},
		{"ICC CIELab", 9},  // Adobe PageMaker 6.0 TIFF Technical Notes
		{}
	}},
	{"Threshholding", 263, (struct tiff_value[]) {
		{"No dithering or halftoning", 1},
		{"Ordered dither or halftoning", 2},
		{"Randomized process", 3},
		{}
	}},
	{"CellWidth", 264, NULL},
	{"CellLength", 265, NULL},
	{"FillOrder", 266, (struct tiff_value[]) {
		{"MSB-first", 1},
		{"LSB-first", 2},
		{}
	}},
	{"DocumentName", 269, NULL},
	{"ImageDescription", 270, NULL},
	{"Make", 271, NULL},
	{"Model", 272, NULL},
	{"StripOffsets", 273, NULL},
	{"Orientation", 274, (struct tiff_value[]) {
		{"TopLeft", 1},
		{"TopRight", 2},
		{"BottomRight", 3},
		{"BottomLeft", 4},
		{"LeftTop", 5},
		{"RightTop", 6},
		{"RightBottom", 7},
		{"LeftBottom", 8},
		{}
	}},
	{"SamplesPerPixel", 277, NULL},
	{"RowsPerStrip", 278, NULL},
	{"StripByteCounts", 279, NULL},
	{"MinSampleValue", 280, NULL},
	{"MaxSampleValue", 281, NULL},
	{"XResolution", 282, NULL},
	{"YResolution", 283, NULL},
	{"PlanarConfiguration", 284, (struct tiff_value[]) {
		{"Chunky", 1},
		{"Planar", 2},
		{}
	}},
	{"PageName", 285, NULL},
	{"XPosition", 286, NULL},
	{"YPosition", 287, NULL},
	{"FreeOffsets", 288, NULL},
	{"FreeByteCounts", 289, NULL},
	{"GrayResponseUnit", 290, (struct tiff_value[]) {
		{"1/10", 1},
		{"1/100", 2},
		{"1/1000", 3},
		{"1/10000", 4},
		{"1/100000", 5},
		{}
	}},
	{"GrayResponseCurve", 291, NULL},
	{"T4Options", 292, NULL},
	{"T6Options", 293, NULL},
	{"ResolutionUnit", 296, (struct tiff_value[]) {
		{"None", 1},
		{"Inch", 2},
		{"Centimeter", 3},
		{}
	}},
	{"PageNumber", 297, NULL},
	{"TransferFunction", 301, NULL},
	{"Software", 305, NULL},
	{"DateTime", 306, NULL},
	{"Artist", 315, NULL},
	{"HostComputer", 316, NULL},
	{"Predictor", 317, (struct tiff_value[]) {
		{"None", 1},
		{"Horizontal", 2},
		{}
	}},
	{"WhitePoint", 318, NULL},
	{"PrimaryChromaticities", 319, NULL},
	{"ColorMap", 320, NULL},
	{"HalftoneHints", 321, NULL},
	{"TileWidth", 322, NULL},
	{"TileLength", 323, NULL},
	{"TileOffsets", 324, NULL},
	{"TileByteCounts", 325, NULL},
	{"SubIFDs", 330, NULL},  // TIFF Technical Note 1: TIFF Trees
	{"InkSet", 332, (struct tiff_value[]) {
		{"CMYK", 1},
		{"Non-CMYK", 2},
		{}
	}},
	{"InkNames", 333, NULL},
	{"NumberOfInks", 334, NULL},
	{"DotRange", 336, NULL},
	{"TargetPrinter", 337, NULL},
	{"ExtraSamples", 338, (struct tiff_value[]) {
		{"Unspecified", 0},
		{"Associated alpha", 1},
		{"Unassociated alpha", 2},
		{}
	}},
	{"SampleFormat", 339, (struct tiff_value[]) {
		{"Unsigned integer", 1},
		{"Two's complement signed integer", 2},
		{"IEEE floating-point", 3},
		{"Undefined", 4},
		{}
	}},
	{"SMinSampleValue", 340, NULL},
	{"SMaxSampleValue", 341, NULL},
	{"TransferRange", 342, NULL},
	{"ClipPath", 343, NULL},  // TIFF Technical Note 2: Clipping Path
	{"XClipPathUnits", 344, NULL},  // TIFF Technical Note 2: Clipping Path
	{"YClipPathUnits", 345, NULL},  // TIFF Technical Note 2: Clipping Path
	{"Indexed", 346, NULL},  // TIFF Technical Note 3: Indexed Images
	{"OPIProxy", 351, NULL},  // Adobe PageMaker 6.0 TIFF Technical Notes
	{"JPEGProc", 512, (struct tiff_value[]) {
		{"Baseline sequential", 1},
		{"Lossless Huffman", 14},
		{}
	}},
	{"JPEGInterchangeFormat", 513, NULL},
	{"JPEGInterchangeFormatLength", 514, NULL},
	{"JPEGRestartInterval", 515, NULL},
	{"JPEGLosslessPredictors", 517, (struct tiff_value[]) {
		{"A", 1},
		{"B", 2},
		{"C", 3},
		{"A+B+C", 4},
		{"A+((B-C)/2)", 5},
		{"B+((A-C)/2)", 6},
		{"(A+B)/2", 7},
		{}
	}},
	{"JPEGPointTransforms", 518, NULL},
	{"JPEGQTables", 519, NULL},
	{"JPEGDCTables", 520, NULL},
	{"JPEGACTables", 521, NULL},
	{"YCbCrCoefficients", 529, NULL},
	{"YCbCrSubSampling", 530, NULL},
	{"YCbCrPositioning", 531, (struct tiff_value[]) {
		{"Centered", 1},
		{"Cosited", 2},
		{}
	}},
	{"ReferenceBlackWhite", 532, NULL},
	{"ImageID", 32781, NULL},  // Adobe PageMaker 6.0 TIFF Technical Notes
	{"Copyright", 33432, NULL},
	{"Exif IFD Pointer", 34665, NULL},  // Exif 2.3
	{"GPS Info IFD Pointer", 34853, NULL},  // Exif 2.3
	{"Interoperability IFD Pointer", 40965, NULL},  // Exif 2.3
	{}
};

// TODO(p): Consider if these can't be inlined into the above table.
static uint16_t tiff_subifd_tags[] = {
	330,    // SubIFDs
	34665,  // Exif IFD Pointer
	34853,  // GPS Info IFD Pointer
	40965,  // Interoperability IFD Pointer
	0
};

// TODO(p): Insert tags and values from other documentation,
// so far only tags and non-bit-field values from TIFF 6.0 and PM6 are present.
//
// TODO(p): Exif 2.3 4.6.5 and on.
// TODO(p): Exif 2.3 4.6.6 and on (note it starts at 0).
// TODO(p): Exif 2.3 4.6.7 and on (note it starts at 1, and collides with GPS).

// --- Analysis ----------------------------------------------------------------

static jv
add_to_subarray(jv o, const char *key, jv value)
{
	// Invalid values are not allocated, and we use up any valid one.
	// Beware that jv_get() returns jv_null() rather than jv_invalid().
	// Also, the header comment is lying, jv_is_valid() doesn't unreference.
	jv a = jv_object_get(jv_copy(o), jv_string(key));
	return jv_set(o, jv_string(key),
		jv_is_valid(a) ? jv_array_append(a, value) : JV_ARRAY(value));
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

// --- Exif --------------------------------------------------------------------

static jv parse_exif_ifd(struct tiffer *T);

static jv
parse_exif_subifds(struct tiffer *T, const struct tiffer_entry *entry)
{
	int64_t offset = 0;
	struct tiffer subT = {};
	if (!tiffer_integer(T, entry, &offset) ||
		offset < 0 || offset > UINT32_MAX || !tiffer_subifd(T, offset, &subT))
		return jv_null();

	// The chain should correspond to the values in the entry,
	// we are not going to verify it.
	jv a = jv_array();
	do a = jv_array_append(a, parse_exif_ifd(&subT));
	while (tiffer_next_ifd(&subT));
	return a;
}

static jv
parse_exif_ascii(struct tiffer_entry *entry)
{
	// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 2.4.2
	// The text may in practice contain any 8-bit encoding, but likely UTF-8.
	// TODO(p): Validate UTF-8, and assume Latin 1 if unsuccessful.
	jv a = jv_array();
	uint8_t *nul = 0;
	while ((nul = memchr(entry->p, 0, entry->remaining_count))) {
		size_t len = nul - entry->p;
		a = jv_array_append(a, jv_string_sized((const char *) entry->p, len));
		entry->remaining_count -= len + 1;
		entry->p += len + 1;
	}

	// Trailing NULs are required, but let's extract everything.
	if (entry->remaining_count) {
		a = jv_array_append(a,
			jv_string_sized((const char *) entry->p, entry->remaining_count));
	}
	return a;
}

static jv
parse_exif_undefined(struct tiffer_entry *entry)
{
	const char *alphabet = "0123456789abcdef";
	char *buf = calloc(1, 2 * entry->remaining_count + 1);
	for (uint32_t i = 0; i < entry->remaining_count; i++) {
		buf[2 * i + 0] = alphabet[entry->p[i] >> 4];
		buf[2 * i + 1] = alphabet[entry->p[i] & 0xF];
	}
	jv s = jv_string(buf);
	free(buf);
	return s;
}

static jv
parse_exif_value(const struct tiff_value *values, double real)
{
	if (values) {
		for (; values->name; values++)
			if (values->value == real)
				return jv_string(values->name);
	}
	return jv_number(real);
}
static jv
parse_exif_extract_sole_array_element(jv a)
{
	return jv_array_length(jv_copy(a)) == 1 ? jv_array_get(a, 0) : a;
}

static jv
parse_exif_entry(jv o, struct tiffer *T, struct tiffer_entry *entry)
{
	const struct tiff_entry *info = tiff_entries;
	for (; info->name; info++)
		if (info->tag == entry->tag)
			break;

	bool is_subifd = false;
	for (const uint16_t *p = tiff_subifd_tags; *p; p++)
		is_subifd |= *p == entry->tag;

	jv v = jv_true();
	double real = 0;
	if (!entry->remaining_count) {
		v = jv_null();
	} else if (entry->type == IFD || is_subifd) {
		v = parse_exif_subifds(T, entry);
	} else if (entry->type == ASCII) {
		v = parse_exif_extract_sole_array_element(parse_exif_ascii(entry));
	} else if (entry->type == UNDEFINED) {
		v = parse_exif_undefined(entry);
	} else if (tiffer_real(T, entry, &real)) {
		v = jv_array();
		do v = jv_array_append(v, parse_exif_value(info->values, real));
		while (tiffer_next_value(entry) && tiffer_real(T, entry, &real));
		v = parse_exif_extract_sole_array_element(v);
	}

	if (info->name)
		return jv_set(o, jv_string(info->name), v);
	return jv_set(o, jv_string_fmt("%u", entry->tag), v);
}

static jv
parse_exif_ifd(struct tiffer *T)
{
	jv ifd = jv_object();
	struct tiffer_entry entry = {};
	while (tiffer_next_entry(T, &entry))
		ifd = parse_exif_entry(ifd, T, &entry);
	return ifd;
}

static jv
parse_exif(jv o, const uint8_t *p, size_t len)
{
	struct tiffer T = {};
	if (!tiffer_init(&T, p, len))
		return add_warning(o, "invalid Exif");
	while (tiffer_next_ifd(&T))
		o = add_to_subarray(o, "Exif", parse_exif_ifd(&T));
	return o;
}

// --- ICC profiles ------------------------------------------------------------
// v2 https://www.color.org/ICC_Minor_Revision_for_Web.pdf
// v4 https://www.color.org/specification/ICC1v43_2010-12.pdf

static jv
parse_icc_mluc(jv o, const uint8_t *tag, uint32_t tag_length)
{
	// v4 10.13
	if (tag_length < 16)
		return add_warning(o, "invalid ICC 'mluc' structure length");

	uint32_t count = u32be(tag + 8);
	if (count == 0)
		return add_warning(o, "unnamed ICC profile");

	// There is no particularly good reason for us to iterate, take the first.
	const uint8_t *record = tag + 16 /* + i * u32be(tag + 12) */;
	uint32_t len = u32be(&record[4]);
	uint32_t off = u32be(&record[8]);

	if (off + len > tag_length)
		return add_warning(o, "invalid ICC 'mluc' structure record");

	// Blindly assume simple ASCII, ensure NUL-termination.
	char name[len], *p = name;
	for (uint32_t i = 0; i < len / 2; i++)
		*p++ = tag[off + i * 2 + 1];
	*p++ = 0;
	return jv_set(o, jv_string("ICC"),
		JV_OBJECT(jv_string("name"), jv_string(name),
			jv_string("version"), jv_number(4)));
}

static jv
parse_icc_desc(jv o, const uint8_t *profile, size_t profile_len,
	uint32_t tag_offset, uint32_t tag_length)
{
	const uint8_t *tag = profile + tag_offset;
	if (tag_offset + tag_length > profile_len)
		return add_warning(o, "unexpected end of ICC profile");
	if (tag_length < 4)
		return add_warning(o, "invalid ICC tag structure length");

	// v2 6.5.17
	uint32_t sig = u32be(tag);
	if (sig == 0x6D6C7563 /* mluc */)
		return parse_icc_mluc(o, profile + tag_offset, tag_length);
	if (sig != 0x64657363 /* desc */)
		return add_warning(o, "invalid ICC 'desc' structure signature");
	if (tag_length < 12)
		return add_warning(o, "invalid ICC 'desc' structure length");

	uint32_t count = u32be(tag + 8);
	if (tag_length < 12 + count)
		return add_warning(o, "invalid ICC 'desc' structure length");

	// Double-ensure a trailing NUL byte.
	char name[count + 1];
	memcpy(name, tag + 12, count);
	name[count] = 0;
	return jv_set(o, jv_string("ICC"),
		JV_OBJECT(jv_string("name"), jv_string(name),
			jv_string("version"), jv_number(2)));
}

static jv
parse_icc(jv o, const uint8_t *profile, size_t profile_len)
{
	// v2 6, v4 7
	if (profile_len < 132)
		return add_warning(o, "ICC profile too short");
	if (u32be(profile) != profile_len)
		return add_warning(o, "ICC profile size mismatch");

	// TODO(p): May decode more of the header fields, and validate them.
	// Need to check both v2 and v4, this is all fairly annoying.
	uint32_t count = u32be(profile + 128);
	if (132 + count * 12 > profile_len)
		return add_warning(o, "unexpected end of ICC profile");

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *entry = profile + 132 + i * 12;
		uint32_t sig = u32be(&entry[0]);
		uint32_t off = u32be(&entry[4]);
		uint32_t len = u32be(&entry[8]);

		// v2 6.4.32, v4 9.2.41
		if (sig == 0x64657363 /* desc */)
			return parse_icc_desc(o, profile, profile_len, off, len);
	}
	// The description is required, so this should be unreachable.
	return jv_set(o, jv_string("ICC"), jv_bool(true));
}

// --- Photoshop Image Resources -----------------------------------------------
// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3 + 3.1.3
// https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/

static jv
process_psir(jv o, uint16_t resource_id, const char *name,
	const uint8_t *data, size_t len)
{
	// TODO(p): These is more to extract here. The name is most often empty.
	(void) name;
	(void) data;
	(void) len;
	return add_to_subarray(o, "PSIR", jv_number(resource_id));
}

static jv
parse_psir_block(jv o, const uint8_t *p, size_t len, size_t *advance)
{
	*advance = 0;
	if (len < 8 || memcmp(p, "8BIM", 4))
		return add_warning(o, "bad PSIR block header");

	uint16_t resource_id = u16be(p + 4);
	uint8_t name_len = p[6];
	const uint8_t *name = &p[7];

	// Add one byte for the Pascal-ish string length prefix,
	// then another one for padding to make the length even.
	size_t name_len_full = (name_len + 2) & ~1U;

	size_t resource_len_offset = 6 + name_len_full,
		header_len = resource_len_offset + 4;
	if (len < header_len)
		return add_warning(o, "bad PSIR block header");

	uint32_t resource_len = u32be(p + resource_len_offset);
	size_t resource_len_padded = (resource_len + 1) & ~1U;
	if (resource_len_padded < resource_len ||
		len < header_len + resource_len_padded)
		return add_warning(o, "runaway PSIR block");

	char *cname = calloc(1, name_len_full);
	strncpy(cname, (const char *) name, name_len);
	o = process_psir(o, resource_id, cname, p + header_len, resource_len);
	free(cname);

	*advance = header_len + resource_len_padded;
	return o;
}

static jv
parse_psir(jv o, const uint8_t *p, size_t len)
{
	if (len == 0)
		return add_warning(o, "empty PSIR data");

	size_t advance = 0;
	while (len && (o = parse_psir_block(o, p, len, &advance), advance)) {
		p += advance;
		len -= advance;
	}
	return o;
}

// --- JPEG --------------------------------------------------------------------
// Because the JPEG file format is simple, just do it manually.
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

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct data {
	bool ended;
	uint8_t *exif, *icc, *psir;
	size_t exif_len, icc_len, psir_len;
	int icc_sequence, icc_done;
	const uint8_t **mpf_offsets, **mpf_next;
};

enum {
	MPFVersion = 45056,
	NumberOfImages = 45057,
	MPEntry = 45058,
	ImageUIDList = 45059,
	TotalFrames = 45060,
};

static jv
parse_mpf_index_entry(
	jv o, struct data *data, struct tiffer *T, struct tiffer_entry *entry)
{
	// 5.2.3.3. MP Entry
	if (entry->tag != MPEntry || entry->type != UNDEFINED ||
		entry->remaining_count % 16) {
		// TODO(p): Parse the remaining special tags instead.
		return parse_exif_entry(o, T, entry);
	}

	uint32_t count = entry->remaining_count / 16;
	jv a = jv_array_sized(count);

	const uint8_t **out = data->mpf_next = data->mpf_offsets =
		calloc(sizeof *data->mpf_offsets, count + 1);
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *p = entry->p + i * 16;
		uint32_t attrs = T->un->u32(p);
		uint32_t size = T->un->u32(p + 4);
		uint32_t offset = T->un->u32(p + 8);
		uint16_t dependent1 = T->un->u16(p + 12);
		uint16_t dependent2 = T->un->u16(p + 14);

		uint32_t type_number = attrs & 0xFFFFFF;
		jv type = jv_number(type_number);
		switch (type_number) {
		break; case 0x030000: type = jv_string("Baseline MP Primary Image");
		break; case 0x010001: type = jv_string("Large Thumbnail VGA");
		break; case 0x010002: type = jv_string("Large Thumbnail Full HD");
		break; case 0x020001: type = jv_string("Multi-Frame Image Panorama");
		break; case 0x020002: type = jv_string("Multi-Frame Image Disparity");
		break; case 0x020003: type = jv_string("Multi-Frame Image Multi-Angle");
		break; case 0x000000: type = jv_string("Undefined");
		}

		uint32_t format_number = (attrs >> 24) & 0x7;
		jv format = jv_number(format_number);
		if (format_number == 0)
			format = jv_string("JPEG");

		a = jv_array_append(a, JV_OBJECT(
			jv_string("Individual Image Attribute"), JV_OBJECT(
				jv_string("Dependent Parent Image"), jv_bool((attrs >> 31) & 1),
				jv_string("Dependent Child Image"), jv_bool((attrs >> 30) & 1),
				jv_string("Representative Image"), jv_bool((attrs >> 29) & 1),
				jv_string("Reserved"), jv_number((attrs >> 27) & 0x3),
				jv_string("Image Data Format"), format,
				jv_string("MP Type Code"), type
			),
			jv_string("Individual Image Size"), jv_number(size),
			jv_string("Individual Image Data Offset"), jv_number(offset),
			jv_string("Dependent Image 1 Entry Number"), jv_number(dependent1),
			jv_string("Dependent Image 2 Entry Number"), jv_number(dependent2)
		));
		if (offset)
			*out++ = T->begin + offset;
	}
	return jv_set(o, jv_string("MPEntry"), a);
}

static jv
parse_mpf_index_ifd(struct data *data, struct tiffer *T)
{
	jv ifd = jv_object();
	struct tiffer_entry entry = {};
	while (tiffer_next_entry(T, &entry))
		ifd = parse_mpf_index_entry(ifd, data, T, &entry);
	return ifd;
}

static jv
parse_mpf_attribute_ifd(struct tiffer *T)
{
	// TODO(p): Parse the special tags instead.
	return parse_exif_ifd(T);
}

static jv
parse_mpf(jv o, struct data *data, const uint8_t *p, size_t len)
{
	struct tiffer T;
	if (!tiffer_init(&T, p, len) || !tiffer_next_ifd(&T))
		return add_warning(o, "invalid MPF segment");

	// First image: IFD0 is Index IFD, any IFD1 is Attribute IFD.
	// Other images: IFD0 is Attribute IFD, there is no Index IFD.
	if (!data->mpf_offsets) {
		o = add_to_subarray(o, "MPF", parse_mpf_index_ifd(data, &T));
		if (!tiffer_next_ifd(&T))
			return o;
	}
	return add_to_subarray(o, "MPF", parse_mpf_attribute_ifd(&T));
}

static void
parse_append(uint8_t **buffer, size_t *buffer_len, const uint8_t *p, size_t len)
{
	size_t buffer_longer = *buffer_len + len;
	*buffer = realloc(*buffer, buffer_longer);
	memcpy(*buffer + *buffer_len, p, len);
	*buffer_len = buffer_longer;
}

static const uint8_t *
parse_marker(uint8_t marker, const uint8_t *p, const uint8_t *end,
	struct data *data, jv *o)
{
	// Suspected: MJPEG? Undetected format recursion, e.g., thumbnails?
	// Found: Random metadata! Multi-Picture Format!
	if ((data->ended = marker == EOI)) {
		// TODO(p): Handle Exifs independently--flush the last one.
		if (data->mpf_offsets && *data->mpf_next)
			return *data->mpf_next++;
		if (p != end)
			*o = add_warning(*o, "trailing data");
	}

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
			jv_string("type"), jv_string(marker_descriptions[marker]),
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

	// CIPA DC-007 (Multi-Picture Format) 5.2
	// http://fileformats.archiveteam.org/wiki/Multi-Picture_Format
	if (marker == APP2 && p - payload >= 8 && !memcmp(payload, "MPF\0", 4)) {
		payload += 4;
		*o = parse_mpf(*o, data, payload, p - payload);
	}

	// CIPA DC-006 (Stereo Still Image Format for Digital Cameras)
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
	// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3
	if (marker == APP1 && p - payload >= 6 && !memcmp(payload, "Exif\0", 5)) {
		payload += 6;
		if (payload[-1] != 0)
			*o = add_warning(*o, "weirdly padded Exif header");
		if (data->exif)
			*o = add_warning(*o, "multiple Exif segments");
		parse_append(&data->exif, &data->exif_len, payload, p - payload);
	}

	// https://www.color.org/specification/ICC1v43_2010-12.pdf B.4
	if (marker == APP2 && p - payload >= 14 &&
		!memcmp(payload, "ICC_PROFILE\0", 12) && !data->icc_done &&
		payload[12] == ++data->icc_sequence && payload[13] >= payload[12]) {
		payload += 14;
		parse_append(&data->icc, &data->icc_len, payload, p - payload);
		data->icc_done = payload[-1] == data->icc_sequence;
	}

	// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3 + 3.1.3
	// https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
	if (marker == APP13 && p - payload >= 14 &&
		!memcmp(payload, "Photoshop 3.0\0", 14)) {
		payload += 14;
		parse_append(&data->psir, &data->psir_len, payload, p - payload);
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
		o = parse_exif(o, data.exif, data.exif_len);
		free(data.exif);
	}
	if (data.icc) {
		if (data.icc_done)
			o = parse_icc(o, data.icc, data.icc_len);
		else
			o = add_warning(o, "bad ICC profile sequence");
		free(data.icc);
	}
	if (data.psir) {
		o = parse_psir(o, data.psir, data.psir_len);
		free(data.psir);
	}

	free(data.mpf_offsets);
	return jv_set(o, jv_string("markers"), markers);
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

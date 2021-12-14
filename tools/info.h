//
// info.h: metadata extraction utilities
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

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// --- Utilities ---------------------------------------------------------------

static char *
binhex(const uint8_t *data, size_t len)
{
	static const char *alphabet = "0123456789abcdef";
	char *buf = calloc(1, len * 2 + 1), *p = buf;
	for (size_t i = 0; i < len; i++) {
		*p++ = alphabet[data[i] >> 4];
		*p++ = alphabet[data[i] & 0xF];
	}
	return buf;
}

static uint64_t
u64be(const uint8_t *p)
{
	return (uint64_t) p[0] << 56 | (uint64_t) p[1] << 48 |
		(uint64_t) p[2] << 40 | (uint64_t) p[3] << 32 |
		(uint64_t) p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
}

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

static uint64_t
u64le(const uint8_t *p)
{
	return (uint64_t) p[7] << 56 | (uint64_t) p[6] << 48 |
		(uint64_t) p[5] << 40 | (uint64_t) p[4] << 32 |
		(uint64_t) p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0];
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
// TIFF Revision 6.0 (1992)
// https://www.adobe.io/content/dam/udp/en/open/standards/tiff/TIFF6.pdf
//
// TIFF Technical Note 1: TIFF Trees (1993)
// https://download.osgeo.org/libtiff/old/TTN1.ps
//
// DRAFT TIFF Technical Note 2 (1995)
// https://www.awaresystems.be/imaging/tiff/specification/TIFFTechNote2.txt
//
// Adobe PageMaker 6.0 TIFF Technical Notes (1995) [includes TTN1]
// https://www.adobe.io/content/dam/udp/en/open/standards/tiff/TIFFPM6.pdf
//
// Adobe Photoshop TIFF Technical Notes (2002)
// https://www.adobe.io/content/dam/udp/en/open/standards/tiff/TIFFphotoshop.pdf
//  - Note that ImageSourceData 8BIM frames are specified differently
//    from how Adobe XMP Specification Part 3 defines them.
//  - The document places a condition on SubIFDs, without further explanation.
//
// Adobe Photoshop TIFF Technical Note 3 (2005)
// http://chriscox.org/TIFFTN3d1.pdf
//
// Exif Version 2.3 (2012)
// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf
//
// Exif Version 2.32 (2019)
// https://www.cipa.jp/e/std/std-sec.html
//
// Digital Negative (DNG) Specification 1.5.0.0 (2019)
// https://www.adobe.com/content/dam/acom/en/products/photoshop/pdfs
// /dng_spec_1.5.0.0.pdf
//
// libtiff is a mess, and the format is not particularly complicated.
// Exiv2 is senselessly copylefted, and cannot do much.
// libexif is only marginally better.
// ExifTool is too user-oriented.

static struct un {
	uint64_t (*u64) (const uint8_t *);
	uint32_t (*u32) (const uint8_t *);
	uint16_t (*u16) (const uint8_t *);
} unbe = {u64be, u32be, u16be}, unle = {u64le, u32le, u16le};

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
	// The first IFD needs to be read by caller explicitly,
	// even though it's required to be present by TIFF 6.0.
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

	// Note that TIFF 6.0 requires there to be at least one entry,
	// but there is no need for us to check it.
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
	// TIFF 6.0 only directly suggests that a reader is should accept
	// any of BYTE/SHORT/LONG for unsigned integers.
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
	// Assuming the host architecture uses IEEE 754.
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

// --- TIFF/Exif tags ----------------------------------------------------------

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
		{"JPEG datastream", 7},  // DRAFT TIFF Technical Note 2 + TIFFphotoshop
		{"Deflate/zlib", 8},  // Adobe Photoshop TIFF Technical Notes
		{"PackBits", 32773},
		{"Deflate/zlib", 32946},  // Adobe Photoshop TIFF Technical Notes
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
		{"ICCLab", 9},  // Adobe PageMaker 6.0 TIFF Technical Notes
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
		{"Floating point", 3},  // Adobe Photoshop TIFF Technical Note 3
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
	{"JPEGTables", 347, NULL},  // DRAFT TIFF Technical Note 2 + TIFFphotoshop
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
		{"Co-sited", 2},
		{}
	}},
	{"ReferenceBlackWhite", 532, NULL},
	{"XMP", 700, NULL},  // Adobe XMP Specification Part 3 Table 12/13/39
	{"ImageID", 32781, NULL},  // Adobe PageMaker 6.0 TIFF Technical Notes
	{"Copyright", 33432, NULL},
	// TODO(p): Extract IPTC DataSets, like we do directly with PSIRs.
	{"IPTC", 33723, NULL},  // Adobe XMP Specification Part 3 Table 12/39
	// TODO(p): Extract PSIRs, like we do directly with the JPEG segment.
	{"Photoshop", 34377, NULL},  // Adobe XMP Specification Part 3 Table 12/39
	{"Exif IFD Pointer", 34665, NULL},  // Exif 2.3
	{"GPS Info IFD Pointer", 34853, NULL},  // Exif 2.3
	{"TIFF/EP StandardID", 37398, NULL},  // ISO 12234 TIFF/EP image data format
	{"ImageSourceData", 37724, NULL},  // Adobe Photoshop TIFF Technical Notes
	{"DNGVersion", 50706, NULL},  // DNG 1.5.0.0
	{"DNGBackwardVersion", 50707, NULL},  // DNG 1.5.0.0
	{"UniqueCameraModel", 50708, NULL},  // DNG 1.5.0.0
	{"LocalizedCameraModel", 50709, NULL},  // DNG 1.5.0.0
	// TODO(p): Add more DNG tags that can be only in IFD0.
	{}
};

// Exif 2.3 4.6.5
static struct tiff_entry exif_entries[] = {
	{"ExposureTime", 33434, NULL},
	{"FNumber", 33437, NULL},
	{"ExposureProgram", 34850, (struct tiff_value[]) {
		{"Not defined", 0},
		{"Manual", 1},
		{"Normal program", 2},
		{"Aperture priority", 3},
		{"Shutter priority", 4},
		{"Creative program", 5},
		{"Action program", 6},
		{"Portrait mode", 7},
		{"Landscape mode", 8},
		{}
	}},
	{"SpectralSensitivity", 34852, NULL},
	{"PhotographicSensitivity", 34855, NULL},
	{"OECF", 34856, NULL},
	{"SensitivityType", 34864, (struct tiff_value[]) {
		{"Unknown", 0},
		{"Standard output sensitivity", 1},
		{"Recommended exposure index", 2},
		{"ISO speed", 3},
		{"SOS and REI", 4},
		{"SOS and ISO speed", 5},
		{"REI and ISO speed", 6},
		{"SOS and REI and ISO speed", 7},
		{}
	}},
	{"StandardOutputSensitivity", 34865, NULL},
	{"RecommendedExposureIndex", 34866, NULL},
	{"ISOSpeed", 34867, NULL},
	{"ISOSpeedLatitudeyyy", 34868, NULL},
	{"ISOSpeedLatitudezzz", 34869, NULL},
	{"ExifVersion", 36864, NULL},
	{"DateTimeOriginal", 36867, NULL},
	{"DateTimeDigitized", 36868, NULL},
	{"OffsetTime", 36880, NULL},  // 2.31
	{"OffsetTimeOriginal", 36881, NULL},  // 2.31
	{"OffsetTimeDigitized", 36882, NULL},  // 2.31
	{"ComponentsConfiguration", 37121, (struct tiff_value[]) {
		{"Does not exist", 0},
		{"Y", 1},
		{"Cb", 2},
		{"Cr", 3},
		{"R", 4},
		{"G", 5},
		{"B", 6},
		{}
	}},
	{"CompressedBitsPerPixel", 37122, NULL},
	{"ShutterSpeedValue", 37377, NULL},
	{"ApertureValue", 37378, NULL},
	{"BrightnessValue", 37379, NULL},
	{"ExposureBiasValue", 37380, NULL},
	{"MaxApertureValue", 37381, NULL},
	{"SubjectDistance", 37382, NULL},
	{"MeteringMode", 37383, (struct tiff_value[]) {
		{"Unknown", 0},
		{"Average", 1},
		{"CenterWeightedAverage", 2},
		{"Spot", 3},
		{"MultiSpot", 4},
		{"Pattern", 5},
		{"Partial", 6},
		{"Other", 255},
		{}
	}},
	{"LightSource", 37384, (struct tiff_value[]) {
		{"Unknown", 0},
		{"Daylight", 1},
		{"Fluorescent", 2},
		{"Tungsten (incandescent light)", 3},
		{"Flash", 4},
		{"Fine weather", 9},
		{"Cloudy weather", 10},
		{"Shade", 11},
		{"Daylight fluorescent (D 5700 - 7100K)", 12},
		{"Day white fluorescent (N 4600 - 5500K)", 13},
		{"Cool white fluorescent (W 3800 - 4500K)", 14},
		{"White fluorescent (WW 3250 - 3800K)", 15},
		{"Warm white fluorescent (L 2600 - 3250K)", 16},
		{"Standard light A", 17},
		{"Standard light B", 18},
		{"Standard light C", 19},
		{"D55", 20},
		{"D65", 21},
		{"D75", 22},
		{"D50", 23},
		{"ISO studio tungsten", 24},
		{"Other light source", 255},
		{}
	}},
	{"Flash", 37385, NULL},
	{"FocalLength", 37386, NULL},
	{"SubjectArea", 37396, NULL},
	{"MakerNote", 37500, NULL},
	// TODO(p): Decode.
	{"UserComment", 37510, NULL},
	{"SubSecTime", 37520, NULL},
	{"SubSecTimeOriginal", 37521, NULL},
	{"SubSecTimeDigitized", 37522, NULL},
	{"Temperature", 37888, NULL},  // 2.31
	{"Humidity", 37889, NULL},  // 2.31
	{"Pressure", 37890, NULL},  // 2.31
	{"WaterDepth", 37891, NULL},  // 2.31
	{"Acceleration", 37892, NULL},  // 2.31
	{"CameraElevationAngle", 37893, NULL},  // 2.31
	{"FlashpixVersion", 40960, NULL},
	{"ColorSpace", 40961, (struct tiff_value[]) {
		{"sRGB", 1},
		{"Uncalibrated", 0xFFFF},
		{}
	}},
	{"PixelXDimension", 40962, NULL},
	{"PixelYDimension", 40963, NULL},
	{"RelatedSoundFile", 40964, NULL},
	{"Interoperability IFD Pointer", 40965, NULL},
	{"FlashEnergy", 41483, NULL},
	{"SpatialFrequencyResponse", 41484, NULL},
	{"FocalPlaneXResolution", 41486, NULL},
	{"FocalPlaneYResolution", 41487, NULL},
	{"FocalPlaneResolutionUnit", 41488, NULL},
	{"SubjectLocation", 41492, NULL},
	{"ExposureIndex", 41493, NULL},
	{"SensingMethod", 41495, (struct tiff_value[]) {
		{"Not defined", 1},
		{"One-chip color area sensor", 2},
		{"Two-chip color area sensor", 3},
		{"Three-chip color area sensor", 4},
		{"Color sequential area sensor", 5},
		{"Trilinear sensor", 7},
		{"Color sequential linear sensor", 8},
		{}
	}},
	{"FileSource", 41728, (struct tiff_value[]) {
		{"Others", 0},
		{"Scanner of transparent type", 1},
		{"Scanner of reflex type", 2},
		{"DSC", 3},
		{}
	}},
	{"SceneType", 41729, (struct tiff_value[]) {
		{"Directly-photographed image", 1},
		{}
	}},
	{"CFAPattern", 41730, NULL},
	{"CustomRendered", 41985, (struct tiff_value[]) {
		{"Normal process", 0},
		{"Custom process", 1},
		{}
	}},
	{"ExposureMode", 41986, (struct tiff_value[]) {
		{"Auto exposure", 0},
		{"Manual exposure", 1},
		{"Auto bracket", 2},
		{}
	}},
	{"WhiteBalance", 41987, (struct tiff_value[]) {
		{"Auto white balance", 0},
		{"Manual white balance", 1},
		{}
	}},
	{"DigitalZoomRatio", 41988, NULL},
	{"FocalLengthIn35mmFilm", 41989, NULL},
	{"SceneCaptureType", 41990, (struct tiff_value[]) {
		{"Standard", 0},
		{"Landscape", 1},
		{"Portrait", 2},
		{"Night scene", 3},
		{}
	}},
	{"GainControl", 41991, (struct tiff_value[]) {
		{"None", 0},
		{"Low gain up", 1},
		{"High gain up", 2},
		{"Low gain down", 3},
		{"High gain down", 4},
		{}
	}},
	{"Contrast", 41992, (struct tiff_value[]) {
		{"Normal", 0},
		{"Soft", 1},
		{"Hard", 2},
		{}
	}},
	{"Saturation", 41993, (struct tiff_value[]) {
		{"Normal", 0},
		{"Low", 1},
		{"High", 2},
		{}
	}},
	{"Sharpness", 41994, (struct tiff_value[]) {
		{"Normal", 0},
		{"Soft", 1},
		{"Hard", 2},
		{}
	}},
	{"DeviceSettingDescription", 41995, NULL},
	{"SubjectDistanceRange", 41996, (struct tiff_value[]) {
		{"Unknown", 0},
		{"Macro", 1},
		{"Close view", 2},
		{"Distant view", 3},
		{}
	}},
	{"ImageUniqueID", 42016, NULL},
	{"CameraOwnerName", 42032, NULL},
	{"BodySerialNumber", 42033, NULL},
	{"LensSpecification", 42034, NULL},
	{"LensMake", 42035, NULL},
	{"LensModel", 42036, NULL},
	{"LensSerialNumber", 42037, NULL},
	{"CompositeImage", 42080, NULL},  // 2.32
	{"SourceImageNumberOfCompositeImage", 42081, NULL},  // 2.32
	{"SourceExposureTimesOfCompositeImage", 42082, NULL},  // 2.32
	{"Gamma", 42240, NULL},
	{}
};

// Exif 2.3 4.6.6 (Notice it starts at 0.)
static struct tiff_entry exif_gps_entries[] = {
	{"GPSVersionID", 0, NULL},
	{"GPSLatitudeRef", 1, NULL},
	{"GPSLatitude", 2, NULL},
	{"GPSLongitudeRef", 3, NULL},
	{"GPSLongitude", 4, NULL},
	{"GPSAltitudeRef", 5, (struct tiff_value[]) {
		{"Sea level", 0},
		{"Sea level reference (negative value)", 1},
		{}
	}},
	{"GPSAltitude", 6, NULL},
	{"GPSTimeStamp", 7, NULL},
	{"GPSSatellites", 8, NULL},
	{"GPSStatus", 9, NULL},
	{"GPSMeasureMode", 10, NULL},
	{"GPSDOP", 11, NULL},
	{"GPSSpeedRef", 12, NULL},
	{"GPSSpeed", 13, NULL},
	{"GPSTrackRef", 14, NULL},
	{"GPSTrack", 15, NULL},
	{"GPSImgDirectionRef", 16, NULL},
	{"GPSImgDirection", 17, NULL},
	{"GPSMapDatum", 18, NULL},
	{"GPSDestLatitudeRef", 19, NULL},
	{"GPSDestLatitude", 20, NULL},
	{"GPSDestLongitudeRef", 21, NULL},
	{"GPSDestLongitude", 22, NULL},
	{"GPSDestBearingRef", 23, NULL},
	{"GPSDestBearing", 24, NULL},
	{"GPSDestDistanceRef", 25, NULL},
	{"GPSDestDistance", 26, NULL},
	{"GPSProcessingMethod", 27, NULL},
	{"GPSAreaInformation", 28, NULL},
	{"GPSDateStamp", 29, NULL},
	{"GPSDifferential", 30, (struct tiff_value[]) {
		{"Measurement without differential correction", 0},
		{"Differential correction applied", 1},
		{}
	}},
	{"GPSHPositioningError", 31, NULL},
	{}
};

// Exif 2.3 4.6.7 (Notice it starts at 1, and collides with GPS.)
static struct tiff_entry exif_interop_entries[] = {
	{"InteroperabilityIndex", 1, NULL},
	{}
};

// TODO(p): Consider if these can't be inlined into `tiff_entries`.
static struct {
	uint16_t tag;
	struct tiff_entry *entries;
} tiff_subifds[] = {
	{330, tiff_entries},   // SubIFDs
	{34665, exif_entries},  // Exif IFD Pointer
	{34853, exif_gps_entries},  // GPS Info IFD Pointer
	{40965, exif_interop_entries},  // Interoperability IFD Pointer
	{}
};

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

static jv parse_exif_ifd(struct tiffer *T, const struct tiff_entry *info);

static jv
parse_exif_subifds(struct tiffer *T, const struct tiffer_entry *entry,
	struct tiff_entry *info)
{
	int64_t offset = 0;
	struct tiffer subT = {};
	if (!tiffer_integer(T, entry, &offset) ||
		offset < 0 || offset > UINT32_MAX || !tiffer_subifd(T, offset, &subT))
		return jv_null();

	// The chain should correspond to the values in the entry
	// (TIFF Technical Note 1), we are not going to verify it.
	// Note that Nikon NEFs do not follow this rule.
	jv a = jv_array();
	do a = jv_array_append(a, parse_exif_ifd(&subT, info));
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
	// Sometimes, it can be ASCII, but the safe bet is to hex-encode it.
	char *buf = binhex(entry->p, entry->remaining_count);
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
parse_exif_entry(jv o, struct tiffer *T, struct tiffer_entry *entry,
	const struct tiff_entry *info)
{
	if (!info)
		info = (struct tiff_entry[]) {{}};

	for (; info->name; info++)
		if (info->tag == entry->tag)
			break;

	struct tiff_entry *subentries = NULL;
	for (size_t i = 0; tiff_subifds[i].tag; i++)
		if (tiff_subifds[i].tag == entry->tag)
			subentries = tiff_subifds[i].entries;

	jv v = jv_true();
	double real = 0;
	if (!entry->remaining_count) {
		v = jv_null();
	} else if (entry->type == IFD || subentries) {
		v = parse_exif_subifds(T, entry, subentries);
	} else if (entry->type == ASCII) {
		v = parse_exif_extract_sole_array_element(parse_exif_ascii(entry));
	} else if (entry->type == UNDEFINED && !info->values) {
		// Several Exif entries of UNDEFINED type contain single-byte numbers.
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
parse_exif_ifd(struct tiffer *T, const struct tiff_entry *info)
{
	jv ifd = jv_object();
	struct tiffer_entry entry = {};
	while (tiffer_next_entry(T, &entry))
		ifd = parse_exif_entry(ifd, T, &entry, info);
	return ifd;
}

static jv
parse_exif(jv o, const uint8_t *p, size_t len)
{
	struct tiffer T = {};
	if (!tiffer_init(&T, p, len))
		return add_warning(o, "invalid Exif");
	while (tiffer_next_ifd(&T))
		o = add_to_subarray(o, "Exif", parse_exif_ifd(&T, tiff_entries));
	return o;
}

// --- Photoshop Image Resources -----------------------------------------------
// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3 + 3.1.3
// https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
// Unless otherwise noted, the descriptions are derived from the above document.

static struct {
	uint16_t id;
	const char *description;
} psir_descriptions[] = {
	{1000, "Number of channels, rows, columns, depth, mode"},
	{1001, "Macintosh print manager print info record"},
	{1002, "Macintosh page format information"},
	{1003, "Indexed color table"},
	{1005, "Resolution information"},
	{1006, "Names of alpha channels (Pascal strings)"},
	{1007, "Display information"},
	{1008, "Caption (Pascal string)"},  // XMP Part 3 3.3.3
	{1009, "Border information"},
	{1010, "Background color"},
	{1011, "Print flags"},
	{1012, "Grayscale and multichannel halftoning information"},
	{1013, "Color halftoning information"},
	{1014, "Duotone halftoning information"},
	{1015, "Grayscale and multichannel transfer function"},
	{1016, "Color transfer functions"},
	{1017, "Duotone transfer functions"},
	{1018, "Duotone image information"},
	{1019, "Effective B/W values for the dot range"},
	{1020, "Caption"},  // XMP Part 3 3.3.3
	{1021, "EPS options"},
	{1022, "Quick Mask information"},
	{1023, "(Obsolete)"},
	{1024, "Layer state information"},
	{1025, "Working path (not saved)"},
	{1026, "Layers group information"},
	{1027, "(Obsolete)"},
	{1028, "IPTC DataSets"},  // XMP Part 3 3.3.3
	{1029, "Image mode for raw format files"},
	{1030, "JPEG quality"},
	{1032, "Grid and guides information"},
	{1033, "Thumbnail resource"},
	{1034, "Copyright flag"},
	{1035, "Copyright information URL"},  // XMP Part 3 3.3.3
	{1036, "Thumbnail resource"},
	{1037, "Global lighting angle for effects layer"},
	{1038, "Color samplers information"},
	{1039, "ICC profile"},
	{1040, "Watermark"},
	{1041, "ICC untagged profile flag"},
	{1042, "Effects visible flag"},
	{1043, "Spot halftone"},
	{1044, "Document-specific IDs seed number"},
	{1045, "Unicode alpha names"},
	{1046, "Indexed color table count"},
	{1047, "Transparent color index"},
	{1049, "Global altitude"},
	{1050, "Slices"},
	{1051, "Workflow URL"},
	{1052, "Jump To XPEP"},
	{1053, "Alpha identifiers"},
	{1054, "URL list"},
	{1057, "Version info"},
	{1058, "Exif metadata 1"},
	{1059, "Exif metadata 3"},
	{1060, "XMP metadata"},
	{1061, "MD5 digest of IPTC data"},  // XMP Part 3 3.3.3
	{1062, "Print scale"},
	{1064, "Pixel aspect ratio"},
	{1065, "Layer comps"},
	{1066, "Alternate duotone colors"},
	{1067, "Alternate spot colors"},
	{1069, "Layer selection IDs"},
	{1070, "HDR toning information"},
	{1071, "Print info"},
	{1072, "Layer group(s) enabled ID"},
	{1073, "Color samplers"},
	{1074, "Measurement scale"},
	{1075, "Timeline information"},
	{1076, "Sheet disclosure"},
	{1077, "Display information to support floating point colors"},
	{1078, "Onion skins"},
	{1080, "Count information"},
	{1082, "Print information"},
	{1083, "Print style"},
	{1084, "Macintosh NSPrintInfo"},
	{1085, "Windows DEVMODE"},
	{1086, "Autosave file path"},
	{1087, "Autosave format"},
	{1088, "Path selection state"},
	// {2000-2997, "Saved paths"},
	{2999, "Name of clipping path"},
	{3000, "Origin path information"},
	// {4000-4999, "Plug-in resource"},
	{7000, "Image Ready variables"},
	{7001, "Image Ready data sets"},
	{7002, "Image Ready default selected state"},
	{7003, "Image Ready 7 rollover expanded state"},
	{7004, "Image Ready rollover expanded state"},
	{7005, "Image Ready save layer settings"},
	{7006, "Image Ready version"},
	{8000, "Lightroom workflow"},
	{10000, "Print flags"},
	{}
};

static jv
process_psir_thumbnail(jv res, const uint8_t *data, size_t len)
{
	uint32_t format_number   = u32be(data + 0);
	uint32_t compressed_size = u32be(data + 20);

	// TODO(p): Recurse into the thumbnail if it's a JPEG.
	jv format = jv_number(format_number);
	switch (format_number) {
	break; case 0: format = jv_string("kJpegRGB");
	break; case 1: format = jv_string("kRawRGB");
	}

	res = jv_object_merge(res, JV_OBJECT(
		jv_string("Format"),         format,
		jv_string("Width"),          jv_number(u32be(data + 4)),
		jv_string("Height"),         jv_number(u32be(data + 8)),
		jv_string("Stride"),         jv_number(u32be(data + 12)),
		jv_string("TotalSize"),      jv_number(u32be(data + 16)),
		jv_string("CompressedSize"), jv_number(compressed_size),
		jv_string("BitsPerPixel"),   jv_number(u16be(data + 24)),
		jv_string("Planes"),         jv_number(u16be(data + 26))
	));
	if (28 + compressed_size <= len) {
		char *buf = binhex(data + 28, compressed_size);
		res = jv_set(res, jv_string("Data"), jv_string(buf));
		free(buf);
	}
	return res;
}

static const char *
process_iptc_dataset(jv *a, const uint8_t **p, size_t len)
{
	const uint8_t *header = *p;
	if (len < 5)
		return "unexpected end of IPTC data";
	if (*header != 0x1c)
		return "invalid tag marker";

	uint8_t record = header[1];
	uint8_t dataset = header[2];
	uint16_t byte_count = header[3] << 8 | header[4];

	// TODO(p): Although highly unlikely to appear, we could decode it.
	if (byte_count & 0x8000)
		return "unsupported extended DataSet";
	if (len - 5 < byte_count)
		return "data overrun";

	char *buf = binhex(header + 5, byte_count);
	*p += 5 + byte_count;
	*a = jv_array_append(*a, JV_OBJECT(
		jv_string("DataSet"), jv_string_fmt("%u:%u", record, dataset),
		jv_string("Data"), jv_string(buf)
	));
	free(buf);
	return NULL;
}

static jv
process_psir_iptc(jv res, const uint8_t *data, size_t len)
{
	// https://iptc.org/standards/iim/
	// https://iptc.org/std/IIM/4.2/specification/IIMV4.2.pdf
	jv a = jv_array();
	const uint8_t *end = data + len;
	while (data < end) {
		const char *err = process_iptc_dataset(&a, &data, end - data);
		if (err) {
			a = jv_array_append(a, jv_string(err));
			break;
		}
	}
	return jv_set(res, jv_string("DataSets"), a);
}

static jv
process_psir(jv o, uint16_t resource_id, const char *name,
	const uint8_t *data, size_t len)
{
	const char *description = NULL;
	if (resource_id >= 2000 && resource_id <= 2997)
		description = "Saved paths";
	if (resource_id >= 4000 && resource_id <= 4999)
		description = "Plug-in resource";
	for (size_t i = 0; psir_descriptions[i].id; i++)
		if (psir_descriptions[i].id == resource_id)
			description = psir_descriptions[i].description;

	jv res = JV_OBJECT(
		jv_string("name"), jv_string(name),
		jv_string("id"), jv_number(resource_id),
		jv_string("description"),
			description ? jv_string(description) : jv_null(),
		jv_string("size"), jv_number(len)
	);

	// Both are thumbnails, older is BGR, newer is RGB.
	if ((resource_id == 1033 || resource_id == 1036) && len >= 28)
		res = process_psir_thumbnail(res, data, len);
	if (resource_id == 1028)
		res = process_psir_iptc(res, data, len);

	return add_to_subarray(o, "PSIR", res);
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

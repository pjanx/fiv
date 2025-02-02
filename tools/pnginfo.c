//
// pnginfo.c: acquire information about PNG files in JSON format
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

#include <png.h>
#include <jv.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>

// --- Utilities ---------------------------------------------------------------

#if defined __GNUC__
#define ATTRIBUTE_PRINTF(x, y) __attribute__((format (printf, x, y)))
#else // ! __GNUC__
#define ATTRIBUTE_PRINTF(x, y)
#endif // ! __GNUC__

static char *strfmt(const char *format, ...) ATTRIBUTE_PRINTF(1, 2);

static char *
strvfmt(const char *format, va_list ap)
{
	va_list aq;
	va_copy(aq, ap);
	int size = vsnprintf(NULL, 0, format, aq);
	va_end(aq);
	if (size < 0)
		return NULL;

	char buf[size + 1];
	size = vsnprintf(buf, sizeof buf, format, ap);
	if (size < 0)
		return NULL;

	return strdup(buf);
}

static char *
strfmt(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	char *result = strvfmt(format, ap);
	va_end(ap);
	return result;
}

static uint8_t *
hexbin(const char *string, size_t *len)
{
	static const char *alphabet = "0123456789abcdef";
	uint8_t *buf = calloc(1, strlen(string) / 2 + 1), *p = buf;
	while (true) {
		while (*string && strchr(" \t\n\r\v\f", *string))
			string++;
		if (!*string)
			break;

		const char *hi, *lo;
		if (!(hi = strchr(alphabet, tolower(*string++))) || !*string ||
			!(lo = strchr(alphabet, tolower(*string++)))) {
			free(buf);
			return NULL;
		}

		*p++ = (hi - alphabet) << 4 | (lo - alphabet);
	}
	*len = p - buf;
	return buf;
}

// --- Analysis ----------------------------------------------------------------

static uint8_t *
extract_imagemagick_attribute(const char *string, size_t *len)
{
	if (*string++ != '\n')
		return NULL;

	// TODO(p): Try to verify this profile type, also present in the key,
	// though beware that it may contain "generic profile" for APP1, etc.
	const char *type = string;
	if (!(string = strchr(type, '\n')))
		return NULL;

	// strtol() skips initial whitespace, this is mostly desired.
	char *end = NULL;
	long size = strtol(++string, &end, 10);
	if (size < 0 || end == string || *end++ != '\n')
		return NULL;

	uint8_t *bin = hexbin(end, len);
	if (!bin || (long) *len != size) {
		free(bin);
		return NULL;
	}
	return bin;
}

static jv
extract_imagemagick_exif(jv o, const char *string)
{
	size_t exif_len = 0;
	uint8_t *exif = extract_imagemagick_attribute(string, &exif_len);
	if (!exif)
		return add_warning(o, "invalid ImageMagick 'exif'");

	o = parse_exif(o, exif, exif_len);
	free(exif);
	return o;
}

static jv
extract_imagemagick_psir(jv o, const char *string)
{
	size_t psir_len = 0;
	uint8_t *psir = extract_imagemagick_attribute(string, &psir_len);
	if (!psir)
		return add_warning(o, "invalid ImageMagick '8bim'");

	o = parse_psir(o, psir, psir_len);
	free(psir);
	return o;
}

static bool
process_text(jv *o, png_textp text)
{
	// TODO(p): Refactor info.h, so that it's the value of the text chunk,
	// and that warnings are added to the top-level JSON.

	// These seem to originate in ImageMagick,
	// but are also used by ExifTool and GIMP, among others.
	// https://exiftool.org/TagNames/PNG.html
	// TODO(p): "iptc": may contain 8BIM or IPTC IIM directly.
	// TODO(p): "APP1": may contain Exif or XMP.
	if (!strcmp(text->key, "Raw profile type exif")) {
		*o = extract_imagemagick_exif(*o, text->text);
		return true;
	}
	if (!strcmp(text->key, "Raw profile type 8bim")) {
		*o = extract_imagemagick_psir(*o, text->text);
		return true;
	}
	return false;
}

static jv
retrieve_texts(jv o, png_structp pngp, png_infop infop)
{
	int texts_len = 0;
	png_textp texts = NULL;
	png_get_text(pngp, infop, &texts, &texts_len);

	jv to = jv_object();
	for (int i = 0; i < texts_len; i++) {
		png_textp text = texts + i;
		to = jv_object_set(to, jv_string(text->key),
			process_text(&o, text) ? jv_true() : jv_string(text->text));
	}
	return jv_object_set(o, jv_string("texts"), to);
}

static jv
extract_chunks(png_structp pngp, png_infop infop)
{
	jv o = jv_object();

	// With a fully separated infop from the end of the file,
	// png_get_IHDR() causes a warning and an error. Avoid that.
	uint32_t width = 0, height = 0;
	int bit_depth = 0, color_type = 0, interlace_method = 0;
	if (png_get_image_width(pngp, infop)
	 && png_get_IHDR(pngp, infop, &width, &height, &bit_depth, &color_type,
			&interlace_method, NULL, NULL)) {
		const char *color = "?";
		switch (color_type) {
		case 0:
		case 4: color = "gray";       break;
		case 2:
		case 6: color = "rgb";        break;
		case 3: color = "palette";    break;
		}

		o = jv_object_set(o, jv_string("depth"), jv_number(bit_depth));
		o = jv_object_set(o, jv_string("width"), jv_number(width));
		o = jv_object_set(o, jv_string("height"), jv_number(height));
		o = jv_object_set(o, jv_string("interlace"), jv_bool(interlace_method));
		o = jv_object_set(o, jv_string("color"), jv_string(color));
		o = jv_object_set(o, jv_string("alpha"),
			jv_bool((color_type & PNG_COLOR_MASK_ALPHA) ||
				png_get_valid(pngp, infop, PNG_INFO_tRNS)));
	}

	double gamma = 0;
	if (png_get_gAMA(pngp, infop, &gamma))
		// XXX: Might want to round it or store as integer.
		o = jv_object_set(o, jv_string("gamma"), jv_number(1 / gamma));

	// Note that sRGB overrides both gAMA and cHRM.
	int intent = -1;
	if (png_get_sRGB(pngp, infop, &intent)) {
		const char *name = "?";
		switch (intent) {
		case PNG_sRGB_INTENT_PERCEPTUAL: name = "perceptual"; break;
		case PNG_sRGB_INTENT_RELATIVE:   name = "relative";   break;
		case PNG_sRGB_INTENT_SATURATION: name = "saturation"; break;
		case PNG_sRGB_INTENT_ABSOLUTE:   name = "absolute";   break;
		}
		o = jv_object_set(o, jv_string("sRGB"), jv_string(name));
	}

	// Note that iCCP overrides both gAMA and cHRM.
	char *name = NULL;
	png_bytep profile = NULL;
	uint32_t profile_len = 0;
	if (png_get_iCCP(pngp, infop, &name, NULL, &profile, &profile_len))
		o = jv_object_set(o, jv_string("ICC"), jv_string(name));

	jv set = jv_object();
	png_unknown_chunkp unknowns = NULL;
	int unknowns_len = png_get_unknown_chunks(pngp, infop, &unknowns);
	for (int i = 0; i < unknowns_len; i++) {
		set = jv_object_set(set,
			jv_string((const char *) unknowns[i].name), jv_true());

		// https://ftp-osl.osuosl.org/pub/libpng/documents/pngext-1.5.0.html
		//
		// Some software also supports the adjacent zXIf proposal,
		// which ended up being rejected. Such files are rare, and best ignored.
		// http://www.simplesystems.org/png-group/proposals/zXIf/history
		// /png-proposed-zXIf-chunk-2017-03-05.html
		if (!strcmp((const char *) unknowns[i].name, "eXIf"))
			o = parse_exif(o, unknowns[i].data, unknowns[i].size);
	}

	jv a = jv_array();
	jv_object_keys_foreach(set, key)
		a = jv_array_append(a, jv_copy(key));
	o = jv_object_set(o, jv_string("chunks"), a);
	jv_free(set);

	return retrieve_texts(o, pngp, infop);
}

static void
redirect_libpng_error(png_structp pngp, const char* message)
{
	char **storage = png_get_error_ptr(pngp);
	*storage = strfmt("%s", message);
}

static jv
do_file(const char *filename, volatile jv o)
{
	png_bytep volatile buffer = NULL;
	png_bytepp volatile rows = NULL;

	char *volatile err = NULL;
	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		err = strfmt("%s", strerror(errno));
		goto error;
	}

	// TODO(p): Extract libpng warnings.
	png_structp pngp = png_create_read_struct(PNG_LIBPNG_VER_STRING,
		(png_voidp) &err, redirect_libpng_error, NULL);
	if (!pngp) {
		err = strfmt("%s", strerror(errno));
		goto error_png;
	}

	// We want to read these separately, which libpng allows and spng doesn't.
	png_infop infop = png_create_info_struct(pngp);
	png_infop endp = png_create_info_struct(pngp);
	if (!infop || !endp) {
		err = strfmt("%s", strerror(errno));
		goto error_decode;
	}

	png_init_io(pngp, fp);
	if (setjmp(png_jmpbuf(pngp)))
		goto error_decode;

	// Following the list of PNG_INFO_*, we just scan for their existence.
	png_byte basic[] =
		"cHRM\0bKGD\0hIST\0pHYs\0oFFs\0tIME\0pCAL\0sPLT\0sCAL\0eXIf";
	png_set_keep_unknown_chunks(pngp, PNG_HANDLE_CHUNK_ALWAYS, basic,
		sizeof basic / 5);
	png_set_keep_unknown_chunks(pngp, PNG_HANDLE_CHUNK_ALWAYS, NULL, 0);

	png_read_info(pngp, infop);
	o = jv_object_set(o, jv_string("info"), extract_chunks(pngp, infop));

	// Run over the data in the simplest possible manner.
	size_t height = png_get_image_height(pngp, infop);
	size_t row_bytes = png_get_rowbytes(pngp, infop);

	if (!(buffer = calloc(height, row_bytes))
	 || !(rows = calloc(height, sizeof(png_bytep))))
		png_error(pngp, strerror(errno));
	for (size_t i = 0; i < height; i++)
		rows[i] = buffer + i * row_bytes;

	if (!getenv("PNGINFO_SKIP_TRAILING")) {
		png_read_image(pngp, rows);

		png_read_end(pngp, endp);
		o = jv_object_set(o, jv_string("end"), extract_chunks(pngp, endp));
	}

error_decode:
	free(buffer);
	free(rows);
	png_destroy_read_struct(&pngp, &infop, &endp);
error_png:
	fclose(fp);
error:
	if (err) {
		o = add_error(o, err);
		free(err);
	}
	return o;
}

int
main(int argc, char *argv[])
{
	(void) parse_icc;

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

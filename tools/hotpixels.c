//
// hotpixels.c: look for hot pixels in raw image files
//
// Usage: pass a bunch of raw photo images taken with the lens cap on at,
// e.g., ISO 8000-12800 @ 1/20-1/60, and store the resulting file as,
// e.g., Nikon D7500.badpixels, which can then be directly used by Rawtherapee.
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

#include <libraw.h>

#if LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 21, 0)
#error LibRaw 0.21.0 or newer is required.
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *
xreallocarray(void *o, size_t n, size_t m)
{
	if (m && n > SIZE_MAX / m) {
		fprintf(stderr, "xreallocarray: %s\n", strerror(ENOMEM));
		exit(EXIT_FAILURE);
	}
	void *p = realloc(o, n * m);
	if (!p && n && m) {
		fprintf(stderr, "xreallocarray: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	return p;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct coord { ushort x, y; };

static bool
coord_equals(struct coord a, struct coord b)
{
	return a.x == b.x && a.y == b.y;
}

static int
coord_cmp(const void *a, const void *b)
{
	const struct coord *ca = (const struct coord *) a;
	const struct coord *cb = (const struct coord *) b;
	return ca->y != cb->y
		? (int) ca->y - (int) cb->y
		: (int) ca->x - (int) cb->x;
}

struct candidates {
	struct coord *xy;
	size_t len;
	size_t alloc;
};

static void
candidates_add(struct candidates *c, ushort x, ushort y)
{
	if (c->len == c->alloc) {
		c->alloc += 64;
		c->xy = xreallocarray(c->xy, sizeof *c->xy, c->alloc);
	}

	c->xy[c->len++] = (struct coord) {x, y};
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// A stretch of zeroes that is assumed to mean start of outliers.
#define SPAN 10

static const char *
process_raw(struct candidates *c, const uint8_t *p, size_t len)
{
	libraw_data_t *iprc = libraw_init(LIBRAW_OPIONS_NO_DATAERR_CALLBACK);
	if (!iprc)
		return "failed to obtain a LibRaw handle";

	int err = 0;
	if ((err = libraw_open_buffer(iprc, p, len)) ||
		(err = libraw_unpack(iprc))) {
		libraw_close(iprc);
		return libraw_strerror(err);
	}
	if (!iprc->rawdata.raw_image) {
		libraw_close(iprc);
		return "only Bayer raws are supported, not Foveon";
	}

	// Make a histogram.
	uint64_t bins[USHRT_MAX] = {};
	for (ushort yy = 0; yy < iprc->sizes.height; yy++) {
		for (ushort xx = 0; xx < iprc->sizes.width; xx++) {
			ushort y = iprc->sizes.top_margin + yy;
			ushort x = iprc->sizes.left_margin + xx;
			bins[iprc->rawdata.raw_image[y * iprc->sizes.raw_width + x]]++;
		}
	}

	// Detecting outliers is not completely straight-forward,
	// it may help to see the histogram.
	if (getenv("HOTPIXELS_HISTOGRAM")) {
		for (ushort i = 0; i < USHRT_MAX; i++)
			fprintf(stderr, "%u ", (unsigned) bins[i]);
		fputc('\n', stderr);
	}

	// Go to the first non-zero pixel value.
	size_t last = 0;
	for (; last < USHRT_MAX; last++)
		if (bins[last])
			break;

	// Find the last pixel value we assume to not be hot.
	for (; last < USHRT_MAX - SPAN - 1; last++) {
		uint64_t nonzero = 0;
		for (int i = 1; i <= SPAN; i++)
			nonzero += bins[last + i];
		if (!nonzero)
			break;
	}

	// Store coordinates for all pixels above that value.
	for (ushort yy = 0; yy < iprc->sizes.height; yy++) {
		for (ushort xx = 0; xx < iprc->sizes.width; xx++) {
			ushort y = iprc->sizes.top_margin + yy;
			ushort x = iprc->sizes.left_margin + xx;
			if (iprc->rawdata.raw_image[y * iprc->sizes.raw_width + x] > last)
				candidates_add(c, xx, yy);
		}
	}

	libraw_close(iprc);
	return NULL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static const char *
do_file(struct candidates *c, const char *filename)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp)
		return strerror(errno);

	uint8_t *data = NULL, buf[256 << 10];
	size_t n, len = 0;
	while ((n = fread(buf, sizeof *buf, sizeof buf / sizeof *buf, fp))) {
		data = xreallocarray(data, len + n, 1);
		memcpy(data + len, buf, n);
		len += n;
	}

	const char *err = ferror(fp)
		? strerror(errno)
		: process_raw(c, data, len);

	fclose(fp);
	free(data);
	return err;
}

int
main(int argc, char *argv[])
{
	struct candidates c = {};
	for (int i = 1; i < argc; i++) {
		const char *filename = argv[i], *err = do_file(&c, filename);
		if (err) {
			fprintf(stderr, "%s: %s\n", filename, err);
			return EXIT_FAILURE;
		}
	}

	qsort(c.xy, c.len, sizeof *c.xy, coord_cmp);

	// If it is detected in all passed photos, it is probably indeed bad.
	int count = 1;
	for (size_t i = 1; i <= c.len; i++) {
		if (i != c.len && coord_equals(c.xy[i - 1], c.xy[i])) {
			count++;
			continue;
		}

		if (count == argc - 1)
			printf("%u %u\n", c.xy[i - 1].x, c.xy[i - 1].y);

		count = 1;
	}
	return 0;
}

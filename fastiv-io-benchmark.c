//
// fastiv-io-benchmark.c: see if we're worth the name
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

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <time.h>

#include "fastiv-view.h"

static double
timestamp(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1.e9;
}

static void
one_file(const char *filename)
{
	double since_us = timestamp();
	cairo_surface_t *loaded_by_us = fastiv_io_open(filename, NULL);
	if (!loaded_by_us)
		return;

	cairo_surface_destroy(loaded_by_us);
	double us = timestamp() - since_us;

	double since_pixbuf = timestamp();
	GdkPixbuf *gdk_pixbuf = gdk_pixbuf_new_from_file(filename, NULL);
	if (!gdk_pixbuf)
		return;

	cairo_surface_t *loaded_by_pixbuf =
		gdk_cairo_surface_create_from_pixbuf(gdk_pixbuf, 1, NULL);
	g_object_unref(gdk_pixbuf);
	cairo_surface_destroy(loaded_by_pixbuf);
	double pixbuf = timestamp() - since_pixbuf;

	printf("%f\t%f\t%.0f%%\t%s\n", us, pixbuf, us / pixbuf * 100, filename);
}

int
main(int argc, char *argv[])
{
	// Needed for gdk_cairo_surface_create_from_pixbuf().
	gdk_init(&argc, &argv);

	for (int i = 1; i < argc; i++)
		one_file(argv[i]);
	return 0;
}

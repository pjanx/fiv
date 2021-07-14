//
// fastiv.c: fast image viewer
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

#include <gtk/gtk.h>
#include <glib.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <locale.h>

#include "config.h"
#include "fastiv-view.h"

// --- Utilities ---------------------------------------------------------------

static void
exit_fatal(const gchar *format, ...) G_GNUC_PRINTF(1, 2);

static void
exit_fatal(const gchar *format, ...)
{
	va_list ap;
	va_start(ap, format);

	gchar *format_nl = g_strdup_printf("%s\n", format);
	vfprintf(stderr, format_nl, ap);
	free(format_nl);

	va_end(ap);
	exit(EXIT_FAILURE);
}

// --- Main --------------------------------------------------------------------

struct {
	// TODO(p): Add some state variables.
	//  - Directory filenames, index within the list.
} g;

int
main(int argc, char *argv[])
{
	if (!setlocale(LC_CTYPE, ""))
		exit_fatal("cannot set locale");

	gboolean show_version = FALSE;
	gchar **files = NULL;
	const GOptionEntry options[] = {
		{G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &files,
			NULL, "[FILE | DIRECTORY]..."},
		{"version", 'V', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE,
		 &show_version, "output version information and exit", NULL},
		{},
	};

	GError *error = NULL;
	if (!gtk_init_with_args(&argc, &argv, " - fast image viewer",
		options, NULL, &error))
		exit_fatal("%s", error->message);
	if (show_version) {
		printf(PROJECT_NAME " " PROJECT_VERSION "\n");
		return 0;
	}

	GtkWidget *view = g_object_new(FASTIV_TYPE_VIEW, NULL);
	GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_container_add(GTK_CONTAINER(window), view);

	// TODO(p): Load directory entries, store in `g`.
	//  - Only when there's just one filename.
	//     - stat() it if it's a dictionary or a filename;
	//       can just blindly try to readdir(), followed by dirname and retry
	//     - But how do we filter these? We don't want to have non-images
	//       on the list.
	//  - When there are multiple, just take them verbatim as a list.
	//     - Not entirely sure about how much sense this makes,
	//       we might want to rather open several windows, or simply fork,
	//       or even disallow this completely.
	gsize files_len = g_strv_length(files);
	if (files_len) {
		GDir *dir = NULL;
		if ((dir = g_dir_open(files[0], 0, NULL))) {
			const gchar *name = NULL;
			while ((name = g_dir_read_name(dir)))
				;
			g_dir_close(dir);
		}

		if (!fastiv_view_open(FASTIV_VIEW(view), files[0], &error))
			g_printerr("error: %s\n", error->message);
		else
			gtk_window_set_title(GTK_WINDOW(window), files[0]);
	}

	gtk_widget_show_all(window);
	gtk_main();
	return 0;
}

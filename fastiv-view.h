//
// fastiv-view.h: fast image viewer - view widget
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

#pragma once

#include <gtk/gtk.h>

#define FASTIV_TYPE_VIEW (fastiv_view_get_type())
G_DECLARE_FINAL_TYPE(FastivView, fastiv_view, FASTIV, VIEW, GtkWidget)

/// Try to open the given file, synchronously, to be displayed by the widget.
gboolean fastiv_view_open(FastivView *self, const gchar *path, GError **error);

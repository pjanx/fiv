//
// fiv-browser.h: filesystem browsing widget
//
// Copyright (c) 2021 - 2022, Přemysl Eric Janouch <p@janouch.name>
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

#include "fiv-io-model.h"

#include <gtk/gtk.h>

#define FIV_TYPE_BROWSER (fiv_browser_get_type())
G_DECLARE_FINAL_TYPE(FivBrowser, fiv_browser, FIV, BROWSER, GtkWidget)

GtkWidget *fiv_browser_new(FivIoModel *model);
void fiv_browser_select(FivBrowser *self, const char *uri);

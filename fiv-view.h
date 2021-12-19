//
// fiv-view.h: fast image viewer - view widget
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

#define FIV_TYPE_VIEW (fiv_view_get_type())
G_DECLARE_FINAL_TYPE(FivView, fiv_view, FIV, VIEW, GtkWidget)

/// Try to open the given file, synchronously, to be displayed by the widget.
gboolean fiv_view_open(FivView *self, const gchar *path, GError **error);

typedef enum _FivViewCommand {
	FIV_VIEW_COMMAND_ROTATE_LEFT = 1,
	FIV_VIEW_COMMAND_MIRROR,
	FIV_VIEW_COMMAND_ROTATE_RIGHT,

	FIV_VIEW_COMMAND_PAGE_FIRST,
	FIV_VIEW_COMMAND_PAGE_PREVIOUS,
	FIV_VIEW_COMMAND_PAGE_NEXT,
	FIV_VIEW_COMMAND_PAGE_LAST,

	FIV_VIEW_COMMAND_FRAME_FIRST,
	FIV_VIEW_COMMAND_FRAME_PREVIOUS,
	FIV_VIEW_COMMAND_FRAME_NEXT,
	// Going to the end frame makes no sense, wrap around if needed.
	FIV_VIEW_COMMAND_TOGGLE_PLAYBACK,

	FIV_VIEW_COMMAND_TOGGLE_FILTER,
	FIV_VIEW_COMMAND_TOGGLE_CHECKERBOARD,
	FIV_VIEW_COMMAND_PRINT,
	FIV_VIEW_COMMAND_SAVE_PAGE,
	FIV_VIEW_COMMAND_INFO,

	FIV_VIEW_COMMAND_ZOOM_IN,
	FIV_VIEW_COMMAND_ZOOM_OUT,
	FIV_VIEW_COMMAND_ZOOM_1,
	FIV_VIEW_COMMAND_TOGGLE_SCALE_TO_FIT,
} FivViewCommand;

/// Execute a user action.
void fiv_view_command(FivView *self, FivViewCommand command);

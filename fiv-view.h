//
// fiv-view.h: image viewing widget
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

#include <gtk/gtk.h>

#define FIV_TYPE_VIEW (fiv_view_get_type())
G_DECLARE_FINAL_TYPE(FivView, fiv_view, FIV, VIEW, GtkWidget)

/// Try to open the given file, synchronously, to be displayed by the widget.
/// The current image is cleared on failure.
gboolean fiv_view_set_uri(FivView *self, const char *uri);

// And this is how you avoid glib-mkenums.
typedef enum _FivViewCommand {
#define FIV_VIEW_COMMANDS(XX)                                                  \
	XX(FIV_VIEW_COMMAND_RELOAD,              "reload")                         \
	\
	XX(FIV_VIEW_COMMAND_ROTATE_LEFT,         "rotate-left")                    \
	XX(FIV_VIEW_COMMAND_MIRROR,              "mirror")                         \
	XX(FIV_VIEW_COMMAND_ROTATE_RIGHT,        "rotate-right")                   \
	\
	XX(FIV_VIEW_COMMAND_PAGE_FIRST,          "page-first")                     \
	XX(FIV_VIEW_COMMAND_PAGE_PREVIOUS,       "page-previous")                  \
	XX(FIV_VIEW_COMMAND_PAGE_NEXT,           "page-next")                      \
	XX(FIV_VIEW_COMMAND_PAGE_LAST,           "page-last")                      \
	\
	XX(FIV_VIEW_COMMAND_FRAME_FIRST,         "frame-first")                    \
	XX(FIV_VIEW_COMMAND_FRAME_PREVIOUS,      "frame-previous")                 \
	XX(FIV_VIEW_COMMAND_FRAME_NEXT,          "frame-next")                     \
	/* Going to the end frame makes no sense, wrap around if needed. */        \
	XX(FIV_VIEW_COMMAND_TOGGLE_PLAYBACK,     "toggle-playback")                \
	\
	XX(FIV_VIEW_COMMAND_TOGGLE_CMS,          "toggle-cms")                     \
	XX(FIV_VIEW_COMMAND_TOGGLE_FILTER,       "toggle-filter")                  \
	XX(FIV_VIEW_COMMAND_TOGGLE_CHECKERBOARD, "toggle-checkerboard")            \
	XX(FIV_VIEW_COMMAND_TOGGLE_ENHANCE,      "toggle-enhance")                 \
	XX(FIV_VIEW_COMMAND_PRINT,               "print")                          \
	XX(FIV_VIEW_COMMAND_SAVE_PAGE,           "save-page")                      \
	XX(FIV_VIEW_COMMAND_SAVE_FRAME,          "save-frame")                     \
	XX(FIV_VIEW_COMMAND_INFO,                "info")                           \
	\
	XX(FIV_VIEW_COMMAND_ZOOM_IN,             "zoom-in")                        \
	XX(FIV_VIEW_COMMAND_ZOOM_OUT,            "zoom-out")                       \
	XX(FIV_VIEW_COMMAND_ZOOM_1,              "zoom-1")                         \
	XX(FIV_VIEW_COMMAND_FIT_WIDTH,           "fit-width")                      \
	XX(FIV_VIEW_COMMAND_FIT_HEIGHT,          "fit-height")                     \
	XX(FIV_VIEW_COMMAND_TOGGLE_SCALE_TO_FIT, "toggle-scale-to-fit")            \
	XX(FIV_VIEW_COMMAND_TOGGLE_FIXATE,       "toggle-fixate")
#define XX(constant, name) constant,
	FIV_VIEW_COMMANDS(XX)
#undef XX
} FivViewCommand;

GType fiv_view_command_get_type(void) G_GNUC_CONST;
#define FIV_TYPE_VIEW_COMMAND (fiv_view_command_get_type())

/// Execute a user action.
void fiv_view_command(FivView *self, FivViewCommand command);

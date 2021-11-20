//
// fastiv-sidebar.c: molesting GtkPlacesSidebar
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

#include "fastiv-sidebar.h"

struct _FastivSidebar {
	GtkScrolledWindow parent_instance;
	GtkPlacesSidebar *places;
	GtkWidget *listbox;
	GFile *location;
};

G_DEFINE_TYPE(FastivSidebar, fastiv_sidebar, GTK_TYPE_SCROLLED_WINDOW)

G_DEFINE_QUARK(fastiv-sidebar-location-quark, fastiv_sidebar_location)

enum {
	OPEN_LOCATION,
	LAST_SIGNAL,
};

// Globals are, sadly, the canonical way of storing signal numbers.
static guint sidebar_signals[LAST_SIGNAL];

static void
fastiv_sidebar_dispose(GObject *gobject)
{
	FastivSidebar *self = FASTIV_SIDEBAR(gobject);
	g_clear_object(&self->location);

	G_OBJECT_CLASS(fastiv_sidebar_parent_class)->dispose(gobject);
}

static void
fastiv_sidebar_class_init(FastivSidebarClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = fastiv_sidebar_dispose;

	// You're giving me no choice, Adwaita.
	// Your style is hardcoded to match against the class' CSS name.
	// And I need replicate the internal widget structure.
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	gtk_widget_class_set_css_name(widget_class, "placessidebar");

	// TODO(p): Consider a return value, and using it.
	sidebar_signals[OPEN_LOCATION] =
		g_signal_new("open_location", G_TYPE_FROM_CLASS(klass), 0, 0,
			NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_FILE);
}

static GtkWidget *
create_row(GFile *file, const char *icon_name)
{
	// TODO(p): Handle errors better.
	GFileInfo *info =
		g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
	if (!info)
		return NULL;

	const char *name = g_file_info_get_display_name(info);
	GtkWidget *rowbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	GtkWidget *rowimage =
		gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(rowimage), "sidebar-icon");
	gtk_container_add(GTK_CONTAINER(rowbox), rowimage);

	GtkWidget *rowlabel = gtk_label_new(name);
	gtk_label_set_ellipsize(GTK_LABEL(rowlabel), PANGO_ELLIPSIZE_END);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(rowlabel), "sidebar-label");
	gtk_container_add(GTK_CONTAINER(rowbox), rowlabel);

	GtkWidget *revealer = gtk_revealer_new();
	gtk_revealer_set_reveal_child(
		GTK_REVEALER(revealer), TRUE);
	gtk_revealer_set_transition_type(
		GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_NONE);
	gtk_container_add(GTK_CONTAINER(revealer), rowbox);

	GtkWidget *row = gtk_list_box_row_new();
	g_object_set_qdata_full(G_OBJECT(row), fastiv_sidebar_location_quark(),
		g_object_ref(file), (GDestroyNotify) g_object_unref);
	gtk_container_add(GTK_CONTAINER(row), revealer);
	gtk_widget_show_all(row);
	return row;
}

static gint
listbox_sort(
	GtkListBoxRow *row1, GtkListBoxRow *row2, G_GNUC_UNUSED gpointer user_data)
{
	GFile *location1 =
		g_object_get_qdata(G_OBJECT(row1), fastiv_sidebar_location_quark());
	GFile *location2 =
		g_object_get_qdata(G_OBJECT(row2), fastiv_sidebar_location_quark());
	if (g_file_has_prefix(location1, location2))
		return +1;
	if (g_file_has_prefix(location2, location1))
		return -1;

	gchar *name1 = g_file_get_parse_name(location1);
	gchar *name2 = g_file_get_parse_name(location2);
	gint result = g_utf8_collate(name1, name2);
	g_free(name1);
	g_free(name2);
	return result;
}

static void
update_location(FastivSidebar *self, GFile *location)
{
	if (location) {
		g_clear_object(&self->location);
		self->location = g_object_ref(location);
	}

	gtk_places_sidebar_set_location(self->places, self->location);
	gtk_container_foreach(GTK_CONTAINER(self->listbox),
		(GtkCallback) gtk_widget_destroy, NULL);
	g_return_if_fail(self->location != NULL);

	GFile *iter = g_object_ref(self->location);
	while (TRUE) {
		GFile *parent = g_file_get_parent(iter);
		g_object_unref(iter);
		if (!(iter = parent))
			break;

		gtk_list_box_prepend(GTK_LIST_BOX(self->listbox),
			create_row(parent, "go-up-symbolic"));
	}

	// Other options are "folder-{visiting,open}-symbolic", though the former
	// is mildly inappropriate (means: open in another window).
	gtk_container_add(GTK_CONTAINER(self->listbox),
		create_row(self->location, "circle-filled-symbolic"));

	GFileEnumerator *enumerator = g_file_enumerate_children(self->location,
		G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_TYPE
		"," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!enumerator)
		return;

	// TODO(p): gtk_list_box_set_filter_func(), or even use a model,
	// which could be shared with FastivBrowser.
	while (TRUE) {
		GFileInfo *info = NULL;
		GFile *child = NULL;
		if (!g_file_enumerator_iterate(enumerator, &info, &child, NULL, NULL) ||
			!info)
			break;

		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY &&
			!g_file_info_get_is_hidden(info))
			gtk_container_add(GTK_CONTAINER(self->listbox),
				create_row(child, "go-down-symbolic"));
	}
	g_object_unref(enumerator);
}

static void
on_open_breadcrumb(
	G_GNUC_UNUSED GtkListBox *listbox, GtkListBoxRow *row, gpointer user_data)
{
	FastivSidebar *self = FASTIV_SIDEBAR(user_data);
	GFile *location =
		g_object_get_qdata(G_OBJECT(row), fastiv_sidebar_location_quark());
	g_signal_emit(self, sidebar_signals[OPEN_LOCATION], 0, location);
}

static void
on_open_location(G_GNUC_UNUSED GtkPlacesSidebar *sidebar, GFile *location,
	G_GNUC_UNUSED GtkPlacesOpenFlags flags, gpointer user_data)
{
	FastivSidebar *self = FASTIV_SIDEBAR(user_data);
	g_signal_emit(self, sidebar_signals[OPEN_LOCATION], 0, location);

	// Deselect the item in GtkPlacesSidebar, if unsuccessful.
	update_location(self, NULL);
}

static void
complete_path(GFile *location, GtkListStore *model)
{
	// TODO(p): Do not enter directories unless followed by '/'.
	// This information has already been stripped from `location`.
	GFile *parent = G_FILE_TYPE_DIRECTORY ==
		g_file_query_file_type(location, G_FILE_QUERY_INFO_NONE, NULL)
		? g_object_ref(location)
		: g_file_get_parent(location);
	if (!parent)
		return;

	GFileEnumerator *enumerator = g_file_enumerate_children(parent,
		G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_TYPE
		"," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!enumerator)
		goto fail_enumerator;

	while (TRUE) {
		GFileInfo *info = NULL;
		GFile *child = NULL;
		if (!g_file_enumerator_iterate(enumerator, &info, &child, NULL, NULL) ||
			!info)
			break;

		if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY ||
			g_file_info_get_is_hidden(info))
			continue;

		// TODO(p): Resolve ~ paths a bit better.
		char *parse_name = g_file_get_parse_name(child);
		gtk_list_store_insert_with_values(model, NULL, -1, 0, parse_name, -1);
		g_free(parse_name);
	}

	g_object_unref(enumerator);
fail_enumerator:
	g_object_unref(parent);
}

static GFile *
resolve_location(FastivSidebar *self, const char *text)
{
	// Relative paths produce invalid GFile objects with this function.
	// And even if they didn't, we have our own root for them.
	GFile *file = g_file_parse_name(text);
	if (g_uri_is_valid(text, G_URI_FLAGS_PARSE_RELAXED, NULL) ||
		g_file_peek_path(file))
		return file;

	GFile *absolute =
		g_file_get_child_for_display_name(self->location, text, NULL);
	if (!absolute)
		return file;

	g_object_unref(file);
	return absolute;
}

static void
on_enter_location_changed(GtkEntry *entry, gpointer user_data)
{
	FastivSidebar *self = FASTIV_SIDEBAR(user_data);
	const char *text = gtk_entry_get_text(entry);
	GFile *location = resolve_location(self, text);

	// Don't touch the network anywhere around here, URIs are a no-no.
	GtkStyleContext *style = gtk_widget_get_style_context(GTK_WIDGET(entry));
	if (!g_file_peek_path(location) || g_file_query_exists(location, NULL))
		gtk_style_context_remove_class(style, GTK_STYLE_CLASS_WARNING);
	else
		gtk_style_context_add_class(style, GTK_STYLE_CLASS_WARNING);

	// XXX: For some reason, this jumps around with longer lists.
	GtkEntryCompletion *completion = gtk_entry_get_completion(entry);
	GtkTreeModel *model = gtk_entry_completion_get_model(completion);
	gtk_list_store_clear(GTK_LIST_STORE(model));
	if (g_file_peek_path(location))
		complete_path(location, GTK_LIST_STORE(model));
	g_object_unref(location);
}

static void
on_show_enter_location(G_GNUC_UNUSED GtkPlacesSidebar *sidebar,
	G_GNUC_UNUSED gpointer user_data)
{
	FastivSidebar *self = FASTIV_SIDEBAR(user_data);
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Enter location",
		GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(self))),
		GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL |
			GTK_DIALOG_USE_HEADER_BAR,
		"_Open", GTK_RESPONSE_ACCEPT, "_Cancel", GTK_RESPONSE_CANCEL, NULL);

	GtkListStore *model = gtk_list_store_new(1, G_TYPE_STRING);
	gtk_tree_sortable_set_sort_column_id(
		GTK_TREE_SORTABLE(model), 0, GTK_SORT_ASCENDING);

	GtkEntryCompletion *completion = gtk_entry_completion_new();
	gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(model));
	gtk_entry_completion_set_text_column(completion, 0);
	gtk_entry_completion_set_match_func(
		completion, (GtkEntryCompletionMatchFunc) gtk_true, NULL, NULL);
	g_object_unref(model);

	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_completion(GTK_ENTRY(entry), completion);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	g_signal_connect(entry, "changed",
		G_CALLBACK(on_enter_location_changed), self);

	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_add(GTK_CONTAINER(content), entry);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 800, -1);

	GdkGeometry geometry = {.max_width = G_MAXSHORT, .max_height = -1};
	gtk_window_set_geometry_hints(
		GTK_WINDOW(dialog), NULL, &geometry, GDK_HINT_MAX_SIZE);
	gtk_widget_show_all(dialog);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
		GFile *location = resolve_location(self, text);
		g_signal_emit(self, sidebar_signals[OPEN_LOCATION], 0, location);
		g_object_unref(location);
	}
	gtk_widget_destroy(dialog);
	g_object_unref(completion);

	// Deselect the item in GtkPlacesSidebar, if unsuccessful.
	update_location(self, NULL);
}

static void
fastiv_sidebar_init(FastivSidebar *self)
{
	// TODO(p): Transplant functionality from the shitty GtkPlacesSidebar.
	// We cannot reasonably place any new items within its own GtkListBox,
	// so we need to replicate the style hierarchy to some extent.
	self->places = GTK_PLACES_SIDEBAR(gtk_places_sidebar_new());
	gtk_places_sidebar_set_show_recent(self->places, FALSE);
	gtk_places_sidebar_set_show_trash(self->places, FALSE);
	gtk_places_sidebar_set_open_flags(self->places,
		GTK_PLACES_OPEN_NORMAL | GTK_PLACES_OPEN_NEW_WINDOW);
	g_signal_connect(self->places, "open-location",
		G_CALLBACK(on_open_location), self);

	gtk_places_sidebar_set_show_enter_location(self->places, TRUE);
	g_signal_connect(self->places, "show-enter-location",
		G_CALLBACK(on_show_enter_location), self);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->places),
		GTK_POLICY_NEVER, GTK_POLICY_NEVER);

	// Fill up what would otherwise be wasted space,
	// as it is in the examples of Nautilus and Thunar.
	GtkWidget *plus = gtk_button_new_from_icon_name("zoom-in-symbolic",
		GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(plus, "Larger thumbnails");
	GtkWidget *minus = gtk_button_new_from_icon_name("zoom-out-symbolic",
		GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(minus, "Smaller thumbnails");

	GtkWidget *zoom_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(zoom_group), GTK_STYLE_CLASS_LINKED);
	gtk_box_pack_start(GTK_BOX(zoom_group), plus, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(zoom_group), minus, FALSE, FALSE, 0);

	GtkWidget *funnel = gtk_toggle_button_new();
	gtk_container_add(GTK_CONTAINER(funnel),
		gtk_image_new_from_icon_name("funnel-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_widget_set_tooltip_text(funnel, "Hide unsupported files");

	// None of GtkActionBar, GtkToolbar, .inline-toolbar is appropriate.
	// It is either borders or padding.
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(buttons), GTK_STYLE_CLASS_TOOLBAR);
	gtk_box_pack_start(GTK_BOX(buttons), zoom_group, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(buttons), funnel, FALSE, FALSE, 0);
	gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);

	// TODO(p): Implement. Probably fill `buttons` in externally.
	gtk_widget_set_sensitive(plus, FALSE);
	gtk_widget_set_sensitive(minus, FALSE);
	gtk_widget_set_sensitive(funnel, FALSE);

	self->listbox = gtk_list_box_new();
	gtk_list_box_set_selection_mode(
		GTK_LIST_BOX(self->listbox), GTK_SELECTION_NONE);
	g_signal_connect(self->listbox, "row-activated",
		G_CALLBACK(on_open_breadcrumb), self);

	gtk_list_box_set_sort_func(
		GTK_LIST_BOX(self->listbox), listbox_sort, self, NULL);

	GtkWidget *superbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(
		GTK_CONTAINER(superbox), GTK_WIDGET(self->places));
	gtk_container_add(
		GTK_CONTAINER(superbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
	gtk_container_add(
		GTK_CONTAINER(superbox), buttons);
	gtk_container_add(
		GTK_CONTAINER(superbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
	gtk_container_add(
		GTK_CONTAINER(superbox), self->listbox);
	gtk_container_add(GTK_CONTAINER(self), superbox);

	gtk_scrolled_window_set_policy(
		GTK_SCROLLED_WINDOW(self), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)),
		GTK_STYLE_CLASS_SIDEBAR);
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)),
		"fastiv");
}

// --- Public interface --------------------------------------------------------

void
fastiv_sidebar_set_location(FastivSidebar *self, GFile *location)
{
	g_return_if_fail(FASTIV_IS_SIDEBAR(self));
	update_location(self, location);
}

void
fastiv_sidebar_show_enter_location(FastivSidebar *self)
{
	g_return_if_fail(FASTIV_IS_SIDEBAR(self));
	g_signal_emit_by_name(self->places, "show-enter-location");
}

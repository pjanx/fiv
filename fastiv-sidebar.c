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

	// Another option would be "folder-open-symbolic",
	// "*-visiting-*" is mildly inappropriate (means: open in another window).
	// TODO(p): Try out "circle-filled-symbolic".
	gtk_container_add(GTK_CONTAINER(self->listbox),
		create_row(self->location, "folder-visiting-symbolic"));

	GFileEnumerator *enumerator = g_file_enumerate_children(self->location,
		G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_TYPE
		"," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!enumerator)
		return;

	// TODO(p): gtk_list_box_set_sort_func(), gtk_list_box_set_filter_func(),
	// or even use a model.
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
	GFile *parent =
		g_file_query_file_type(location, G_FILE_QUERY_INFO_NONE, NULL)
		? g_object_ref(location)
		: g_file_get_parent(location);
	if (!parent)
		return;

	GFileEnumerator *enumerator = g_file_enumerate_children(parent,
		G_FILE_ATTRIBUTE_STANDARD_NAME,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!enumerator)
		goto fail_enumerator;

	// TODO(p): Resolve ~ paths a bit better.
	while (TRUE) {
		GFileInfo *info = NULL;
		GFile *child = NULL;
		if (!g_file_enumerator_iterate(enumerator, &info, &child, NULL, NULL) ||
			!info)
			break;

		GtkTreeIter iter;
		gtk_list_store_append(model, &iter);
		gtk_list_store_set(
			model, &iter, 0, g_file_get_parse_name(child), -1);
	}

	g_object_unref(enumerator);
fail_enumerator:
	g_object_unref(parent);
}

static void
on_enter_location_changed(GtkEntry *entry, G_GNUC_UNUSED gpointer user_data)
{
	const char *text = gtk_entry_get_text(entry);
	GFile *location = g_file_parse_name(text);

	// Don't touch the network, URIs are a no-no.
	// FIXME: This uses a different relative root from the fastiv.c opener.
	GtkStyleContext *style = gtk_widget_get_style_context(GTK_WIDGET(entry));
	if (g_uri_is_valid(text, G_URI_FLAGS_PARSE_RELAXED, NULL) ||
		g_file_query_exists(location, NULL))
		gtk_style_context_remove_class(style, GTK_STYLE_CLASS_WARNING);
	else
		gtk_style_context_add_class(style, GTK_STYLE_CLASS_WARNING);

	GtkListStore *model = gtk_list_store_new(1, G_TYPE_STRING);
	gtk_tree_sortable_set_sort_column_id(
		GTK_TREE_SORTABLE(model), 0, GTK_SORT_ASCENDING);
	if (!g_uri_is_valid(text, G_URI_FLAGS_PARSE_RELAXED, NULL))
		complete_path(location, model);

	// TODO(p): Try to make this not be as jumpy.
	GtkEntryCompletion *completion = gtk_entry_get_completion(entry);
	gtk_entry_completion_set_model(completion, NULL);
	gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(model));
	gtk_entry_completion_set_text_column(completion, 0);
	gtk_entry_completion_set_inline_completion(completion, TRUE);
	gtk_entry_completion_set_match_func(
		completion, (GtkEntryCompletionMatchFunc) gtk_true, NULL, NULL);
	gtk_entry_completion_complete(completion);
	g_object_unref(model);

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

	GtkWidget *entry = gtk_entry_new();
	GtkEntryCompletion *completion = gtk_entry_completion_new();
	gtk_entry_set_completion(GTK_ENTRY(entry), completion);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	g_signal_connect(entry, "changed",
		G_CALLBACK(on_enter_location_changed), self);

	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_add(GTK_CONTAINER(content), entry);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);
	gtk_widget_show_all(dialog);

	GdkGeometry geometry = {.max_width = G_MAXSHORT, .max_height = -1};
	gtk_window_set_geometry_hints(
		GTK_WINDOW(dialog), NULL, &geometry, GDK_HINT_MAX_SIZE);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
		GFile *location = g_file_parse_name(text);
		g_signal_emit(self, sidebar_signals[OPEN_LOCATION], 0, location);
		g_object_unref(location);
	}
	gtk_widget_destroy(dialog);

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
	// as it is in the example of Nautilus and Thunar.
	GtkWidget *superbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(
		GTK_CONTAINER(superbox), GTK_WIDGET(self->places));
	gtk_container_add(
		GTK_CONTAINER(superbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

	self->listbox = gtk_list_box_new();
	gtk_list_box_set_selection_mode(
		GTK_LIST_BOX(self->listbox), GTK_SELECTION_NONE);
	g_signal_connect(self->listbox, "row-activated",
		G_CALLBACK(on_open_breadcrumb), self);
	gtk_container_add(GTK_CONTAINER(superbox), self->listbox);

	gtk_scrolled_window_set_policy(
		GTK_SCROLLED_WINDOW(self), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(self)),
		GTK_STYLE_CLASS_SIDEBAR);
	gtk_container_add(GTK_CONTAINER(self), superbox);
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

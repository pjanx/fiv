//
// fiv-sidebar.c: molesting GtkPlacesSidebar
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

#include "fiv-io.h"  // fiv_io_filecmp
#include "fiv-sidebar.h"

struct _FivSidebar {
	GtkScrolledWindow parent_instance;
	GtkPlacesSidebar *places;
	GtkWidget *toolbar;
	GtkWidget *listbox;
	GFile *location;
};

G_DEFINE_TYPE(FivSidebar, fiv_sidebar, GTK_TYPE_SCROLLED_WINDOW)

G_DEFINE_QUARK(fiv-sidebar-location-quark, fiv_sidebar_location)

enum {
	OPEN_LOCATION,
	LAST_SIGNAL,
};

// Globals are, sadly, the canonical way of storing signal numbers.
static guint sidebar_signals[LAST_SIGNAL];

static void
fiv_sidebar_dispose(GObject *gobject)
{
	FivSidebar *self = FIV_SIDEBAR(gobject);
	g_clear_object(&self->location);

	G_OBJECT_CLASS(fiv_sidebar_parent_class)->dispose(gobject);
}

static void
fiv_sidebar_class_init(FivSidebarClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = fiv_sidebar_dispose;

	// You're giving me no choice, Adwaita.
	// Your style is hardcoded to match against the class' CSS name.
	// And I need replicate the internal widget structure.
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	gtk_widget_class_set_css_name(widget_class, "placessidebar");

	// TODO(p): Consider a return value, and using it.
	sidebar_signals[OPEN_LOCATION] =
		g_signal_new("open_location", G_TYPE_FROM_CLASS(klass), 0, 0,
			NULL, NULL, NULL, G_TYPE_NONE,
			2, G_TYPE_FILE, GTK_TYPE_PLACES_OPEN_FLAGS);
}

static gboolean
on_rowlabel_query_tooltip(GtkWidget *widget,
	G_GNUC_UNUSED gint x, G_GNUC_UNUSED gint y,
	G_GNUC_UNUSED gboolean keyboard_tooltip, GtkTooltip *tooltip)
{
	GtkLabel *label = GTK_LABEL(widget);
	if (!pango_layout_is_ellipsized(gtk_label_get_layout(label)))
		return FALSE;

	gtk_tooltip_set_text(tooltip, gtk_label_get_text(label));
	return TRUE;
}

static GtkWidget *
create_row(GFile *file, const char *icon_name)
{
	// TODO(p): Handle errors better.
	GError *error = NULL;
	GFileInfo *info =
		g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
	if (!info) {
		g_debug("%s", error->message);
		g_error_free(error);
		return NULL;
	}

	const char *name = g_file_info_get_display_name(info);
	GtkWidget *rowbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	GtkWidget *rowimage =
		gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(rowimage), "sidebar-icon");
	gtk_container_add(GTK_CONTAINER(rowbox), rowimage);

	GtkWidget *rowlabel = gtk_label_new(name);
	gtk_label_set_ellipsize(GTK_LABEL(rowlabel), PANGO_ELLIPSIZE_END);
	gtk_widget_set_has_tooltip(rowlabel, TRUE);
	g_signal_connect(rowlabel, "query-tooltip",
		G_CALLBACK(on_rowlabel_query_tooltip), NULL);
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
	g_object_set_qdata_full(G_OBJECT(row), fiv_sidebar_location_quark(),
		g_object_ref(file), (GDestroyNotify) g_object_unref);
	gtk_container_add(GTK_CONTAINER(row), revealer);
	gtk_widget_show_all(row);
	return row;
}

static gint
listbox_compare(
	GtkListBoxRow *row1, GtkListBoxRow *row2, G_GNUC_UNUSED gpointer user_data)
{
	return fiv_io_filecmp(
		g_object_get_qdata(G_OBJECT(row1), fiv_sidebar_location_quark()),
		g_object_get_qdata(G_OBJECT(row2), fiv_sidebar_location_quark()));
}

static void
update_location(FivSidebar *self, GFile *location)
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
	GtkWidget *row = NULL;
	while (TRUE) {
		GFile *parent = g_file_get_parent(iter);
		g_object_unref(iter);
		if (!(iter = parent))
			break;

		if ((row = create_row(parent, "go-up-symbolic")))
			gtk_list_box_prepend(GTK_LIST_BOX(self->listbox), row);
	}

	// Other options are "folder-{visiting,open}-symbolic", though the former
	// is mildly inappropriate (means: open in another window).
	if ((row = create_row(self->location, "circle-filled-symbolic")))
		gtk_container_add(GTK_CONTAINER(self->listbox), row);

	GFileEnumerator *enumerator = g_file_enumerate_children(self->location,
		G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_NAME
		"," G_FILE_ATTRIBUTE_STANDARD_TYPE
		"," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!enumerator)
		return;

	// TODO(p): gtk_list_box_set_filter_func(), or even use a model,
	// which could be shared with FivBrowser.
	while (TRUE) {
		GFileInfo *info = NULL;
		GFile *child = NULL;
		if (!g_file_enumerator_iterate(enumerator, &info, &child, NULL, NULL) ||
			!info)
			break;

		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY &&
			!g_file_info_get_is_hidden(info) &&
			(row = create_row(child, "go-down-symbolic")))
				gtk_container_add(GTK_CONTAINER(self->listbox), row);
	}
	g_object_unref(enumerator);
}

static void
on_open_breadcrumb(
	G_GNUC_UNUSED GtkListBox *listbox, GtkListBoxRow *row, gpointer user_data)
{
	FivSidebar *self = FIV_SIDEBAR(user_data);
	GFile *location =
		g_object_get_qdata(G_OBJECT(row), fiv_sidebar_location_quark());
	g_signal_emit(self, sidebar_signals[OPEN_LOCATION], 0,
		location, GTK_PLACES_OPEN_NORMAL);
}

static void
on_open_location(G_GNUC_UNUSED GtkPlacesSidebar *sidebar, GFile *location,
	GtkPlacesOpenFlags flags, gpointer user_data)
{
	FivSidebar *self = FIV_SIDEBAR(user_data);
	g_signal_emit(self, sidebar_signals[OPEN_LOCATION], 0, location, flags);

	// Deselect the item in GtkPlacesSidebar, if unsuccessful.
	update_location(self, NULL);
}

static void
complete_path(GFile *location, GtkListStore *model)
{
	// TODO(p): Do not enter directories unless followed by '/'.
	// This information has already been stripped from `location`.
	// TODO(p): Try out GFileCompleter.
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

		char *parse_name = g_file_get_parse_name(child);
		if (!g_str_has_suffix(parse_name, G_DIR_SEPARATOR_S)) {
			char *save = parse_name;
			parse_name = g_strdup_printf("%s%c", parse_name, G_DIR_SEPARATOR);
			g_free(save);
		}
		gtk_list_store_insert_with_values(model, NULL, -1, 0, parse_name, -1);
		g_free(parse_name);
	}

	g_object_unref(enumerator);
fail_enumerator:
	g_object_unref(parent);
}

static GFile *
resolve_location(FivSidebar *self, const char *text)
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
	FivSidebar *self = FIV_SIDEBAR(user_data);
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
on_show_enter_location(
	G_GNUC_UNUSED GtkPlacesSidebar *sidebar, G_GNUC_UNUSED gpointer user_data)
{
	FivSidebar *self = FIV_SIDEBAR(user_data);
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
	// TODO(p): Complete ~ paths so that they start with ~, then we can filter.
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
		g_signal_emit(self, sidebar_signals[OPEN_LOCATION], 0,
			location, GTK_PLACES_OPEN_NORMAL);
		g_object_unref(location);
	}
	gtk_widget_destroy(dialog);
	g_object_unref(completion);

	// Deselect the item in GtkPlacesSidebar, if unsuccessful.
	update_location(self, NULL);
}

static void
fiv_sidebar_init(FivSidebar *self)
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

	// None of GtkActionBar, GtkToolbar, .inline-toolbar is appropriate.
	// It is either side-favouring borders or excess button padding.
	self->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(self->toolbar), GTK_STYLE_CLASS_TOOLBAR);

	self->listbox = gtk_list_box_new();
	gtk_list_box_set_selection_mode(
		GTK_LIST_BOX(self->listbox), GTK_SELECTION_NONE);
	g_signal_connect(self->listbox, "row-activated",
		G_CALLBACK(on_open_breadcrumb), self);
	gtk_list_box_set_sort_func(
		GTK_LIST_BOX(self->listbox), listbox_compare, self, NULL);

	// Fill up what would otherwise be wasted space,
	// as it is in the examples of Nautilus and Thunar.
	GtkWidget *superbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(
		GTK_CONTAINER(superbox), GTK_WIDGET(self->places));
	gtk_container_add(
		GTK_CONTAINER(superbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
	gtk_container_add(
		GTK_CONTAINER(superbox), self->toolbar);
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
		"fiv");
}

// --- Public interface --------------------------------------------------------

void
fiv_sidebar_set_location(FivSidebar *self, GFile *location)
{
	g_return_if_fail(FIV_IS_SIDEBAR(self));
	update_location(self, location);
}

void
fiv_sidebar_show_enter_location(FivSidebar *self)
{
	g_return_if_fail(FIV_IS_SIDEBAR(self));
	g_signal_emit_by_name(self->places, "show-enter-location");
}

GtkBox *
fiv_sidebar_get_toolbar(FivSidebar *self)
{
	g_return_val_if_fail(FIV_IS_SIDEBAR(self), NULL);
	return GTK_BOX(self->toolbar);
}

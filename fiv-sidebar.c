//
// fiv-sidebar.c: molesting GtkPlacesSidebar
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

#include <gtk/gtk.h>

#include "fiv-collection.h"
#include "fiv-context-menu.h"
#include "fiv-io.h"
#include "fiv-sidebar.h"

struct _FivSidebar {
	GtkScrolledWindow parent_instance;
	GtkPlacesSidebar *places;
	GtkWidget *listbox;
	FivIoModel *model;
};

G_DEFINE_TYPE(FivSidebar, fiv_sidebar, GTK_TYPE_SCROLLED_WINDOW)

G_DEFINE_QUARK(fiv-sidebar-drag-gesture-quark, fiv_sidebar_drag_gesture)
G_DEFINE_QUARK(fiv-sidebar-location-quark, fiv_sidebar_location)
G_DEFINE_QUARK(fiv-sidebar-self-quark, fiv_sidebar_self)

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
	if (self->model) {
		g_signal_handlers_disconnect_by_data(self->model, self);
		g_clear_object(&self->model);
	}

	G_OBJECT_CLASS(fiv_sidebar_parent_class)->dispose(gobject);
}

static void
fiv_sidebar_realize(GtkWidget *widget)
{
	GTK_WIDGET_CLASS(fiv_sidebar_parent_class)->realize(widget);

	// Fucking GTK+. With no bookmarks, the revealer takes up space anyway.
	FivSidebar *self = FIV_SIDEBAR(widget);
	gtk_places_sidebar_set_drop_targets_visible(self->places, TRUE, NULL);
	gtk_places_sidebar_set_drop_targets_visible(self->places, FALSE, NULL);
}

static void
fiv_sidebar_class_init(FivSidebarClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = fiv_sidebar_dispose;

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	widget_class->realize = fiv_sidebar_realize;

	// You're giving me no choice, Adwaita.
	// Your style is hardcoded to match against the class' CSS name.
	// And I need to replicate the internal widget structure.
	gtk_widget_class_set_css_name(widget_class, "placessidebar");

	// TODO(p): Consider a return value, and using it.
	sidebar_signals[OPEN_LOCATION] =
		g_signal_new("open-location", G_TYPE_FROM_CLASS(klass), 0, 0,
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

static gboolean
on_breadcrumb_button_release(
	G_GNUC_UNUSED GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	// This also prevents unwanted primary button click handling in GtkListBox.
	if (event->x > gdk_window_get_width(event->window) ||
		event->y > gdk_window_get_height(event->window))
		return TRUE;

	guint state = event->state & gtk_accelerator_get_default_mod_mask();
	if (event->button != GDK_BUTTON_MIDDLE || state != 0)
		return FALSE;

	GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
	g_signal_emit(g_object_get_qdata(G_OBJECT(row), fiv_sidebar_self_quark()),
		sidebar_signals[OPEN_LOCATION], 0,
		g_object_get_qdata(G_OBJECT(row), fiv_sidebar_location_quark()),
		GTK_PLACES_OPEN_NEW_WINDOW);
	return TRUE;
}

static void
on_breadcrumb_gesture_drag_begin(GtkGestureDrag *drag,
	G_GNUC_UNUSED gdouble start_x, G_GNUC_UNUSED gdouble start_y,
	G_GNUC_UNUSED gpointer user_data)
{
	// Touch screen dragging is how you scroll the parent GtkScrolledWindow,
	// don't steal that gesture. Moreover, touch screen dragging fails
	// in the middle, without ever invoking drag-end.
	GtkGesture *gesture = GTK_GESTURE(drag);
	if (gdk_device_get_source(
			gdk_event_get_source_device(gtk_gesture_get_last_event(
				gesture, gtk_gesture_get_last_updated_sequence(gesture)))) ==
		GDK_SOURCE_TOUCHSCREEN)
		gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_DENIED);
}

static void
on_breadcrumb_gesture_drag_update(GtkGestureDrag *drag,
	gdouble offset_x, gdouble offset_y, gpointer user_data)
{
	GtkWidget *widget = GTK_WIDGET(user_data);
	gdouble start_x = 0, start_y = 0;
	if (!gtk_gesture_drag_get_start_point(drag, &start_x, &start_y) ||
		!gtk_drag_check_threshold(widget, start_x, start_y,
			start_x + offset_x, start_y + offset_y))
		return;

	GtkGesture *gesture = GTK_GESTURE(drag);
	gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);

	GdkEvent *event = gdk_event_copy(gtk_gesture_get_last_event(
		gesture, gtk_gesture_get_last_updated_sequence(gesture)));
	GtkTargetList *target_list = gtk_target_list_new(NULL, 0);
	gtk_target_list_add_uri_targets(target_list, 0);

	gtk_drag_begin_with_coordinates(
		widget, target_list, GDK_ACTION_LINK, 1, event, start_x, start_y);

	gtk_target_list_unref(target_list);
	gdk_event_free(event);
}

static void
on_breadcrumb_drag_data_get(G_GNUC_UNUSED GtkWidget *widget,
	G_GNUC_UNUSED GdkDragContext *context, GtkSelectionData *selection_data,
	G_GNUC_UNUSED guint info, G_GNUC_UNUSED guint time_, gpointer user_data)
{
	GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
	GFile *location =
		g_object_get_qdata(G_OBJECT(row), fiv_sidebar_location_quark());

	gchar *uris[] = {g_file_get_uri(location), NULL};
	gtk_selection_data_set_uris(selection_data, uris);
	g_free(*uris);
}

static void
on_breadcrumb_drag_begin(G_GNUC_UNUSED GtkWidget *widget,
	GdkDragContext *context, gpointer user_data)
{
	gtk_drag_set_icon_name(context, "inode-directory-symbolic", 0, 0);
	gtk_places_sidebar_set_drop_targets_visible(user_data, TRUE, context);
}

static void
on_breadcrumb_drag_end(G_GNUC_UNUSED GtkWidget *widget,
	GdkDragContext *context, gpointer user_data)
{
	gtk_places_sidebar_set_drop_targets_visible(user_data, FALSE, context);
}

static gboolean
on_breadcrumb_button_press(GtkWidget *widget, GdkEventButton *event,
	G_GNUC_UNUSED gpointer user_data)
{
	if (!gdk_event_triggers_context_menu((const GdkEvent *) event))
		return GDK_EVENT_PROPAGATE;

	GFile *location =
		g_object_get_qdata(G_OBJECT(widget), fiv_sidebar_location_quark());
	gtk_menu_popup_at_pointer(fiv_context_menu_new(widget, location), NULL);
	return GDK_EVENT_STOP;
}

static gboolean
on_breadcrumb_popup_menu(GtkWidget *widget, G_GNUC_UNUSED gpointer user_data)
{
	GFile *location =
		g_object_get_qdata(G_OBJECT(widget), fiv_sidebar_location_quark());
	gtk_menu_popup_at_widget(fiv_context_menu_new(widget, location), widget,
		GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
	return TRUE;
}

static GtkWidget *
create_row(FivSidebar *self, GFile *file, const char *icon_name)
{
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

	// The revealer is primarily necessary to match Adwaita CSS rules,
	// but it conveniently also has its own GdkWindow to hook events on.
	GtkWidget *revealer = gtk_revealer_new();
	gtk_widget_add_events(
		revealer, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
	gtk_revealer_set_reveal_child(
		GTK_REVEALER(revealer), TRUE);
	gtk_revealer_set_transition_type(
		GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_NONE);
	gtk_container_add(GTK_CONTAINER(revealer), rowbox);

	GtkGesture *drag = gtk_gesture_drag_new(revealer);
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
	gtk_event_controller_set_propagation_phase(
		GTK_EVENT_CONTROLLER(drag), GTK_PHASE_BUBBLE);
	g_object_set_qdata_full(G_OBJECT(revealer),
		fiv_sidebar_drag_gesture_quark(),
		drag, (GDestroyNotify) g_object_unref);
	g_signal_connect(drag, "drag-begin",
		G_CALLBACK(on_breadcrumb_gesture_drag_begin), revealer);
	g_signal_connect(drag, "drag-update",
		G_CALLBACK(on_breadcrumb_gesture_drag_update), revealer);

	GtkWidget *row = gtk_list_box_row_new();
	g_object_set_qdata_full(G_OBJECT(row), fiv_sidebar_location_quark(),
		g_object_ref(file), (GDestroyNotify) g_object_unref);
	g_object_set_qdata_full(G_OBJECT(row), fiv_sidebar_self_quark(),
		g_object_ref(self), (GDestroyNotify) g_object_unref);
	g_signal_connect(row, "button-press-event",
		G_CALLBACK(on_breadcrumb_button_press), NULL);
	g_signal_connect(row, "popup-menu",
		G_CALLBACK(on_breadcrumb_popup_menu), NULL);

	// Drag signals need to be hooked to a widget with its own GdkWindow.
	g_signal_connect(revealer, "button-release-event",
		G_CALLBACK(on_breadcrumb_button_release), row);
	g_signal_connect(revealer, "drag-data-get",
		G_CALLBACK(on_breadcrumb_drag_data_get), row);
	g_signal_connect(revealer, "drag-begin",
		G_CALLBACK(on_breadcrumb_drag_begin), self->places);
	g_signal_connect(revealer, "drag-end",
		G_CALLBACK(on_breadcrumb_drag_end), self->places);

	gtk_container_add(GTK_CONTAINER(row), revealer);
	gtk_widget_show_all(row);
	g_object_unref(info);
	return row;
}

static void
on_update_task(GTask *task, G_GNUC_UNUSED gpointer source_object,
	G_GNUC_UNUSED gpointer task_data, G_GNUC_UNUSED GCancellable *cancellable)
{
	g_task_return_boolean(task, TRUE);
}

static void
on_update_task_done(GObject *source_object, G_GNUC_UNUSED GAsyncResult *res,
	G_GNUC_UNUSED gpointer user_data)
{
	FivSidebar *self = FIV_SIDEBAR(source_object);
	gtk_places_sidebar_set_location(
		self->places, fiv_io_model_get_location(self->model));
}

static void
update_location(FivSidebar *self)
{
	GFile *location = fiv_io_model_get_location(self->model);

	GFile *collection = g_file_new_for_uri(FIV_COLLECTION_SCHEME ":/");
	gtk_places_sidebar_remove_shortcut(self->places, collection);
	if (location && g_file_has_uri_scheme(location, FIV_COLLECTION_SCHEME)) {
		// add_shortcut() asynchronously requests GFileInfo, and only fills in
		// the new row's "uri" data field once that's finished, resulting in
		// the immediate set_location() call below failing to find it.
		gtk_places_sidebar_add_shortcut(self->places, collection);

		// Queue up a callback using the same mechanism that GFile uses.
		GTask *task = g_task_new(self, NULL, on_update_task_done, NULL);
		g_task_set_name(task, __func__);
		g_task_set_priority(task, G_PRIORITY_LOW);
		g_task_run_in_thread(task, on_update_task);
		g_object_unref(task);
	}
	g_object_unref(collection);

	gtk_places_sidebar_set_location(self->places, location);
	gtk_container_foreach(GTK_CONTAINER(self->listbox),
		(GtkCallback) gtk_widget_destroy, NULL);
	if (!location)
		return;

	GFile *iter = g_object_ref(location);
	GtkWidget *row = NULL;
	while (TRUE) {
		GFile *parent = g_file_get_parent(iter);
		g_object_unref(iter);
		if (!(iter = parent))
			break;

		if ((row = create_row(self, parent, "go-up-symbolic")))
			gtk_list_box_prepend(GTK_LIST_BOX(self->listbox), row);
	}

	// Other options are "folder-{visiting,open}-symbolic", though the former
	// is mildly inappropriate (means: open in another window).
	if ((row = create_row(self, location, "circle-filled-symbolic")))
		gtk_container_add(GTK_CONTAINER(self->listbox), row);

	gsize len = 0;
	const FivIoModelEntry *subdirs =
		fiv_io_model_get_subdirs(self->model, &len);
	for (gsize i = 0; i < len; i++) {
		GFile *file = g_file_new_for_uri(subdirs[i].uri);
		if ((row = create_row(self, file, "go-down-symbolic")))
			gtk_container_add(GTK_CONTAINER(self->listbox), row);
		g_object_unref(file);
	}
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
	update_location(self);
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

		gchar *parse_name = g_file_get_parse_name(child);
		if (!g_str_has_suffix(parse_name, G_DIR_SEPARATOR_S)) {
			gchar *save = parse_name;
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
	if (g_file_peek_path(file))
		return file;

	// Neither branch looks like a particularly good solution.
	// Though in general, false positives are preferred over negatives.
#if GLIB_CHECK_VERSION(2, 66, 0)
	if (g_uri_is_valid(text, G_URI_FLAGS_PARSE_RELAXED, NULL))
		return file;
#else
	gchar *scheme = g_uri_parse_scheme(text);
	g_free(scheme);
	if (scheme)
		return file;
#endif

	GFile *absolute = g_file_get_child_for_display_name(
		fiv_io_model_get_location(self->model), text, NULL);
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

	// Can't have it ellipsized and word-wrapped at the same time.
	GtkWidget *protocols = gtk_label_new("");
	gtk_label_set_ellipsize(GTK_LABEL(protocols), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(protocols), 0);

	gchar *protos = g_strjoinv(
		", ", (gchar **) g_vfs_get_supported_uri_schemes(g_vfs_get_default()));
	gchar *label = g_strdup_printf("<i>Available protocols:</i> %s", protos);
	g_free(protos);
	gtk_label_set_markup(GTK_LABEL(protocols), label);
	g_free(label);

	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	g_object_set(content, "margin", 12, NULL);
	gtk_box_set_spacing(GTK_BOX(content), 6);
	gtk_container_add(GTK_CONTAINER(content), entry);
	gtk_container_add(GTK_CONTAINER(content), protocols);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 800, -1);
	gtk_window_set_geometry_hints(GTK_WINDOW(dialog), NULL,
		&(GdkGeometry) {.max_width = G_MAXSHORT, .max_height = -1},
		GDK_HINT_MAX_SIZE);
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
	update_location(self);
}

static void
fiv_sidebar_init(FivSidebar *self)
{
	// TODO(p): Transplant functionality from the shitty GtkPlacesSidebar.
	// We cannot reasonably place any new items within its own GtkListBox,
	// so we need to replicate the style hierarchy to some extent.
	GtkWidget *places = gtk_places_sidebar_new();
	self->places = GTK_PLACES_SIDEBAR(places);
	gtk_places_sidebar_set_show_recent(self->places, FALSE);
	gtk_places_sidebar_set_show_trash(self->places, FALSE);
	gtk_places_sidebar_set_open_flags(self->places,
		GTK_PLACES_OPEN_NORMAL | GTK_PLACES_OPEN_NEW_WINDOW);
	g_signal_connect(self->places, "open-location",
		G_CALLBACK(on_open_location), self);

	gint minimum_width = -1;
	gtk_widget_get_size_request(places, &minimum_width, NULL);
	gtk_widget_set_size_request(places, minimum_width, -1);

	gtk_places_sidebar_set_show_enter_location(self->places, TRUE);
	g_signal_connect(self->places, "show-enter-location",
		G_CALLBACK(on_show_enter_location), self);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->places),
		GTK_POLICY_NEVER, GTK_POLICY_NEVER);

	self->listbox = gtk_list_box_new();
	gtk_list_box_set_selection_mode(
		GTK_LIST_BOX(self->listbox), GTK_SELECTION_NONE);
	g_signal_connect(self->listbox, "row-activated",
		G_CALLBACK(on_open_breadcrumb), self);

	// Fill up what would otherwise be wasted space,
	// as it is in the examples of Nautilus and Thunar.
	GtkWidget *superbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(
		GTK_CONTAINER(superbox), GTK_WIDGET(self->places));
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

GtkWidget *
fiv_sidebar_new(FivIoModel *model)
{
	g_return_val_if_fail(FIV_IS_IO_MODEL(model), NULL);

	FivSidebar *self = g_object_new(FIV_TYPE_SIDEBAR, NULL);

	// This doesn't work from the init function.
	GtkWidget *sidebar_port = gtk_bin_get_child(GTK_BIN(self));
	gtk_container_set_focus_hadjustment(GTK_CONTAINER(sidebar_port),
		gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self)));
	gtk_container_set_focus_vadjustment(GTK_CONTAINER(sidebar_port),
		gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self)));

	// TODO(p): There should be an extra signal to watch location changes only.
	self->model = g_object_ref(model);
	g_signal_connect_swapped(self->model, "subdirectories-changed",
		G_CALLBACK(update_location), self);

	return GTK_WIDGET(self);
}

void
fiv_sidebar_show_enter_location(FivSidebar *self)
{
	g_return_if_fail(FIV_IS_SIDEBAR(self));
	g_signal_emit_by_name(self->places, "show-enter-location");
}

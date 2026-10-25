#pragma once
// GTK 3 public ABI used by the optional module. Opaque types keep the small
// cross-build independent of GTK's drawing/font development dependencies.
// All declarations match GTK 3.24 headers; no private GTK/NetSurf structures.
#include <glib-object.h>
typedef struct _GtkWidget GtkWidget;
typedef struct _GtkWindow GtkWindow;
typedef struct _GtkWindowGroup GtkWindowGroup;
typedef struct _GtkBuildable GtkBuildable;
typedef struct _GdkWindow GdkWindow;
GList *gtk_window_list_toplevels(void);
gboolean gtk_widget_get_mapped(GtkWidget *);
void gtk_widget_hide(GtkWidget *);
GdkWindow *gtk_widget_get_window(GtkWidget *);
GtkWidget *gtk_widget_get_toplevel(GtkWidget *);
gboolean gtk_window_is_active(GtkWindow *);
gboolean gtk_window_get_modal(GtkWindow *);
int gtk_window_get_window_type(GtkWindow *);
GtkWindowGroup *gtk_window_get_group(GtkWindow *);
GtkWidget *gtk_window_group_get_current_grab(GtkWindowGroup *);
const gchar *gtk_buildable_get_name(GtkBuildable *);
GType gtk_dialog_get_type(void);
GType gdk_wayland_window_get_type(void);
void gdk_wayland_window_set_application_id(GdkWindow *, const char *);

// GTK 3 icon-theme API: register application-provided fallback zoom glyphs.
typedef struct _GdkPixbuf GdkPixbuf;
GdkPixbuf *gdk_pixbuf_new_from_file(const char *, GError **);
void gtk_icon_theme_add_builtin_icon(const gchar *, gint, GdkPixbuf *);

// Public GTK 3 menu APIs (opaque ABI; no GTK development headers required).
GType gtk_widget_get_type(void);
GType gtk_menu_get_type(void);
GType gtk_toolbar_get_type(void);
GType gtk_notebook_get_type(void);
gint gtk_notebook_page_num(void *notebook, GtkWidget *child);
void gtk_notebook_set_current_page(void *notebook, gint page_num);
GType gtk_check_menu_item_get_type(void);
GType gtk_container_get_type(void);
GType gtk_menu_item_get_type(void);
GList *gtk_container_get_children(void *container);
GtkWidget *gtk_menu_item_get_submenu(void *item);
GtkWidget *gtk_menu_get_attach_widget(void *menu);
GtkWidget *gtk_menu_item_new_with_label(const gchar *label);
void gtk_menu_shell_append(void *menu, GtkWidget *child);
void gtk_widget_show(GtkWidget *widget);
void gtk_check_menu_item_set_active(void *item, gboolean active);
gboolean gtk_check_menu_item_get_active(void *item);

// GtkToolbar tool items: GTK provides their overflow-menu proxies.
GtkWidget *gtk_tool_button_new(GtkWidget *icon_widget, const gchar *label);
void gtk_toolbar_insert(void *toolbar, void *tool_item, gint pos);
void gtk_tool_item_set_homogeneous(void *tool_item, gboolean homogeneous);
void gtk_tool_item_set_expand(void *tool_item, gboolean expand);

// GTK 3 CSS API. Scope the compact style to NetSurf's browser toolbar only.
typedef struct _GtkCssProvider GtkCssProvider;
typedef struct _GtkStyleContext GtkStyleContext;
typedef struct _GdkScreen GdkScreen;
GtkCssProvider *gtk_css_provider_new(void);
gboolean gtk_css_provider_load_from_data(GtkCssProvider *, const gchar *, gssize, GError **);
void gtk_style_context_add_provider_for_screen(GdkScreen *, void *, guint);
GdkScreen *gtk_widget_get_screen(GtkWidget *);
GtkStyleContext *gtk_widget_get_style_context(GtkWidget *);
void gtk_style_context_add_class(GtkStyleContext *, const gchar *);
void gtk_toolbar_set_icon_size(void *, gint);

// GtkToolbarStyle GTK_TOOLBAR_ICONS = 0, GtkIconSize SMALL_TOOLBAR = 2.
// Keep using GTK's public ABI without requiring the full gtk/gtk.h headers.
enum { ZERO_GTK_TOOLBAR_ICONS = 0, ZERO_GTK_ICON_SIZE_SMALL_TOOLBAR = 2 };
void gtk_toolbar_set_style(void *toolbar, gint style);
void gtk_tool_button_set_icon_name(void *tool_button, const gchar *icon_name);

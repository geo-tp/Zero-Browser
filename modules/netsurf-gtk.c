#include "gtk-api.h"
#include "Browser/GtkEscapeProtocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int channel = -1;
static unsigned next_id;

static const char *identify(GtkWindow *window) {
    GdkWindow *surface = gtk_widget_get_window((GtkWidget *)window);
    if (!surface || !G_TYPE_CHECK_INSTANCE_TYPE(surface, gdk_wayland_window_get_type())) return "";
    // Store the identity on the GdkWindow: an unrealize/remap must get a new
    // identity, even if GTK reuses the GtkWindow object.
    const char *id = g_object_get_data(G_OBJECT(surface), "zero-browser-id");
    if (!id) {
        char *created = g_strdup_printf("org.cardputer.ZeroBrowser.NetSurf.%ld.%u", (long)getpid(), ++next_id);
        gdk_wayland_window_set_application_id(surface, created);
        g_object_set_data_full(G_OBJECT(surface), "zero-browser-id", created, g_free);
        id = created;
    }
    return id;
}

static uint32_t kind(GtkWindow *window) {
    const char *name = gtk_buildable_get_name((GtkBuildable *)window);
    if (!name) return ZERO_GTK_UNKNOWN;
    if (!strcmp(name, "wndBrowser")) return ZERO_GTK_MAIN;
    // NetSurf 3.11's delete-event handlers hide these windows. In particular
    // wndDownloads close is NOT its Cancel button (which aborts/unlinks).
    if (!strcmp(name, "wndDownloads")) return ZERO_GTK_DOWNLOADS;
    if (!strcmp(name, "wndHistory")) return ZERO_GTK_HISTORY;
    if (!strcmp(name, "dialogPreferences")) return ZERO_GTK_SETTINGS;
    return ZERO_GTK_UNKNOWN;
}

static gboolean tick(gpointer unused) {
    (void)unused;
    struct ZeroGtkState state = { .version = ZERO_GTK_PROTOCOL };
    unsigned active_count = 0, main_count = 0;
    GtkWindow *active_downloads = NULL;
    GList *windows = gtk_window_list_toplevels();
    for (GList *node = windows; node; node = node->next) {
        GtkWindow *window = node->data;
        if (!gtk_widget_get_mapped((GtkWidget *)window)) continue;
        // GTK_WINDOW_POPUP, including menus, never has a foreign toplevel.
        if (gtk_window_get_window_type(window) != 0) { state.popup = 1; continue; }
        const char *id = identify(window);
        uint32_t role = kind(window);
        if (role == ZERO_GTK_MAIN) {
            ++main_count;
            g_strlcpy(state.main_id, id, sizeof state.main_id);
        }
        if (!gtk_window_is_active(window)) continue;
        GtkWidget *grab = gtk_window_group_get_current_grab(gtk_window_get_group(window));
        if (grab && gtk_widget_get_toplevel(grab) != (GtkWidget *)window) state.popup = 1;
        ++active_count;
        state.kind = role;
        if (role == ZERO_GTK_DOWNLOADS) active_downloads = window;
        state.modal = gtk_window_get_modal(window) || G_TYPE_CHECK_INSTANCE_TYPE(window, gtk_dialog_get_type());
        g_strlcpy(state.app_id, id, sizeof state.app_id);
    }
    g_list_free(windows);
    if (main_count != 1) state.main_id[0] = '\0';
    if (active_count != 1) { state.kind = ZERO_GTK_UNKNOWN; state.app_id[0] = '\0'; }
    struct ZeroGtkQuery query;
    // Bounded work even if the other endpoint misbehaves.
    for (int i = 0; i < 8; ++i) {
        ssize_t n = recv(channel, &query, sizeof query, MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n <= 0) { close(channel); channel = -1; return G_SOURCE_REMOVE; }
        if (n != sizeof query || query.version != ZERO_GTK_PROTOCOL) continue;
        state.serial = query.serial;
        // ESC for the download manager must hide its GTK toplevel, not
        // activate Cancel (which could abort a transfer). Do this on GTK's
        // main thread: a Wayland toplevel close request can be missed while
        // focus/grabs change, leaving the user stuck in Downloads.
        if (active_downloads && active_count == 1 && main_count == 1 &&
            !state.modal && !state.popup && query.serial != ZERO_GTK_OPEN_SEARCHBAR) {
            gtk_widget_hide((GtkWidget *)active_downloads);
            state.kind = ZERO_GTK_DOWNLOADS_HIDDEN;
        }
        send(channel, &state, sizeof state, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    return G_SOURCE_CONTINUE;
}

// NetSurf 3.11 requests legacy GTK stock icon names absent on minimal themes.
// Register 16px fallbacks before NetSurf constructs its native toolbar.
static void install_zoom_icons(void) {
    const char *directory = g_getenv("ZERO_BROWSER_ICON_DIR");
    if (!directory || !*directory) directory = "/usr/share/zero-browser/icons";
    const char *names[] = { "gtk-zoom-in", "gtk-zoom-out", "gtk-zoom-100", "gtk-home", "go-home" };
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
        char *file = g_strdup_printf("%s/%s.png", directory, names[i]);
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(file, NULL);
        if (pixbuf) {
            gtk_icon_theme_add_builtin_icon(names[i], 16, pixbuf);
            g_object_unref(pixbuf);
        }
        g_free(file);
    }
}


// NetSurf's View -> Toolbars contains two native GtkCheckMenuItems in
// this order: Menu bar, Button bar. Trigger those instead of hiding widgets
// ourselves, so NetSurf keeps the menus and their check marks in sync.
static GtkWidget *menu_bar_check;
static GtkWidget *button_bar_check;

static gboolean is_type(void *obj, GType type) {
    return obj && G_TYPE_CHECK_INSTANCE_TYPE((GTypeInstance *)obj, type);
}

static void switch_bars(GtkWidget *item, gpointer unused) {
    (void)item; (void)unused;
    if (!menu_bar_check || !button_bar_check) return;
    gboolean buttons = gtk_check_menu_item_get_active(button_bar_check);
    if (buttons) {
        // Ensure the menubar is accessible BEFORE hiding the toolbar.
        gtk_check_menu_item_set_active(menu_bar_check, TRUE);
        gtk_check_menu_item_set_active(button_bar_check, FALSE);
    } else {
        gtk_check_menu_item_set_active(button_bar_check, TRUE);
        gtk_check_menu_item_set_active(menu_bar_check, FALSE);
    }
}

// Ask the Zero Browser host to open its existing ESC search overlay.
// A dedicated unsolicited packet avoids synthesizing keyboard events in GTK.
static void open_searchbar(GtkWidget *item, gpointer unused) {
    (void)item; (void)unused;
    if (channel < 0) return;
    struct ZeroGtkState request = { .version = ZERO_GTK_PROTOCOL,
                                    .serial = ZERO_GTK_OPEN_SEARCHBAR,
                                    .kind = ZERO_GTK_MAIN };
    send(channel, &request, sizeof request, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static gboolean looks_like_toolbars_menu(GtkWidget *menu, GtkWidget **mb, GtkWidget **tb) {
    GList *children = gtk_container_get_children(menu);
    gboolean matched = FALSE;
    if (children && children->next &&
        is_type(children->data, gtk_check_menu_item_get_type()) &&
        is_type(children->next->data, gtk_check_menu_item_get_type())) {
        // In NetSurf 3.11 the Toolbars submenu is exactly two checkboxes,
        // followed by a separator and the Customize action.
        GList *third = children->next->next;
        if (third && !is_type(third->data, gtk_check_menu_item_get_type()) &&
            third->next) {
            *mb = children->data;
            *tb = children->next->data;
            matched = TRUE;
        }
    }
    g_list_free(children);
    return matched;
}

static void discover_toolbars(GtkWidget *widget, unsigned depth) {
    if (!widget || depth > 18 || (menu_bar_check && button_bar_check)) return;
    if (is_type(widget, gtk_menu_get_type())) {
        GtkWidget *mb = NULL, *tb = NULL;
        if (looks_like_toolbars_menu(widget, &mb, &tb)) {
            menu_bar_check = mb;
            button_bar_check = tb;
            g_object_add_weak_pointer(G_OBJECT(mb), (gpointer *)&menu_bar_check);
            g_object_add_weak_pointer(G_OBJECT(tb), (gpointer *)&button_bar_check);
            return;
        }
    }
    if (is_type(widget, gtk_menu_item_get_type())) {
        GtkWidget *submenu = gtk_menu_item_get_submenu(widget);
        if (submenu) discover_toolbars(submenu, depth + 1);
    }
    if (is_type(widget, gtk_container_get_type())) {
        GList *children = gtk_container_get_children(widget);
        for (GList *n = children; n; n = n->next)
            discover_toolbars(n->data, depth + 1);
        g_list_free(children);
    }
}

// Compact only the browser button toolbar (not NetSurf's menu bar, dialogs,
// downloads or page content). GTK allocates height from its largest child,
// typically the URL entry, so both the buttons and the entry must shrink.
// Target ~22-24px instead of the default theme's ~34-40px; GTK may keep a
// slightly larger allocation depending on the installed font/theme.
static void compact_browser_toolbar(GtkWidget *toolbar) {
    if (g_object_get_data(G_OBJECT(toolbar), "zero-browser-compact-installed")) return;
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar),
                                "zero-browser-compact-toolbar");
    gtk_toolbar_set_icon_size(toolbar, ZERO_GTK_ICON_SIZE_SMALL_TOOLBAR);
    // Keep an icon-only toolbar: compacting its height must not make GTK
    // allocate the full text labels horizontally on each tool button.
    gtk_toolbar_set_style(toolbar, ZERO_GTK_TOOLBAR_ICONS);
    GtkCssProvider *css = gtk_css_provider_new();
    const char *rules =
        "toolbar.zero-browser-compact-toolbar { min-height: 0; padding: 0; margin: 0; border-width: 0; }"
        "toolbar.zero-browser-compact-toolbar toolbutton button { min-height: 18px; min-width: 0; padding: 1px 5px; margin: 0; border-width: 0; }"
        "toolbar.zero-browser-compact-toolbar toolbutton { min-width: 0; padding: 0; margin: 0; }"
        "toolbar.zero-browser-compact-toolbar entry { min-height: 18px; padding: 1px 3px; margin: 0; border-width: 0; }"
        "toolbar.zero-browser-compact-toolbar separator { min-height: 0; margin: 0; padding: 0; }";
    GError *error = NULL;
    if (gtk_css_provider_load_from_data(css, rules, -1, &error)) {
        gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(toolbar),
                                                   css, 800); // APPLICATION priority
    } else if (error) {
        g_warning("Zero Browser: failed to apply compact NetSurf toolbar CSS: %s", error->message);
        g_error_free(error);
    }
    g_object_unref(css);
    g_object_set_data(G_OBJECT(toolbar), "zero-browser-compact-installed", GINT_TO_POINTER(1));
}

// NetSurf 3.11 builds its bottom row from a GtkPaned containing a GtkLabel
// ("status_bar") and the horizontal scrollbar. It is NOT a GtkStatusbar.
// Scope these rules to the primary browser window; Downloads is unaffected.
// Leave the right-hand vertical scrollbar at the theme's normal size.
static void compact_browser_chrome(GtkWidget *browser) {
    if (g_object_get_data(G_OBJECT(browser), "zero-browser-chrome-compact-installed")) return;
    gtk_style_context_add_class(gtk_widget_get_style_context(browser),
                                "zero-browser-compact-browser");
    GtkCssProvider *css = gtk_css_provider_new();
    const char *rules =
        // Shrink only the horizontal scrollbar. GTK themes can otherwise
        // reserve extra height for the trough and slider.
        "window.zero-browser-compact-browser scrollbar.horizontal { min-height: 6px; padding: 0; margin: 0; border: 0; }"
        "window.zero-browser-compact-browser scrollbar.horizontal trough { min-height: 6px; padding: 0; margin: 0; border: 0; }"
        "window.zero-browser-compact-browser scrollbar.horizontal slider { min-height: 6px; margin: 0; border: 0; }"
        // The bottom status message is a GtkLabel inside GtkPaned. Reduce
        // its font and padding, which determine the bottom row's real height.
        "window.zero-browser-compact-browser paned label { font-size: 9px; min-height: 0; padding: 0; margin: 0; border: 0; }";
    GError *error = NULL;
    if (gtk_css_provider_load_from_data(css, rules, -1, &error)) {
        gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(browser), css, 800);
    } else if (error) {
        g_warning("Zero Browser: failed to apply compact bottom bar CSS: %s", error->message);
        g_error_free(error);
    }
    g_object_unref(css);
    g_object_set_data(G_OBJECT(browser), "zero-browser-chrome-compact-installed", GINT_TO_POINTER(1));
}

// Add a real GtkToolButton to the native GtkToolbar. GTK creates the
// overflow proxy automatically; modifying a generated GtkMenu on "show"
// is unreliable because GTK rebuilds that menu each time.
static void install_toolbar_toggle(GtkWidget *widget, unsigned depth) {
    if (!widget || depth > 18) return;
    if (is_type(widget, gtk_toolbar_get_type()) &&
        !g_object_get_data(G_OBJECT(widget), "zero-browser-bar-toggle-installed")) {
        compact_browser_toolbar(widget);
        // NetSurf native order: Back, Forward, Reload, Home, Downloads, −, +,
        // Normal Size, URL, Open menu. Put Searchbar immediately after Home.
        // Put Toggle after the URL, just before Open menu, so it overflows last.
        GtkWidget *search = gtk_tool_button_new(NULL, "Open Searchbar");
        // Text-labelled custom items must not widen NetSurf's homogeneous icon buttons.
        gtk_tool_button_set_icon_name(search, "edit-find");
        gtk_tool_item_set_homogeneous(search, FALSE);
        gtk_tool_item_set_expand(search, FALSE);
        gtk_toolbar_insert(widget, (void *)search, 4);
        g_signal_connect(search, "clicked", G_CALLBACK(open_searchbar), NULL);
        gtk_widget_show(search);

        GtkWidget *toggle = gtk_tool_button_new(NULL, "Toggle Bar");
        gtk_tool_button_set_icon_name(toggle, "view-refresh");
        gtk_tool_item_set_homogeneous(toggle, FALSE);
        gtk_tool_item_set_expand(toggle, FALSE);
        gtk_toolbar_insert(widget, (void *)toggle, 10);
        g_signal_connect(toggle, "clicked", G_CALLBACK(switch_bars), NULL);
        gtk_widget_show(toggle);
        g_object_set_data(G_OBJECT(widget), "zero-browser-bar-toggle-installed", GINT_TO_POINTER(1));
    }
    if (is_type(widget, gtk_container_get_type())) {
        GList *children = gtk_container_get_children(widget);
        for (GList *n = children; n; n = n->next)
            install_toolbar_toggle(n->data, depth + 1);
        g_list_free(children);
    }
}

// NetSurf normally creates target=_blank tabs in the background. Select only
// newly added pages; do not interfere with ordinary tab changes or initial
// browser setup. An idle callback runs after NetSurf finishes page creation.
struct PendingTab { GtkWidget *notebook; GtkWidget *child; };

static gboolean activate_new_tab(gpointer data) {
    struct PendingTab *tab = data;
    int index = gtk_notebook_page_num(tab->notebook, tab->child);
    if (index >= 0) gtk_notebook_set_current_page(tab->notebook, index);
    g_object_unref(tab->child);
    g_object_unref(tab->notebook);
    g_free(tab);
    return G_SOURCE_REMOVE;
}

static void on_tab_added(GtkWidget *notebook, GtkWidget *child,
                         guint page_num, gpointer unused) {
    (void)page_num; (void)unused;
    struct PendingTab *tab = g_new(struct PendingTab, 1);
    tab->notebook = g_object_ref(notebook);
    tab->child = g_object_ref(child);
    g_idle_add(activate_new_tab, tab);
}

static void enable_foreground_tabs(GtkWidget *widget, unsigned depth) {
    if (!widget || depth > 18) return;
    if (is_type(widget, gtk_notebook_get_type()) &&
        !g_object_get_data(G_OBJECT(widget), "zero-browser-foreground-tabs")) {
        g_signal_connect(widget, "page-added", G_CALLBACK(on_tab_added), NULL);
        g_object_set_data(G_OBJECT(widget), "zero-browser-foreground-tabs", GINT_TO_POINTER(1));
    }
    if (is_type(widget, gtk_container_get_type())) {
        GList *children = gtk_container_get_children(widget);
        for (GList *n = children; n; n = n->next)
            enable_foreground_tabs(n->data, depth + 1);
        g_list_free(children);
    }
}

static gboolean prepare_toolbar(gpointer unused) {
    (void)unused;
    GList *windows = gtk_window_list_toplevels();
    for (GList *n = windows; n; n = n->next) {
        GtkWindow *win = n->data;
        const char *name = gtk_buildable_get_name((GtkBuildable *)win);
        if (!name || strcmp(name, "wndBrowser")) continue;
        compact_browser_chrome((GtkWidget *)win);
        enable_foreground_tabs((GtkWidget *)win, 0);
        if (!menu_bar_check || !button_bar_check)
            discover_toolbars((GtkWidget *)win, 0);
        install_toolbar_toggle((GtkWidget *)win, 0);
    }
    g_list_free(windows);
    return G_SOURCE_CONTINUE;
}

void gtk_module_init(gint *argc, gchar ***argv) {
    (void)argc; (void)argv;
    install_zoom_icons();
    g_timeout_add(250, prepare_toolbar, NULL);
    const char *value = g_getenv("ZERO_BROWSER_GTK_FD");
    if (!value || channel >= 0) return;
    char *end;
    long fd = strtol(value, &end, 10);
    gboolean valid = !*end && fd >= 3 && fd <= 65535;
    g_unsetenv("ZERO_BROWSER_GTK_FD");
    if (!valid) return;
    int type = 0;
    socklen_t size = sizeof type;
    if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_SEQPACKET) return;
    channel = (int)fd;
    fcntl(channel, F_SETFD, FD_CLOEXEC);
    g_timeout_add(40, tick, NULL);
}

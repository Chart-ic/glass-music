#include "tray.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _TrayService {
    GDBusConnection *conn;
    guint object_reg_id;
    AppConfig *config;
    ConfigChangedCallback callback;
    void *user_data;
    GtkWidget *settings_window;

    // Widgets in settings window for live updating
    GtkWidget *lbl_title_font;
    GtkWidget *lbl_artist_font;
    GtkWidget *lbl_np_font;
    GtkWidget *chk_autohide;
    GtkWidget *spin_margin;
    GtkWidget *spin_autohide;
    GtkWidget *spin_marquee;
};

static const gchar sni_introspection_xml[] =
"<node>"
"  <interface name='org.kde.StatusNotifierItem'>"
"    <property name='Category' type='s' access='read'/>"
"    <property name='Id' type='s' access='read'/>"
"    <property name='Title' type='s' access='read'/>"
"    <property name='Status' type='s' access='read'/>"
"    <property name='WindowId' type='i' access='read'/>"
"    <property name='IconName' type='s' access='read'/>"
"    <property name='IconThemePath' type='s' access='read'/>"
"    <property name='ItemIsMenu' type='b' access='read'/>"
"    <method name='ContextMenu'>"
"      <arg type='i' name='x' direction='in'/>"
"      <arg type='i' name='y' direction='in'/>"
"    </method>"
"    <method name='Activate'>"
"      <arg type='i' name='x' direction='in'/>"
"      <arg type='i' name='y' direction='in'/>"
"    </method>"
"    <method name='SecondaryActivate'>"
"      <arg type='i' name='x' direction='in'/>"
"      <arg type='i' name='y' direction='in'/>"
"    </method>"
"    <signal name='NewTitle'/>"
"    <signal name='NewIcon'/>"
"    <signal name='NewStatus'>"
"      <arg type='s' name='status'/>"
"    </signal>"
"  </interface>"
"</node>";

static void notify_config_changed(TrayService *tray) {
    config_save(tray->config);
    if (tray->callback) {
        tray->callback(tray->config, tray->user_data);
    }
}

// Callback when user picks a font in GtkFontChooserDialog
static void on_pick_title_font(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TrayService *tray = (TrayService *)user_data;
    GtkWidget *dialog = gtk_font_chooser_dialog_new("Select Title Font", GTK_WINDOW(tray->settings_window));
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(dialog), tray->config->title_font);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        char *font_name = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(dialog));
        if (font_name) {
            g_free(tray->config->title_font);
            tray->config->title_font = font_name;
            gtk_label_set_text(GTK_LABEL(tray->lbl_title_font), font_name);
            notify_config_changed(tray);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_pick_artist_font(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TrayService *tray = (TrayService *)user_data;
    GtkWidget *dialog = gtk_font_chooser_dialog_new("Select Artist Font", GTK_WINDOW(tray->settings_window));
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(dialog), tray->config->artist_font);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        char *font_name = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(dialog));
        if (font_name) {
            g_free(tray->config->artist_font);
            tray->config->artist_font = font_name;
            gtk_label_set_text(GTK_LABEL(tray->lbl_artist_font), font_name);
            notify_config_changed(tray);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_pick_np_font(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TrayService *tray = (TrayService *)user_data;
    GtkWidget *dialog = gtk_font_chooser_dialog_new("Select 'Not Playing' Font", GTK_WINDOW(tray->settings_window));
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(dialog), tray->config->not_playing_font);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        char *font_name = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(dialog));
        if (font_name) {
            g_free(tray->config->not_playing_font);
            tray->config->not_playing_font = font_name;
            gtk_label_set_text(GTK_LABEL(tray->lbl_np_font), font_name);
            notify_config_changed(tray);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_toggle_autohide(GtkToggleButton *btn, gpointer user_data) {
    TrayService *tray = (TrayService *)user_data;
    tray->config->autohide_not_playing = gtk_toggle_button_get_active(btn);
    tray->config->always_show = !tray->config->autohide_not_playing;
    if (tray->spin_autohide) {
        gtk_widget_set_sensitive(tray->spin_autohide, tray->config->autohide_not_playing);
    }
    notify_config_changed(tray);
}

static void on_change_margin(GtkSpinButton *spin, gpointer user_data) {
    TrayService *tray = (TrayService *)user_data;
    tray->config->margin_top = (int)gtk_spin_button_get_value(spin);
    notify_config_changed(tray);
}

static void on_change_autohide(GtkSpinButton *spin, gpointer user_data) {
    TrayService *tray = (TrayService *)user_data;
    tray->config->autohide_delay_sec = (int)gtk_spin_button_get_value(spin);
    notify_config_changed(tray);
}

static void on_change_marquee(GtkSpinButton *spin, gpointer user_data) {
    TrayService *tray = (TrayService *)user_data;
    tray->config->marquee_speed = gtk_spin_button_get_value(spin);
    notify_config_changed(tray);
}

static void on_window_destroy(GtkWidget *w, gpointer user_data) {
    (void)w;
    TrayService *tray = (TrayService *)user_data;
    tray->settings_window = NULL;
}

void tray_service_open_font_dialog(TrayService *tray) {
    if (!tray) return;

    if (tray->settings_window) {
        gtk_window_present(GTK_WINDOW(tray->settings_window));
        return;
    }

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    tray->settings_window = win;
    gtk_window_set_title(GTK_WINDOW(win), "Glass Music - Options");
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 480);
    gtk_container_set_border_width(GTK_CONTAINER(win), 18);
    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), tray);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    // Header Title
    GtkWidget *lbl_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_header), "<span size='x-large' weight='bold'>Glass Music Settings</span>");
    gtk_widget_set_halign(lbl_header, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), lbl_header, FALSE, FALSE, 0);

    // Separator
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // Section 1: Font Settings
    GtkWidget *lbl_font_sec = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_font_sec), "<b>Typography &amp; Fonts</b>");
    gtk_widget_set_halign(lbl_font_sec, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), lbl_font_sec, FALSE, FALSE, 0);

    GtkWidget *grid_fonts = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_fonts), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid_fonts), 12);
    gtk_box_pack_start(GTK_BOX(vbox), grid_fonts, FALSE, FALSE, 0);

    // Title Font
    GtkWidget *lbl1 = gtk_label_new("Song Title Font:");
    gtk_widget_set_halign(lbl1, GTK_ALIGN_START);
    tray->lbl_title_font = gtk_label_new(tray->config->title_font);
    gtk_label_set_ellipsize(GTK_LABEL(tray->lbl_title_font), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(tray->lbl_title_font, TRUE);
    gtk_widget_set_halign(tray->lbl_title_font, GTK_ALIGN_START);
    GtkWidget *btn_title = gtk_button_new_with_label("Change...");
    g_signal_connect(btn_title, "clicked", G_CALLBACK(on_pick_title_font), tray);

    gtk_grid_attach(GTK_GRID(grid_fonts), lbl1, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_fonts), tray->lbl_title_font, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_fonts), btn_title, 2, 0, 1, 1);

    // Artist Font
    GtkWidget *lbl2 = gtk_label_new("Artist Name Font:");
    gtk_widget_set_halign(lbl2, GTK_ALIGN_START);
    tray->lbl_artist_font = gtk_label_new(tray->config->artist_font);
    gtk_label_set_ellipsize(GTK_LABEL(tray->lbl_artist_font), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(tray->lbl_artist_font, TRUE);
    gtk_widget_set_halign(tray->lbl_artist_font, GTK_ALIGN_START);
    GtkWidget *btn_artist = gtk_button_new_with_label("Change...");
    g_signal_connect(btn_artist, "clicked", G_CALLBACK(on_pick_artist_font), tray);

    gtk_grid_attach(GTK_GRID(grid_fonts), lbl2, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_fonts), tray->lbl_artist_font, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_fonts), btn_artist, 2, 1, 1, 1);

    // Not Playing Font
    GtkWidget *lbl3 = gtk_label_new("'Not Playing' Font:");
    gtk_widget_set_halign(lbl3, GTK_ALIGN_START);
    tray->lbl_np_font = gtk_label_new(tray->config->not_playing_font);
    gtk_label_set_ellipsize(GTK_LABEL(tray->lbl_np_font), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(tray->lbl_np_font, TRUE);
    gtk_widget_set_halign(tray->lbl_np_font, GTK_ALIGN_START);
    GtkWidget *btn_np = gtk_button_new_with_label("Change...");
    g_signal_connect(btn_np, "clicked", G_CALLBACK(on_pick_np_font), tray);

    gtk_grid_attach(GTK_GRID(grid_fonts), lbl3, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_fonts), tray->lbl_np_font, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_fonts), btn_np, 2, 2, 1, 1);

    // Separator
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // Section 2: Display & Behavior
    GtkWidget *lbl_disp_sec = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_disp_sec), "<b>Display &amp; Behavior</b>");
    gtk_widget_set_halign(lbl_disp_sec, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), lbl_disp_sec, FALSE, FALSE, 0);

    tray->chk_autohide = gtk_check_button_new_with_label("Not Playing 전환 후 오버레이 자동 숨김");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->chk_autohide), tray->config->autohide_not_playing);
    g_signal_connect(tray->chk_autohide, "toggled", G_CALLBACK(on_toggle_autohide), tray);
    gtk_box_pack_start(GTK_BOX(vbox), tray->chk_autohide, FALSE, FALSE, 0);

    GtkWidget *grid_opts = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_opts), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid_opts), 12);
    gtk_box_pack_start(GTK_BOX(vbox), grid_opts, FALSE, FALSE, 0);

    // Auto-hide delay
    GtkWidget *lbl_ah = gtk_label_new("자동 숨김 대기 시간 (초):");
    gtk_widget_set_halign(lbl_ah, GTK_ALIGN_START);
    tray->spin_autohide = gtk_spin_button_new_with_range(1, 60, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tray->spin_autohide),
                              tray->config->autohide_delay_sec > 0 ? tray->config->autohide_delay_sec : 5);
    gtk_widget_set_sensitive(tray->spin_autohide, tray->config->autohide_not_playing);
    g_signal_connect(tray->spin_autohide, "value-changed", G_CALLBACK(on_change_autohide), tray);
    gtk_grid_attach(GTK_GRID(grid_opts), lbl_ah, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_opts), tray->spin_autohide, 1, 0, 1, 1);

    // Margin top
    GtkWidget *lbl_m = gtk_label_new("상단 화면 여백 (px):");
    gtk_widget_set_halign(lbl_m, GTK_ALIGN_START);
    tray->spin_margin = gtk_spin_button_new_with_range(0, 300, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tray->spin_margin), tray->config->margin_top);
    g_signal_connect(tray->spin_margin, "value-changed", G_CALLBACK(on_change_margin), tray);
    gtk_grid_attach(GTK_GRID(grid_opts), lbl_m, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_opts), tray->spin_margin, 1, 1, 1, 1);

    // Marquee Speed
    GtkWidget *lbl_mq = gtk_label_new("마퀴 롤링 속도 (px/sec):");
    gtk_widget_set_halign(lbl_mq, GTK_ALIGN_START);
    tray->spin_marquee = gtk_spin_button_new_with_range(10, 120, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tray->spin_marquee), tray->config->marquee_speed);
    g_signal_connect(tray->spin_marquee, "value-changed", G_CALLBACK(on_change_marquee), tray);
    gtk_grid_attach(GTK_GRID(grid_opts), lbl_mq, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_opts), tray->spin_marquee, 1, 2, 1, 1);

    // Bottom close button
    GtkWidget *btn_close = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(gtk_widget_destroy), win);
    gtk_widget_set_halign(btn_close, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(vbox), btn_close, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
}

static gboolean on_open_dialog_idle(gpointer user_data) {
    TrayService *tray = (TrayService *)user_data;
    tray_service_open_font_dialog(tray);
    return G_SOURCE_REMOVE;
}

static void sni_handle_method_call(GDBusConnection *conn, const gchar *sender,
                                   const gchar *object_path, const gchar *iface,
                                   const gchar *method, GVariant *params,
                                   GDBusMethodInvocation *inv, gpointer user_data) {
    (void)conn; (void)sender; (void)object_path; (void)iface; (void)params;
    TrayService *tray = (TrayService *)user_data;

    if (strcmp(method, "Activate") == 0) {
        g_idle_add(on_open_dialog_idle, tray);
    } else if (strcmp(method, "ContextMenu") == 0) {
        g_idle_add(on_open_dialog_idle, tray);
    }

    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *sni_handle_get_property(GDBusConnection *conn, const gchar *sender,
                                         const gchar *object_path, const gchar *iface,
                                         const gchar *prop, GError **err, gpointer user_data) {
    (void)conn; (void)sender; (void)object_path; (void)iface; (void)err; (void)user_data;
    if (strcmp(prop, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (strcmp(prop, "Id") == 0) return g_variant_new_string("glass-music");
    if (strcmp(prop, "Title") == 0) return g_variant_new_string("Glass Music Overlay");
    if (strcmp(prop, "Status") == 0) return g_variant_new_string("Active");
    if (strcmp(prop, "WindowId") == 0) return g_variant_new_int32(0);
    if (strcmp(prop, "IconName") == 0) return g_variant_new_string("multimedia-player");
    if (strcmp(prop, "IconThemePath") == 0) return g_variant_new_string("");
    if (strcmp(prop, "ItemIsMenu") == 0) return g_variant_new_boolean(FALSE);
    return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
    .method_call = sni_handle_method_call,
    .get_property = sni_handle_get_property,
};

TrayService *tray_service_new(AppConfig *config, ConfigChangedCallback cb, void *user_data) {
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) {
        if (err) g_error_free(err);
        return NULL;
    }

    TrayService *tray = g_new0(TrayService, 1);
    tray->conn = conn;
    tray->config = config;
    tray->callback = cb;
    tray->user_data = user_data;

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(sni_introspection_xml, NULL);
    tray->object_reg_id = g_dbus_connection_register_object(
        tray->conn,
        "/StatusNotifierItem",
        node->interfaces[0],
        &sni_vtable,
        tray,
        NULL,
        &err
    );

    if (tray->object_reg_id > 0) {
        // Register with KDE StatusNotifierWatcher
        g_dbus_connection_call(
            tray->conn,
            "org.kde.StatusNotifierWatcher",
            "/StatusNotifierWatcher",
            "org.kde.StatusNotifierWatcher",
            "RegisterStatusNotifierItem",
            g_variant_new("(s)", "/StatusNotifierItem"),
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            NULL,
            NULL,
            NULL
        );
        g_print("[GlassMusic] StatusNotifierItem registered on system tray.\n");
    }

    g_dbus_node_info_unref(node);
    return tray;
}

void tray_service_free(TrayService *tray) {
    if (!tray) return;

    if (tray->settings_window) {
        gtk_widget_destroy(tray->settings_window);
    }
    if (tray->object_reg_id > 0) {
        g_dbus_connection_unregister_object(tray->conn, tray->object_reg_id);
    }
    if (tray->conn) {
        g_object_unref(tray->conn);
    }
    g_free(tray);
}

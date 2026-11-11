#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <glib-unix.h>

#include "config.h"
#include "overlay.h"
#include "mpris.h"
#include "tray.h"

#define VERSION "1.1.0"

static GMainLoop *main_loop = NULL;

static gboolean on_sigint(gpointer user_data) {
    (void)user_data;
    g_print("\n[GlassMusic] Terminating overlay...\n");
    if (main_loop) g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_wayland_io(GIOChannel *source, GIOCondition condition, gpointer data) {
    (void)source;
    GlassOverlay *overlay = (GlassOverlay *)data;
    if (condition & (G_IO_ERR | G_IO_HUP)) {
        g_printerr("[GlassMusic] Wayland display connection lost\n");
        if (main_loop) g_main_loop_quit(main_loop);
        return G_SOURCE_REMOVE;
    }
    overlay_dispatch_wayland(overlay);
    return G_SOURCE_CONTINUE;
}

static void on_track_update(const MprisTrackInfo *track, void *user_data) {
    GlassOverlay *overlay = (GlassOverlay *)user_data;
    if (track && track->title && track->is_playing) {
        g_print("[GlassMusic] Now Playing: %s - %s\n",
                track->artist ? track->artist : "Unknown",
                track->title ? track->title : "Unknown");
    } else {
        g_print("[GlassMusic] State: Not Playing\n");
    }
    overlay_set_track(overlay, track);
}

static void on_tray_config_changed(const AppConfig *new_config, void *user_data) {
    GlassOverlay *overlay = (GlassOverlay *)user_data;
    g_print("[GlassMusic] Config updated from Tray options (fonts/display)\n");
    overlay_update_config(overlay, new_config);
}

static void print_help(const char *prog_name) {
    printf("Glass Music Overlay - Native Wayland Glassmorphic Music HUD\n");
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");
    printf("  -m, --margin <pixels>   Top screen margin in pixels (default from config: 25)\n");
    printf("  -a, --autohide <sec>    Auto-hide delay after pause in seconds (default: 5, 0 to disable)\n");
    printf("  -s, --always-show       Keep the overlay visible even when paused (shows 'Not Playing' HUD)\n");
    printf("  -t, --test              Run in test mode with sample track (HIMEHINA - キスキツネ)\n");
    printf("  --marquee-test          Run in test mode with long track title to demo Marquee effect\n");
    printf("  --idle-test             Run in test mode showing 'Not Playing' state\n");
    printf("  -v, --version           Print version information\n");
    printf("  -h, --help              Show this help message\n\n");
    printf("Controls:\n");
    printf("  Tray Icon:              Click / Right-click to open Font & Display Settings\n");
    printf("  Left Click on HUD:      Toggle Play / Pause\n");
    printf("  Right Click on HUD:     Open Options & Settings Window\n");
    printf("  Middle Click on HUD:    Next Track\n");
    printf("  Mouse Scroll on HUD:    Adjust Volume Up / Down\n");
}

int main(int argc, char *argv[]) {
    // Check for help/version flags first before attempting any display connection
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("glass-music version %s\n", VERSION);
            return 0;
        }
    }

    // Initialize GTK for font dialog & tray menus
    if (!gtk_init_check(&argc, &argv)) {
        g_printerr("[GlassMusic] Error: Unable to open Wayland/X11 display. Please ensure WAYLAND_DISPLAY is set.\n");
        return 1;
    }

    AppConfig *config = config_load();
    gboolean test_mode = FALSE;
    gboolean marquee_test_mode = FALSE;
    gboolean idle_test_mode = FALSE;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--margin") == 0) && i + 1 < argc) {
            config->margin_top = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--autohide") == 0) && i + 1 < argc) {
            config->autohide_delay_sec = atoi(argv[++i]);
            config->autohide_not_playing = (config->autohide_delay_sec > 0);
            config->always_show = !config->autohide_not_playing;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--always-show") == 0) {
            config->always_show = TRUE;
            config->autohide_not_playing = FALSE;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
            test_mode = TRUE;
            config->always_show = TRUE;
        } else if (strcmp(argv[i], "--marquee-test") == 0) {
            marquee_test_mode = TRUE;
            config->always_show = TRUE;
        } else if (strcmp(argv[i], "--idle-test") == 0) {
            idle_test_mode = TRUE;
            config->always_show = TRUE;
        } else {
            g_printerr("Unknown option: %s\nRun with --help for options.\n", argv[i]);
            config_free(config);
            return 1;
        }
    }

    g_print("[GlassMusic] Starting Glass Music Overlay v%s (Native Wayland / KWin Blur)...\n", VERSION);

    // Initialize Wayland overlay
    GlassOverlay *overlay = overlay_new(config, NULL);
    if (!overlay) {
        g_printerr("[GlassMusic] Fatal: Failed to initialize Wayland layer-shell overlay\n");
        config_free(config);
        return 1;
    }

    // Initialize MPRIS client and associate with overlay
    MprisClient *mpris = mpris_client_new(on_track_update, overlay);
    overlay_set_mpris(overlay, mpris);

    // Initialize System Tray (StatusNotifierItem) for font settings
    TrayService *tray = tray_service_new(config, on_tray_config_changed, overlay);
    overlay_set_tray(overlay, tray);

    if (test_mode) {
        g_print("[GlassMusic] Running in TEST mode with Frame 2.svg track (HIMEHINA - キスキツネ)!\n");
        mpris_client_set_mock_track(mpris, "キスキツネ", "HIMEHINA", config->default_art);
    } else if (marquee_test_mode) {
        g_print("[GlassMusic] Running in MARQUEE TEST mode with long track title!\n");
        mpris_client_set_mock_track(
            mpris,
            "キスキツネ (Special Extended Edition - Marquee Effect Demo)",
            "HIMEHINA feat. Special Guest Orchestra",
            config->default_art
        );
    } else if (idle_test_mode) {
        g_print("[GlassMusic] Running in IDLE TEST mode ('Not Playing' state)!\n");
        overlay_set_track(overlay, NULL);
    } else {
        const MprisTrackInfo *initial_track = mpris_client_get_current_track(mpris);
        overlay_set_track(overlay, initial_track);
        g_print("[GlassMusic] Listening for MPRIS2 media players (Spotify, Chrome, Firefox, etc.)...\n");
    }

    // Setup GLib main loop and Wayland event watch
    main_loop = g_main_loop_new(NULL, FALSE);

    int wl_fd = overlay_get_wayland_fd(overlay);
    GIOChannel *wl_channel = g_io_channel_unix_new(wl_fd);
    g_io_add_watch(wl_channel, G_IO_IN | G_IO_ERR | G_IO_HUP, on_wayland_io, overlay);
    g_io_channel_unref(wl_channel);

    // UNIX signal handlers
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    g_unix_signal_add(SIGTERM, on_sigint, NULL);

    g_print("[GlassMusic] Overlay active! Tray icon enabled for Font Options. Press Ctrl+C to stop.\n");
    g_main_loop_run(main_loop);

    // Cleanup
    g_main_loop_unref(main_loop);
    if (tray) tray_service_free(tray);
    overlay_free(overlay);
    if (mpris) mpris_client_free(mpris);
    config_free(config);

    g_print("[GlassMusic] Shutdown complete.\n");
    return 0;
}

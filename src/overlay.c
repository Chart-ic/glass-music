#include "overlay.h"
#include "tray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <math.h>

#include "protocols/wlr-layer-shell-unstable-v1-client-protocol.h"
#include "protocols/xdg-shell-client-protocol.h"

#define NUM_BUFFERS 4
#define MAX_SURFACE_WIDTH 580
#define SURFACE_HEIGHT 92

typedef struct {
    struct wl_buffer *wl_buffer;
    void *shm_data;
    gboolean busy;
    int size;
    int width;
    int height;
} ShmBuffer;

struct _GlassOverlay {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_seat *seat;
    struct wl_pointer *pointer;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;

    ShmBuffer buffers[NUM_BUFFERS];

    PillRenderer *renderer;
    MprisClient *mpris;
    TrayService *tray;
    AppConfig config;

    MprisTrackInfo current_track;
    RenderState render_state;

    int surface_width;
    int surface_height;
    double target_pill_width;

    gboolean is_configured;
    gboolean is_active;          // Overall overlay active state
    gboolean is_not_playing;     // True when no track or paused
    guint autohide_timer_id;
    guint anim_timer_id;
    gint64 last_anim_time_us;
};

static int create_shm_file(off_t size) {
    int fd = memfd_create("glass_music_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        char name[] = "/wl_shm-XXXXXX";
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) shm_unlink(name);
    }
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
    (void)wl_buffer;
    ShmBuffer *b = (ShmBuffer *)data;
    b->busy = FALSE;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void destroy_buffers(GlassOverlay *overlay) {
    for (int i = 0; i < NUM_BUFFERS; i++) {
        ShmBuffer *b = &overlay->buffers[i];
        if (b->wl_buffer) {
            wl_buffer_destroy(b->wl_buffer);
            b->wl_buffer = NULL;
        }
        if (b->shm_data && b->size > 0) {
            munmap(b->shm_data, b->size);
            b->shm_data = NULL;
        }
        b->size = 0;
        b->busy = FALSE;
    }
}

// Single-allocation buffer pool: fixed MAX_SURFACE_WIDTH x SURFACE_HEIGHT
static ShmBuffer *get_free_buffer(GlassOverlay *overlay) {
    if (overlay->display) {
        wl_display_dispatch_pending(overlay->display);
    }

    if (!overlay->buffers[0].wl_buffer) {
        int stride = MAX_SURFACE_WIDTH * 4;
        int size = stride * SURFACE_HEIGHT;
        for (int i = 0; i < NUM_BUFFERS; i++) {
            ShmBuffer *b = &overlay->buffers[i];
            int fd = create_shm_file(size);
            if (fd < 0) return NULL;

            b->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            b->size = size;
            b->width = MAX_SURFACE_WIDTH;
            b->height = SURFACE_HEIGHT;
            b->busy = FALSE;

            struct wl_shm_pool *pool = wl_shm_create_pool(overlay->shm, fd, size);
            b->wl_buffer = wl_shm_pool_create_buffer(pool, 0, MAX_SURFACE_WIDTH, SURFACE_HEIGHT, stride, WL_SHM_FORMAT_ARGB8888);
            wl_buffer_add_listener(b->wl_buffer, &buffer_listener, b);
            wl_shm_pool_destroy(pool);
            close(fd);
        }
        return &overlay->buffers[0];
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        ShmBuffer *b = &overlay->buffers[i];
        if (!b->busy) {
            return b;
        }
    }

    return NULL;
}

static void overlay_redraw(GlassOverlay *overlay);
static void start_animation(GlassOverlay *overlay);

static gboolean on_animation_step(gpointer user_data) {
    GlassOverlay *overlay = (GlassOverlay *)user_data;
    gint64 now_us = g_get_monotonic_time();
    double dt = 0.016;
    if (overlay->last_anim_time_us > 0) {
        dt = (now_us - overlay->last_anim_time_us) / 1000000.0;
        if (dt > 0.05) dt = 0.05;
        if (dt < 0.001) dt = 0.001;
    }
    overlay->last_anim_time_us = now_us;
    overlay->render_state.elapsed_time_sec += dt;
    overlay->render_state.is_active = overlay->is_active;

    gboolean still_animating = FALSE;

    // 1. Entrance / Exit Animation (~350ms in, ~380ms out)
    if (overlay->is_active) {
        // Entrance: Expand horizontally from center
        if (overlay->render_state.slide_progress < 1.0) {
            overlay->render_state.slide_progress += dt / 0.35;
            if (overlay->render_state.slide_progress >= 1.0) {
                overlay->render_state.slide_progress = 1.0;
            }
            overlay->render_state.alpha = overlay->render_state.slide_progress;
            still_animating = TRUE;
        } else {
            overlay->render_state.slide_progress = 1.0;
            overlay->render_state.alpha = 1.0;
        }
    } else {
        // Exit: Collapse horizontally towards center
        if (overlay->render_state.slide_progress > 0.0) {
            overlay->render_state.slide_progress -= dt / 0.38;
            if (overlay->render_state.slide_progress <= 0.0) {
                overlay->render_state.slide_progress = 0.0;
            }
            double u = 1.0 - overlay->render_state.slide_progress;
            overlay->render_state.alpha = 1.0 - pow(u, 1.8);
            if (overlay->render_state.alpha < 0.0) overlay->render_state.alpha = 0.0;
            still_animating = TRUE;
        } else {
            overlay->render_state.slide_progress = 0.0;
            overlay->render_state.alpha = 0.0;
        }
    }

    // 2. Dynamic Width Smooth Morphing (Time-based snappy decay)
    double width_diff = overlay->target_pill_width - overlay->render_state.current_pill_width;
    if (fabs(width_diff) > 1.0) {
        double width_factor = 1.0 - exp(-16.0 * dt);
        overlay->render_state.current_pill_width += width_diff * width_factor;
        still_animating = TRUE;
    } else {
        overlay->render_state.current_pill_width = overlay->target_pill_width;
    }

    // 3. Marquee Continuous Animation
    double pad_margin = 10.0;
    double cur_pill_w = overlay->render_state.current_pill_width - (pad_margin * 2.0);
    double tgt_pill_w = overlay->target_pill_width - (pad_margin * 2.0);
    double view_w_cur = renderer_get_view_width(overlay->renderer, &overlay->current_track, cur_pill_w);
    double view_w_tgt = renderer_get_view_width(overlay->renderer, &overlay->current_track, tgt_pill_w);

    if (overlay->is_active && !overlay->is_not_playing &&
        (renderer_needs_marquee(overlay->renderer, &overlay->current_track, view_w_cur) ||
         renderer_needs_marquee(overlay->renderer, &overlay->current_track, view_w_tgt))) {
        still_animating = TRUE;
    }

    overlay_redraw(overlay);

    if (still_animating) {
        return G_SOURCE_CONTINUE;
    } else {
        overlay->anim_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
}

static void start_animation(GlassOverlay *overlay) {
    if (overlay->anim_timer_id == 0) {
        overlay->last_anim_time_us = g_get_monotonic_time();
        overlay->anim_timer_id = g_timeout_add(16, on_animation_step, overlay); // ~60fps
    }
}

// Update Wayland input region so clicks inside the pill are captured, outside clicks pass through
static void update_input_region(GlassOverlay *overlay,
                                double pill_x, double pill_y,
                                double pill_w, double pill_h) {
    if (!overlay->surface || !overlay->compositor) return;

    struct wl_region *in_reg = wl_compositor_create_region(overlay->compositor);

    if (overlay->is_active && overlay->render_state.slide_progress > 0.05 &&
        overlay->render_state.alpha > 0.05 && pill_w > 5.0 && pill_h > 5.0) {
        int in_x = (int)floor(pill_x);
        int in_y = (int)floor(pill_y);
        int in_w = (int)ceil(pill_w);
        int in_h = (int)ceil(pill_h);
        if (in_x < 0) in_x = 0;
        if (in_y < 0) in_y = 0;
        if (in_x + in_w > MAX_SURFACE_WIDTH) in_w = MAX_SURFACE_WIDTH - in_x;
        if (in_y + in_h > SURFACE_HEIGHT) in_h = SURFACE_HEIGHT - in_y;
        if (in_w > 0 && in_h > 0 && in_reg) {
            wl_region_add(in_reg, in_x, in_y, in_w, in_h);
        }
    }

    wl_surface_set_input_region(overlay->surface, in_reg);
    wl_region_destroy(in_reg);
}

static void overlay_redraw(GlassOverlay *overlay) {
    if (!overlay->is_configured || !overlay->surface) return;

    ShmBuffer *b = get_free_buffer(overlay);
    if (!b) return;

    b->busy = TRUE;

    renderer_render(
        overlay->renderer,
        b->shm_data,
        MAX_SURFACE_WIDTH,
        SURFACE_HEIGHT,
        &overlay->current_track,
        overlay->is_not_playing,
        &overlay->render_state
    );

    double pill_x = 0.0, pill_y = 0.0, pill_w = 0.0, pill_h = 0.0;
    renderer_get_pill_rect(&overlay->render_state, MAX_SURFACE_WIDTH, SURFACE_HEIGHT,
                           &pill_x, &pill_y, &pill_w, &pill_h);

    update_input_region(overlay, pill_x, pill_y, pill_w, pill_h);

    wl_surface_attach(overlay->surface, b->wl_buffer, 0, 0);
    wl_surface_damage_buffer(overlay->surface, 0, 0, MAX_SURFACE_WIDTH, SURFACE_HEIGHT);
    wl_surface_commit(overlay->surface);
    wl_display_flush(overlay->display);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    (void)width; (void)height;
    GlassOverlay *overlay = (GlassOverlay *)data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    overlay->is_configured = TRUE;
    overlay_redraw(overlay);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data; (void)surface;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static gboolean is_point_inside_pill(GlassOverlay *overlay, double x, double y) {
    if (!overlay || overlay->render_state.slide_progress <= 0.05 || overlay->render_state.alpha <= 0.05) return FALSE;

    double pill_x = 0.0, pill_y = 0.0, pill_w = 0.0, pill_h = 0.0;
    renderer_get_pill_rect(&overlay->render_state, MAX_SURFACE_WIDTH, SURFACE_HEIGHT,
                           &pill_x, &pill_y, &pill_w, &pill_h);

    if (pill_w <= 5.0 || pill_h <= 5.0) return FALSE;

    if (pill_w <= pill_h) {
        // Circle hit test
        double r = pill_w / 2.0;
        double cx = pill_x + r;
        double cy = pill_y + pill_h / 2.0;
        double dx = x - cx;
        double dy = y - cy;
        return (dx * dx + dy * dy <= r * r);
    }

    double r = pill_h / 2.0;
    double cy = pill_y + r;
    if (y < pill_y || y > pill_y + pill_h) return FALSE;
    if (x < pill_x || x > pill_x + pill_w) return FALSE;

    // Check left semicircle
    if (x < pill_x + r) {
        double dx = (pill_x + r) - x;
        double dy = cy - y;
        return (dx * dx + dy * dy <= r * r);
    }
    // Check right semicircle
    if (x > pill_x + pill_w - r) {
        double dx = x - (pill_x + pill_w - r);
        double dy = cy - y;
        return (dx * dx + dy * dy <= r * r);
    }
    return TRUE;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)pointer; (void)serial; (void)surface;
    GlassOverlay *overlay = (GlassOverlay *)data;
    double x = wl_fixed_to_double(surface_x);
    double y = wl_fixed_to_double(surface_y);
    overlay->render_state.hover_x = x;
    overlay->render_state.hover_y = y;
    gboolean inside = is_point_inside_pill(overlay, x, y);
    if (overlay->render_state.is_hovered != inside) {
        overlay->render_state.is_hovered = inside;
        overlay_redraw(overlay);
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
    (void)pointer; (void)serial; (void)surface;
    GlassOverlay *overlay = (GlassOverlay *)data;
    if (overlay->render_state.is_hovered) {
        overlay->render_state.is_hovered = FALSE;
        overlay_redraw(overlay);
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)pointer; (void)time;
    GlassOverlay *overlay = (GlassOverlay *)data;
    double x = wl_fixed_to_double(surface_x);
    double y = wl_fixed_to_double(surface_y);
    overlay->render_state.hover_x = x;
    overlay->render_state.hover_y = y;
    gboolean inside = is_point_inside_pill(overlay, x, y);
    if (overlay->render_state.is_hovered != inside) {
        overlay->render_state.is_hovered = inside;
        overlay_redraw(overlay);
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer; (void)serial; (void)time;
    GlassOverlay *overlay = (GlassOverlay *)data;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED && overlay->render_state.is_hovered) {
        if (button == BTN_LEFT) {
            if (overlay->mpris) mpris_client_play_pause(overlay->mpris);
        } else if (button == BTN_RIGHT) {
            // Right Click opens Font & Display Options / Settings!
            if (overlay->tray) tray_service_open_font_dialog((TrayService *)overlay->tray);
        } else if (button == BTN_MIDDLE) {
            // Middle Click: Next Track
            if (overlay->mpris) mpris_client_next(overlay->mpris);
        }
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)pointer; (void)time;
    GlassOverlay *overlay = (GlassOverlay *)data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && overlay->render_state.is_hovered) {
        double v = wl_fixed_to_double(value);
        if (v < 0 && overlay->mpris) mpris_client_adjust_volume(overlay->mpris, 0.05);
        if (v > 0 && overlay->mpris) mpris_client_adjust_volume(overlay->mpris, -0.05);
    }
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {
    (void)data; (void)pointer; (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120) {
    (void)data; (void)pointer; (void)axis; (void)value120;
}

static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis, uint32_t direction) {
    (void)data; (void)pointer; (void)axis; (void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    GlassOverlay *overlay = (GlassOverlay *)data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !overlay->pointer) {
        overlay->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(overlay->pointer, &pointer_listener, overlay);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && overlay->pointer) {
        wl_pointer_destroy(overlay->pointer);
        overlay->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    (void)version;
    GlassOverlay *overlay = (GlassOverlay *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        overlay->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        overlay->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        overlay->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 4);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        overlay->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(overlay->seat, &seat_listener, overlay);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static gboolean on_autohide_timeout(gpointer data) {
    GlassOverlay *overlay = (GlassOverlay *)data;
    overlay->autohide_timer_id = 0;

    // If mouse is currently hovering, postpone hiding by 2 more seconds
    if (overlay->render_state.is_hovered) {
        overlay->autohide_timer_id = g_timeout_add_seconds(2, on_autohide_timeout, overlay);
        return G_SOURCE_REMOVE;
    }

    if (overlay->is_not_playing) {
        g_print("[GlassMusic] Auto-hide timer expired in Not Playing state. Exiting overlay...\n");
        overlay->is_active = FALSE;
        start_animation(overlay);
    }
    return G_SOURCE_REMOVE;
}

void overlay_set_track(GlassOverlay *overlay, const MprisTrackInfo *track) {
    if (!overlay) return;

    gboolean has_track = (track && track->title && strlen(track->title) > 0);
    gboolean playing = (track && track->is_playing);
    gboolean new_not_playing = !(has_track && playing);

    gboolean same_title = (g_strcmp0(overlay->current_track.title, track ? track->title : NULL) == 0);
    gboolean same_artist = (g_strcmp0(overlay->current_track.artist, track ? track->artist : NULL) == 0);
    gboolean same_art = (g_strcmp0(overlay->current_track.cached_art_path, track ? track->cached_art_path : NULL) == 0);
    gboolean same_playing = (overlay->current_track.is_playing == (track ? track->is_playing : FALSE));

    if (same_title && same_artist && same_art && same_playing && (overlay->is_not_playing == new_not_playing)) {
        // Redundant MPRIS state update. Skip re-measuring and avoid restarting animation!
        return;
    }

    // If we were already in Not Playing state, and the new state is STILL Not Playing,
    // and the overlay has already timed out and hidden (is_active == FALSE),
    // stay hidden! Do not wake up on background metadata/poll signals.
    if (overlay->is_not_playing && new_not_playing && !overlay->is_active) {
        return;
    }

    g_free(overlay->current_track.player_bus_name);
    g_free(overlay->current_track.title);
    g_free(overlay->current_track.artist);
    g_free(overlay->current_track.album);
    g_free(overlay->current_track.art_url);
    g_free(overlay->current_track.cached_art_path);

    if (track) {
        overlay->current_track.player_bus_name = g_strdup(track->player_bus_name);
        overlay->current_track.title = g_strdup(track->title);
        overlay->current_track.artist = g_strdup(track->artist);
        overlay->current_track.album = g_strdup(track->album);
        overlay->current_track.art_url = g_strdup(track->art_url);
        overlay->current_track.cached_art_path = g_strdup(track->cached_art_path);
        overlay->current_track.is_playing = track->is_playing;
        overlay->current_track.length_us = track->length_us;
    } else {
        memset(&overlay->current_track, 0, sizeof(MprisTrackInfo));
    }

    // Determine if in "Not Playing" state
    overlay->is_not_playing = new_not_playing;
    overlay->render_state.elapsed_time_sec = 0.0; // Reset marquee timer for fresh title

    // Measure target width based on content
    int target_w = 0, target_h = 0;
    renderer_measure_size(overlay->renderer, &overlay->current_track, overlay->is_not_playing, &target_w, &target_h);
    if (target_w < 180) target_w = 180;
    if (target_w > MAX_SURFACE_WIDTH) target_w = MAX_SURFACE_WIDTH;
    overlay->target_pill_width = (double)target_w;
    if (overlay->render_state.current_pill_width <= 0.0) {
        overlay->render_state.current_pill_width = (double)target_w;
    }

    // Clear any existing autohide timer
    if (overlay->autohide_timer_id > 0) {
        g_source_remove(overlay->autohide_timer_id);
        overlay->autohide_timer_id = 0;
    }

    // Overlay is active (visible) when playing OR newly transitioning to Not Playing
    overlay->is_active = TRUE;

    if (overlay->is_not_playing) {
        // Transitioned to Not Playing:
        // If autohide is enabled, start the timer to hide after delay
        if (overlay->config.autohide_not_playing && overlay->config.autohide_delay_sec > 0) {
            overlay->autohide_timer_id = g_timeout_add_seconds(
                overlay->config.autohide_delay_sec,
                on_autohide_timeout,
                overlay
            );
        }
    }

    start_animation(overlay);
    overlay_redraw(overlay);
}

void overlay_update_config(GlassOverlay *overlay, const AppConfig *config) {
    if (!overlay || !config) return;
    overlay->config = *config;
    renderer_update_config(overlay->renderer, config);

    // Update margin with 8px pad_top offset
    if (overlay->layer_surface) {
        int visual_margin = (config->margin_top >= 0) ? config->margin_top : 16;
        int layer_margin = visual_margin - 8;
        if (layer_margin < 0) layer_margin = 0;
        zwlr_layer_surface_v1_set_margin(overlay->layer_surface, layer_margin, 0, 0, 0);
        wl_surface_commit(overlay->surface);
    }

    // Clear any existing autohide timer
    if (overlay->autohide_timer_id > 0) {
        g_source_remove(overlay->autohide_timer_id);
        overlay->autohide_timer_id = 0;
    }

    if (overlay->is_not_playing) {
        if (!overlay->config.autohide_not_playing) {
            // "Always show" mode: ensure overlay is active and visible
            overlay->is_active = TRUE;
            start_animation(overlay);
        } else if (overlay->is_active) {
            // Auto-hide enabled and overlay is currently visible: restart timer
            if (overlay->config.autohide_delay_sec > 0) {
                overlay->autohide_timer_id = g_timeout_add_seconds(
                    overlay->config.autohide_delay_sec,
                    on_autohide_timeout,
                    overlay
                );
            }
        }
    }

    overlay_redraw(overlay);
}

void overlay_set_mpris(GlassOverlay *overlay, MprisClient *mpris) {
    if (!overlay) return;
    overlay->mpris = mpris;
}

void overlay_set_tray(GlassOverlay *overlay, void *tray) {
    if (!overlay) return;
    overlay->tray = (TrayService *)tray;
}

GlassOverlay *overlay_new(const AppConfig *config, MprisClient *mpris) {
    GlassOverlay *overlay = g_new0(GlassOverlay, 1);
    overlay->config = *config;
    overlay->mpris = mpris;
    overlay->renderer = renderer_new(config);

    overlay->display = wl_display_connect(NULL);
    if (!overlay->display) {
        g_printerr("Overlay: Cannot connect to Wayland display\n");
        g_free(overlay);
        return NULL;
    }

    overlay->registry = wl_display_get_registry(overlay->display);
    wl_registry_add_listener(overlay->registry, &registry_listener, overlay);
    wl_display_roundtrip(overlay->display);

    if (!overlay->compositor || !overlay->shm || !overlay->layer_shell) {
        g_printerr("Overlay: Missing required Wayland protocol globals\n");
        overlay_free(overlay);
        return NULL;
    }

    int init_w = 0, init_h = 0;
    renderer_measure_size(overlay->renderer, NULL, TRUE, &init_w, &init_h);
    if (init_w < 180) init_w = 180;
    if (init_w > MAX_SURFACE_WIDTH) init_w = MAX_SURFACE_WIDTH;
    overlay->surface_width = MAX_SURFACE_WIDTH;
    overlay->surface_height = SURFACE_HEIGHT;
    overlay->target_pill_width = (double)init_w;
    overlay->render_state.current_pill_width = (double)init_w;
    overlay->is_not_playing = TRUE;

    overlay->surface = wl_compositor_create_surface(overlay->compositor);
    overlay->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        overlay->layer_shell,
        overlay->surface,
        NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        "glass_music_overlay"
    );

    int visual_margin = (overlay->config.margin_top >= 0) ? overlay->config.margin_top : 16;
    int layer_margin = visual_margin - 8;
    if (layer_margin < 0) layer_margin = 0;

    zwlr_layer_surface_v1_set_size(overlay->layer_surface, MAX_SURFACE_WIDTH, SURFACE_HEIGHT);
    zwlr_layer_surface_v1_set_anchor(overlay->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
    zwlr_layer_surface_v1_set_margin(overlay->layer_surface, layer_margin, 0, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(overlay->layer_surface, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(overlay->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(overlay->layer_surface, &layer_surface_listener, overlay);

    overlay->render_state.slide_progress = 1.0;
    overlay->render_state.alpha = 1.0;
    overlay->is_active = TRUE;

    if (overlay->config.autohide_not_playing && overlay->config.autohide_delay_sec > 0) {
        overlay->autohide_timer_id = g_timeout_add_seconds(
            overlay->config.autohide_delay_sec,
            on_autohide_timeout,
            overlay
        );
    }

    wl_surface_commit(overlay->surface);
    wl_display_roundtrip(overlay->display);

    return overlay;
}

void overlay_free(GlassOverlay *overlay) {
    if (!overlay) return;

    if (overlay->autohide_timer_id > 0) g_source_remove(overlay->autohide_timer_id);
    if (overlay->anim_timer_id > 0) g_source_remove(overlay->anim_timer_id);

    destroy_buffers(overlay);

    if (overlay->layer_surface) zwlr_layer_surface_v1_destroy(overlay->layer_surface);
    if (overlay->surface) wl_surface_destroy(overlay->surface);
    if (overlay->pointer) wl_pointer_destroy(overlay->pointer);

    if (overlay->display) wl_display_disconnect(overlay->display);

    g_free(overlay->current_track.player_bus_name);
    g_free(overlay->current_track.title);
    g_free(overlay->current_track.artist);
    g_free(overlay->current_track.album);
    g_free(overlay->current_track.art_url);
    g_free(overlay->current_track.cached_art_path);

    if (overlay->renderer) renderer_free(overlay->renderer);
    g_free(overlay);
}

int overlay_get_wayland_fd(GlassOverlay *overlay) {
    if (!overlay || !overlay->display) return -1;
    return wl_display_get_fd(overlay->display);
}

void overlay_dispatch_wayland(GlassOverlay *overlay) {
    if (!overlay || !overlay->display) return;
    if (wl_display_dispatch(overlay->display) < 0) {
        g_printerr("[GlassMusic] Wayland dispatch error\n");
    }
    wl_display_flush(overlay->display);
}

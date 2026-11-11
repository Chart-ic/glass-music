#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gdk/gdk.h>

#define PAD_MARGIN 10.0
#define PILL_SIDE_PADDING 32.0
#define ART_TEXT_GAP 14.0
#define DEFAULT_TITLE_FONT "Pretendard, Noto Sans CJK KR, Noto Sans, Sans-Serif Bold 12"
#define DEFAULT_ARTIST_FONT "Pretendard, Inter, Noto Sans CJK KR, Noto Sans, Sans-Serif Bold 9.5"
#define DEFAULT_NOT_PLAYING_FONT "Pretendard, Noto Sans CJK KR, Noto Sans, Sans-Serif Bold 12.5"

struct _PillRenderer {
    AppConfig config;
    GdkPixbuf *cached_pixbuf;
    char *cached_pixbuf_key;
    char *cached_dim_path;
    double cached_dim_w;
    double cached_dim_h;
    PangoFontMap *font_map;
    PangoContext *pango_context;
};

PillRenderer *renderer_new(const AppConfig *config) {
    PillRenderer *r = g_new0(PillRenderer, 1);
    if (config) {
        r->config = *config;
        r->config.title_font = g_strdup(config->title_font);
        r->config.artist_font = g_strdup(config->artist_font);
        r->config.not_playing_font = g_strdup(config->not_playing_font);
        r->config.default_art = g_strdup(config->default_art);
    }
    r->font_map = pango_cairo_font_map_get_default();
    r->pango_context = pango_font_map_create_context(r->font_map);
    return r;
}

void renderer_free(PillRenderer *r) {
    if (!r) return;
    if (r->cached_pixbuf) g_object_unref(r->cached_pixbuf);
    g_free(r->cached_pixbuf_key);
    g_free(r->cached_dim_path);
    g_free(r->config.title_font);
    g_free(r->config.artist_font);
    g_free(r->config.not_playing_font);
    g_free(r->config.default_art);
    if (r->pango_context) g_object_unref(r->pango_context);
    g_free(r);
}

void renderer_update_config(PillRenderer *r, const AppConfig *config) {
    if (!r || !config) return;

    g_free(r->config.title_font);
    g_free(r->config.artist_font);
    g_free(r->config.not_playing_font);
    g_free(r->config.default_art);

    r->config = *config;
    r->config.title_font = g_strdup(config->title_font);
    r->config.artist_font = g_strdup(config->artist_font);
    r->config.not_playing_font = g_strdup(config->not_playing_font);
    r->config.default_art = g_strdup(config->default_art);
}

static void get_art_dimensions(PillRenderer *r, const MprisTrackInfo *track, double target_h, double *out_w, double *out_h) {
    *out_h = target_h;
    *out_w = target_h; // Default square (1:1)

    const char *path = (track && track->cached_art_path) ? track->cached_art_path : NULL;
    if (!path || !path[0]) return;

    // Fast in-memory cache lookup (avoids disk access & glycin IPC 200x per second!)
    if (r && r->cached_dim_path && strcmp(r->cached_dim_path, path) == 0 && r->cached_dim_h == target_h) {
        *out_w = r->cached_dim_w;
        *out_h = r->cached_dim_h;
        return;
    }

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return;

    int orig_w = 0, orig_h = 0;
    if (gdk_pixbuf_get_file_info(path, &orig_w, &orig_h) && orig_h > 0) {
        double aspect = (double)orig_w / (double)orig_h;
        double w = target_h;
        if (aspect > 1.15) {
            // Widescreen thumbnail (16:9, etc.)
            w = target_h * aspect;
            if (w > target_h * 1.80) w = target_h * 1.80; // max ~93px
        } else if (aspect < 0.85) {
            // Portrait
            w = target_h * aspect;
            if (w < target_h * 0.70) w = target_h * 0.70;
        }
        *out_w = w;
        *out_h = target_h;

        if (r) {
            g_free(r->cached_dim_path);
            r->cached_dim_path = g_strdup(path);
            r->cached_dim_w = w;
            r->cached_dim_h = target_h;
        }
    }
}

static GdkPixbuf *get_art_pixbuf(PillRenderer *r, const MprisTrackInfo *track, int target_w, int target_h) {
    const char *path_to_load = NULL;
    if (track && track->cached_art_path && g_file_test(track->cached_art_path, G_FILE_TEST_EXISTS)) {
        path_to_load = track->cached_art_path;
    }

    if (!path_to_load) return NULL;

    char key[512];
    snprintf(key, sizeof(key), "%s@%dx%d", path_to_load, target_w, target_h);

    if (r->cached_pixbuf && r->cached_pixbuf_key && strcmp(r->cached_pixbuf_key, key) == 0) {
        return r->cached_pixbuf;
    }

    if (r->cached_pixbuf) {
        g_object_unref(r->cached_pixbuf);
        r->cached_pixbuf = NULL;
    }
    g_free(r->cached_pixbuf_key);
    r->cached_pixbuf_key = NULL;

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path_to_load, target_w, target_h, FALSE, &err);
    if (pb) {
        r->cached_pixbuf = pb;
        r->cached_pixbuf_key = g_strdup(key);
    } else {
        if (err) g_error_free(err);
    }

    return r->cached_pixbuf;
}

void renderer_measure_size(PillRenderer *r,
                           const MprisTrackInfo *track,
                           gboolean not_playing,
                           int *out_width,
                           int *out_height) {
    int total_h = 92; // 72px inner capsule + 2*10px outer pad
    *out_height = total_h;

    double art_w = 40.0;
    double art_h = 40.0;

    if (not_playing) {
        // "Not Playing" state
        PangoLayout *layout = pango_layout_new(r->pango_context);
        pango_layout_set_text(layout, "Not Playing", -1);
        PangoFontDescription *fd = pango_font_description_from_string(
            r->config.not_playing_font ? r->config.not_playing_font : DEFAULT_NOT_PLAYING_FONT
        );
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);

        int text_w = 0, text_h = 0;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        g_object_unref(layout);

        int total_w = (int)ceil((PAD_MARGIN * 2.0) + (PILL_SIDE_PADDING * 2.0) + art_w + ART_TEXT_GAP + text_w);
        if (total_w < 220) total_w = 220;
        *out_width = total_w;
        return;
    }

    get_art_dimensions(r, track, 40.0, &art_w, &art_h);

    const char *title = (track && track->title && strlen(track->title) > 0) ? track->title : "No Track";
    const char *artist = (track && track->artist && strlen(track->artist) > 0) ? track->artist : "Media Player";

    PangoLayout *layout_title = pango_layout_new(r->pango_context);
    pango_layout_set_text(layout_title, title, -1);
    PangoFontDescription *fd_title = pango_font_description_from_string(
        r->config.title_font ? r->config.title_font : DEFAULT_TITLE_FONT
    );
    pango_layout_set_font_description(layout_title, fd_title);
    pango_font_description_free(fd_title);

    PangoLayout *layout_artist = pango_layout_new(r->pango_context);
    pango_layout_set_text(layout_artist, artist, -1);
    PangoFontDescription *fd_artist = pango_font_description_from_string(
        r->config.artist_font ? r->config.artist_font : DEFAULT_ARTIST_FONT
    );
    pango_layout_set_font_description(layout_artist, fd_artist);
    pango_font_description_free(fd_artist);

    int title_w = 0, title_h = 0;
    int artist_w = 0, artist_h = 0;
    pango_layout_get_pixel_size(layout_title, &title_w, &title_h);
    pango_layout_get_pixel_size(layout_artist, &artist_w, &artist_h);

    g_object_unref(layout_title);
    g_object_unref(layout_artist);

    int max_limit = (r->config.max_text_width > 0) ? r->config.max_text_width : 340;
    int text_w = (title_w > artist_w) ? title_w : artist_w;
    if (text_w > max_limit) text_w = max_limit; // Marquee handles overflow beyond max_limit!
    if (text_w < 80) text_w = 80;

    int total_w = (int)ceil((PAD_MARGIN * 2.0) + (PILL_SIDE_PADDING * 2.0) + art_w + ART_TEXT_GAP + text_w + 2.0);
    if (total_w < 220) total_w = 220;
    if (total_w > 560) total_w = 560;

    *out_width = total_w;
}

double renderer_get_view_width(PillRenderer *r, const MprisTrackInfo *track, double pill_w) {
    double art_w = 40.0, art_h = 40.0;
    get_art_dimensions(r, track, 40.0, &art_w, &art_h);
    double view_w = pill_w - (PILL_SIDE_PADDING * 2.0) - art_w - ART_TEXT_GAP;
    if (view_w < 80.0) view_w = 80.0;
    return view_w;
}

gboolean renderer_needs_marquee(PillRenderer *r, const MprisTrackInfo *track, double view_w) {
    if (!track) return FALSE;

    double limit = (view_w > 0.0) ? view_w : ((r->config.max_text_width > 0) ? r->config.max_text_width : 240.0);

    const char *title_font_str = r->config.title_font ? r->config.title_font : DEFAULT_TITLE_FONT;
    const char *artist_font_str = r->config.artist_font ? r->config.artist_font : DEFAULT_ARTIST_FONT;

    if (track->title && strlen(track->title) > 0) {
        PangoLayout *layout_title = pango_layout_new(r->pango_context);
        pango_layout_set_text(layout_title, track->title, -1);
        PangoFontDescription *fd = pango_font_description_from_string(title_font_str);
        pango_layout_set_font_description(layout_title, fd);
        pango_font_description_free(fd);

        int title_w = 0, title_h = 0;
        pango_layout_get_pixel_size(layout_title, &title_w, &title_h);
        g_object_unref(layout_title);

        if (title_w > (int)limit + 2) return TRUE;
    }

    if (track->artist && strlen(track->artist) > 0) {
        PangoLayout *layout_artist = pango_layout_new(r->pango_context);
        pango_layout_set_text(layout_artist, track->artist, -1);
        PangoFontDescription *fd_a = pango_font_description_from_string(artist_font_str);
        pango_layout_set_font_description(layout_artist, fd_a);
        pango_font_description_free(fd_a);

        int a_w = 0, a_h = 0;
        pango_layout_get_pixel_size(layout_artist, &a_w, &a_h);
        g_object_unref(layout_artist);

        if (a_w > (int)limit + 2) return TRUE;
    }

    return FALSE;
}

void renderer_get_pill_rect(const RenderState *state,
                            int buffer_width,
                            int buffer_height,
                            double *out_x,
                            double *out_y,
                            double *out_w,
                            double *out_h) {
    (void)buffer_height;
    double pad_margin = 10.0;
    double pad_top = 8.0;
    double base_h = 72.0;

    double cur_w = (state && state->current_pill_width > 0) ? state->current_pill_width : 340.0;
    if (cur_w > (double)buffer_width) cur_w = (double)buffer_width;
    double base_pill_w = cur_w - (pad_margin * 2.0);
    if (base_pill_w < 140.0) base_pill_w = 140.0;

    double t = state ? state->slide_progress : 1.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    double scale = 1.0;
    double lift_y = 0.0;

    if (state && !state->is_active) {
        // Exit: Smooth horizontal collapse towards center
        // Starts gentle, accelerates smoothly as it narrows into a point
        double u = 1.0 - t; // 0.0 (full size) -> 1.0 (fully collapsed)
        scale = 1.0 - pow(u, 1.8);
        if (scale < 0.0) scale = 0.0;
        // Subtle 6px upward lift as it contracts
        lift_y = pow(u, 2.0) * 6.0;
    } else if (state && state->is_active && t < 1.0) {
        // Entrance: Smooth horizontal expansion from center
        scale = 1.0 - pow(1.0 - t, 2.8);
        if (scale < 0.0) scale = 0.0;
        if (scale > 1.0) scale = 1.0;
    }

    double pill_w = base_pill_w * scale;
    double pill_h = base_h;
    if (pill_w < base_h) {
        // When width is narrower than 72px, height shrinks proportionally into a circle/point
        pill_h = (pill_w > 0.0) ? pill_w : 0.0;
    }

    double center_x = (double)buffer_width / 2.0;
    double center_y = pad_top + (base_h / 2.0) - lift_y;

    double pill_x = center_x - (pill_w / 2.0);
    double pill_y = center_y - (pill_h / 2.0);

    if (out_x) *out_x = pill_x;
    if (out_y) *out_y = pill_y;
    if (out_w) *out_w = pill_w;
    if (out_h) *out_h = pill_h;
}

static void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static void draw_capsule_pill(cairo_t *cr, double x, double y, double w, double h) {
    if (w <= 0.0 || h <= 0.0) return;
    if (w <= h) {
        // Draw a smooth circle centered in the bounding box
        double r = w / 2.0;
        double cx = x + r;
        double cy = y + h / 2.0;
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2.0 * M_PI);
        cairo_close_path(cr);
        return;
    }
    double r = h / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + r, r, M_PI / 2.0, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

// Draw marquee text with smooth scrolling, seamless loop, and true alpha gradient dissolve
static void draw_marquee_text(cairo_t *cr,
                              PangoLayout *layout,
                              int text_w,
                              double x,
                              double y,
                              double view_w,
                              double time_sec,
                              double speed,
                              double alpha) {
    if (text_w <= (int)view_w + 2) {
        // Normal rendering - text fits completely, no marquee needed
        cairo_save(cr);
        cairo_rectangle(cr, x, y - 2, view_w + 10.0, 30.0);
        cairo_clip(cr);

        cairo_move_to(cr, x + 1.0, y + 1.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45 * alpha);
        pango_cairo_show_layout(cr, layout);

        cairo_move_to(cr, x, y);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0 * alpha);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        return;
    }

    // Marquee scrolling:
    // 1. Initial comfortable pause at reading start (2.5 seconds)
    // 2. Smooth scrolling speed (~26 px/sec)
    // 3. Seamless loop with 60px gap between repetitions
    double gap = 60.0;
    double scroll_dist = text_w + gap;
    double actual_speed = (speed > 10.0 && speed < 120.0) ? speed : 26.0;
    double scroll_time = scroll_dist / actual_speed;
    double pause_start = 2.5;
    double total_period = pause_start + scroll_time;

    double phase = fmod(time_sec, total_period);
    double offset = 0.0;
    if (phase > pause_start) {
        offset = (phase - pause_start) * actual_speed;
    }

    cairo_save(cr);
    cairo_rectangle(cr, x, y - 2, view_w, 30.0);
    cairo_clip(cr);

    // Use cairo group to apply a true alpha mask to the text itself (no opaque dark smudges)
    cairo_push_group(cr);

    // Draw copy 1
    cairo_move_to(cr, x - offset + 1.0, y + 1.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45 * alpha);
    pango_cairo_show_layout(cr, layout);

    cairo_move_to(cr, x - offset, y);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0 * alpha);
    pango_cairo_show_layout(cr, layout);

    // Draw copy 2 (seamless continuation)
    cairo_move_to(cr, x - offset + scroll_dist + 1.0, y + 1.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45 * alpha);
    pango_cairo_show_layout(cr, layout);

    cairo_move_to(cr, x - offset + scroll_dist, y);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0 * alpha);
    pango_cairo_show_layout(cr, layout);

    cairo_pop_group_to_source(cr);

    // Soft Edge Alpha Gradient Mask:
    // - Left edge fades in softly only when text has scrolled (offset > 1.0)
    // - Right edge always fades out softly so text never cuts off abruptly
    cairo_pattern_t *mask_pat = cairo_pattern_create_linear(x, y, x + view_w, y);
    double fade_left_len = 16.0;
    double fade_right_len = 22.0;

    if (offset > 1.0) {
        double left_stop = fade_left_len / view_w;
        if (left_stop > 0.25) left_stop = 0.25;
        cairo_pattern_add_color_stop_rgba(mask_pat, 0.0, 1, 1, 1, 0.0);
        cairo_pattern_add_color_stop_rgba(mask_pat, left_stop, 1, 1, 1, 1.0);
    } else {
        cairo_pattern_add_color_stop_rgba(mask_pat, 0.0, 1, 1, 1, 1.0);
    }

    double right_stop = 1.0 - (fade_right_len / view_w);
    if (right_stop < 0.75) right_stop = 0.75;
    cairo_pattern_add_color_stop_rgba(mask_pat, right_stop, 1, 1, 1, 1.0);
    cairo_pattern_add_color_stop_rgba(mask_pat, 1.0, 1, 1, 1, 0.0);

    cairo_mask(cr, mask_pat);
    cairo_pattern_destroy(mask_pat);

    cairo_restore(cr);
}

void renderer_render(PillRenderer *r,
                     void *shm_data,
                     int buffer_width,
                     int buffer_height,
                     const MprisTrackInfo *track,
                     gboolean not_playing,
                     const RenderState *state) {
    if (!shm_data || buffer_width <= 0 || buffer_height <= 0) return;

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        (unsigned char *)shm_data,
        CAIRO_FORMAT_ARGB32,
        buffer_width,
        buffer_height,
        buffer_width * 4
    );
    cairo_t *cr = cairo_create(surface);

    // Clear transparent
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // If fully hidden, finish early
    if (state->alpha <= 0.001 || (state->slide_progress <= 0.001 && !state->is_active)) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return;
    }

    double pill_x = 0.0, pill_y = 0.0, pill_w = 0.0, pill_h = 0.0;
    renderer_get_pill_rect(state, buffer_width, buffer_height, &pill_x, &pill_y, &pill_w, &pill_h);

    if (pill_w <= 1.0 || pill_h <= 1.0) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return;
    }

    double pad_margin = 10.0;
    double pad_top = 8.0;
    // Base resting dimensions for content layout
    double cur_w = (state && state->current_pill_width > 0) ? state->current_pill_width : buffer_width;
    if (cur_w > buffer_width) cur_w = buffer_width;
    double base_pill_w = cur_w - (pad_margin * 2.0);
    if (base_pill_w < 140.0) base_pill_w = 140.0;
    double base_pill_x = (buffer_width - base_pill_w) / 2.0;
    double base_pill_y = pad_top;

    // ==========================================
    // 1. Drop Shadow (Ambient Depth)
    // ==========================================
    cairo_save(cr);
    for (int s = 5; s >= 1; s--) {
        draw_capsule_pill(cr, pill_x - s * 0.4, pill_y + s * 1.5, pill_w + s * 0.8, pill_h + s * 0.4);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.05 * state->alpha);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    // ==========================================
    // 2. High-Alpha Obsidian Glass Fill
    // ==========================================
    cairo_save(cr);
    draw_capsule_pill(cr, pill_x, pill_y, pill_w, pill_h);

    cairo_pattern_t *glass_pat = cairo_pattern_create_linear(pill_x, pill_y, pill_x, pill_y + pill_h);
    if (state->is_hovered) {
        cairo_pattern_add_color_stop_rgba(glass_pat, 0.0, 0.16, 0.18, 0.22, 0.90 * state->alpha);
        cairo_pattern_add_color_stop_rgba(glass_pat, 1.0, 0.08, 0.10, 0.13, 0.95 * state->alpha);
    } else {
        cairo_pattern_add_color_stop_rgba(glass_pat, 0.0, 0.12, 0.14, 0.17, 0.85 * state->alpha);
        cairo_pattern_add_color_stop_rgba(glass_pat, 1.0, 0.05, 0.06, 0.08, 0.91 * state->alpha);
    }
    cairo_set_source(cr, glass_pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(glass_pat);

    // Inner frosted luster highlight (top edge glow)
    cairo_pattern_t *inner_glow = cairo_pattern_create_linear(pill_x, pill_y, pill_x, pill_y + pill_h * 0.45);
    cairo_pattern_add_color_stop_rgba(inner_glow, 0.0, 1.0, 1.0, 1.0, 0.15 * state->alpha);
    cairo_pattern_add_color_stop_rgba(inner_glow, 1.0, 1.0, 1.0, 1.0, 0.00 * state->alpha);
    cairo_set_source(cr, inner_glow);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(inner_glow);

    // ==========================================
    // 3. Specular Border (Glass Highlight)
    // ==========================================
    cairo_pattern_t *border_pat = cairo_pattern_create_linear(pill_x, pill_y, pill_x, pill_y + pill_h);
    cairo_pattern_add_color_stop_rgba(border_pat, 0.0, 1.0, 1.0, 1.0, 0.35 * state->alpha);
    cairo_pattern_add_color_stop_rgba(border_pat, 0.5, 1.0, 1.0, 1.0, 0.12 * state->alpha);
    cairo_pattern_add_color_stop_rgba(border_pat, 1.0, 1.0, 1.0, 1.0, 0.05 * state->alpha);
    cairo_set_source(cr, border_pat);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    cairo_pattern_destroy(border_pat);
    cairo_restore(cr);

    // ==========================================
    // 4. Inner Content (Clipped to Animated Capsule)
    // ==========================================
    cairo_save(cr);
    draw_capsule_pill(cr, pill_x, pill_y, pill_w, pill_h);
    cairo_clip(cr);

    double content_alpha = state->alpha;
    if (!state->is_active && state->slide_progress < 1.0) {
        double u = 1.0 - state->slide_progress;
        double s = 1.0 - pow(u, 1.4);
        if (s < 0.0) s = 0.0;
        content_alpha = state->alpha * s;
    } else if (state->is_active && state->slide_progress < 1.0) {
        content_alpha = state->alpha * state->slide_progress;
    }

    if (content_alpha > 0.005) {
        double art_h = 40.0;
        double art_w = 40.0;
        double art_r = 6.0;

        if (!not_playing) {
            get_art_dimensions(r, track, art_h, &art_w, &art_h);
        }

        // Layout relative to base pill position, matching pill center vertical movement
        double content_y_offset = (pill_y + pill_h / 2.0) - (base_pill_y + 36.0);
        double art_x = base_pill_x + PILL_SIDE_PADDING;
        double art_y = base_pill_y + content_y_offset + (72.0 - art_h) / 2.0;

        if (not_playing) {
            // "Not Playing" placeholder: frosted translucent square
            cairo_save(cr);
            draw_rounded_rect(cr, art_x, art_y, art_w, art_h, art_r);

            // Subtle translucent etched glass fill
            cairo_pattern_t *np_pat = cairo_pattern_create_linear(art_x, art_y, art_x, art_y + art_h);
            cairo_pattern_add_color_stop_rgba(np_pat, 0.0, 1.0, 1.0, 1.0, 0.16 * content_alpha);
            cairo_pattern_add_color_stop_rgba(np_pat, 1.0, 1.0, 1.0, 1.0, 0.06 * content_alpha);
            cairo_set_source(cr, np_pat);
            cairo_fill_preserve(cr);
            cairo_pattern_destroy(np_pat);

            // 1px border
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25 * content_alpha);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);

            // Subtle musical pause / disc mark
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35 * content_alpha);
            double cx = art_x + art_w / 2.0;
            double cy = art_y + art_h / 2.0;
            cairo_arc(cr, cx, cy, 8.0, 0, 2 * M_PI);
            cairo_stroke(cr);
            cairo_restore(cr);

            // ==========================================
            // 5. "Not Playing" Single Line Text
            // ==========================================
            double text_x = art_x + art_w + ART_TEXT_GAP;
            PangoLayout *layout_np = pango_layout_new(r->pango_context);
            pango_layout_set_text(layout_np, "Not Playing", -1);
            PangoFontDescription *fd_np = pango_font_description_from_string(
                r->config.not_playing_font ? r->config.not_playing_font : DEFAULT_NOT_PLAYING_FONT
            );
            pango_layout_set_font_description(layout_np, fd_np);
            pango_font_description_free(fd_np);

            int np_w = 0, np_h = 0;
            pango_layout_get_pixel_size(layout_np, &np_w, &np_h);
            double np_y = base_pill_y + content_y_offset + (72.0 - np_h) / 2.0;

            // Shadow
            cairo_move_to(cr, text_x + 1.0, np_y + 1.0);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.50 * content_alpha);
            pango_cairo_show_layout(cr, layout_np);

            // Text
            cairo_move_to(cr, text_x, np_y);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * content_alpha);
            pango_cairo_show_layout(cr, layout_np);

            g_object_unref(layout_np);
        } else {
            // Active Song Playing: Dynamic album art
            cairo_save(cr);
            draw_rounded_rect(cr, art_x, art_y, art_w, art_h, art_r);
            cairo_clip(cr);

            GdkPixbuf *pb = get_art_pixbuf(r, track, (int)ceil(art_w), (int)ceil(art_h));
            if (pb) {
                gdk_cairo_set_source_pixbuf(cr, pb, art_x, art_y);
                cairo_paint_with_alpha(cr, content_alpha);
            } else {
                // Gradient vinyl placeholder
                double cx = art_x + art_w / 2.0;
                double cy = art_y + art_h / 2.0;
                cairo_pattern_t *pat = cairo_pattern_create_radial(cx, cy, 2, cx, cy, art_h / 2.0);
                cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.28, 0.28, 0.32, 1.0);
                cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.12, 0.12, 0.15, 1.0);
                cairo_set_source(cr, pat);
                cairo_arc(cr, cx, cy, art_h / 2.0, 0, 2 * M_PI);
                cairo_fill(cr);
                cairo_pattern_destroy(pat);
            }

            // Hover Play/Pause indicator centered on album art
            if (state->is_hovered) {
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.40);
                cairo_paint(cr);

                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
                double icon_cx = art_x + art_w / 2.0;
                double icon_cy = art_y + art_h / 2.0;

                if (track && track->is_playing) {
                    double bar_w = 3.0, bar_h = 12.0, bar_gap = 3.0;
                    cairo_rectangle(cr, icon_cx - bar_gap / 2.0 - bar_w, icon_cy - bar_h / 2.0, bar_w, bar_h);
                    cairo_rectangle(cr, icon_cx + bar_gap / 2.0, icon_cy - bar_h / 2.0, bar_w, bar_h);
                    cairo_fill(cr);
                } else {
                    cairo_move_to(cr, icon_cx - 5.0, icon_cy - 7.0);
                    cairo_line_to(cr, icon_cx + 7.0, icon_cy);
                    cairo_line_to(cr, icon_cx - 5.0, icon_cy + 7.0);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                }
            }
            cairo_restore(cr);

            // Border on album art
            cairo_save(cr);
            draw_rounded_rect(cr, art_x, art_y, art_w, art_h, art_r);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18 * content_alpha);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_restore(cr);

            // ==========================================
            // 5. Typography: Artist & Title (with True Alpha Marquee)
            // ==========================================
            const char *title = (track && track->title && strlen(track->title) > 0) ? track->title : "No Track";
            const char *artist = (track && track->artist && strlen(track->artist) > 0) ? track->artist : "Unknown Artist";

            double text_x = art_x + art_w + ART_TEXT_GAP;
            double view_w = base_pill_w - (text_x - base_pill_x) - PILL_SIDE_PADDING;
            if (view_w < 80.0) view_w = 80.0;

            // Artist Layout
            PangoLayout *layout_artist = pango_layout_new(r->pango_context);
            pango_layout_set_text(layout_artist, artist, -1);
            PangoFontDescription *fd_a = pango_font_description_from_string(
                r->config.artist_font ? r->config.artist_font : DEFAULT_ARTIST_FONT
            );
            pango_layout_set_font_description(layout_artist, fd_a);
            pango_font_description_free(fd_a);

            // Title Layout
            PangoLayout *layout_title = pango_layout_new(r->pango_context);
            pango_layout_set_text(layout_title, title, -1);
            PangoFontDescription *fd_t = pango_font_description_from_string(
                r->config.title_font ? r->config.title_font : DEFAULT_TITLE_FONT
            );
            pango_layout_set_font_description(layout_title, fd_t);
            pango_font_description_free(fd_t);

            int a_w = 0, a_h = 0;
            int t_w = 0, t_h = 0;
            pango_layout_get_pixel_size(layout_artist, &a_w, &a_h);
            pango_layout_get_pixel_size(layout_title, &t_w, &t_h);

            double total_text_h = a_h + t_h + 2.0;
            double text_y_start = base_pill_y + content_y_offset + (72.0 - total_text_h) / 2.0;
            double title_y = text_y_start + a_h + 2.0;

            // Draw Artist (Marquee if longer than view_w)
            draw_marquee_text(
                cr, layout_artist, a_w,
                text_x, text_y_start, view_w,
                state->elapsed_time_sec,
                r->config.marquee_speed,
                content_alpha * 0.90
            );

            // Draw Title (Marquee if longer than view_w)
            draw_marquee_text(
                cr, layout_title, t_w,
                text_x, title_y, view_w,
                state->elapsed_time_sec,
                r->config.marquee_speed,
                content_alpha * 1.0
            );

            g_object_unref(layout_artist);
            g_object_unref(layout_title);
        }
    }

    cairo_restore(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

#ifndef GLASS_MUSIC_RENDERER_H
#define GLASS_MUSIC_RENDERER_H

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "mpris.h"
#include "config.h"

typedef struct {
    double slide_progress;      // 0.0 (hidden above) to 1.0 (fully down)
    double alpha;               // 0.0 to 1.0
    double current_pill_width;  // Smoothly interpolated pill width
    double elapsed_time_sec;    // Monotonic clock time in seconds for marquee
    gboolean is_hovered;
    gboolean is_active;         // TRUE when HUD is active (entrance/visible), FALSE when exiting
    double hover_x;
    double hover_y;
    int buffer_width;
    int buffer_height;
} RenderState;

typedef struct _PillRenderer PillRenderer;

PillRenderer *renderer_new(const AppConfig *config);
void renderer_free(PillRenderer *r);

void renderer_update_config(PillRenderer *r, const AppConfig *config);

// Measure the target width and height for the given track info and state
void renderer_measure_size(PillRenderer *r,
                           const MprisTrackInfo *track,
                           gboolean not_playing,
                           int *out_width,
                           int *out_height);

// Calculate visible width available for text inside the pill
double renderer_get_view_width(PillRenderer *r, const MprisTrackInfo *track, double pill_w);

// Returns TRUE if marquee animation is active (i.e. text is wider than available view width)
gboolean renderer_needs_marquee(PillRenderer *r, const MprisTrackInfo *track, double view_w);

// Compute the pill bounding box accounting for width morphing and entrance/exit animation
void renderer_get_pill_rect(const RenderState *state,
                            int buffer_width,
                            int buffer_height,
                            double *out_x,
                            double *out_y,
                            double *out_w,
                            double *out_h);

// Render the glassmorphism pill into a shared memory ARGB32 buffer
void renderer_render(PillRenderer *r,
                     void *shm_data,
                     int buffer_width,
                     int buffer_height,
                     const MprisTrackInfo *track,
                     gboolean not_playing,
                     const RenderState *state);

#endif /* GLASS_MUSIC_RENDERER_H */

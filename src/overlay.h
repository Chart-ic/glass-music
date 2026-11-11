#ifndef GLASS_MUSIC_OVERLAY_H
#define GLASS_MUSIC_OVERLAY_H

#include <wayland-client.h>
#include <glib.h>
#include "renderer.h"
#include "mpris.h"
#include "config.h"

typedef struct _GlassOverlay GlassOverlay;

GlassOverlay *overlay_new(const AppConfig *config, MprisClient *mpris);
void overlay_set_mpris(GlassOverlay *overlay, MprisClient *mpris);
void overlay_set_tray(GlassOverlay *overlay, void *tray);
void overlay_free(GlassOverlay *overlay);

// Update track information and trigger redraw/resize/animation
void overlay_set_track(GlassOverlay *overlay, const MprisTrackInfo *track);

// Update configuration live (fonts, margin, autohide, etc.)
void overlay_update_config(GlassOverlay *overlay, const AppConfig *config);

// Wayland display fd for GLib event loop
int overlay_get_wayland_fd(GlassOverlay *overlay);
void overlay_dispatch_wayland(GlassOverlay *overlay);

#endif /* GLASS_MUSIC_OVERLAY_H */

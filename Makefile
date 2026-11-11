CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -D_GNU_SOURCE
PKG_DEPS = wayland-client cairo pangocairo gdk-3.0 gtk+-3.0 gio-2.0 glib-2.0

CFLAGS += $(shell pkg-config --cflags $(PKG_DEPS)) -I. -Iprotocols -Isrc
LIBS += $(shell pkg-config --libs $(PKG_DEPS)) -lm

BIN_DIR = bin
BUILD_DIR = build
TARGET = $(BIN_DIR)/glass-music

PROTO_SOURCES = \
    protocols/xdg-shell-protocol.c \
    protocols/wlr-layer-shell-unstable-v1-protocol.c \
    protocols/org-kde-kwin-blur-protocol.c

SRC_SOURCES = \
    src/config.c \
    src/mpris.c \
    src/renderer.c \
    src/overlay.c \
    src/tray.c \
    src/main.c

SOURCES = $(PROTO_SOURCES) $(SRC_SOURCES)
OBJECTS = $(SOURCES:%.c=$(BUILD_DIR)/%.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LIBS)
	@echo "Built successfully: $@"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

protocols:
	@mkdir -p protocols
	wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml protocols/xdg-shell-client-protocol.h
	wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml protocols/xdg-shell-protocol.c
	wayland-scanner client-header protocols/wlr-layer-shell-unstable-v1.xml protocols/wlr-layer-shell-unstable-v1-client-protocol.h
	wayland-scanner private-code protocols/wlr-layer-shell-unstable-v1.xml protocols/wlr-layer-shell-unstable-v1-protocol.c
	wayland-scanner client-header protocols/org-kde-kwin-blur.xml protocols/org-kde-kwin-blur-client-protocol.h
	wayland-scanner private-code protocols/org-kde-kwin-blur.xml protocols/org-kde-kwin-blur-protocol.c

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
APPDIR ?= $(PREFIX)/share/applications
SYSTEMDDIR ?= $(PREFIX)/share/systemd/user

install: $(TARGET)
	@mkdir -p $(DESTDIR)$(BINDIR)
	@install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/glass-music
	@mkdir -p $(DESTDIR)$(APPDIR)
	@install -m 644 glass-music.desktop $(DESTDIR)$(APPDIR)/glass-music.desktop
	@mkdir -p $(DESTDIR)$(SYSTEMDDIR)
	@install -m 644 glass-music.service $(DESTDIR)$(SYSTEMDDIR)/glass-music.service
	@echo "Installed glass-music to $(BINDIR)"

uninstall:
	@rm -f $(DESTDIR)$(BINDIR)/glass-music
	@rm -f $(DESTDIR)$(APPDIR)/glass-music.desktop
	@rm -f $(DESTDIR)$(SYSTEMDDIR)/glass-music.service
	@echo "Uninstalled glass-music"

.PHONY: all protocols clean install uninstall

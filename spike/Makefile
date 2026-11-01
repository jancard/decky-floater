CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
X11_FLAGS := $(shell pkg-config --cflags --libs x11 xext xrender)
DMABUF_FLAGS := $(shell pkg-config --cflags --libs x11 xcomposite xcb xcb-dri3 egl gbm libdrm glesv2)
MIRROR_FLAGS := $(shell pkg-config --cflags --libs x11 xext xrender xcomposite xdamage xfixes)
TARGET := ../out/x11-overlay-probe
DMABUF_TARGET := ../out/xcomposite-dmabuf-probe
MIRROR_TARGET := ../out/mangoapp-mirror-probe

.PHONY: all
all: $(TARGET) $(DMABUF_TARGET) $(MIRROR_TARGET)

$(TARGET): x11-overlay-probe.c
	mkdir -p ../out
	$(CC) $(CFLAGS) $< -o $@ $(X11_FLAGS)

$(DMABUF_TARGET): xcomposite-dmabuf-probe.c
	mkdir -p ../out
	$(CC) $(CFLAGS) $< -o $@ $(DMABUF_FLAGS)

$(MIRROR_TARGET): mangoapp-mirror-probe.c
	mkdir -p ../out
	$(CC) $(CFLAGS) $< -o $@ $(MIRROR_FLAGS)
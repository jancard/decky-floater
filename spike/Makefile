CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
X11_FLAGS := $(shell pkg-config --cflags --libs x11 xext xrender)
DMABUF_FLAGS := $(shell pkg-config --cflags --libs x11 xcomposite xcb xcb-dri3 egl gbm libdrm glesv2)
TARGET := ../out/x11-overlay-probe
DMABUF_TARGET := ../out/xcomposite-dmabuf-probe

.PHONY: all
all: $(TARGET) $(DMABUF_TARGET)

$(TARGET): x11-overlay-probe.c
	mkdir -p ../out
	$(CC) $(CFLAGS) $< -o $@ $(X11_FLAGS)

$(DMABUF_TARGET): xcomposite-dmabuf-probe.c
	mkdir -p ../out
	$(CC) $(CFLAGS) $< -o $@ $(DMABUF_FLAGS)
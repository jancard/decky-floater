CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11
PKGS := libpipewire-0.3 x11 xext xrender xcomposite xdamage xfixes
OUT := out
HELPER := $(OUT)/pip-overlay
CORE_SRC := src/protocol.c src/layout.c

.PHONY: all
all: $(HELPER)

$(HELPER): src/pip-overlay.c $(CORE_SRC) src/protocol.h src/layout.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) src/pip-overlay.c $(CORE_SRC) -o $@ $(shell pkg-config --cflags --libs $(PKGS)) -lm

.PHONY: test
test: $(OUT)/test_protocol
	./$(OUT)/test_protocol

$(OUT)/test_protocol: tests/test_protocol.c $(CORE_SRC) src/protocol.h src/layout.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -Isrc tests/test_protocol.c $(CORE_SRC) -o $@ -lm

.PHONY: clean
clean:
	rm -rf $(OUT)

#pragma once

#include "protocol.h"

struct pip_size {
    unsigned int width;
    unsigned int height;
};

struct pip_rect {
    int x;
    int y;
    unsigned int width;
    unsigned int height;
};

/* Capture size for a Picture in Picture window covering scale of the screen width, even dimensions. */
struct pip_size pip_capture_size(struct pip_size screen, double scale);

/* Place a frame in the requested corner with a margin proportional to the screen height. */
struct pip_rect pip_place(struct pip_size screen, struct pip_size frame, enum pip_corner corner);

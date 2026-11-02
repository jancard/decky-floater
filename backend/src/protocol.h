#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum pip_corner {
    PIP_CORNER_TOP_LEFT,
    PIP_CORNER_TOP_RIGHT,
    PIP_CORNER_BOTTOM_LEFT,
    PIP_CORNER_BOTTOM_RIGHT,
};

enum pip_verb {
    PIP_VERB_SHOW,
    PIP_VERB_SET,
    PIP_VERB_HIDE,
    PIP_VERB_QUIT,
};

struct pip_settings {
    enum pip_corner corner;
    double scale;
    double opacity;
    unsigned int fps;
};

struct pip_command {
    enum pip_verb verb;
    uint32_t appid;
    bool has_appid;
    bool has_corner;
    bool has_scale;
    bool has_opacity;
    bool has_fps;
    struct pip_settings settings;
};

#define PIP_SCALE_MIN 0.10
#define PIP_SCALE_MAX 0.50
#define PIP_OPACITY_MIN 0.20
#define PIP_OPACITY_MAX 1.00
#define PIP_FPS_MIN 1u
#define PIP_FPS_MAX 60u
#define PIP_MAX_LINE 512

static const struct pip_settings pip_default_settings = {
    .corner = PIP_CORNER_BOTTOM_RIGHT,
    .scale = 0.25,
    .opacity = 1.0,
    .fps = 30,
};

/*
 * One command per line: a verb followed by key=value pairs.
 *   show appid=N [corner=top-left|top-right|bottom-left|bottom-right] [scale=F] [opacity=F] [fps=N]
 *   set [corner=...] [scale=F] [opacity=F] [fps=N]
 *   hide
 *   quit
 * Returns 0 on success, -1 with a message in error on failure.
 */
int pip_parse_command(const char *line, struct pip_command *command, char *error, size_t error_size);

void pip_apply_settings(struct pip_settings *settings, const struct pip_command *command);

const char *pip_corner_name(enum pip_corner corner);

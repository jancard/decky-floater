#include "protocol.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const corner_names[] = {
    [PIP_CORNER_TOP_LEFT] = "top-left",
    [PIP_CORNER_TOP_RIGHT] = "top-right",
    [PIP_CORNER_BOTTOM_LEFT] = "bottom-left",
    [PIP_CORNER_BOTTOM_RIGHT] = "bottom-right",
};

const char *pip_corner_name(enum pip_corner corner)
{
    return corner_names[corner];
}

static int fail(char *error, size_t error_size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
    return -1;
}

static int parse_unsigned(const char *text, unsigned long long max, unsigned long long *value)
{
    if (*text < '0' || *text > '9')
        return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > max)
        return -1;
    *value = parsed;
    return 0;
}

static int parse_double(const char *text, double min, double max, double *value)
{
    if ((*text < '0' || *text > '9') && *text != '.')
        return -1;
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno != 0 || *end != '\0' || !isfinite(parsed) || parsed < min || parsed > max)
        return -1;
    *value = parsed;
    return 0;
}

static int parse_pair(char *token, struct pip_command *command, char *error, size_t error_size)
{
    char *equals = strchr(token, '=');
    if (equals == NULL || equals == token || equals[1] == '\0')
        return fail(error, error_size, "expected key=value, got '%s'", token);
    *equals = '\0';
    const char *key = token;
    const char *value = equals + 1;
    unsigned long long number = 0;

    if (strcmp(key, "appid") == 0) {
        if (command->verb != PIP_VERB_SHOW)
            return fail(error, error_size, "appid is only valid for show");
        if (parse_unsigned(value, UINT32_MAX, &number) < 0 || number == 0)
            return fail(error, error_size, "invalid appid '%s'", value);
        command->appid = (uint32_t)number;
        command->has_appid = true;
    } else if (strcmp(key, "corner") == 0) {
        size_t index = 0;
        while (index < sizeof(corner_names) / sizeof(corner_names[0]) &&
               strcmp(value, corner_names[index]) != 0)
            index++;
        if (index == sizeof(corner_names) / sizeof(corner_names[0]))
            return fail(error, error_size, "invalid corner '%s'", value);
        command->settings.corner = (enum pip_corner)index;
        command->has_corner = true;
    } else if (strcmp(key, "scale") == 0) {
        if (parse_double(value, PIP_SCALE_MIN, PIP_SCALE_MAX, &command->settings.scale) < 0)
            return fail(error, error_size, "scale must be %.2f-%.2f", PIP_SCALE_MIN, PIP_SCALE_MAX);
        command->has_scale = true;
    } else if (strcmp(key, "opacity") == 0) {
        if (parse_double(value, PIP_OPACITY_MIN, PIP_OPACITY_MAX, &command->settings.opacity) < 0)
            return fail(error, error_size, "opacity must be %.2f-%.2f", PIP_OPACITY_MIN, PIP_OPACITY_MAX);
        command->has_opacity = true;
    } else if (strcmp(key, "fps") == 0) {
        if (parse_unsigned(value, PIP_FPS_MAX, &number) < 0 || number < PIP_FPS_MIN)
            return fail(error, error_size, "fps must be %u-%u", PIP_FPS_MIN, PIP_FPS_MAX);
        command->settings.fps = (unsigned int)number;
        command->has_fps = true;
    } else {
        return fail(error, error_size, "unknown key '%s'", key);
    }
    return 0;
}

int pip_parse_command(const char *line, struct pip_command *command, char *error, size_t error_size)
{
    char buffer[PIP_MAX_LINE];
    size_t length = strlen(line);
    if (length >= sizeof(buffer))
        return fail(error, error_size, "command too long");
    memcpy(buffer, line, length + 1);

    memset(command, 0, sizeof(*command));
    command->settings = pip_default_settings;

    char *saveptr = NULL;
    char *verb = strtok_r(buffer, " \t\r\n", &saveptr);
    if (verb == NULL)
        return fail(error, error_size, "empty command");

    if (strcmp(verb, "show") == 0)
        command->verb = PIP_VERB_SHOW;
    else if (strcmp(verb, "set") == 0)
        command->verb = PIP_VERB_SET;
    else if (strcmp(verb, "hide") == 0)
        command->verb = PIP_VERB_HIDE;
    else if (strcmp(verb, "quit") == 0)
        command->verb = PIP_VERB_QUIT;
    else
        return fail(error, error_size, "unknown command '%s'", verb);

    char *token;
    while ((token = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
        if (command->verb == PIP_VERB_HIDE || command->verb == PIP_VERB_QUIT)
            return fail(error, error_size, "%s takes no arguments", verb);
        if (parse_pair(token, command, error, error_size) < 0)
            return -1;
    }

    if (command->verb == PIP_VERB_SHOW && !command->has_appid)
        return fail(error, error_size, "show requires appid");
    return 0;
}

void pip_apply_settings(struct pip_settings *settings, const struct pip_command *command)
{
    if (command->has_corner)
        settings->corner = command->settings.corner;
    if (command->has_scale)
        settings->scale = command->settings.scale;
    if (command->has_opacity)
        settings->opacity = command->settings.opacity;
    if (command->has_fps)
        settings->fps = command->settings.fps;
}

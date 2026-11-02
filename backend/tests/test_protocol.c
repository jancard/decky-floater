#include "layout.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static int parse(const char *line, struct pip_command *command, char *error)
{
    return pip_parse_command(line, command, error, 128);
}

static void test_show(void)
{
    struct pip_command command;
    char error[128];
    CHECK(parse("show appid=3609786786 corner=top-left scale=0.3 opacity=0.8 fps=60 border=4\n", &command, error) == 0);
    CHECK(command.verb == PIP_VERB_SHOW);
    CHECK(command.appid == 3609786786u);
    CHECK(command.settings.border == 4 && command.has_border);
    CHECK(command.settings.corner == PIP_CORNER_TOP_LEFT);
    CHECK(command.settings.scale == 0.3);
    CHECK(command.settings.opacity == 0.8);
    CHECK(command.settings.fps == 60);

    CHECK(parse("show appid=769", &command, error) == 0);
    CHECK(!command.has_corner && !command.has_scale && !command.has_opacity && !command.has_fps);
}

static void test_rejects(void)
{
    struct pip_command command;
    char error[128];
    const char *bad[] = {
        "", "   ", "dance", "show", "show appid=0", "show appid=4294967296", "show appid=-1",
        "show appid=12x", "show appid=1 corner=middle", "show appid=1 scale=0.9",
        "show appid=1 scale=nan", "show appid=1 opacity=0.1", "show appid=1 fps=0",
        "show appid=1 fps=61", "show appid=1 colour=red", "show appid=1 corner",
        "show appid=1 border=21", "show appid=1 border=-1",
        "show appid=1 =x", "set appid=5", "hide now", "quit 1",
    };
    for (size_t index = 0; index < sizeof(bad) / sizeof(bad[0]); index++) {
        error[0] = '\0';
        if (parse(bad[index], &command, error) == 0) {
            fprintf(stderr, "accepted bad command '%s'\n", bad[index]);
            failures++;
        }
        CHECK(error[0] != '\0');
    }

    char long_line[PIP_MAX_LINE + 8];
    memset(long_line, 'a', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = '\0';
    CHECK(parse(long_line, &command, error) < 0);
}

static void test_set_and_apply(void)
{
    struct pip_command command;
    char error[128];
    struct pip_settings settings = pip_default_settings;
    CHECK(parse("set corner=bottom-left fps=15", &command, error) == 0);
    CHECK(command.verb == PIP_VERB_SET);
    pip_apply_settings(&settings, &command);
    CHECK(settings.corner == PIP_CORNER_BOTTOM_LEFT);
    CHECK(settings.fps == 15);
    CHECK(settings.scale == pip_default_settings.scale);
    CHECK(settings.opacity == pip_default_settings.opacity);

    CHECK(parse("hide", &command, error) == 0 && command.verb == PIP_VERB_HIDE);
    CHECK(parse("apps", &command, error) == 0 && command.verb == PIP_VERB_APPS);
    CHECK(parse("apps all", &command, error) < 0);
    CHECK(parse("quit\r\n", &command, error) == 0 && command.verb == PIP_VERB_QUIT);
    CHECK(strcmp(pip_corner_name(PIP_CORNER_TOP_RIGHT), "top-right") == 0);
}

static void test_layout(void)
{
    struct pip_size uhd = { 3840, 2160 };
    struct pip_size size = pip_capture_size(uhd, 0.25);
    CHECK(size.width == 960 && size.height == 540);

    struct pip_size deck = { 1280, 800 };
    size = pip_capture_size(deck, 0.25);
    CHECK(size.width == 320 && size.height == 200);
    size = pip_capture_size((struct pip_size){ 1366, 768 }, 0.33);
    CHECK(size.width % 2 == 0 && size.height % 2 == 0);

    struct pip_size frame = { 960, 540 };
    struct pip_rect rect = pip_place(uhd, frame, PIP_CORNER_BOTTOM_RIGHT);
    CHECK(rect.x == 3840 - 960 - 54 && rect.y == 2160 - 540 - 54 - 86);
    rect = pip_place(uhd, frame, PIP_CORNER_TOP_LEFT);
    CHECK(rect.x == 54 && rect.y == 54);
    rect = pip_place((struct pip_size){ 100, 100 }, (struct pip_size){ 200, 200 }, PIP_CORNER_BOTTOM_RIGHT);
    CHECK(rect.x == 0 && rect.y == 0);
}

int main(void)
{
    test_show();
    test_rejects();
    test_set_and_apply();
    test_layout();
    if (failures == 0)
        printf("all protocol/layout tests passed\n");
    return failures == 0 ? 0 : 1;
}

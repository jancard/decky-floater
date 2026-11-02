#include "layout.h"

#include <math.h>

static unsigned int even(double value)
{
    unsigned int rounded = (unsigned int)lround(value);
    rounded &= ~1u;
    return rounded < 2 ? 2 : rounded;
}

struct pip_size pip_capture_size(struct pip_size screen, double scale)
{
    unsigned int width = even(screen.width * scale);
    unsigned int height = even((double)width * screen.height / screen.width);
    return (struct pip_size){ width, height };
}

struct pip_rect pip_place(struct pip_size screen, struct pip_size frame, enum pip_corner corner)
{
    int margin = (int)lround(screen.height * 0.025);
    // Steam's footer button hints sit along the bottom edge; keep bottom corners clear of them.
    int footer = (int)lround(screen.height * 0.04);
    bool left = corner == PIP_CORNER_TOP_LEFT || corner == PIP_CORNER_BOTTOM_LEFT;
    bool top = corner == PIP_CORNER_TOP_LEFT || corner == PIP_CORNER_TOP_RIGHT;
    int x = left ? margin : (int)screen.width - (int)frame.width - margin;
    int y = top ? margin : (int)screen.height - (int)frame.height - margin - footer;
    return (struct pip_rect){ x < 0 ? 0 : x, y < 0 ? 0 : y, frame.width, frame.height };
}

#define _POSIX_C_SOURCE 200809L

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#define PIP_WIDTH 640
#define PIP_HEIGHT 360
#define PIP_MARGIN 48
#define MANGOAPP_TITLE "mangoapp overlay window"
#define REPORT_SECONDS 5.0
#define HASH_WIDTH 960
#define HASH_HEIGHT 540

static volatile sig_atomic_t stop_requested;
static unsigned long ignored_x_errors;

struct mirror {
    Window window;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    int viewable;
    int overlay_enabled;
    Picture picture;
    Damage damage;
};

struct hash_state {
    unsigned long samples;
    unsigned long changes;
    uint64_t last;
};

struct probe {
    Display *display;
    Window root;
    Window window;
    Picture picture;
    unsigned int width;
    unsigned int height;
    Atom overlay_atom;
    int damage_event;
    int pip_x;
    int pip_y;
    XserverRegion damage_parts;
    XserverRegion pending;
    int full_repaint;
    struct mirror mango;
    int drawn_x;
    int drawn_y;
    unsigned int drawn_width;
    unsigned int drawn_height;
    unsigned long damage_events;
    unsigned long copies;
    unsigned long long copied_pixels;
    unsigned long raises;
    struct hash_state source_hash;
    struct hash_state copy_hash;
    double last_cpu;
};

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int on_x_error(Display *display, XErrorEvent *error)
{
    (void)display;
    ignored_x_errors++;
    if (ignored_x_errors <= 10)
        fprintf(stderr, "Ignored X error %u (request %u.%u)\n",
            error->error_code, error->request_code, error->minor_code);
    return 0;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int find_argb_visual(Display *display, int screen, XVisualInfo *result)
{
    XVisualInfo template = {
        .screen = screen,
        .depth = 32,
        .class = TrueColor,
    };
    int count = 0;
    XVisualInfo *visuals = XGetVisualInfo(display,
        VisualScreenMask | VisualDepthMask | VisualClassMask, &template, &count);
    if (visuals == NULL)
        return 0;

    int found = 0;
    for (int index = 0; index < count; index++) {
        XRenderPictFormat *format = XRenderFindVisualFormat(display, visuals[index].visual);
        if (format != NULL && format->direct.alphaMask != 0) {
            *result = visuals[index];
            found = 1;
            break;
        }
    }
    XFree(visuals);
    return found;
}

static int get_cardinal(Display *display, Window window, Atom atom, unsigned long *value)
{
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(display, window, atom, 0, 1, False, XA_CARDINAL,
            &type, &format, &count, &remaining, &data) != Success || data == NULL)
        return 0;

    int found = type == XA_CARDINAL && format == 32 && count == 1;
    if (found)
        *value = *(unsigned long *)data;
    XFree(data);
    return found;
}

static Window find_mangoapp(struct probe *probe)
{
    Window root_return = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(probe->display, probe->root, &root_return, &parent, &children, &count))
        return None;

    Window result = None;
    for (unsigned int index = 0; index < count && result == None; index++) {
        if (children[index] == probe->window)
            continue;
        char *name = NULL;
        if (XFetchName(probe->display, children[index], &name) && name != NULL) {
            if (strcmp(name, MANGOAPP_TITLE) == 0)
                result = children[index];
            XFree(name);
        }
    }
    if (children != NULL)
        XFree(children);
    return result;
}

static int mangoapp_is_above_probe(struct probe *probe)
{
    Window root_return = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(probe->display, probe->root, &root_return, &parent, &children, &count))
        return 0;

    long probe_index = -1;
    long mango_index = -1;
    for (unsigned int index = 0; index < count; index++) {
        if (children[index] == probe->window)
            probe_index = index;
        else if (children[index] == probe->mango.window)
            mango_index = index;
    }
    if (children != NULL)
        XFree(children);
    return probe_index >= 0 && mango_index > probe_index;
}

static void release_mirror(struct probe *probe)
{
    struct mirror *mango = &probe->mango;
    if (mango->damage != None)
        XDamageDestroy(probe->display, mango->damage);
    if (mango->picture != None)
        XRenderFreePicture(probe->display, mango->picture);
    mango->damage = None;
    mango->picture = None;
}

static void refresh_mirror(struct probe *probe)
{
    struct mirror *mango = &probe->mango;
    release_mirror(probe);
    if (mango->window == None)
        return;

    XWindowAttributes attributes;
    if (!XGetWindowAttributes(probe->display, mango->window, &attributes)) {
        mango->window = None;
        return;
    }
    mango->x = attributes.x;
    mango->y = attributes.y;
    mango->width = (unsigned int)attributes.width;
    mango->height = (unsigned int)attributes.height;
    mango->viewable = attributes.map_state == IsViewable;

    unsigned long enabled = 0;
    mango->overlay_enabled = get_cardinal(probe->display, mango->window,
        probe->overlay_atom, &enabled) && enabled != 0;
    if (!mango->viewable || !mango->overlay_enabled)
        return;

    XRenderPictFormat *format = XRenderFindVisualFormat(probe->display, attributes.visual);
    if (format == NULL)
        return;

    // A window source follows Present flips; a named pixmap stays on the first buffer.
    XRenderPictureAttributes picture_attributes = {0};
    mango->picture = XRenderCreatePicture(probe->display, mango->window, format, 0,
        &picture_attributes);
    mango->damage = XDamageCreate(probe->display, mango->window, XDamageReportNonEmpty);
    probe->full_repaint = 1;
}

static void track_mangoapp(struct probe *probe, Window window)
{
    probe->mango.window = window;
    XSelectInput(probe->display, window, PropertyChangeMask);
    refresh_mirror(probe);
    fprintf(stderr, "Tracking mangoapp window 0x%lx\n", window);
}

static void draw_pip(struct probe *probe)
{
    XRenderColor fill = { .red = 0xb000, .green = 0x1000, .blue = 0x0400, .alpha = 0xffff };
    XRenderColor border = { .red = 0xffff, .green = 0xffff, .blue = 0xffff, .alpha = 0xffff };
    int x = probe->pip_x;
    int y = probe->pip_y;
    XRenderFillRectangle(probe->display, PictOpSrc, probe->picture, &border,
        x - 4, y - 4, PIP_WIDTH + 8, PIP_HEIGHT + 8);
    XRenderFillRectangle(probe->display, PictOpSrc, probe->picture, &fill,
        x, y, PIP_WIDTH, PIP_HEIGHT);
}

static void repaint(struct probe *probe)
{
    struct mirror *mango = &probe->mango;
    int same_rect = mango->picture != None && probe->drawn_width == mango->width &&
        probe->drawn_height == mango->height && probe->drawn_x == mango->x &&
        probe->drawn_y == mango->y;

    if (!same_rect && probe->drawn_width != 0) {
        XRenderColor clear = {0};
        XRenderFillRectangle(probe->display, PictOpSrc, probe->picture, &clear,
            probe->drawn_x, probe->drawn_y, probe->drawn_width, probe->drawn_height);
        probe->drawn_width = 0;
        probe->drawn_height = 0;
    }

    if (mango->picture != None) {
        // Src replaces our pixels, so transparent mangoapp areas stay transparent without a pre-clear.
        if (!same_rect || probe->full_repaint) {
            XRenderComposite(probe->display, PictOpSrc, mango->picture, None, probe->picture,
                0, 0, 0, 0, mango->x, mango->y, mango->width, mango->height);
            probe->copied_pixels += (unsigned long long)mango->width * mango->height;
        } else {
            int count = 0;
            XRectangle *rects = XFixesFetchRegion(probe->display, probe->pending, &count);
            for (int index = 0; index < count; index++) {
                XRenderComposite(probe->display, PictOpSrc, mango->picture, None, probe->picture,
                    rects[index].x, rects[index].y, 0, 0,
                    mango->x + rects[index].x, mango->y + rects[index].y,
                    rects[index].width, rects[index].height);
                probe->copied_pixels += (unsigned long long)rects[index].width * rects[index].height;
            }
            if (rects != NULL)
                XFree(rects);
        }
        probe->drawn_x = mango->x;
        probe->drawn_y = mango->y;
        probe->drawn_width = mango->width;
        probe->drawn_height = mango->height;
        probe->copies++;
    }
    XFixesSetRegion(probe->display, probe->pending, NULL, 0);
    probe->full_repaint = 0;

    draw_pip(probe);
}

static void sample_drawable(Display *display, Drawable drawable, int x, int y,
    unsigned int width, unsigned int height, struct hash_state *state)
{
    XImage *image = XGetImage(display, drawable, x, y, width, height, AllPlanes, ZPixmap);
    if (image == NULL)
        return;

    uint64_t hash = 1469598103934665603ULL;
    size_t size = (size_t)image->bytes_per_line * (size_t)image->height;
    for (size_t index = 0; index < size; index++) {
        hash ^= (unsigned char)image->data[index];
        hash *= 1099511628211ULL;
    }
    XDestroyImage(image);

    state->samples++;
    if (hash != state->last) {
        state->changes++;
        state->last = hash;
    }
}

static void sample_hash(struct probe *probe)
{
    struct mirror *mango = &probe->mango;
    if (mango->picture == None || mango->width == 0 || mango->height == 0)
        return;

    unsigned int width = mango->width < HASH_WIDTH ? mango->width : HASH_WIDTH;
    unsigned int height = mango->height < HASH_HEIGHT ? mango->height : HASH_HEIGHT;
    sample_drawable(probe->display, mango->window, 0, 0, width, height, &probe->source_hash);
    sample_drawable(probe->display, probe->window, mango->x, mango->y, width, height,
        &probe->copy_hash);
}

static double cpu_seconds(void)
{
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6 +
        (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
}

static void report(struct probe *probe, double elapsed)
{
    struct mirror *mango = &probe->mango;
    double cpu = cpu_seconds();
    fprintf(stderr,
        "[%6.1fs] mangoapp=0x%lx viewable=%d overlay=%d geometry=%ux%u+%d+%d mirrored=%d"
        " | damage=%lu copies=%lu mpix=%.1f raises=%lu source_changes=%lu/%lu"
        " copy_changes=%lu/%lu cpu=%.1f%% x_errors=%lu\n",
        elapsed, mango->window, mango->viewable, mango->overlay_enabled,
        mango->width, mango->height, mango->x, mango->y, mango->picture != None,
        probe->damage_events, probe->copies, (double)probe->copied_pixels / 1e6, probe->raises,
        probe->source_hash.changes, probe->source_hash.samples,
        probe->copy_hash.changes, probe->copy_hash.samples,
        100.0 * (cpu - probe->last_cpu) / REPORT_SECONDS, ignored_x_errors);
    probe->last_cpu = cpu;
    probe->damage_events = 0;
    probe->copies = 0;
    probe->copied_pixels = 0;
    probe->raises = 0;
    probe->source_hash.samples = 0;
    probe->source_hash.changes = 0;
    probe->copy_hash.samples = 0;
    probe->copy_hash.changes = 0;
}

static Window event_window(const XEvent *event)
{
    switch (event->type) {
    case CreateNotify:
        return event->xcreatewindow.window;
    case MapNotify:
        return event->xmap.window;
    case UnmapNotify:
        return event->xunmap.window;
    case ConfigureNotify:
        return event->xconfigure.window;
    case DestroyNotify:
        return event->xdestroywindow.window;
    case PropertyNotify:
        return event->xproperty.window;
    default:
        return None;
    }
}

static int handle_event(struct probe *probe, XEvent *event)
{
    struct mirror *mango = &probe->mango;

    if (event->type == probe->damage_event + XDamageNotify) {
        XDamageNotifyEvent *damage = (XDamageNotifyEvent *)event;
        if (mango->damage == None || damage->damage != mango->damage)
            return 0;
        XDamageSubtract(probe->display, mango->damage, None, probe->damage_parts);
        XFixesUnionRegion(probe->display, probe->pending, probe->pending, probe->damage_parts);
        probe->damage_events++;
        return 1;
    }

    if (event->type == Expose)
        return event->xexpose.window == probe->window;

    Window window = event_window(event);
    if (window == None || window == probe->window)
        return 0;

    int needs_repaint = 0;
    if (mango->window == None && (event->type == MapNotify || event->type == PropertyNotify)) {
        Window found = find_mangoapp(probe);
        if (found != None) {
            track_mangoapp(probe, found);
            needs_repaint = 1;
        }
    } else if (window == mango->window) {
        if (event->type == DestroyNotify) {
            release_mirror(probe);
            mango->window = None;
            mango->viewable = 0;
            mango->overlay_enabled = 0;
            fprintf(stderr, "mangoapp window destroyed\n");
        } else if (event->type != PropertyNotify ||
                   event->xproperty.atom == probe->overlay_atom) {
            refresh_mirror(probe);
        }
        needs_repaint = 1;
    }

    if ((event->type == MapNotify || event->type == ConfigureNotify) &&
        mango->window != None && mangoapp_is_above_probe(probe)) {
        XRaiseWindow(probe->display, probe->window);
        probe->raises++;
    }
    return needs_repaint;
}

int main(int argc, char **argv)
{
    double duration = argc > 1 ? strtod(argv[1], NULL) : 0.0;

    struct sigaction action = { .sa_handler = on_signal };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    struct probe probe = {0};
    probe.display = XOpenDisplay(NULL);
    if (probe.display == NULL) {
        fprintf(stderr, "Cannot open the configured X11 display\n");
        return 1;
    }
    XSetErrorHandler(on_x_error);

    int screen = DefaultScreen(probe.display);
    XVisualInfo visual_info;
    if (!find_argb_visual(probe.display, screen, &visual_info)) {
        fprintf(stderr, "No 32-bit ARGB visual is available\n");
        XCloseDisplay(probe.display);
        return 1;
    }

    int event_base;
    int error_base;
    int damage_error;
    int major = 0;
    int minor = 2;
    if (!XShapeQueryExtension(probe.display, &event_base, &error_base) ||
        !XRenderQueryExtension(probe.display, &event_base, &error_base) ||
        !XCompositeQueryExtension(probe.display, &event_base, &error_base) ||
        !XCompositeQueryVersion(probe.display, &major, &minor) ||
        (major == 0 && minor < 2) ||
        !XDamageQueryExtension(probe.display, &probe.damage_event, &damage_error) ||
        !XFixesQueryExtension(probe.display, &event_base, &error_base)) {
        fprintf(stderr, "A required X extension (Shape, Render, Composite 0.2, Damage, XFixes) is unavailable\n");
        XCloseDisplay(probe.display);
        return 1;
    }

    probe.root = RootWindow(probe.display, screen);
    probe.width = (unsigned int)DisplayWidth(probe.display, screen);
    probe.height = (unsigned int)DisplayHeight(probe.display, screen);
    if (argc > 2 && strcmp(argv[2], "center") == 0) {
        probe.pip_x = ((int)probe.width - PIP_WIDTH) / 2;
        probe.pip_y = ((int)probe.height - PIP_HEIGHT) / 2;
    } else {
        probe.pip_x = (int)probe.width - PIP_WIDTH - PIP_MARGIN;
        probe.pip_y = (int)probe.height - PIP_HEIGHT - PIP_MARGIN;
    }
    probe.overlay_atom = XInternAtom(probe.display, "GAMESCOPE_EXTERNAL_OVERLAY", False);
    probe.damage_parts = XFixesCreateRegion(probe.display, NULL, 0);
    probe.pending = XFixesCreateRegion(probe.display, NULL, 0);
    probe.last_cpu = cpu_seconds();
    XSelectInput(probe.display, probe.root, SubstructureNotifyMask);

    Colormap colormap = XCreateColormap(probe.display, probe.root, visual_info.visual, AllocNone);
    XSetWindowAttributes attributes = {
        .colormap = colormap,
        .border_pixel = 0,
        .background_pixel = 0,
        .override_redirect = True,
        .event_mask = ExposureMask,
    };
    probe.window = XCreateWindow(probe.display, probe.root, 0, 0, probe.width, probe.height, 0,
        visual_info.depth, InputOutput, visual_info.visual,
        CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask,
        &attributes);
    XStoreName(probe.display, probe.window, "Floater Picture in Picture mangoapp mirror probe");
    XWMHints hints = { .flags = InputHint, .input = False };
    XSetWMHints(probe.display, probe.window, &hints);

    unsigned long enabled = 1;
    XChangeProperty(probe.display, probe.window, probe.overlay_atom, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&enabled, 1);
    XShapeCombineRectangles(probe.display, probe.window, ShapeInput, 0, 0,
        NULL, 0, ShapeSet, Unsorted);

    probe.picture = XRenderCreatePicture(probe.display, probe.window,
        XRenderFindVisualFormat(probe.display, visual_info.visual), 0,
        &(XRenderPictureAttributes){0});

    XMapRaised(probe.display, probe.window);
    Window found = find_mangoapp(&probe);
    if (found != None)
        track_mangoapp(&probe, found);
    repaint(&probe);
    XFlush(probe.display);

    fprintf(stderr, "Mirror probe 0x%lx on %ux%u root, Picture in Picture at %dx%d+%d+%d\n",
        probe.window, probe.width, probe.height, PIP_WIDTH, PIP_HEIGHT, probe.pip_x, probe.pip_y);

    double start = monotonic_seconds();
    double next_sample = start + 1.0;
    double next_report = start + REPORT_SECONDS;
    while (!stop_requested) {
        double now = monotonic_seconds();
        if (duration > 0.0 && now - start >= duration)
            break;
        if (now >= next_sample) {
            sample_hash(&probe);
            next_sample += 1.0;
        }
        if (now >= next_report) {
            if (probe.mango.window == None) {
                found = find_mangoapp(&probe);
                if (found != None) {
                    track_mangoapp(&probe, found);
                    repaint(&probe);
                }
            }
            report(&probe, now - start);
            next_report += REPORT_SECONDS;
        }

        if (!XPending(probe.display)) {
            struct pollfd descriptor = { .fd = ConnectionNumber(probe.display), .events = POLLIN };
            poll(&descriptor, 1, 100);
        }

        int needs_repaint = 0;
        while (XPending(probe.display)) {
            XEvent event;
            XNextEvent(probe.display, &event);
            needs_repaint |= handle_event(&probe, &event);
        }
        if (needs_repaint)
            repaint(&probe);
        XFlush(probe.display);
    }

    release_mirror(&probe);
    XFixesDestroyRegion(probe.display, probe.damage_parts);
    XFixesDestroyRegion(probe.display, probe.pending);
    XRenderFreePicture(probe.display, probe.picture);
    XDestroyWindow(probe.display, probe.window);
    XFreeColormap(probe.display, colormap);
    XCloseDisplay(probe.display);
    fprintf(stderr, "Mirror probe stopped\n");
    return 0;
}

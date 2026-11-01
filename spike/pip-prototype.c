#define _GNU_SOURCE

#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#define GAMESCOPE_REQUESTED_SIZE 0x70000
#define GAMESCOPE_FOCUS_APPID 0x70001
#define PIP_MARGIN 48
#define PIP_BORDER 4
#define MANGOAPP_TITLE "mangoapp overlay window"
#define REPORT_SECONDS 5

static unsigned long ignored_x_errors;

struct mirror {
    Window window;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    Picture picture;
    Damage damage;
};

struct frame {
    unsigned int width;
    unsigned int height;
    XShmSegmentInfo shm;
    XImage *image;
    Pixmap pixmap;
    Picture picture;
    GC gc;
    int has_content;
};

struct stats {
    unsigned long buffers;
    unsigned long presented;
    unsigned long changed;
    unsigned long rejected;
    double present_total_us;
    double present_max_us;
    double gap_max_ms;
    unsigned long mango_copies;
    unsigned long long mango_pixels;
    unsigned long raises;
};

struct prototype {
    uint64_t appid;
    unsigned int requested_width;
    unsigned int requested_height;
    unsigned int duration;

    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_source *x11_source;
    struct spa_source *tick_timer;
    struct spa_source *stop_timer;
    struct spa_video_info_raw format;
    int stopping;
    int failed;

    Display *display;
    Window root;
    Window window;
    Picture picture;
    unsigned int width;
    unsigned int height;
    Atom overlay_atom;
    Atom focused_app_atom;
    int damage_event;
    XserverRegion damage_parts;
    XserverRegion pending;
    XserverRegion pip_region;
    XserverRegion mango_clip;
    int mango_full_repaint;
    struct mirror mango;
    struct frame frame;

    int pip_x;
    int pip_y;
    int pip_visible;
    int pip_drawn;
    int overlay_mapped;
    uint32_t focused_appid;

    uint64_t last_hash;
    double last_buffer_time;
    double start_time;
    double last_cpu;
    unsigned int ticks;
    struct stats stats;
};

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

static double cpu_seconds(void)
{
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6 +
        (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
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

static int find_argb_visual(Display *display, int screen, XVisualInfo *result)
{
    XVisualInfo template = { .screen = screen, .depth = 32, .class = TrueColor };
    int count = 0;
    XVisualInfo *visuals = XGetVisualInfo(display,
        VisualScreenMask | VisualDepthMask | VisualClassMask, &template, &count);
    if (visuals == NULL)
        return 0;

    int found = 0;
    for (int index = 0; index < count && !found; index++) {
        XRenderPictFormat *format = XRenderFindVisualFormat(display, visuals[index].visual);
        if (format != NULL && format->direct.alphaMask != 0) {
            *result = visuals[index];
            found = 1;
        }
    }
    XFree(visuals);
    return found;
}

/* Picture in Picture */

static void update_pip_region(struct prototype *proto)
{
    XRectangle rect = {
        .x = (short)(proto->pip_x - PIP_BORDER),
        .y = (short)(proto->pip_y - PIP_BORDER),
        .width = (unsigned short)(proto->frame.width + 2 * PIP_BORDER),
        .height = (unsigned short)(proto->frame.height + 2 * PIP_BORDER),
    };
    XFixesSetRegion(proto->display, proto->pip_region, &rect,
        proto->pip_visible && proto->frame.image != NULL ? 1 : 0);
}

static void draw_pip(struct prototype *proto)
{
    Display *display = proto->display;
    int x = proto->pip_x;
    int y = proto->pip_y;
    unsigned int width = proto->frame.width;
    unsigned int height = proto->frame.height;
    if (proto->frame.image == NULL)
        return;

    if (!proto->pip_visible) {
        if (proto->pip_drawn) {
            XRenderColor clear = {0};
            XRenderFillRectangle(display, PictOpSrc, proto->picture, &clear,
                x - PIP_BORDER, y - PIP_BORDER, width + 2 * PIP_BORDER, height + 2 * PIP_BORDER);
            proto->pip_drawn = 0;
        }
        return;
    }

    XRenderColor border = { .red = 0xffff, .green = 0xffff, .blue = 0xffff, .alpha = 0xffff };
    XRenderFillRectangle(display, PictOpSrc, proto->picture, &border,
        x - PIP_BORDER, y - PIP_BORDER, width + 2 * PIP_BORDER, height + 2 * PIP_BORDER);
    if (proto->frame.has_content) {
        XRenderComposite(display, PictOpSrc, proto->frame.picture, None, proto->picture,
            0, 0, 0, 0, x, y, width, height);
    } else {
        XRenderColor black = { .alpha = 0xffff };
        XRenderFillRectangle(display, PictOpSrc, proto->picture, &black, x, y, width, height);
    }
    proto->pip_drawn = 1;
}

static void set_pip_visible(struct prototype *proto, int visible)
{
    if (proto->pip_visible == visible)
        return;
    proto->pip_visible = visible;
    update_pip_region(proto);
    draw_pip(proto);
    proto->mango_full_repaint = 1;
    fprintf(stderr, "Picture in Picture %s (focused app %u)\n",
        visible ? "shown" : "hidden", proto->focused_appid);
}

static void refresh_focus(struct prototype *proto)
{
    unsigned long focused = 0;
    if (get_cardinal(proto->display, proto->root, proto->focused_app_atom, &focused))
        proto->focused_appid = (uint32_t)focused;
    set_pip_visible(proto, proto->focused_appid != (uint32_t)proto->appid);
}

static void destroy_frame(struct prototype *proto)
{
    struct frame *frame = &proto->frame;
    if (frame->image == NULL)
        return;
    XShmDetach(proto->display, &frame->shm);
    XRenderFreePicture(proto->display, frame->picture);
    XFreeGC(proto->display, frame->gc);
    XFreePixmap(proto->display, frame->pixmap);
    XDestroyImage(frame->image);
    shmdt(frame->shm.shmaddr);
    XSync(proto->display, False);
    memset(frame, 0, sizeof(*frame));
}

static int create_frame(struct prototype *proto, unsigned int width, unsigned int height)
{
    Display *display = proto->display;
    struct frame *frame = &proto->frame;
    int screen = DefaultScreen(display);
    Visual *visual = DefaultVisual(display, screen);
    if (DefaultDepth(display, screen) != 24) {
        fprintf(stderr, "Default visual depth %d is not 24\n", DefaultDepth(display, screen));
        return 0;
    }

    frame->image = XShmCreateImage(display, visual, 24, ZPixmap, NULL, &frame->shm, width, height);
    if (frame->image == NULL)
        return 0;
    frame->shm.shmid = shmget(IPC_PRIVATE,
        (size_t)frame->image->bytes_per_line * height, IPC_CREAT | 0600);
    if (frame->shm.shmid < 0)
        return 0;
    frame->shm.shmaddr = frame->image->data = shmat(frame->shm.shmid, NULL, 0);
    if (frame->shm.shmaddr == (char *)-1) {
        shmctl(frame->shm.shmid, IPC_RMID, NULL);
        return 0;
    }
    frame->shm.readOnly = True;
    XShmAttach(display, &frame->shm);
    XSync(display, False);
    shmctl(frame->shm.shmid, IPC_RMID, NULL);

    frame->pixmap = XCreatePixmap(display, proto->root, width, height, 24);
    frame->gc = XCreateGC(display, frame->pixmap, 0, NULL);
    frame->picture = XRenderCreatePicture(display, frame->pixmap,
        XRenderFindStandardFormat(display, PictStandardRGB24), 0,
        &(XRenderPictureAttributes){0});
    frame->width = width;
    frame->height = height;
    return 1;
}

/* mangoapp mirror */

static Window find_mangoapp(struct prototype *proto)
{
    Window root_return = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(proto->display, proto->root, &root_return, &parent, &children, &count))
        return None;

    Window result = None;
    for (unsigned int index = 0; index < count && result == None; index++) {
        char *name = NULL;
        if (children[index] != proto->window &&
            XFetchName(proto->display, children[index], &name) && name != NULL) {
            if (strcmp(name, MANGOAPP_TITLE) == 0)
                result = children[index];
            XFree(name);
        }
    }
    if (children != NULL)
        XFree(children);
    return result;
}

static int mangoapp_is_above(struct prototype *proto)
{
    Window root_return = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(proto->display, proto->root, &root_return, &parent, &children, &count))
        return 0;

    long own_index = -1;
    long mango_index = -1;
    for (unsigned int index = 0; index < count; index++) {
        if (children[index] == proto->window)
            own_index = index;
        else if (children[index] == proto->mango.window)
            mango_index = index;
    }
    if (children != NULL)
        XFree(children);
    return own_index >= 0 && mango_index > own_index;
}

static void clear_drawn_mango(struct prototype *proto, const struct mirror *previous)
{
    if (previous->width == 0)
        return;
    XRenderColor clear = {0};
    XRenderFillRectangle(proto->display, PictOpSrc, proto->picture, &clear,
        previous->x, previous->y, previous->width, previous->height);
}

static void refresh_mirror(struct prototype *proto)
{
    struct mirror *mango = &proto->mango;
    struct mirror previous = *mango;

    if (mango->damage != None)
        XDamageDestroy(proto->display, mango->damage);
    if (mango->picture != None)
        XRenderFreePicture(proto->display, mango->picture);
    mango->damage = None;
    mango->picture = None;
    mango->width = 0;
    mango->height = 0;

    if (previous.picture != None) {
        clear_drawn_mango(proto, &previous);
        draw_pip(proto);
    }
    if (mango->window == None)
        return;

    XWindowAttributes attributes;
    if (!XGetWindowAttributes(proto->display, mango->window, &attributes)) {
        mango->window = None;
        return;
    }
    unsigned long enabled = 0;
    if (attributes.map_state != IsViewable ||
        !get_cardinal(proto->display, mango->window, proto->overlay_atom, &enabled) || !enabled)
        return;

    XRenderPictFormat *format = XRenderFindVisualFormat(proto->display, attributes.visual);
    if (format == NULL)
        return;

    mango->x = attributes.x;
    mango->y = attributes.y;
    mango->width = (unsigned int)attributes.width;
    mango->height = (unsigned int)attributes.height;
    // A window source follows Present flips; a named pixmap stays on the first buffer.
    mango->picture = XRenderCreatePicture(proto->display, mango->window, format, 0,
        &(XRenderPictureAttributes){0});
    mango->damage = XDamageCreate(proto->display, mango->window, XDamageReportNonEmpty);
    proto->mango_full_repaint = 1;
}

static void track_mangoapp(struct prototype *proto, Window window)
{
    proto->mango.window = window;
    XSelectInput(proto->display, window, PropertyChangeMask);
    refresh_mirror(proto);
    fprintf(stderr, "Tracking mangoapp window 0x%lx\n", window);
}

static void repaint_mango(struct prototype *proto)
{
    struct mirror *mango = &proto->mango;
    if (mango->picture == None) {
        XFixesSetRegion(proto->display, proto->pending, NULL, 0);
        proto->mango_full_repaint = 0;
        return;
    }

    if (proto->mango_full_repaint) {
        XRectangle all = { 0, 0, (unsigned short)mango->width, (unsigned short)mango->height };
        XFixesSetRegion(proto->display, proto->pending, &all, 1);
        proto->mango_full_repaint = 0;
    }
    XFixesTranslateRegion(proto->display, proto->pending, mango->x, mango->y);
    XFixesSubtractRegion(proto->display, proto->pending, proto->pending, proto->pip_region);
    if (proto->mango_clip != None)
        XFixesIntersectRegion(proto->display, proto->pending, proto->pending, proto->mango_clip);

    int count = 0;
    XRectangle *rects = XFixesFetchRegion(proto->display, proto->pending, &count);
    for (int index = 0; index < count; index++) {
        XRenderComposite(proto->display, PictOpSrc, mango->picture, None, proto->picture,
            rects[index].x - mango->x, rects[index].y - mango->y, 0, 0,
            rects[index].x, rects[index].y, rects[index].width, rects[index].height);
        proto->stats.mango_pixels += (unsigned long long)rects[index].width * rects[index].height;
    }
    if (rects != NULL)
        XFree(rects);
    if (count > 0)
        proto->stats.mango_copies++;
    XFixesSetRegion(proto->display, proto->pending, NULL, 0);
}

static Window event_window(const XEvent *event)
{
    switch (event->type) {
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

static int handle_x_event(struct prototype *proto, XEvent *event)
{
    struct mirror *mango = &proto->mango;

    if (event->type == proto->damage_event + XDamageNotify) {
        XDamageNotifyEvent *damage = (XDamageNotifyEvent *)event;
        if (mango->damage == None || damage->damage != mango->damage)
            return 0;
        XDamageSubtract(proto->display, mango->damage, None, proto->damage_parts);
        XFixesUnionRegion(proto->display, proto->pending, proto->pending, proto->damage_parts);
        return 1;
    }
    if (event->type == Expose && event->xexpose.window == proto->window) {
        draw_pip(proto);
        proto->mango_full_repaint = 1;
        return 1;
    }

    Window window = event_window(event);
    if (window == None || window == proto->window)
        return 0;

    if (window == proto->root) {
        if (event->type == PropertyNotify && event->xproperty.atom == proto->focused_app_atom) {
            refresh_focus(proto);
            return 1;
        }
        return 0;
    }

    int needs_repaint = 0;
    if (mango->window == None && event->type == MapNotify) {
        Window found = find_mangoapp(proto);
        if (found != None) {
            track_mangoapp(proto, found);
            needs_repaint = 1;
        }
    } else if (window == mango->window) {
        if (event->type == DestroyNotify)
            mango->window = None;
        if (event->type != PropertyNotify || event->xproperty.atom == proto->overlay_atom)
            refresh_mirror(proto);
        needs_repaint = 1;
    }

    if ((event->type == MapNotify || event->type == ConfigureNotify) &&
        mango->window != None && mangoapp_is_above(proto)) {
        XRaiseWindow(proto->display, proto->window);
        proto->stats.raises++;
    }
    return needs_repaint;
}

static void process_x_events(struct prototype *proto)
{
    int needs_repaint = 0;
    while (XPending(proto->display)) {
        XEvent event;
        XNextEvent(proto->display, &event);
        needs_repaint |= handle_x_event(proto, &event);
    }
    if (needs_repaint)
        repaint_mango(proto);
    XFlush(proto->display);
}

static void on_x11_readable(void *userdata, int fd, uint32_t mask)
{
    (void)fd;
    (void)mask;
    process_x_events(userdata);
}

/* PipeWire */

static void present_buffer(struct prototype *proto, struct spa_buffer *buffer)
{
    struct frame *frame = &proto->frame;
    if (frame->image == NULL || buffer->n_datas == 0) {
        proto->stats.rejected++;
        return;
    }

    struct spa_data *data = &buffer->datas[0];
    struct spa_chunk *chunk = data->chunk;
    size_t row_bytes = (size_t)frame->width * 4;
    if (data->data == NULL || chunk == NULL || (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
        chunk->stride < (int32_t)row_bytes || chunk->offset > data->maxsize ||
        (uint64_t)chunk->stride * frame->height > data->maxsize - chunk->offset) {
        proto->stats.rejected++;
        return;
    }

    double begin = monotonic_seconds();
    const uint8_t *source = (const uint8_t *)data->data + chunk->offset;
    uint8_t *target = (uint8_t *)frame->image->data;
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned int row = 0; row < frame->height; row++) {
        memcpy(target + (size_t)row * frame->image->bytes_per_line,
            source + (size_t)row * chunk->stride, row_bytes);
        if ((row & 15) == 0) {
            const uint8_t *sample = source + (size_t)row * chunk->stride;
            for (size_t column = 0; column < row_bytes; column += 64) {
                hash ^= sample[column];
                hash *= 1099511628211ULL;
            }
        }
    }
    if (hash != proto->last_hash) {
        proto->stats.changed++;
        proto->last_hash = hash;
    }

    if (proto->overlay_mapped) {
        XShmPutImage(proto->display, frame->pixmap, frame->gc, frame->image,
            0, 0, 0, 0, frame->width, frame->height, False);
        frame->has_content = 1;
        if (proto->pip_visible)
            XRenderComposite(proto->display, PictOpSrc, frame->picture, None, proto->picture,
                0, 0, 0, 0, proto->pip_x, proto->pip_y, frame->width, frame->height);
        // The shared segment is rewritten by the next buffer, so wait for the server to read it.
        XSync(proto->display, False);
    }

    double elapsed_us = (monotonic_seconds() - begin) * 1e6;
    proto->stats.presented++;
    proto->stats.present_total_us += elapsed_us;
    if (elapsed_us > proto->stats.present_max_us)
        proto->stats.present_max_us = elapsed_us;
}

static void on_process(void *userdata)
{
    struct prototype *proto = userdata;
    struct pw_buffer *latest = NULL;
    struct pw_buffer *available;
    while ((available = pw_stream_dequeue_buffer(proto->stream)) != NULL) {
        if (latest != NULL)
            pw_stream_queue_buffer(proto->stream, latest);
        latest = available;
    }
    if (latest == NULL)
        return;

    if (!proto->stopping) {
        double now = monotonic_seconds();
        if (proto->last_buffer_time > 0.0) {
            double gap_ms = (now - proto->last_buffer_time) * 1e3;
            if (gap_ms > proto->stats.gap_max_ms)
                proto->stats.gap_max_ms = gap_ms;
        }
        proto->last_buffer_time = now;
        proto->stats.buffers++;
        present_buffer(proto, latest->buffer);
    }
    pw_stream_queue_buffer(proto->stream, latest);
    process_x_events(proto);
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
    struct prototype *proto = userdata;
    if (id != SPA_PARAM_Format || param == NULL)
        return;
    if (spa_format_video_raw_parse(param, &proto->format) < 0 ||
        proto->format.format != SPA_VIDEO_FORMAT_BGRx) {
        fprintf(stderr, "Unexpected Gamescope stream format\n");
        proto->failed = 1;
        pw_main_loop_quit(proto->loop);
        return;
    }

    unsigned int width = proto->format.size.width;
    unsigned int height = proto->format.size.height;
    fprintf(stderr, "Negotiated BGRx %ux%u for AppID %llu\n",
        width, height, (unsigned long long)proto->appid);

    if (width != proto->frame.width || height != proto->frame.height) {
        int visible = proto->pip_visible;
        set_pip_visible(proto, 0);
        destroy_frame(proto);
        if (!create_frame(proto, width, height)) {
            fprintf(stderr, "Cannot create the MIT-SHM frame\n");
            proto->failed = 1;
            pw_main_loop_quit(proto->loop);
            return;
        }
        proto->pip_x = (int)proto->width - (int)width - PIP_MARGIN;
        proto->pip_y = (int)proto->height - (int)height - PIP_MARGIN;
        set_pip_visible(proto, visible);
        process_x_events(proto);
    }

    uint8_t storage[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod *params[] = {
        spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)))
    };
    pw_stream_update_params(proto->stream, params, 1);
}

static void on_state_changed(void *userdata, enum pw_stream_state previous,
    enum pw_stream_state state, const char *error)
{
    (void)previous;
    struct prototype *proto = userdata;
    fprintf(stderr, "PipeWire stream %s\n", pw_stream_state_as_string(state));
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "PipeWire error: %s\n", error ? error : "unknown");
        proto->failed = 1;
        pw_main_loop_quit(proto->loop);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

/* Lifecycle */

static void begin_stop(struct prototype *proto, const char *reason)
{
    if (proto->stopping)
        return;
    proto->stopping = 1;
    if (proto->stream == NULL) {
        pw_main_loop_quit(proto->loop);
        return;
    }
    fprintf(stderr, "Stopping (%s); draining PipeWire for 500 ms\n", reason);
    pw_stream_set_active(proto->stream, false);
    struct timespec drain = { .tv_sec = 0, .tv_nsec = 500000000 };
    pw_loop_update_timer(pw_main_loop_get_loop(proto->loop), proto->stop_timer,
        &drain, NULL, false);
}

static void on_stop_timer(void *userdata, uint64_t expirations)
{
    (void)expirations;
    struct prototype *proto = userdata;
    pw_main_loop_quit(proto->loop);
}

static void on_signal(void *userdata, int signal_number)
{
    begin_stop(userdata, signal_number == SIGINT ? "SIGINT" : "SIGTERM");
}

static void report(struct prototype *proto)
{
    struct stats *stats = &proto->stats;
    double cpu = cpu_seconds();
    fprintf(stderr,
        "[%6.1fs] buffers=%.1f/s changed=%.1f/s presented=%lu rejected=%lu"
        " present_avg=%.0fus present_max=%.0fus gap_max=%.0fms pip=%s focused=%u"
        " | mango=%s copies=%lu mpix=%.1f raises=%lu | cpu=%.1f%% x_errors=%lu\n",
        monotonic_seconds() - proto->start_time,
        stats->buffers / (double)REPORT_SECONDS, stats->changed / (double)REPORT_SECONDS,
        stats->presented, stats->rejected,
        stats->presented ? stats->present_total_us / stats->presented : 0.0,
        stats->present_max_us, stats->gap_max_ms,
        proto->pip_visible ? "shown" : "hidden", proto->focused_appid,
        proto->mango.picture != None ? "mirrored" : "off",
        stats->mango_copies, stats->mango_pixels / 1e6, stats->raises,
        100.0 * (cpu - proto->last_cpu) / REPORT_SECONDS, ignored_x_errors);
    proto->last_cpu = cpu;
    memset(stats, 0, sizeof(*stats));
}

static void on_tick(void *userdata, uint64_t expirations)
{
    (void)expirations;
    struct prototype *proto = userdata;
    proto->ticks++;
    if (proto->mango.window == None) {
        Window found = find_mangoapp(proto);
        if (found != None) {
            track_mangoapp(proto, found);
            repaint_mango(proto);
        }
    }
    if (proto->ticks % REPORT_SECONDS == 0)
        report(proto);
    if (proto->duration > 0 && proto->ticks >= proto->duration)
        begin_stop(proto, "duration reached");
    process_x_events(proto);
}

static int setup_x11(struct prototype *proto)
{
    proto->display = XOpenDisplay(NULL);
    if (proto->display == NULL) {
        fprintf(stderr, "Cannot open the configured X11 display\n");
        return 0;
    }
    XSetErrorHandler(on_x_error);
    Display *display = proto->display;
    int screen = DefaultScreen(display);

    int event_base;
    int error_base;
    int damage_error;
    int major = 0;
    int minor = 2;
    if (!XShmQueryExtension(display) ||
        !XShapeQueryExtension(display, &event_base, &error_base) ||
        !XRenderQueryExtension(display, &event_base, &error_base) ||
        !XCompositeQueryExtension(display, &event_base, &error_base) ||
        !XCompositeQueryVersion(display, &major, &minor) || (major == 0 && minor < 2) ||
        !XDamageQueryExtension(display, &proto->damage_event, &damage_error) ||
        !XFixesQueryExtension(display, &event_base, &error_base)) {
        fprintf(stderr, "A required X extension (MIT-SHM, Shape, Render, Composite 0.2, Damage, XFixes) is unavailable\n");
        return 0;
    }

    XVisualInfo visual_info;
    if (!find_argb_visual(display, screen, &visual_info)) {
        fprintf(stderr, "No 32-bit ARGB visual is available\n");
        return 0;
    }

    proto->root = RootWindow(display, screen);
    proto->width = (unsigned int)DisplayWidth(display, screen);
    proto->height = (unsigned int)DisplayHeight(display, screen);
    proto->overlay_atom = XInternAtom(display, "GAMESCOPE_EXTERNAL_OVERLAY", False);
    proto->focused_app_atom = XInternAtom(display, "GAMESCOPE_FOCUSED_APP", False);
    proto->damage_parts = XFixesCreateRegion(display, NULL, 0);
    proto->pending = XFixesCreateRegion(display, NULL, 0);
    proto->pip_region = XFixesCreateRegion(display, NULL, 0);
    unsigned int clip_width = 0;
    unsigned int clip_height = 0;
    const char *clip = getenv("FLOATER_MANGO_CLIP");
    if (clip != NULL && sscanf(clip, "%ux%u", &clip_width, &clip_height) == 2) {
        XRectangle rect = { 0, 0, (unsigned short)clip_width, (unsigned short)clip_height };
        proto->mango_clip = XFixesCreateRegion(display, &rect, 1);
        fprintf(stderr, "Clipping mangoapp copies to %ux%u+0+0\n", clip_width, clip_height);
    }
    XSelectInput(display, proto->root, SubstructureNotifyMask | PropertyChangeMask);

    XSetWindowAttributes attributes = {
        .colormap = XCreateColormap(display, proto->root, visual_info.visual, AllocNone),
        .border_pixel = 0,
        .background_pixel = 0,
        .override_redirect = True,
        .event_mask = ExposureMask,
    };
    proto->window = XCreateWindow(display, proto->root, 0, 0, proto->width, proto->height, 0,
        visual_info.depth, InputOutput, visual_info.visual,
        CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask,
        &attributes);
    XStoreName(display, proto->window, "Floater Picture in Picture prototype");
    XWMHints hints = { .flags = InputHint, .input = False };
    XSetWMHints(display, proto->window, &hints);
    unsigned long enabled = 1;
    XChangeProperty(display, proto->window, proto->overlay_atom, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&enabled, 1);
    XShapeCombineRectangles(display, proto->window, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
    proto->picture = XRenderCreatePicture(display, proto->window,
        XRenderFindVisualFormat(display, visual_info.visual), 0, &(XRenderPictureAttributes){0});
    if (getenv("FLOATER_NO_OVERLAY") == NULL) {
        XMapRaised(display, proto->window);
        proto->overlay_mapped = 1;
    } else {
        fprintf(stderr, "Overlay window left unmapped; frames are copied but not uploaded\n");
    }

    Window found = find_mangoapp(proto);
    if (found != None)
        track_mangoapp(proto, found);
    XSync(display, False);
    return 1;
}

static int connect_stream(struct prototype *proto)
{
    struct pw_loop *loop = pw_main_loop_get_loop(proto->loop);
    proto->stream = pw_stream_new_simple(loop, "Floater Picture in Picture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_TARGET_OBJECT, "gamescope", NULL), &stream_events, proto);
    if (proto->stream == NULL)
        return 0;

    uint8_t storage[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    struct spa_rectangle preferred = SPA_RECTANGLE(proto->requested_width, proto->requested_height);
    struct spa_rectangle minimum = SPA_RECTANGLE(1, 1);
    struct spa_rectangle maximum = SPA_RECTANGLE(8192, 8192);
    const struct spa_pod *params[] = {
        spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum),
            GAMESCOPE_FOCUS_APPID, SPA_POD_Long((int64_t)proto->appid),
            GAMESCOPE_REQUESTED_SIZE, SPA_POD_Rectangle(&preferred))
    };
    return pw_stream_connect(proto->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_ASYNC,
        params, 1) >= 0;
}

int main(int argc, char **argv)
{
    struct prototype proto = { .requested_width = 640, .requested_height = 360 };
    if (argc < 2 || argc > 5 || sscanf(argv[1], "%" SCNu64, &proto.appid) != 1 ||
        (argc > 2 && sscanf(argv[2], "%u", &proto.duration) != 1) ||
        (argc > 4 && (sscanf(argv[3], "%u", &proto.requested_width) != 1 ||
                      sscanf(argv[4], "%u", &proto.requested_height) != 1)) ||
        proto.requested_width < 16 || proto.requested_width > 1920 ||
        proto.requested_height < 16 || proto.requested_height > 1080) {
        fprintf(stderr, "Usage: %s APPID [SECONDS] [WIDTH HEIGHT]\n", argv[0]);
        return 2;
    }

    if (!setup_x11(&proto))
        return 1;

    pw_init(NULL, NULL);
    proto.loop = pw_main_loop_new(NULL);
    struct pw_loop *loop = pw_main_loop_get_loop(proto.loop);
    proto.x11_source = pw_loop_add_io(loop, ConnectionNumber(proto.display), SPA_IO_IN, false,
        on_x11_readable, &proto);
    proto.tick_timer = pw_loop_add_timer(loop, on_tick, &proto);
    proto.stop_timer = pw_loop_add_timer(loop, on_stop_timer, &proto);
    pw_loop_add_signal(loop, SIGINT, on_signal, &proto);
    pw_loop_add_signal(loop, SIGTERM, on_signal, &proto);
    struct timespec second = { .tv_sec = 1 };
    pw_loop_update_timer(loop, proto.tick_timer, &second, &second, false);

    proto.start_time = monotonic_seconds();
    proto.last_cpu = cpu_seconds();
    refresh_focus(&proto);
    process_x_events(&proto);

    int result = -1;
    if (getenv("FLOATER_NO_CAPTURE") != NULL) {
        fprintf(stderr, "Capture disabled; drawing a static box\n");
        if (create_frame(&proto, proto.requested_width, proto.requested_height)) {
            proto.pip_x = (int)proto.width - (int)proto.requested_width - PIP_MARGIN;
            proto.pip_y = (int)proto.height - (int)proto.requested_height - PIP_MARGIN;
            update_pip_region(&proto);
            draw_pip(&proto);
            process_x_events(&proto);
            result = pw_main_loop_run(proto.loop);
        }
    } else if (connect_stream(&proto)) {
        result = pw_main_loop_run(proto.loop);
    }

    if (proto.stream != NULL)
        pw_stream_destroy(proto.stream);
    pw_loop_destroy_source(loop, proto.x11_source);
    pw_loop_destroy_source(loop, proto.tick_timer);
    pw_loop_destroy_source(loop, proto.stop_timer);
    pw_main_loop_destroy(proto.loop);
    pw_deinit();

    destroy_frame(&proto);
    XDestroyWindow(proto.display, proto.window);
    XCloseDisplay(proto.display);
    fprintf(stderr, "Prototype stopped\n");
    return result < 0 || proto.failed ? 1 : 0;
}

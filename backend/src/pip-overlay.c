#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

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

#include "layout.h"
#include "protocol.h"

#define GAMESCOPE_REQUESTED_SIZE 0x70000
#define GAMESCOPE_FOCUS_APPID 0x70001
#define GAMESCOPE_NODE_NAME "gamescope"
#define MANGOAPP_TITLE "mangoapp overlay window"
#define MAX_HELD_BUFFERS 16
#define MAX_LINKS 64
#define DRAIN_NSEC 500000000L
#define STATS_TICKS 5
#define SOURCE_GONE_TICKS 2
#define HIDDEN_FPS 2

enum capture_state { CAPTURE_OFF, CAPTURE_ON, CAPTURE_DRAINING };
enum after_drain { AFTER_NONE, AFTER_RESTART, AFTER_QUIT };

struct link {
    uint32_t id;
    uint32_t output_node;
    uint32_t input_node;
};

struct frame {
    struct pip_size size;
    XShmSegmentInfo shm;
    XImage *image;
    Pixmap pixmap;
    Picture picture;
    GC gc;
    bool has_content;
};

struct mirror {
    Window window;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    Picture picture;
    Damage damage;
};

struct helper {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    bool registry_ready;
    int sync_seq;
    int restart_seq;
    bool restart_pending;
    uint32_t gamescope_node;
    struct link links[MAX_LINKS];
    size_t link_count;

    struct pw_stream *stream;
    struct spa_hook stream_listener;
    enum capture_state capture;
    enum after_drain after_drain;
    const char *stop_reason;
    struct pw_buffer *held[MAX_HELD_BUFFERS];
    size_t held_count;
    struct spa_source *drain_timer;
    struct spa_source *release_timer;
    struct spa_source *tick_timer;
    struct spa_source *x11_source;
    struct spa_source *stdin_source;
    char input[PIP_MAX_LINE * 2];
    size_t input_length;

    bool active;
    uint32_t appid;
    struct pip_settings settings;
    unsigned int source_missing_ticks;
    unsigned int ticks;
    unsigned long buffers;
    unsigned long presented;
    double last_cpu;

    Display *display;
    Window root;
    Window window;
    Picture picture;
    Picture opacity_mask;
    struct pip_size screen;
    bool window_mapped;
    Atom overlay_atom;
    Atom focused_app_atom;
    Atom focusable_apps_atom;
    int damage_event;
    XserverRegion damage_parts;
    XserverRegion pending;
    XserverRegion pip_region;
    bool mango_full_repaint;
    struct mirror mango;
    struct frame frame;
    struct pip_rect rect;
    bool pip_drawn;
    uint32_t focused_appid;
};

static void stop_capture(struct helper *helper, enum after_drain after, const char *reason);
static void process_x_events(struct helper *helper);
static void update_release_timer(struct helper *helper);

/* Events */

static void emit(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

static void emit_error(const char *message)
{
    char escaped[PIP_MAX_LINE * 2];
    size_t out = 0;
    for (const char *p = message; *p != '\0' && out + 7 < sizeof(escaped); p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            escaped[out++] = '\\';
            escaped[out++] = (char)c;
        } else if (c < 0x20) {
            out += (size_t)snprintf(escaped + out, sizeof(escaped) - out, "\\u%04x", c);
        } else {
            escaped[out++] = (char)c;
        }
    }
    escaped[out] = '\0';
    emit("{\"event\":\"error\",\"message\":\"%s\"}", escaped);
}

static int on_x_error(Display *display, XErrorEvent *error)
{
    (void)display;
    // Windows we track (mangoapp) can vanish between requests; those errors are expected.
    fprintf(stderr, "pip-overlay: ignored X error %u (request %u.%u)\n",
        error->error_code, error->request_code, error->minor_code);
    return 0;
}

static double cpu_seconds(void)
{
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6 +
        (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
}

/* X11 helpers */

static bool get_cardinals(Display *display, Window window, Atom atom, unsigned long *values,
    unsigned long max, unsigned long *count)
{
    Atom type = None;
    int format = 0;
    unsigned long items = 0;
    unsigned long remaining = 0;
    unsigned char *data = NULL;
    *count = 0;
    if (XGetWindowProperty(display, window, atom, 0, (long)max, False, XA_CARDINAL,
            &type, &format, &items, &remaining, &data) != Success || data == NULL)
        return false;
    bool ok = type == XA_CARDINAL && format == 32;
    if (ok) {
        *count = items < max ? items : max;
        memcpy(values, data, *count * sizeof(unsigned long));
    }
    XFree(data);
    return ok;
}

static bool find_argb_visual(Display *display, int screen, XVisualInfo *result)
{
    XVisualInfo template = { .screen = screen, .depth = 32, .class = TrueColor };
    int count = 0;
    XVisualInfo *visuals = XGetVisualInfo(display,
        VisualScreenMask | VisualDepthMask | VisualClassMask, &template, &count);
    if (visuals == NULL)
        return false;
    bool found = false;
    for (int index = 0; index < count && !found; index++) {
        XRenderPictFormat *format = XRenderFindVisualFormat(display, visuals[index].visual);
        if (format != NULL && format->direct.alphaMask != 0) {
            *result = visuals[index];
            found = true;
        }
    }
    XFree(visuals);
    return found;
}

static void clear_rect(struct helper *helper, int x, int y, unsigned int width, unsigned int height)
{
    XRenderColor clear = {0};
    XRenderFillRectangle(helper->display, PictOpSrc, helper->picture, &clear, x, y, width, height);
}

/* Picture in Picture drawing */

static bool pip_visible(const struct helper *helper)
{
    return helper->active && helper->frame.image != NULL && helper->focused_appid != helper->appid;
}

static void update_pip_region(struct helper *helper)
{
    XRectangle rect = {
        (short)helper->rect.x, (short)helper->rect.y,
        (unsigned short)helper->rect.width, (unsigned short)helper->rect.height,
    };
    XFixesSetRegion(helper->display, helper->pip_region, &rect, pip_visible(helper) ? 1 : 0);
}

static void set_opacity(struct helper *helper)
{
    if (helper->opacity_mask != None)
        XRenderFreePicture(helper->display, helper->opacity_mask);
    XRenderColor alpha = { .alpha = (unsigned short)(helper->settings.opacity * 0xffff) };
    helper->opacity_mask = XRenderCreateSolidFill(helper->display, &alpha);
}

static void draw_pip(struct helper *helper)
{
    struct pip_rect *rect = &helper->rect;
    if (!pip_visible(helper)) {
        if (helper->pip_drawn) {
            clear_rect(helper, rect->x, rect->y, rect->width, rect->height);
            helper->pip_drawn = false;
            helper->mango_full_repaint = true;
        }
        return;
    }
    if (helper->frame.has_content) {
        XRenderComposite(helper->display, PictOpSrc, helper->frame.picture, helper->opacity_mask,
            helper->picture, 0, 0, 0, 0, rect->x, rect->y, rect->width, rect->height);
    } else {
        XRenderColor black = { .alpha = (unsigned short)(helper->settings.opacity * 0xffff) };
        XRenderFillRectangle(helper->display, PictOpSrc, helper->picture, &black,
            rect->x, rect->y, rect->width, rect->height);
    }
    helper->pip_drawn = true;
}

static void relayout(struct helper *helper)
{
    if (helper->pip_drawn) {
        clear_rect(helper, helper->rect.x, helper->rect.y, helper->rect.width, helper->rect.height);
        helper->pip_drawn = false;
        helper->mango_full_repaint = true;
    }
    helper->rect = pip_place(helper->screen, helper->frame.size, helper->settings.corner);
    update_pip_region(helper);
    draw_pip(helper);
}

static void refresh_visibility(struct helper *helper)
{
    bool was_drawn = helper->pip_drawn;
    update_pip_region(helper);
    draw_pip(helper);
    if (was_drawn != helper->pip_drawn) {
        emit("{\"event\":\"visible\",\"value\":%s}", helper->pip_drawn ? "true" : "false");
        if (helper->capture == CAPTURE_ON)
            update_release_timer(helper);
    }
}

static void destroy_frame(struct helper *helper)
{
    struct frame *frame = &helper->frame;
    if (frame->image == NULL)
        return;
    XShmDetach(helper->display, &frame->shm);
    XRenderFreePicture(helper->display, frame->picture);
    XFreeGC(helper->display, frame->gc);
    XFreePixmap(helper->display, frame->pixmap);
    XDestroyImage(frame->image);
    shmdt(frame->shm.shmaddr);
    XSync(helper->display, False);
    memset(frame, 0, sizeof(*frame));
}

static bool create_frame(struct helper *helper, struct pip_size size)
{
    Display *display = helper->display;
    struct frame *frame = &helper->frame;
    int screen = DefaultScreen(display);
    if (DefaultDepth(display, screen) != 24)
        return false;

    frame->image = XShmCreateImage(display, DefaultVisual(display, screen), 24, ZPixmap, NULL,
        &frame->shm, size.width, size.height);
    if (frame->image == NULL)
        return false;
    frame->shm.shmid = shmget(IPC_PRIVATE, (size_t)frame->image->bytes_per_line * size.height,
        IPC_CREAT | 0600);
    if (frame->shm.shmid < 0) {
        XDestroyImage(frame->image);
        frame->image = NULL;
        return false;
    }
    frame->shm.shmaddr = frame->image->data = shmat(frame->shm.shmid, NULL, 0);
    shmctl(frame->shm.shmid, IPC_RMID, NULL);
    if (frame->shm.shmaddr == (char *)-1) {
        XDestroyImage(frame->image);
        frame->image = NULL;
        return false;
    }
    frame->shm.readOnly = True;
    XShmAttach(display, &frame->shm);
    frame->pixmap = XCreatePixmap(display, helper->root, size.width, size.height, 24);
    frame->gc = XCreateGC(display, frame->pixmap, 0, NULL);
    frame->picture = XRenderCreatePicture(display, frame->pixmap,
        XRenderFindStandardFormat(display, PictStandardRGB24), 0, &(XRenderPictureAttributes){0});
    frame->size = size;
    XSync(display, False);
    return true;
}

static void map_window(struct helper *helper, bool mapped)
{
    if (helper->window_mapped == mapped)
        return;
    helper->window_mapped = mapped;
    if (mapped) {
        XMapRaised(helper->display, helper->window);
    } else {
        XUnmapWindow(helper->display, helper->window);
        clear_rect(helper, 0, 0, helper->screen.width, helper->screen.height);
        helper->pip_drawn = false;
    }
}

/* mangoapp mirror: Gamescope paints only one external overlay, so we carry mangoapp's pixels. */

static Window find_mangoapp(struct helper *helper)
{
    Window root_return = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(helper->display, helper->root, &root_return, &parent, &children, &count))
        return None;
    Window result = None;
    for (unsigned int index = 0; index < count && result == None; index++) {
        char *name = NULL;
        if (children[index] != helper->window &&
            XFetchName(helper->display, children[index], &name) && name != NULL) {
            if (strcmp(name, MANGOAPP_TITLE) == 0)
                result = children[index];
            XFree(name);
        }
    }
    if (children != NULL)
        XFree(children);
    return result;
}

static bool mangoapp_is_above(struct helper *helper)
{
    Window root_return = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(helper->display, helper->root, &root_return, &parent, &children, &count))
        return false;
    long own_index = -1;
    long mango_index = -1;
    for (unsigned int index = 0; index < count; index++) {
        if (children[index] == helper->window)
            own_index = index;
        else if (children[index] == helper->mango.window)
            mango_index = index;
    }
    if (children != NULL)
        XFree(children);
    return own_index >= 0 && mango_index > own_index;
}

static void refresh_mirror(struct helper *helper)
{
    struct mirror *mango = &helper->mango;
    if (mango->damage != None)
        XDamageDestroy(helper->display, mango->damage);
    if (mango->picture != None) {
        XRenderFreePicture(helper->display, mango->picture);
        clear_rect(helper, mango->x, mango->y, mango->width, mango->height);
        helper->pip_drawn = false;
        draw_pip(helper);
    }
    mango->damage = None;
    mango->picture = None;
    if (mango->window == None)
        return;

    XWindowAttributes attributes;
    if (!XGetWindowAttributes(helper->display, mango->window, &attributes)) {
        mango->window = None;
        return;
    }
    unsigned long enabled = 0;
    unsigned long count = 0;
    if (attributes.map_state != IsViewable ||
        !get_cardinals(helper->display, mango->window, helper->overlay_atom, &enabled, 1, &count) ||
        count != 1 || enabled == 0)
        return;
    XRenderPictFormat *format = XRenderFindVisualFormat(helper->display, attributes.visual);
    if (format == NULL)
        return;

    mango->x = attributes.x;
    mango->y = attributes.y;
    mango->width = (unsigned int)attributes.width;
    mango->height = (unsigned int)attributes.height;
    // A window source follows Present flips; a named pixmap stays on the first buffer.
    mango->picture = XRenderCreatePicture(helper->display, mango->window, format, 0,
        &(XRenderPictureAttributes){0});
    mango->damage = XDamageCreate(helper->display, mango->window, XDamageReportNonEmpty);
    helper->mango_full_repaint = true;
}

static void track_mangoapp(struct helper *helper, Window window)
{
    helper->mango.window = window;
    XSelectInput(helper->display, window, PropertyChangeMask);
    refresh_mirror(helper);
}

static void repaint_mango(struct helper *helper)
{
    struct mirror *mango = &helper->mango;
    if (mango->picture == None || !helper->window_mapped) {
        XFixesSetRegion(helper->display, helper->pending, NULL, 0);
        helper->mango_full_repaint = false;
        return;
    }
    if (helper->mango_full_repaint) {
        XRectangle all = { 0, 0, (unsigned short)mango->width, (unsigned short)mango->height };
        XFixesSetRegion(helper->display, helper->pending, &all, 1);
        helper->mango_full_repaint = false;
    }
    XFixesTranslateRegion(helper->display, helper->pending, mango->x, mango->y);
    XFixesSubtractRegion(helper->display, helper->pending, helper->pending, helper->pip_region);

    int count = 0;
    XRectangle *rects = XFixesFetchRegion(helper->display, helper->pending, &count);
    for (int index = 0; index < count; index++) {
        XRenderComposite(helper->display, PictOpSrc, mango->picture, None, helper->picture,
            rects[index].x - mango->x, rects[index].y - mango->y, 0, 0,
            rects[index].x, rects[index].y, rects[index].width, rects[index].height);
    }
    if (rects != NULL)
        XFree(rects);
    XFixesSetRegion(helper->display, helper->pending, NULL, 0);
}

/* Focus */

static void refresh_focus(struct helper *helper)
{
    unsigned long focused = 0;
    unsigned long count = 0;
    get_cardinals(helper->display, helper->root, helper->focused_app_atom, &focused, 1, &count);
    helper->focused_appid = count == 1 ? (uint32_t)focused : 0;
    refresh_visibility(helper);
}

static bool source_is_running(struct helper *helper)
{
    unsigned long apps[64];
    unsigned long count = 0;
    if (!get_cardinals(helper->display, helper->root, helper->focusable_apps_atom, apps, 64, &count))
        return true;
    for (unsigned long index = 0; index < count; index++) {
        if ((uint32_t)apps[index] == helper->appid)
            return true;
    }
    return false;
}

static void emit_apps(struct helper *helper)
{
    unsigned long apps[64];
    unsigned long count = 0;
    get_cardinals(helper->display, helper->root, helper->focusable_apps_atom, apps, 64, &count);
    char list[64 * 12 + 1] = "";
    size_t used = 0;
    for (unsigned long index = 0; index < count; index++)
        used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%" PRIu32,
            index ? "," : "", (uint32_t)apps[index]);
    unsigned long focused = 0;
    get_cardinals(helper->display, helper->root, helper->focused_app_atom, &focused, 1, &count);
    emit("{\"event\":\"apps\",\"focused\":%" PRIu32 ",\"running\":[%s]}",
        count == 1 ? (uint32_t)focused : 0, list);
}

/* X11 events */

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

static bool handle_x_event(struct helper *helper, XEvent *event)
{
    struct mirror *mango = &helper->mango;
    if (event->type == helper->damage_event + XDamageNotify) {
        XDamageNotifyEvent *damage = (XDamageNotifyEvent *)event;
        if (mango->damage == None || damage->damage != mango->damage)
            return false;
        XDamageSubtract(helper->display, mango->damage, None, helper->damage_parts);
        XFixesUnionRegion(helper->display, helper->pending, helper->pending, helper->damage_parts);
        return true;
    }
    if (event->type == Expose && event->xexpose.window == helper->window) {
        draw_pip(helper);
        helper->mango_full_repaint = true;
        return true;
    }

    Window window = event_window(event);
    if (window == None || window == helper->window)
        return false;

    if (window == helper->root) {
        if (event->type == PropertyNotify && event->xproperty.atom == helper->focused_app_atom) {
            refresh_focus(helper);
            return true;
        }
        if (event->type == ConfigureNotify) {
            struct pip_size screen = { (unsigned int)event->xconfigure.width,
                                       (unsigned int)event->xconfigure.height };
            if (screen.width != helper->screen.width || screen.height != helper->screen.height) {
                helper->screen = screen;
                XResizeWindow(helper->display, helper->window, screen.width, screen.height);
                emit("{\"event\":\"screen\",\"width\":%u,\"height\":%u}", screen.width, screen.height);
                if (helper->capture == CAPTURE_ON)
                    stop_capture(helper, AFTER_RESTART, NULL);
            }
        }
        return false;
    }

    bool needs_repaint = false;
    if (mango->window == None && event->type == MapNotify) {
        Window found = find_mangoapp(helper);
        if (found != None) {
            track_mangoapp(helper, found);
            needs_repaint = true;
        }
    } else if (window == mango->window) {
        if (event->type == DestroyNotify)
            mango->window = None;
        if (event->type != PropertyNotify || event->xproperty.atom == helper->overlay_atom)
            refresh_mirror(helper);
        needs_repaint = true;
    }

    if ((event->type == MapNotify || event->type == ConfigureNotify) && helper->window_mapped &&
        mango->window != None && mangoapp_is_above(helper))
        XRaiseWindow(helper->display, helper->window);
    return needs_repaint;
}

static void process_x_events(struct helper *helper)
{
    bool needs_repaint = false;
    while (XPending(helper->display)) {
        XEvent event;
        XNextEvent(helper->display, &event);
        needs_repaint |= handle_x_event(helper, &event);
    }
    if (needs_repaint || helper->mango_full_repaint)
        repaint_mango(helper);
    XFlush(helper->display);
}

static void on_x11_readable(void *userdata, int fd, uint32_t mask)
{
    (void)fd;
    struct helper *helper = userdata;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        emit("{\"event\":\"stopped\",\"reason\":\"display-closed\"}");
        pw_main_loop_quit(helper->loop);
        return;
    }
    process_x_events(helper);
}

/* PipeWire registry: refuse to share the Gamescope stream with another consumer. */

static uint32_t parse_id(const char *text)
{
    if (text == NULL)
        return SPA_ID_INVALID;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    return end != text && *end == '\0' ? (uint32_t)value : SPA_ID_INVALID;
}

static bool foreign_consumer(struct helper *helper)
{
    uint32_t own = helper->stream != NULL ? pw_stream_get_node_id(helper->stream) : SPA_ID_INVALID;
    // Until our node id is known our own link is indistinguishable; the STREAMING check covers it.
    if (helper->stream != NULL && own == SPA_ID_INVALID)
        return false;
    for (size_t index = 0; index < helper->link_count; index++) {
        const struct link *link = &helper->links[index];
        if (link->output_node == helper->gamescope_node && link->input_node != own) {
            fprintf(stderr, "pip-overlay: link %u from the Gamescope node to node %u (own %u)\n",
                link->id, link->input_node, own);
            return true;
        }
    }
    return false;
}

static void on_registry_global(void *userdata, uint32_t id, uint32_t permissions, const char *type,
    uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    (void)version;
    struct helper *helper = userdata;
    if (props == NULL)
        return;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (name != NULL && strcmp(name, GAMESCOPE_NODE_NAME) == 0 &&
            media_class != NULL && strcmp(media_class, "Video/Source") == 0)
            helper->gamescope_node = id;
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0 && helper->link_count < MAX_LINKS) {
        helper->links[helper->link_count++] = (struct link){
            id,
            parse_id(spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE)),
            parse_id(spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE)),
        };
        if (helper->capture == CAPTURE_ON && foreign_consumer(helper))
            stop_capture(helper, AFTER_NONE, "capture-busy");
    }
}

static void on_registry_global_remove(void *userdata, uint32_t id)
{
    struct helper *helper = userdata;
    if (id == helper->gamescope_node)
        helper->gamescope_node = SPA_ID_INVALID;
    for (size_t index = 0; index < helper->link_count; index++) {
        if (helper->links[index].id == id) {
            helper->links[index] = helper->links[--helper->link_count];
            break;
        }
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void start_capture(struct helper *helper);

static void on_core_done(void *userdata, uint32_t id, int seq)
{
    struct helper *helper = userdata;
    if (id != PW_ID_CORE)
        return;
    if (seq == helper->sync_seq)
        helper->registry_ready = true;
    if (helper->restart_pending && seq == helper->restart_seq) {
        helper->restart_pending = false;
        if (helper->active && helper->capture == CAPTURE_OFF) {
            start_capture(helper);
            process_x_events(helper);
        }
    }
}

static void on_core_error(void *userdata, uint32_t id, int seq, int res, const char *message)
{
    (void)seq;
    struct helper *helper = userdata;
    fprintf(stderr, "pip-overlay: PipeWire error on %u: %s (%d)\n", id, message, res);
    if (id == PW_ID_CORE && res == -EPIPE) {
        emit("{\"event\":\"stopped\",\"reason\":\"pipewire-lost\"}");
        pw_main_loop_quit(helper->loop);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

/* PipeWire stream */

static void release_held(struct helper *helper, size_t count)
{
    while (count-- > 0 && helper->held_count > 0) {
        pw_stream_queue_buffer(helper->stream, helper->held[0]);
        helper->held_count--;
        memmove(helper->held, helper->held + 1, helper->held_count * sizeof(helper->held[0]));
    }
}

static void on_release_timer(void *userdata, uint64_t expirations)
{
    (void)expirations;
    release_held(userdata, 1);
}

static void update_release_timer(struct helper *helper)
{
    // While the source app is focused the box is hidden, so keep only a slow trickle of frames.
    unsigned int fps = pip_visible(helper) ? helper->settings.fps : HIDDEN_FPS;
    long nanoseconds = 1000000000L / fps;
    struct timespec interval = { .tv_sec = nanoseconds / 1000000000L, .tv_nsec = nanoseconds % 1000000000L };
    pw_loop_update_timer(pw_main_loop_get_loop(helper->loop), helper->release_timer,
        &interval, &interval, false);
}

static void present_buffer(struct helper *helper, struct spa_buffer *buffer)
{
    struct frame *frame = &helper->frame;
    if (frame->image == NULL || buffer->n_datas == 0 || !pip_visible(helper))
        return;
    struct spa_data *data = &buffer->datas[0];
    struct spa_chunk *chunk = data->chunk;
    size_t row_bytes = (size_t)frame->size.width * 4;
    if (data->data == NULL || chunk == NULL || (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
        chunk->stride < (int32_t)row_bytes || chunk->offset > data->maxsize ||
        (uint64_t)chunk->stride * frame->size.height > data->maxsize - chunk->offset)
        return;

    const uint8_t *source = (const uint8_t *)data->data + chunk->offset;
    uint8_t *target = (uint8_t *)frame->image->data;
    for (unsigned int row = 0; row < frame->size.height; row++)
        memcpy(target + (size_t)row * frame->image->bytes_per_line,
            source + (size_t)row * chunk->stride, row_bytes);

    XShmPutImage(helper->display, frame->pixmap, frame->gc, frame->image,
        0, 0, 0, 0, frame->size.width, frame->size.height, False);
    bool first = !frame->has_content;
    frame->has_content = true;
    draw_pip(helper);
    // The shared segment is rewritten by the next buffer, so wait for the server to read it.
    XSync(helper->display, False);
    helper->presented++;
    if (first)
        emit("{\"event\":\"streaming\",\"appid\":%" PRIu32 "}", helper->appid);
}

static void on_process(void *userdata)
{
    struct helper *helper = userdata;
    struct pw_buffer *latest = NULL;
    struct pw_buffer *available;
    bool hold = helper->capture == CAPTURE_ON;
    while ((available = pw_stream_dequeue_buffer(helper->stream)) != NULL) {
        if (latest != NULL) {
            if (hold && helper->held_count < MAX_HELD_BUFFERS)
                helper->held[helper->held_count++] = latest;
            else
                pw_stream_queue_buffer(helper->stream, latest);
        }
        latest = available;
    }
    if (latest == NULL)
        return;
    if (hold) {
        helper->buffers++;
        present_buffer(helper, latest->buffer);
    }
    // Gamescope only renders a capture when it can dequeue a free buffer, so returning them at fps throttles it.
    if (hold && helper->held_count < MAX_HELD_BUFFERS)
        helper->held[helper->held_count++] = latest;
    else
        pw_stream_queue_buffer(helper->stream, latest);
    process_x_events(helper);
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
    struct helper *helper = userdata;
    if (id != SPA_PARAM_Format || param == NULL || helper->capture != CAPTURE_ON)
        return;
    struct spa_video_info_raw format;
    if (spa_format_video_raw_parse(param, &format) < 0 || format.format != SPA_VIDEO_FORMAT_BGRx) {
        stop_capture(helper, AFTER_NONE, "unsupported-format");
        return;
    }

    struct pip_size size = { format.size.width, format.size.height };
    if (size.width != helper->frame.size.width || size.height != helper->frame.size.height) {
        bool was_drawn = helper->pip_drawn;
        destroy_frame(helper);
        if (!create_frame(helper, size)) {
            stop_capture(helper, AFTER_NONE, "shared-memory-failed");
            return;
        }
        relayout(helper);
        update_release_timer(helper);
        if (was_drawn != helper->pip_drawn)
            emit("{\"event\":\"visible\",\"value\":%s}", helper->pip_drawn ? "true" : "false");
        process_x_events(helper);
    }

    uint8_t storage[256];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod *params[] = {
        spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))),
    };
    pw_stream_update_params(helper->stream, params, 1);
}

static void on_state_changed(void *userdata, enum pw_stream_state previous,
    enum pw_stream_state state, const char *error)
{
    (void)previous;
    struct helper *helper = userdata;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "pip-overlay: stream error: %s\n", error ? error : "unknown");
        stop_capture(helper, AFTER_NONE, "stream-error");
    } else if (state == PW_STREAM_STATE_STREAMING && helper->capture == CAPTURE_ON &&
               foreign_consumer(helper)) {
        stop_capture(helper, AFTER_NONE, "capture-busy");
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void start_capture(struct helper *helper)
{
    if (helper->gamescope_node == SPA_ID_INVALID) {
        helper->active = false;
        emit("{\"event\":\"stopped\",\"reason\":\"gamescope-stream-missing\"}");
        return;
    }
    if (foreign_consumer(helper)) {
        helper->active = false;
        emit("{\"event\":\"stopped\",\"reason\":\"capture-busy\"}");
        return;
    }

    helper->stream = pw_stream_new(helper->core, "Floater Picture in Picture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_TARGET_OBJECT, GAMESCOPE_NODE_NAME, NULL));
    if (helper->stream == NULL) {
        helper->active = false;
        emit("{\"event\":\"stopped\",\"reason\":\"stream-error\"}");
        return;
    }
    pw_stream_add_listener(helper->stream, &helper->stream_listener, &stream_events, helper);

    struct pip_size size = pip_capture_size(helper->screen, helper->settings.scale);
    struct spa_rectangle preferred = SPA_RECTANGLE(size.width, size.height);
    struct spa_rectangle minimum = SPA_RECTANGLE(1, 1);
    struct spa_rectangle maximum = SPA_RECTANGLE(8192, 8192);
    uint8_t storage[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod *params[] = {
        spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum),
            GAMESCOPE_FOCUS_APPID, SPA_POD_Long((int64_t)helper->appid),
            GAMESCOPE_REQUESTED_SIZE, SPA_POD_Rectangle(&preferred)),
    };
    if (pw_stream_connect(helper->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_ASYNC,
            params, 1) < 0) {
        pw_stream_destroy(helper->stream);
        helper->stream = NULL;
        helper->active = false;
        emit("{\"event\":\"stopped\",\"reason\":\"stream-error\"}");
        return;
    }
    helper->capture = CAPTURE_ON;
    helper->source_missing_ticks = 0;
    map_window(helper, true);
    update_release_timer(helper);
    refresh_focus(helper);
    emit("{\"event\":\"starting\",\"appid\":%" PRIu32 ",\"width\":%u,\"height\":%u}",
        helper->appid, size.width, size.height);
}

static void finish_stop(struct helper *helper)
{
    struct pw_loop *loop = pw_main_loop_get_loop(helper->loop);
    pw_loop_update_timer(loop, helper->release_timer, NULL, NULL, false);
    if (helper->stream != NULL) {
        spa_hook_remove(&helper->stream_listener);
        pw_stream_disconnect(helper->stream);
        pw_stream_destroy(helper->stream);
        helper->stream = NULL;
    }
    helper->capture = CAPTURE_OFF;

    enum after_drain after = helper->after_drain;
    helper->after_drain = AFTER_NONE;
    if (after == AFTER_RESTART && helper->active) {
        helper->stop_reason = NULL;
        // Reconnect once PipeWire has removed our old link, or the busy check would see it.
        helper->restart_pending = true;
        helper->restart_seq = pw_core_sync(helper->core, PW_ID_CORE, 0);
        return;
    }
    helper->active = false;
    destroy_frame(helper);
    map_window(helper, false);
    XFlush(helper->display);
    if (helper->stop_reason != NULL)
        emit("{\"event\":\"stopped\",\"reason\":\"%s\"}", helper->stop_reason);
    helper->stop_reason = NULL;
    if (after == AFTER_QUIT)
        pw_main_loop_quit(helper->loop);
}

static void on_drain_timer(void *userdata, uint64_t expirations)
{
    (void)expirations;
    finish_stop(userdata);
}

// Gamescope asserts if a consumer vanishes mid-frame, so pause the stream and let it drain first.
static void stop_capture(struct helper *helper, enum after_drain after, const char *reason)
{
    if (after > helper->after_drain)
        helper->after_drain = after;
    if (reason != NULL && helper->stop_reason == NULL)
        helper->stop_reason = reason;
    if (helper->capture == CAPTURE_DRAINING)
        return;
    if (helper->capture == CAPTURE_OFF) {
        finish_stop(helper);
        return;
    }
    helper->capture = CAPTURE_DRAINING;
    release_held(helper, MAX_HELD_BUFFERS);
    pw_stream_set_active(helper->stream, false);
    struct timespec drain = { .tv_nsec = DRAIN_NSEC };
    pw_loop_update_timer(pw_main_loop_get_loop(helper->loop), helper->drain_timer, &drain, NULL, false);
}

/* Commands */

static void handle_command(struct helper *helper, const char *line)
{
    struct pip_command command;
    char error[160];
    if (pip_parse_command(line, &command, error, sizeof(error)) < 0) {
        emit_error(error);
        return;
    }

    switch (command.verb) {
    case PIP_VERB_SHOW:
        pip_apply_settings(&helper->settings, &command);
        set_opacity(helper);
        if (helper->active && helper->appid == command.appid) {
            relayout(helper);
            update_release_timer(helper);
            break;
        }
        bool restart = helper->capture != CAPTURE_OFF;
        helper->appid = command.appid;
        helper->active = true;
        if (restart)
            stop_capture(helper, AFTER_RESTART, NULL);
        else
            start_capture(helper);
        break;
    case PIP_VERB_SET: {
        double old_scale = helper->settings.scale;
        pip_apply_settings(&helper->settings, &command);
        set_opacity(helper);
        if (helper->capture != CAPTURE_ON)
            break;
        update_release_timer(helper);
        if (helper->settings.scale != old_scale)
            stop_capture(helper, AFTER_RESTART, NULL);
        else
            relayout(helper);
        break;
    }
    case PIP_VERB_HIDE:
        helper->active = false;
        stop_capture(helper, AFTER_NONE, "hidden");
        break;
    case PIP_VERB_APPS:
        emit_apps(helper);
        break;
    case PIP_VERB_QUIT:
        helper->active = false;
        stop_capture(helper, AFTER_QUIT, "quit");
        break;
    }
    process_x_events(helper);
}

static void on_stdin(void *userdata, int fd, uint32_t mask)
{
    struct helper *helper = userdata;
    if (mask & SPA_IO_IN) {
        ssize_t count = read(fd, helper->input + helper->input_length,
            sizeof(helper->input) - helper->input_length - 1);
        if (count > 0) {
            helper->input_length += (size_t)count;
            helper->input[helper->input_length] = '\0';
            char *newline;
            while ((newline = strchr(helper->input, '\n')) != NULL) {
                *newline = '\0';
                handle_command(helper, helper->input);
                size_t consumed = (size_t)(newline - helper->input) + 1;
                helper->input_length -= consumed;
                memmove(helper->input, newline + 1, helper->input_length + 1);
            }
            if (helper->input_length == sizeof(helper->input) - 1) {
                emit_error("command too long");
                helper->input_length = 0;
            }
            return;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            return;
    }
    // stdin closed: the plugin backend went away, so tear down cleanly.
    pw_loop_update_io(pw_main_loop_get_loop(helper->loop), helper->stdin_source, 0);
    helper->active = false;
    stop_capture(helper, AFTER_QUIT, "quit");
}

static void on_signal(void *userdata, int signal_number)
{
    (void)signal_number;
    struct helper *helper = userdata;
    helper->active = false;
    stop_capture(helper, AFTER_QUIT, "quit");
}

static void on_tick(void *userdata, uint64_t expirations)
{
    (void)expirations;
    struct helper *helper = userdata;
    helper->ticks++;

    if (helper->mango.window == None) {
        Window found = find_mangoapp(helper);
        if (found != None)
            track_mangoapp(helper, found);
    }

    if (helper->capture == CAPTURE_ON) {
        helper->source_missing_ticks = source_is_running(helper) ? 0 : helper->source_missing_ticks + 1;
        if (helper->source_missing_ticks >= SOURCE_GONE_TICKS) {
            helper->active = false;
            stop_capture(helper, AFTER_NONE, "source-closed");
        }
    }

    if (helper->ticks % STATS_TICKS == 0) {
        double cpu = cpu_seconds();
        if (helper->capture == CAPTURE_ON)
            emit("{\"event\":\"stats\",\"buffers_per_second\":%.1f,\"presented\":%lu,\"cpu_percent\":%.1f}",
                helper->buffers / (double)STATS_TICKS, helper->presented,
                100.0 * (cpu - helper->last_cpu) / STATS_TICKS);
        helper->last_cpu = cpu;
        helper->buffers = 0;
        helper->presented = 0;
    }
    process_x_events(helper);
}

/* Setup */

static bool setup_x11(struct helper *helper)
{
    helper->display = XOpenDisplay(NULL);
    if (helper->display == NULL) {
        fprintf(stderr, "pip-overlay: cannot open the X11 display\n");
        return false;
    }
    XSetErrorHandler(on_x_error);
    Display *display = helper->display;
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
        !XDamageQueryExtension(display, &helper->damage_event, &damage_error) ||
        !XFixesQueryExtension(display, &event_base, &error_base)) {
        fprintf(stderr, "pip-overlay: missing X extension (MIT-SHM, Shape, Render, Composite, Damage, XFixes)\n");
        return false;
    }
    XVisualInfo visual_info;
    if (!find_argb_visual(display, screen, &visual_info)) {
        fprintf(stderr, "pip-overlay: no 32-bit ARGB visual\n");
        return false;
    }

    helper->root = RootWindow(display, screen);
    helper->screen = (struct pip_size){ (unsigned int)DisplayWidth(display, screen),
                                        (unsigned int)DisplayHeight(display, screen) };
    helper->overlay_atom = XInternAtom(display, "GAMESCOPE_EXTERNAL_OVERLAY", False);
    helper->focused_app_atom = XInternAtom(display, "GAMESCOPE_FOCUSED_APP", False);
    helper->focusable_apps_atom = XInternAtom(display, "GAMESCOPE_FOCUSABLE_APPS", False);
    helper->damage_parts = XFixesCreateRegion(display, NULL, 0);
    helper->pending = XFixesCreateRegion(display, NULL, 0);
    helper->pip_region = XFixesCreateRegion(display, NULL, 0);
    XSelectInput(display, helper->root, SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);

    XSetWindowAttributes attributes = {
        .colormap = XCreateColormap(display, helper->root, visual_info.visual, AllocNone),
        .border_pixel = 0,
        .background_pixel = 0,
        .override_redirect = True,
        .event_mask = ExposureMask,
    };
    helper->window = XCreateWindow(display, helper->root, 0, 0, helper->screen.width,
        helper->screen.height, 0, visual_info.depth, InputOutput, visual_info.visual,
        CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask, &attributes);
    XStoreName(display, helper->window, "Floater Picture in Picture");
    XWMHints hints = { .flags = InputHint, .input = False };
    XSetWMHints(display, helper->window, &hints);
    unsigned long enabled = 1;
    XChangeProperty(display, helper->window, helper->overlay_atom, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&enabled, 1);
    XShapeCombineRectangles(display, helper->window, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
    helper->picture = XRenderCreatePicture(display, helper->window,
        XRenderFindVisualFormat(display, visual_info.visual), 0, &(XRenderPictureAttributes){0});
    set_opacity(helper);

    Window found = find_mangoapp(helper);
    if (found != None)
        track_mangoapp(helper, found);
    XSync(display, False);
    return true;
}

static bool setup_pipewire(struct helper *helper)
{
    struct pw_loop *loop = pw_main_loop_get_loop(helper->loop);
    helper->context = pw_context_new(loop, NULL, 0);
    if (helper->context == NULL)
        return false;
    helper->core = pw_context_connect(helper->context, NULL, 0);
    if (helper->core == NULL) {
        fprintf(stderr, "pip-overlay: cannot connect to PipeWire\n");
        return false;
    }
    pw_core_add_listener(helper->core, &helper->core_listener, &core_events, helper);
    helper->registry = pw_core_get_registry(helper->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(helper->registry, &helper->registry_listener, &registry_events, helper);
    helper->sync_seq = pw_core_sync(helper->core, PW_ID_CORE, 0);

    pw_loop_enter(loop);
    for (int attempt = 0; attempt < 50 && !helper->registry_ready; attempt++)
        pw_loop_iterate(loop, 100);
    pw_loop_leave(loop);
    return helper->registry_ready;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    static struct helper helper = {
        .gamescope_node = SPA_ID_INVALID,
    };
    helper.settings = pip_default_settings;

    if (!setup_x11(&helper))
        return 1;

    pw_init(NULL, NULL);
    helper.loop = pw_main_loop_new(NULL);
    if (helper.loop == NULL || !setup_pipewire(&helper)) {
        fprintf(stderr, "pip-overlay: PipeWire setup failed\n");
        return 1;
    }

    struct pw_loop *loop = pw_main_loop_get_loop(helper.loop);
    helper.x11_source = pw_loop_add_io(loop, ConnectionNumber(helper.display),
        SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_x11_readable, &helper);
    helper.stdin_source = pw_loop_add_io(loop, STDIN_FILENO, SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
        false, on_stdin, &helper);
    helper.drain_timer = pw_loop_add_timer(loop, on_drain_timer, &helper);
    helper.release_timer = pw_loop_add_timer(loop, on_release_timer, &helper);
    helper.tick_timer = pw_loop_add_timer(loop, on_tick, &helper);
    pw_loop_add_signal(loop, SIGINT, on_signal, &helper);
    pw_loop_add_signal(loop, SIGTERM, on_signal, &helper);
    struct timespec second = { .tv_sec = 1 };
    pw_loop_update_timer(loop, helper.tick_timer, &second, &second, false);
    helper.last_cpu = cpu_seconds();

    emit("{\"event\":\"ready\",\"width\":%u,\"height\":%u,\"gamescope_stream\":%s}",
        helper.screen.width, helper.screen.height,
        helper.gamescope_node != SPA_ID_INVALID ? "true" : "false");
    process_x_events(&helper);
    pw_main_loop_run(helper.loop);

    if (helper.stream != NULL)
        pw_stream_destroy(helper.stream);
    pw_proxy_destroy((struct pw_proxy *)helper.registry);
    pw_core_disconnect(helper.core);
    pw_context_destroy(helper.context);
    pw_main_loop_destroy(helper.loop);
    pw_deinit();

    destroy_frame(&helper);
    XDestroyWindow(helper.display, helper.window);
    XCloseDisplay(helper.display);
    return 0;
}

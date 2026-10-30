#include <stdio.h>
#include <stdlib.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>

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

static void draw_probe(Display *display, Window window, XRenderPictFormat *format)
{
    XRenderPictureAttributes attributes = {0};
    Picture picture = XRenderCreatePicture(display, window, format, 0, &attributes);
    XRenderColor color = {
        .red = 0xffff,
        .green = 0x2400,
        .blue = 0x0800,
        .alpha = 0x8000,
    };
    XRenderFillRectangle(display, PictOpOver, picture, &color, 32, 32, 320, 180);
    XRenderFreePicture(display, picture);
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "Cannot open the configured X11 display\n");
        return 1;
    }

    int screen = DefaultScreen(display);
    XVisualInfo visual_info;
    if (!find_argb_visual(display, screen, &visual_info)) {
        fprintf(stderr, "No 32-bit ARGB visual is available\n");
        XCloseDisplay(display);
        return 1;
    }

    int shape_event;
    int shape_error;
    int render_event;
    int render_error;
    if (!XShapeQueryExtension(display, &shape_event, &shape_error) ||
        !XRenderQueryExtension(display, &render_event, &render_error)) {
        fprintf(stderr, "X Shape or X Render extension is unavailable\n");
        XCloseDisplay(display);
        return 1;
    }

    XRenderPictFormat *format = XRenderFindVisualFormat(display, visual_info.visual);
    Window root = RootWindow(display, screen);
    Colormap colormap = XCreateColormap(display, root, visual_info.visual, AllocNone);
    XSetWindowAttributes attributes = {
        .colormap = colormap,
        .border_pixel = 0,
        .background_pixel = 0,
        .override_redirect = True,
        .event_mask = ExposureMask | StructureNotifyMask,
    };
    Window window = XCreateWindow(display, root, 0, 0,
        (unsigned int)DisplayWidth(display, screen),
        (unsigned int)DisplayHeight(display, screen), 0,
        visual_info.depth, InputOutput, visual_info.visual,
        CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask,
        &attributes);
    if (window == None) {
        fprintf(stderr, "Cannot create the overlay probe window\n");
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        return 1;
    }

    XStoreName(display, window, "Floater Picture in Picture overlay probe");
    XWMHints hints = {
        .flags = InputHint,
        .input = False,
    };
    XSetWMHints(display, window, &hints);

    Atom overlay_atom = XInternAtom(display, "GAMESCOPE_EXTERNAL_OVERLAY", False);
    unsigned long enabled = 1;
    XChangeProperty(display, window, overlay_atom, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&enabled, 1);
    XShapeCombineRectangles(display, window, ShapeInput, 0, 0,
        NULL, 0, ShapeSet, Unsorted);

    XMapRaised(display, window);
    draw_probe(display, window, format);
    XFlush(display);
    fprintf(stderr, "Overlay probe mapped as X11 window 0x%lx; press Ctrl-C to stop\n",
        window);

    for (;;) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == Expose)
            draw_probe(display, window, format);
        else if (event.type == DestroyNotify)
            break;
    }

    XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
    XCloseDisplay(display);
    return 0;
}
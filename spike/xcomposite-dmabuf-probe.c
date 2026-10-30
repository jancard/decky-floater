#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xcb/dri3.h>
#include <xcb/xcb.h>

static int has_extension(const char *extensions, const char *name)
{
    size_t length = strlen(name);
    const char *current = extensions;
    while ((current = strstr(current, name)) != NULL) {
        if ((current == extensions || current[-1] == ' ') &&
            (current[length] == '\0' || current[length] == ' '))
            return 1;
        current += length;
    }
    return 0;
}

static uint32_t drm_format_for_pixmap(uint8_t depth, uint8_t bpp)
{
    if (depth == 24 && bpp == 32)
        return DRM_FORMAT_XRGB8888;
    if (depth == 32 && bpp == 32)
        return DRM_FORMAT_ARGB8888;
    if (depth == 30 && bpp == 32)
        return DRM_FORMAT_XRGB2101010;
    return DRM_FORMAT_INVALID;
}

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        fprintf(stderr, "GL shader compile failed: %.*s\n", (int)length, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static uint64_t hash_pixels(const uint8_t *pixels, size_t length)
{
    uint64_t hash = 14695981039346656037ull;
    for (size_t index = 0; index < length; index++) {
        hash ^= pixels[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

static EGLImageKHR import_window_pixmap(EGLDisplay egl_display, Display *display,
    xcb_connection_t *connection, Window window, Pixmap *current_pixmap)
{
    Pixmap pixmap = XCompositeNameWindowPixmap(display, window);
    XSync(display, False);
    if (pixmap == None) {
        fprintf(stderr, "XComposite could not refresh the named pixmap\n");
        return EGL_NO_IMAGE_KHR;
    }

    xcb_dri3_buffers_from_pixmap_cookie_t cookie =
        xcb_dri3_buffers_from_pixmap(connection, (xcb_pixmap_t)pixmap);
    xcb_generic_error_t *xcb_error = NULL;
    xcb_dri3_buffers_from_pixmap_reply_t *buffers =
        xcb_dri3_buffers_from_pixmap_reply(connection, cookie, &xcb_error);
    if (buffers == NULL || xcb_error != NULL || buffers->nfd == 0) {
        fprintf(stderr, "DRI3 could not refresh pixmap buffers\n");
        free(xcb_error);
        free(buffers);
        XFreePixmap(display, pixmap);
        return EGL_NO_IMAGE_KHR;
    }

    int plane_count = xcb_dri3_buffers_from_pixmap_strides_length(buffers);
    int offset_count = xcb_dri3_buffers_from_pixmap_offsets_length(buffers);
    int fd_count = xcb_dri3_buffers_from_pixmap_buffers_length(buffers);
    uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(buffers);
    uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(buffers);
    int *fds = xcb_dri3_buffers_from_pixmap_reply_fds(connection, buffers);
    uint32_t format = drm_format_for_pixmap(buffers->depth, buffers->bpp);
    if (plane_count != 1 || offset_count < 1 || fd_count < 1 || fds == NULL ||
        format == DRM_FORMAT_INVALID) {
        fprintf(stderr, "Refreshed DRI3 pixmap layout is unsupported\n");
        for (int index = 0; index < fd_count; index++)
            close(fds[index]);
        free(buffers);
        XFreePixmap(display, pixmap);
        return EGL_NO_IMAGE_KHR;
    }

    const char *extensions = eglQueryString(egl_display, EGL_EXTENSIONS);
    int modifiers_supported = extensions != NULL &&
        has_extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    EGLint attributes[20];
    int attribute = 0;
    attributes[attribute++] = EGL_WIDTH;
    attributes[attribute++] = buffers->width;
    attributes[attribute++] = EGL_HEIGHT;
    attributes[attribute++] = buffers->height;
    attributes[attribute++] = EGL_LINUX_DRM_FOURCC_EXT;
    attributes[attribute++] = (EGLint)format;
    attributes[attribute++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attributes[attribute++] = fds[0];
    attributes[attribute++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attributes[attribute++] = (EGLint)offsets[0];
    attributes[attribute++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attributes[attribute++] = (EGLint)strides[0];
    if (modifiers_supported) {
        attributes[attribute++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attributes[attribute++] = (EGLint)(buffers->modifier & 0xffffffffu);
        attributes[attribute++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attributes[attribute++] = (EGLint)(buffers->modifier >> 32);
    }
    attributes[attribute] = EGL_NONE;

    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    EGLImageKHR image = create_image != NULL
        ? create_image(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes)
        : EGL_NO_IMAGE_KHR;
    for (int index = 0; index < fd_count; index++)
        close(fds[index]);
    free(buffers);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "EGL could not import refreshed pixmap: 0x%x\n", eglGetError());
        XFreePixmap(display, pixmap);
        return EGL_NO_IMAGE_KHR;
    }

    XFreePixmap(display, *current_pixmap);
    *current_pixmap = pixmap;
    return image;
}

static int sample_imported_image(EGLDisplay display, EGLImageKHR *image,
    Display *xdisplay, xcb_connection_t *connection, Window window, Pixmap *pixmap)
{
    const char *egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (egl_extensions == NULL ||
        !has_extension(egl_extensions, "EGL_KHR_no_config_context") ||
        !has_extension(egl_extensions, "EGL_KHR_surfaceless_context")) {
        fprintf(stderr, "GBM EGL lacks no-config surfaceless context support\n");
        return 1;
    }

    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
        context_attributes);
    if (context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        fprintf(stderr, "Cannot create current surfaceless GLES2 context: 0x%x\n",
            eglGetError());
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        return 1;
    }

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (gl_extensions == NULL ||
        !has_extension(gl_extensions, "GL_OES_EGL_image_external")) {
        fprintf(stderr, "GLES2 lacks GL_OES_EGL_image_external\n");
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        return 1;
    }

    const char *vertex_source =
        "attribute vec2 position; varying vec2 uv;"
        "void main(){uv=(position+1.0)*0.5;gl_Position=vec4(position,0.0,1.0);}";
    const char *fragment_source =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float; uniform samplerExternalOES source; varying vec2 uv;"
        "void main(){gl_FragColor=texture2D(source,uv);}";
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    GLuint program = 0;
    if (vertex_shader != 0 && fragment_shader != 0) {
        program = glCreateProgram();
        glAttachShader(program, vertex_shader);
        glAttachShader(program, fragment_shader);
        glBindAttribLocation(program, 0, "position");
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            fprintf(stderr, "GL shader program link failed\n");
            glDeleteProgram(program);
            program = 0;
        }
    }

    int result = 0;
    GLuint source_texture = 0;
    GLuint output_texture = 0;
    GLuint framebuffer = 0;
    GLuint vertex_buffer = 0;
    const size_t sample_size = 32u * 32u * 4u;
    uint8_t *pixels = malloc(sample_size);
    uint64_t hashes[10] = {0};
    if (image_target == NULL || program == 0 || pixels == NULL) {
        fprintf(stderr, "Could not prepare GLES2 DMA-BUF sampler\n");
        result = 1;
    } else {
        const GLfloat vertices[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f,
        };
        glGenTextures(1, &source_texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, source_texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        image_target(GL_TEXTURE_EXTERNAL_OES, image);
        GLenum image_error = glGetError();
        if (image_error != GL_NO_ERROR) {
            fprintf(stderr, "Initial EGLImage texture bind error: 0x%x\n", image_error);
            result = 1;
        }

        glGenBuffers(1, &vertex_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glGenTextures(1, &output_texture);
        glBindTexture(GL_TEXTURE_2D, output_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, output_texture, 0);
        GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "Imported texture framebuffer status: 0x%x\n",
                framebuffer_status);
            result = 1;
        }
        glViewport(0, 0, 32, 32);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "source"), 0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

        for (size_t index = 0; result == 0 && index < 10; index++) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, source_texture);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFinish();
            GLenum sampling_error = glGetError();
            if (sampling_error != GL_NO_ERROR) {
                fprintf(stderr, "GLES2 sampling failed: 0x%x\n", sampling_error);
                result = 1;
                break;
            }
            glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            GLenum readback_error = glGetError();
            if (readback_error != GL_NO_ERROR) {
                fprintf(stderr, "GLES2 readback failed: 0x%x\n", readback_error);
                result = 1;
                break;
            }
            hashes[index] = hash_pixels(pixels, sample_size);
            if (index + 1 < 10) {
                const struct timespec interval = { .tv_sec = 0, .tv_nsec = 100000000 };
                nanosleep(&interval, NULL);
                EGLImageKHR refreshed = import_window_pixmap(display,
                    xdisplay, connection, window, pixmap);
                if (refreshed == EGL_NO_IMAGE_KHR) {
                    result = 1;
                    break;
                }
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, source_texture);
                image_target(GL_TEXTURE_EXTERNAL_OES, refreshed);
                GLenum refreshed_error = glGetError();
                if (refreshed_error != GL_NO_ERROR) {
                    fprintf(stderr, "Refreshed EGLImage texture bind error: 0x%x\n",
                        refreshed_error);
                    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
                        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
                    if (destroy_image != NULL)
                        destroy_image(display, refreshed);
                    result = 1;
                    break;
                }
                PFNEGLDESTROYIMAGEKHRPROC destroy_image =
                    (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
                if (destroy_image != NULL)
                    destroy_image(display, *image);
                *image = refreshed;
            }
        }

        if (result == 0) {
            unsigned int distinct = 1;
            for (size_t index = 1; index < 10; index++) {
                int seen = 0;
                for (size_t previous = 0; previous < index; previous++)
                    seen |= hashes[index] == hashes[previous];
                distinct += !seen;
            }
            printf("samples=10 distinct=%u first_last_changed=%s\n", distinct,
                hashes[0] != hashes[9] ? "yes" : "no");
        }
    }

    free(pixels);
    if (vertex_buffer != 0)
        glDeleteBuffers(1, &vertex_buffer);
    if (framebuffer != 0)
        glDeleteFramebuffers(1, &framebuffer);
    if (output_texture != 0)
        glDeleteTextures(1, &output_texture);
    if (source_texture != 0)
        glDeleteTextures(1, &source_texture);
    if (program != 0)
        glDeleteProgram(program);
    if (vertex_shader != 0)
        glDeleteShader(vertex_shader);
    if (fragment_shader != 0)
        glDeleteShader(fragment_shader);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <X11-window-id>\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    unsigned long window_id = strtoul(argv[1], &end, 0);
    if (end == argv[1] || *end != '\0' || window_id == 0) {
        fprintf(stderr, "Invalid X11 window ID\n");
        return 2;
    }

    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "Cannot open X11 display\n");
        return 1;
    }

    XWindowAttributes window_attributes;
    if (!XGetWindowAttributes(display, (Window)window_id, &window_attributes)) {
        fprintf(stderr, "Cannot inspect X11 window 0x%lx\n", window_id);
        XCloseDisplay(display);
        return 1;
    }

    Pixmap pixmap = XCompositeNameWindowPixmap(display, (Window)window_id);
    XSync(display, False);
    if (pixmap == None) {
        fprintf(stderr, "XComposite could not name the window pixmap\n");
        XCloseDisplay(display);
        return 1;
    }

    int screen = 0;
    xcb_connection_t *connection = xcb_connect(DisplayString(display), &screen);
    if (connection == NULL || xcb_connection_has_error(connection)) {
        fprintf(stderr, "Cannot open XCB connection\n");
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }

    xcb_generic_error_t *xcb_error = NULL;
    xcb_dri3_query_version_cookie_t version_cookie =
        xcb_dri3_query_version(connection, 1, 2);
    xcb_dri3_query_version_reply_t *version =
        xcb_dri3_query_version_reply(connection, version_cookie, &xcb_error);
    if (version == NULL || xcb_error != NULL || version->major_version < 1 ||
        (version->major_version == 1 && version->minor_version < 2)) {
        fprintf(stderr, "X server does not provide DRI3 1.2\n");
        free(xcb_error);
        free(version);
        xcb_disconnect(connection);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }
    free(version);

    xcb_dri3_buffers_from_pixmap_cookie_t buffers_cookie =
        xcb_dri3_buffers_from_pixmap(connection, (xcb_pixmap_t)pixmap);
    xcb_dri3_buffers_from_pixmap_reply_t *buffers =
        xcb_dri3_buffers_from_pixmap_reply(connection, buffers_cookie, &xcb_error);
    if (buffers == NULL || xcb_error != NULL || buffers->nfd == 0) {
        fprintf(stderr, "DRI3 could not export pixmap buffers\n");
        free(xcb_error);
        free(buffers);
        xcb_disconnect(connection);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }

    int plane_count = xcb_dri3_buffers_from_pixmap_strides_length(buffers);
    int offset_count = xcb_dri3_buffers_from_pixmap_offsets_length(buffers);
    int fd_count = xcb_dri3_buffers_from_pixmap_buffers_length(buffers);
    uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(buffers);
    uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(buffers);
    int *fds = xcb_dri3_buffers_from_pixmap_reply_fds(connection, buffers);
    uint32_t format = drm_format_for_pixmap(buffers->depth, buffers->bpp);
    if (plane_count < 1 || offset_count < 1 || fd_count < 1 || fds == NULL ||
        format == DRM_FORMAT_INVALID) {
        fprintf(stderr, "Unsupported DRI3 pixmap layout: depth=%u bpp=%u planes=%d fds=%d\n",
            buffers->depth, buffers->bpp, plane_count, fd_count);
        for (int index = 0; index < fd_count; index++)
            close(fds[index]);
        free(buffers);
        xcb_disconnect(connection);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }

    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = render_fd >= 0 ? gbm_create_device(render_fd) : NULL;
    PFNEGLGETPLATFORMDISPLAYPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYPROC)eglGetProcAddress("eglGetPlatformDisplay");
    EGLDisplay egl_display = gbm != NULL && get_platform_display != NULL
        ? get_platform_display(EGL_PLATFORM_GBM_MESA, gbm, NULL)
        : EGL_NO_DISPLAY;
    EGLint major = 0;
    EGLint minor = 0;
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "Cannot initialize GBM EGL: 0x%x\n", eglGetError());
        for (int index = 0; index < fd_count; index++)
            close(fds[index]);
        free(buffers);
        if (gbm != NULL)
            gbm_device_destroy(gbm);
        if (render_fd >= 0)
            close(render_fd);
        xcb_disconnect(connection);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }

    const char *extensions = eglQueryString(egl_display, EGL_EXTENSIONS);
    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    int import_supported = extensions != NULL &&
        has_extension(extensions, "EGL_EXT_image_dma_buf_import");
    int modifiers_supported = extensions != NULL &&
        has_extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    if (!import_supported || create_image == NULL || plane_count != 1) {
        fprintf(stderr, "EGL DMA-BUF import unavailable or probe requires one plane\n");
        for (int index = 0; index < fd_count; index++)
            close(fds[index]);
        eglTerminate(egl_display);
        gbm_device_destroy(gbm);
        close(render_fd);
        free(buffers);
        xcb_disconnect(connection);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }

    EGLint attributes[20];
    int attribute = 0;
    attributes[attribute++] = EGL_WIDTH;
    attributes[attribute++] = buffers->width;
    attributes[attribute++] = EGL_HEIGHT;
    attributes[attribute++] = buffers->height;
    attributes[attribute++] = EGL_LINUX_DRM_FOURCC_EXT;
    attributes[attribute++] = (EGLint)format;
    attributes[attribute++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attributes[attribute++] = fds[0];
    attributes[attribute++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attributes[attribute++] = (EGLint)offsets[0];
    attributes[attribute++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attributes[attribute++] = (EGLint)strides[0];
    if (modifiers_supported) {
        attributes[attribute++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attributes[attribute++] = (EGLint)(buffers->modifier & 0xffffffffu);
        attributes[attribute++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attributes[attribute++] = (EGLint)(buffers->modifier >> 32);
    }
    attributes[attribute] = EGL_NONE;

    EGLImageKHR image = create_image(egl_display, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "EGL DMA-BUF import failed: 0x%x (format=0x%x modifier=0x%llx)\n",
            eglGetError(), format, (unsigned long long)buffers->modifier);
        for (int index = 0; index < fd_count; index++)
            close(fds[index]);
        eglTerminate(egl_display);
        gbm_device_destroy(gbm);
        close(render_fd);
        free(buffers);
        xcb_disconnect(connection);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }

    printf("DRI3/EGL import succeeded: window=0x%lx pixmap=%ux%u format=0x%x modifier=0x%llx planes=%d\n",
        window_id, buffers->width, buffers->height, format,
        (unsigned long long)buffers->modifier, plane_count);
    int sample_result = sample_imported_image(egl_display, &image,
        display, connection, (Window)window_id, &pixmap);
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    if (destroy_image != NULL)
        destroy_image(egl_display, image);
    for (int index = 0; index < fd_count; index++)
        close(fds[index]);
    eglTerminate(egl_display);
    gbm_device_destroy(gbm);
    close(render_fd);
    free(buffers);
    xcb_disconnect(connection);
    XFreePixmap(display, pixmap);
    XCloseDisplay(display);
    return sample_result;
}
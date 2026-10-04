#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"
#include "ui/shader.h"
#include "standard-headers/drm/drm_fourcc.h"
#include "trace.h"

#ifdef CONFIG_LINUX
#include "ui/vulkan-readback.h"
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#endif

typedef struct egl_dpy {
    DisplayChangeListener dcl;
    DisplayGLCtx *ctx;
    DisplaySurface *ds;
    QemuGLShader *gls;
    egl_fb guest_fb;
    egl_fb cursor_fb;
    egl_fb blit_fb;
    bool y_0_top;
    uint32_t pos_x;
    uint32_t pos_y;
#if defined(CONFIG_GBM) && defined(CONFIG_LINUX)
    HeliosVulkanReadback *vk_readback;
    QemuDmaBuf *vk_dmabuf;
    HeliosVulkanReadbackCache *vk_readback_cache;
    QEMUTimer *vk_publish_timer;
    int64_t vk_next_publish_ns;
    bool vk_pending;
    uint32_t vk_pending_x;
    uint32_t vk_pending_y;
    uint32_t vk_pending_width;
    uint32_t vk_pending_height;
    bool force_full_update;
    QemuDmaBuf *cpu_dmabuf;
    void *cpu_map;
    size_t cpu_map_len;
    uint32_t cpu_offset;
    uint32_t cpu_stride;
    uint64_t cpu_flushes;
    int cpu_sync_state;
    uint64_t read_seq;
#endif
} egl_dpy;

static GPtrArray *egl_dpys;

typedef struct EGLHeadlessContext {
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw;
    EGLSurface read;
    bool saved;
} EGLHeadlessContext;

static void egl_headless_context_restore(EGLHeadlessContext *scope)
{
    if (scope->saved &&
        (eglGetCurrentDisplay() != scope->display ||
         eglGetCurrentContext() != scope->context ||
         eglGetCurrentSurface(EGL_DRAW) != scope->draw ||
         eglGetCurrentSurface(EGL_READ) != scope->read) &&
        !eglMakeCurrent(scope->display ? scope->display : qemu_egl_display,
                        scope->draw, scope->read, scope->context)) {
        error_report("egl-headless: restoring EGL context failed: %s",
                     qemu_egl_get_error_string());
    }
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(EGLHeadlessContext,
                               egl_headless_context_restore)

static bool egl_headless_context_bind(EGLHeadlessContext *scope)
{
    scope->display = eglGetCurrentDisplay();
    scope->context = eglGetCurrentContext();
    scope->draw = eglGetCurrentSurface(EGL_DRAW);
    scope->read = eglGetCurrentSurface(EGL_READ);
    if (scope->display == qemu_egl_display &&
        scope->context == qemu_egl_rn_ctx) {
        scope->saved = true;
        return true;
    }
    /* Venus blob commands do not leave a GL context current.  Epoxy resolves
     * EGL image extensions against the current display, and our framebuffer
     * objects belong to the display context rather than a renderer context.
     * Restore the renderer's state (including no context) at callback exit. */
    if (qemu_egl_make_context_current(NULL, qemu_egl_rn_ctx) < 0) {
        return false;
    }
    scope->saved = true;
    return true;
}

/* ------------------------------------------------------------------ */

static void egl_refresh(DisplayChangeListener *dcl)
{
    qemu_console_hw_update(dcl->con);
}

static void egl_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
}

static void egl_gfx_switch(DisplayChangeListener *dcl,
                           struct DisplaySurface *new_surface)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->ds = new_surface;
}

#if defined(CONFIG_GBM) && defined(CONFIG_LINUX)

/*
 * Helios scan-out oracle (defect 0ab-B).
 *
 * Per published remote frame, record what the host actually put on screen and
 * which guest buffer it came out of.
 *
 * Two identities are emitted deliberately:
 *   bound_ino -- the DMA-BUF inode of the resource the guest currently has bound
 *                (fstat'd at flush time: what SHOULD be read), and
 *   read_ino  -- the DMA-BUF inode the active readback IMPORTED (what IS read).
 * A divergence proves the host reads the wrong buffer; equality for two
 * different resource ids proves the guest aliased them.
 */
typedef struct HeliosScanoutStats {
    uint32_t sampled;
    uint32_t nonzero;
    uint32_t max;
    uint64_t csum;
} HeliosScanoutStats;

/* Subsample the flushed rect: enough pixels to classify, cheap at 142 flushes/s. */
#define HELIOS_ORACLE_STEP_X 4
#define HELIOS_ORACLE_STEP_Y 4

static void helios_scanout_stats(DisplaySurface *ds,
                                 uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h,
                                 HeliosScanoutStats *out)
{
    const uint8_t *base;
    size_t stride;
    uint32_t row, col;

    memset(out, 0, sizeof(*out));
    if (!ds || !w || !h ||
        x >= (uint32_t)surface_width(ds) || y >= (uint32_t)surface_height(ds)) {
        return;
    }
    base = surface_data(ds);
    stride = surface_stride(ds);
    if (!base) {
        return;
    }
    w = MIN(w, (uint32_t)surface_width(ds) - x);
    h = MIN(h, (uint32_t)surface_height(ds) - y);

    for (row = 0; row < h; row += HELIOS_ORACLE_STEP_Y) {
        const uint8_t *line = base + (size_t)(y + row) * stride +
                              (size_t)x * 4;

        for (col = 0; col < w; col += HELIOS_ORACLE_STEP_X) {
            uint32_t v = ldl_he_p(line + (size_t)col * 4) & 0x00ffffffu;
            uint32_t r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
            uint32_t peak = MAX(r, MAX(g, b));

            out->sampled++;
            if (v) {
                out->nonzero++;
            }
            if (peak > out->max) {
                out->max = peak;
            }
            /* FNV-1a over the sampled pixels: identifies repeated content. */
            out->csum = (out->csum ^ v) * 0x100000001b3ull;
        }
    }
}

static uint64_t helios_dmabuf_ino(QemuDmaBuf *dmabuf)
{
    const int *fds;
    struct stat st;
    int nfds;

    if (!dmabuf) {
        return 0;
    }
    fds = qemu_dmabuf_get_fds(dmabuf, &nfds);
    if (nfds < 1 || fds[0] < 0 || fstat(fds[0], &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_ino;
}

static void egl_vulkan_readback_deactivate(egl_dpy *edpy)
{
    if (edpy->vk_publish_timer) {
        timer_del(edpy->vk_publish_timer);
    }
    helios_vulkan_readback_cache_deactivate(edpy->vk_readback_cache,
                                            edpy->vk_dmabuf);
    edpy->vk_readback = NULL;
    edpy->vk_dmabuf = NULL;
    edpy->vk_pending = false;
    edpy->force_full_update = true;
}

static void egl_vulkan_readback_cache_clear(egl_dpy *edpy)
{
    egl_vulkan_readback_deactivate(edpy);
    g_clear_pointer(&edpy->vk_readback_cache,
                    helios_vulkan_readback_cache_free);
    edpy->vk_next_publish_ns = 0;
    edpy->vk_pending = false;
}

static HeliosVulkanReadback *
egl_vulkan_readback_cache_get(egl_dpy *edpy, QemuDmaBuf *dmabuf,
                              bool direct_optimal)
{
    if (!edpy->vk_readback_cache) {
        edpy->vk_readback_cache = helios_vulkan_readback_cache_new();
    }
    return helios_vulkan_readback_cache_activate(edpy->vk_readback_cache,
                                                  dmabuf,
                                                  direct_optimal);
}

/*
 * One line per BIND naming the resource, the buffer it is backed by, and the
 * buffer the chosen readback path actually imported.  Two resource ids that
 * report the same bound_ino are aliased; a readback whose read_ino differs from
 * bound_ino is reading a buffer the guest did not bind.
 */
static void egl_trace_scanout_bind(QemuDmaBuf *dmabuf,
                                   HeliosVulkanReadback *readback,
                                   const char *path)
{
    uint64_t read_ino = 0, read_size = 0, reuse = 0;
    const uint32_t *offsets, *strides;
    int noffsets, nstrides;

    if (!trace_event_get_state_backends(TRACE_HELIOS_SCANOUT_BIND)) {
        return;
    }

    helios_vulkan_readback_identity(readback, &read_ino, &read_size, &reuse);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &noffsets);
    strides = qemu_dmabuf_get_strides(dmabuf, &nstrides);

    trace_helios_scanout_bind(qemu_dmabuf_get_source_id(dmabuf),
                              helios_dmabuf_ino(dmabuf),
                              qemu_dmabuf_get_allocation_size(dmabuf),
                              qemu_dmabuf_get_backing_width(dmabuf),
                              qemu_dmabuf_get_backing_height(dmabuf),
                              nstrides > 0 ? strides[0] : 0,
                              noffsets > 0 ? offsets[0] : 0,
                              read_ino, reuse, path);
}

/*
 * One line per frame the host actually publishes, carrying
 * both identities and a content verdict for the pixels the VNC encoder is about
 * to read.  `nonzero == 0` is the black-frame flash of defect 0ab-B; `csum`
 * distinguishes a genuinely new frame from a re-read of the previous one.
 */
static void egl_trace_scanout_read(egl_dpy *edpy, QemuDmaBuf *dmabuf,
                                   HeliosVulkanReadback *readback,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h)
{
    HeliosScanoutStats stats;
    uint64_t read_ino = 0, read_size = 0, reuse = 0;

    if (!trace_event_get_state_backends(TRACE_HELIOS_SCANOUT_READ)) {
        return;
    }

    helios_vulkan_readback_identity(readback, &read_ino, &read_size, &reuse);
    helios_scanout_stats(edpy->ds, x, y, w, h, &stats);

    trace_helios_scanout_read(dmabuf ? qemu_dmabuf_get_source_id(dmabuf) : 0,
                              helios_dmabuf_ino(dmabuf), read_ino,
                              (x << 16) | (y & 0xffff),
                              (w << 16) | (h & 0xffff),
                              stats.sampled, stats.nonzero, stats.max,
                              stats.csum, edpy->read_seq++);
}

/*
 * VNC's default refresh interval is 30 ms. Capturing every guest flush is
 * still required for external ownership and to preserve the newest/final
 * frame, but publishing device-local snapshots to host RAM any faster only
 * creates work the remote display cannot consume.
 */
#define HELIOS_VK_PUBLISH_INTERVAL_NS (30 * SCALE_MS)

static bool egl_vulkan_pending_union(egl_dpy *edpy,
                                     uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t *out_x, uint32_t *out_y,
                                     uint32_t *out_width,
                                     uint32_t *out_height)
{
    uint32_t x2, y2;

    if (!width || !height || !edpy->ds ||
        x >= surface_width(edpy->ds) || y >= surface_height(edpy->ds)) {
        return false;
    }
    width = MIN(width, (uint32_t)surface_width(edpy->ds) - x);
    height = MIN(height, (uint32_t)surface_height(edpy->ds) - y);

    if (!edpy->vk_pending) {
        *out_x = x;
        *out_y = y;
        *out_width = width;
        *out_height = height;
        return true;
    }

    x2 = MAX(edpy->vk_pending_x + edpy->vk_pending_width, x + width);
    y2 = MAX(edpy->vk_pending_y + edpy->vk_pending_height, y + height);
    *out_x = MIN(edpy->vk_pending_x, x);
    *out_y = MIN(edpy->vk_pending_y, y);
    *out_width = x2 - *out_x;
    *out_height = y2 - *out_y;
    return true;
}

static void egl_vulkan_publish_timer(void *opaque)
{
    egl_dpy *edpy = opaque;
    int64_t now;
    uint32_t x, y, width, height;
    HeliosVulkanReadbackRect rect;

    if (!edpy->vk_pending || !edpy->vk_readback ||
        !edpy->vk_dmabuf || !edpy->ds) {
        return;
    }
    x = edpy->vk_pending_x;
    y = edpy->vk_pending_y;
    width = edpy->vk_pending_width;
    height = edpy->vk_pending_height;
    rect = (HeliosVulkanReadbackRect) { x, y, width, height };
    if (!helios_vulkan_readback_publish(edpy->vk_readback, edpy->ds, &rect)) {
        /* Do not turn a permanent device error into a main-loop retry storm. */
        edpy->vk_pending = false;
        edpy->force_full_update = true;
        return;
    }

    edpy->vk_pending = false;
    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    edpy->vk_next_publish_ns = now + HELIOS_VK_PUBLISH_INTERVAL_NS;
    egl_trace_scanout_read(edpy, edpy->vk_dmabuf, edpy->vk_readback,
                           x, y, width, height);
    qemu_console_update(edpy->dcl.con, x, y, width, height);
}

static void egl_cpu_dmabuf_unmap(egl_dpy *edpy)
{
    if (edpy->cpu_map) {
        munmap(edpy->cpu_map, edpy->cpu_map_len);
    }

    edpy->cpu_dmabuf = NULL;
    edpy->cpu_map = NULL;
    edpy->cpu_map_len = 0;
    edpy->cpu_offset = 0;
    edpy->cpu_stride = 0;
    edpy->cpu_flushes = 0;
    edpy->cpu_sync_state = 0;
}

/*
 * Explicit Vulkan/CPU fallbacks reconstruct a LINEAR image and therefore need
 * the advertised offset + stride * height span to fit the DMA-BUF allocation.
 * A native OPTIMAL export can legitimately fail that test; only EGL may import
 * it because the same host driver owns its implicit layout metadata.
 */
static bool egl_dmabuf_linear_span_fits_fd(QemuDmaBuf *dmabuf)
{
    const int *fds;
    const uint32_t *offsets, *strides;
    struct stat st;
    uint64_t rows, required;
    int nfds, noffsets, nstrides;

    fds = qemu_dmabuf_get_fds(dmabuf, &nfds);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &noffsets);
    strides = qemu_dmabuf_get_strides(dmabuf, &nstrides);
    if (nfds < 1 || noffsets < 1 || nstrides < 1 || fds[0] < 0 ||
        __builtin_mul_overflow((uint64_t)strides[0],
                               qemu_dmabuf_get_backing_height(dmabuf), &rows) ||
        __builtin_add_overflow((uint64_t)offsets[0], rows, &required)) {
        return false;
    }

    /* Some DMA-BUF exporters report st_size=0; then EGL remains authoritative. */
    if (fstat(fds[0], &st) == 0 && st.st_size > 0 &&
        required > (uint64_t)st.st_size) {
        return false;
    }
    return true;
}

static bool egl_cpu_dmabuf_map(egl_dpy *edpy, QemuDmaBuf *dmabuf)
{
    const int *fds;
    const uint32_t *offsets;
    const uint32_t *strides;
    uint32_t width, height, fourcc;
    uint64_t modifier;
    uint64_t required;
    void *map;
    int nfds, noffsets, nstrides;
    int64_t start_ns;

    egl_cpu_dmabuf_unmap(edpy);

    if (qemu_dmabuf_get_num_planes(dmabuf) != 1) {
        return false;
    }

    fourcc = qemu_dmabuf_get_fourcc(dmabuf);
    if (fourcc != DRM_FORMAT_XRGB8888 && fourcc != DRM_FORMAT_ARGB8888) {
        return false;
    }

    modifier = qemu_dmabuf_get_modifier(dmabuf);
    if (modifier != DRM_FORMAT_MOD_INVALID &&
        modifier != DRM_FORMAT_MOD_LINEAR) {
        return false;
    }

    fds = qemu_dmabuf_get_fds(dmabuf, &nfds);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &noffsets);
    strides = qemu_dmabuf_get_strides(dmabuf, &nstrides);
    width = qemu_dmabuf_get_backing_width(dmabuf);
    height = qemu_dmabuf_get_backing_height(dmabuf);

    if (nfds < 1 || noffsets < 1 || nstrides < 1 || fds[0] < 0 ||
        !width || !height || strides[0] < width * 4u) {
        return false;
    }

    required = (uint64_t)offsets[0] + (uint64_t)strides[0] * height;
    if (required > SIZE_MAX) {
        return false;
    }

    start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    map = mmap(NULL, required, PROT_READ, MAP_SHARED, fds[0], 0);
    if (map == MAP_FAILED) {
        error_report("egl-headless: DMA-BUF mmap failed: %s",
                     strerror(errno));
        return false;
    }

    edpy->cpu_dmabuf = dmabuf;
    edpy->cpu_map = map;
    edpy->cpu_map_len = required;
    edpy->cpu_offset = offsets[0];
    edpy->cpu_stride = strides[0];
    edpy->cpu_flushes = 0;
    edpy->cpu_sync_state = 0;

    error_report("egl-headless: mapped DMA-BUF %ux%u fourcc=0x%08x "
                 "stride=%u offset=%u in %.3f ms",
                 width, height, fourcc, strides[0], offsets[0],
                 (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start_ns) / 1e6);
    return true;
}

static bool egl_cpu_dmabuf_flush(egl_dpy *edpy,
                                 uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h)
{
    QemuDmaBuf *dmabuf = edpy->cpu_dmabuf;
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START |
                                          DMA_BUF_SYNC_READ };
    uint32_t src_x, src_y, width, height, row;
    uint32_t backing_width, backing_height;
    uint8_t *src, *dst;
    bool sync_started = false;
    int fd;
    int64_t start_ns, sync_ns, copy_ns, end_ns;

    if (!dmabuf || !edpy->cpu_map || !edpy->ds) {
        return false;
    }

    if (!w || !h || x >= surface_width(edpy->ds) ||
        y >= surface_height(edpy->ds)) {
        return true;
    }

    src_x = qemu_dmabuf_get_x(dmabuf) + x;
    src_y = qemu_dmabuf_get_y(dmabuf) + y;
    backing_width = qemu_dmabuf_get_backing_width(dmabuf);
    backing_height = qemu_dmabuf_get_backing_height(dmabuf);
    if (src_x >= backing_width || src_y >= backing_height) {
        return true;
    }

    width = MIN(w, MIN(backing_width - src_x,
                       (uint32_t)surface_width(edpy->ds) - x));
    height = MIN(h, MIN(backing_height - src_y,
                        (uint32_t)surface_height(edpy->ds) - y));
    fd = qemu_dmabuf_get_fds(dmabuf, NULL)[0];
    start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (edpy->cpu_sync_state >= 0) {
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0) {
            sync_started = true;
            edpy->cpu_sync_state = 1;
        } else {
            if (edpy->cpu_sync_state == 0) {
                error_report("egl-headless: DMA_BUF_IOCTL_SYNC unavailable: %s",
                             strerror(errno));
            }
            edpy->cpu_sync_state = -1;
        }
    }
    sync_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    src = (uint8_t *)edpy->cpu_map + edpy->cpu_offset +
          (size_t)src_y * edpy->cpu_stride + (size_t)src_x * 4;
    dst = surface_data(edpy->ds) + (size_t)y * surface_stride(edpy->ds) +
          (size_t)x * 4;
    for (row = 0; row < height; row++) {
        memcpy(dst, src, (size_t)width * 4);
        src += edpy->cpu_stride;
        dst += surface_stride(edpy->ds);
    }
    copy_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (sync_started) {
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
            error_report("egl-headless: DMA_BUF_IOCTL_SYNC end failed: %s",
                         strerror(errno));
            edpy->cpu_sync_state = -1;
        }
    }
    end_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    edpy->cpu_flushes++;
    if (edpy->cpu_flushes == 1 || end_ns - start_ns > 50000000) {
        error_report("egl-headless: DMA-BUF flush #%" PRIu64
                     " %ux%u sync=%.3f ms copy=%.3f ms total=%.3f ms",
                     edpy->cpu_flushes, width, height,
                     (sync_ns - start_ns) / 1e6,
                     (copy_ns - sync_ns) / 1e6,
                     (end_ns - start_ns) / 1e6);
    }

    return true;
}

#endif

static QEMUGLContext egl_create_context(DisplayGLCtx *dgc,
                                        QEMUGLParams *params)
{
    return qemu_egl_create_context(dgc, params, qemu_egl_rn_ctx);
}

static void egl_scanout_disable(DisplayChangeListener *dcl)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    g_auto(EGLHeadlessContext) scope = { 0 };

    if (!egl_headless_context_bind(&scope)) {
        return;
    }

#if defined(CONFIG_GBM) && defined(CONFIG_LINUX)
    egl_vulkan_readback_cache_clear(edpy);
    egl_cpu_dmabuf_unmap(edpy);
#endif
    egl_fb_destroy(&edpy->guest_fb);
    egl_fb_destroy(&edpy->blit_fb);
}

static void egl_scanout_texture(DisplayChangeListener *dcl,
                                uint32_t backing_id,
                                bool backing_y_0_top,
                                uint32_t backing_width,
                                uint32_t backing_height,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h,
                                void *d3d_tex2d)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    g_auto(EGLHeadlessContext) scope = { 0 };

    if (!egl_headless_context_bind(&scope)) {
        return;
    }

    edpy->y_0_top = backing_y_0_top;

    /* source framebuffer */
    egl_fb_setup_for_tex(&edpy->guest_fb,
                         backing_width, backing_height, backing_id, false);

    /* dest framebuffer */
    if (edpy->blit_fb.width  != backing_width ||
        edpy->blit_fb.height != backing_height) {
        egl_fb_destroy(&edpy->blit_fb);
        egl_fb_setup_new_tex(&edpy->blit_fb, backing_width, backing_height);
    }
}

#ifdef CONFIG_GBM

static void egl_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    uint32_t width, height, texture;
    g_auto(EGLHeadlessContext) scope = { 0 };

    if (!egl_headless_context_bind(&scope)) {
        return;
    }
#ifdef CONFIG_LINUX
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    bool same_readback =
        helios_vulkan_readback_matches(edpy->vk_readback, dmabuf, true) ||
        helios_vulkan_readback_matches(edpy->vk_readback, dmabuf, false);

    if (!same_readback) {
        egl_vulkan_readback_deactivate(edpy);
    }
    egl_cpu_dmabuf_unmap(edpy);

    /*
     * A modifier-less Venus image may be the exact DirectOptimal primary.
     * Reconstruct that producer VkImage first, but accept it only when its
     * driver memory requirement exactly equals the DMA-BUF allocation size.
     * A proven LINEAR image will fail this shape check and continue to EGL.
     */
    if (qemu_dmabuf_get_modifier(dmabuf) == DRM_FORMAT_MOD_INVALID) {
        edpy->vk_readback = egl_vulkan_readback_cache_get(edpy, dmabuf, true);
        if (edpy->vk_readback) {
            edpy->vk_dmabuf = dmabuf;
            egl_trace_scanout_bind(dmabuf, edpy->vk_readback, "vk-optimal");
            return;
        }
    }
#endif

    /* The Vulkan ICD may release this thread's EGL context while initializing.
     * Bind again after the native readback attempt, before epoxy resolves image
     * extensions against the current display or any GL framebuffer work. */
    if (qemu_egl_make_context_current(NULL, qemu_egl_rn_ctx) < 0) {
        return;
    }

    /* Explicit modifiers and the proven LINEAR export remain EGL-importable. */
    while (glGetError() != GL_NO_ERROR) {
        /* Attribute any subsequent error to this import attempt. */
    }
    egl_dmabuf_import_texture(dmabuf);
    glGetError();
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (texture) {
#ifdef CONFIG_LINUX
        if (edpy->vk_readback) {
            egl_vulkan_readback_deactivate(edpy);
        }
#endif
        width = qemu_dmabuf_get_width(dmabuf);
        height = qemu_dmabuf_get_height(dmabuf);

        error_report("egl-headless: zero-copy EGL DMA-BUF import %ux%u "
                     "fourcc=0x%08x modifier=0x%016" PRIx64,
                     width, height, qemu_dmabuf_get_fourcc(dmabuf),
                     qemu_dmabuf_get_modifier(dmabuf));

        egl_scanout_texture(dcl, texture, false, width, height, 0, 0,
                            width, height, NULL);
#ifdef CONFIG_LINUX
        egl_trace_scanout_bind(dmabuf, NULL, "egl-texture");
#endif
        return;
    }

    error_report("egl-headless: EGL DMA-BUF import failed for fourcc=0x%08x "
                 "modifier=0x%016" PRIx64,
                 qemu_dmabuf_get_fourcc(dmabuf),
                 qemu_dmabuf_get_modifier(dmabuf));

#ifdef CONFIG_LINUX
    /*
     * MOD_INVALID carries no layout contract.  It may describe the proven
     * plain-LINEAR scanout shape, but it may equally be an opaque Vulkan
     * OPTIMAL allocation.  If EGL rejected it, rebuilding it as LINEAR is not
     * a fallback; it is a potentially wrong alias that produces corruption.
     */
    if (qemu_dmabuf_get_modifier(dmabuf) == DRM_FORMAT_MOD_INVALID) {
        error_report("egl-headless: modifier-less DMA-BUF rejected; refusing "
                     "implicit LINEAR reinterpretation");
        return;
    }

    /* Explicit readback paths are fallbacks for DMA-BUFs EGL cannot import. */
    if (!egl_dmabuf_linear_span_fits_fd(dmabuf)) {
        error_report("egl-headless: native DMA-BUF layout requires EGL import");
        return;
    }

    edpy->vk_readback = egl_vulkan_readback_cache_get(edpy, dmabuf, false);
    if (edpy->vk_readback) {
        edpy->vk_dmabuf = dmabuf;
        egl_trace_scanout_bind(dmabuf, edpy->vk_readback, "vk-linear");
        return;
    }

    if (egl_cpu_dmabuf_map(edpy, dmabuf)) {
        egl_trace_scanout_bind(dmabuf, NULL, "cpu-mmap");
    }
#endif
}

static void egl_cursor_dmabuf(DisplayChangeListener *dcl,
                              QemuDmaBuf *dmabuf, bool have_hot,
                              uint32_t hot_x, uint32_t hot_y)
{
    uint32_t width, height, texture;
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    g_auto(EGLHeadlessContext) scope = { 0 };

    if (!egl_headless_context_bind(&scope)) {
        return;
    }

    if (dmabuf) {
        egl_dmabuf_import_texture(dmabuf);
        texture = qemu_dmabuf_get_texture(dmabuf);
        if (!texture) {
            return;
        }

        width = qemu_dmabuf_get_width(dmabuf);
        height = qemu_dmabuf_get_height(dmabuf);
        egl_fb_setup_for_tex(&edpy->cursor_fb, width, height, texture, false);
    } else {
        egl_fb_destroy(&edpy->cursor_fb);
    }
}

static void egl_release_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    g_auto(EGLHeadlessContext) scope = { 0 };

    if (!egl_headless_context_bind(&scope)) {
        return;
    }
#ifdef CONFIG_LINUX
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    if (edpy->vk_dmabuf == dmabuf) {
        egl_vulkan_readback_deactivate(edpy);
    }
    if (edpy->cpu_dmabuf == dmabuf) {
        egl_cpu_dmabuf_unmap(edpy);
    }
#endif
    egl_dmabuf_release_texture(dmabuf);
}

#endif

static void egl_cursor_position(DisplayChangeListener *dcl,
                                uint32_t pos_x, uint32_t pos_y)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->pos_x = pos_x;
    edpy->pos_y = pos_y;
}

static void egl_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    if (!edpy->ds) {
        return;
    }
    assert(surface_format(edpy->ds) == PIXMAN_x8r8g8b8);

#if defined(CONFIG_GBM) && defined(CONFIG_LINUX)
    if (edpy->force_full_update) {
        x = 0;
        y = 0;
        w = surface_width(edpy->ds);
        h = surface_height(edpy->ds);
    }

    if (edpy->vk_readback) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        bool publish = !edpy->vk_next_publish_ns ||
                       now >= edpy->vk_next_publish_ns;
        uint32_t pending_x, pending_y, pending_width, pending_height;
        HeliosVulkanReadbackRect capture_rect, publish_rect;

        if (!egl_vulkan_pending_union(edpy, x, y, w, h,
                                      &pending_x, &pending_y,
                                      &pending_width, &pending_height)) {
            return;
        }
        capture_rect = (HeliosVulkanReadbackRect) { x, y, w, h };
        publish_rect = (HeliosVulkanReadbackRect) {
            pending_x, pending_y, pending_width, pending_height,
        };
        if (!helios_vulkan_readback_capture(
                edpy->vk_readback, edpy->ds, &capture_rect,
                publish ? &publish_rect : NULL)) {
            timer_del(edpy->vk_publish_timer);
            edpy->vk_pending = false;
            edpy->force_full_update = true;
            return;
        }

        edpy->force_full_update = false;
        if (publish) {
            edpy->vk_pending = false;
            edpy->vk_next_publish_ns =
                qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                HELIOS_VK_PUBLISH_INTERVAL_NS;
            timer_del(edpy->vk_publish_timer);
            egl_trace_scanout_read(edpy, edpy->vk_dmabuf, edpy->vk_readback,
                                   pending_x, pending_y,
                                   pending_width, pending_height);
            qemu_console_update(edpy->dcl.con, pending_x, pending_y,
                                pending_width, pending_height);
        } else {
            edpy->vk_pending = true;
            edpy->vk_pending_x = pending_x;
            edpy->vk_pending_y = pending_y;
            edpy->vk_pending_width = pending_width;
            edpy->vk_pending_height = pending_height;
            timer_mod_ns(edpy->vk_publish_timer,
                         edpy->vk_next_publish_ns);
        }
        return;
    }

    if (egl_cpu_dmabuf_flush(edpy, x, y, w, h)) {
        egl_trace_scanout_read(edpy, edpy->cpu_dmabuf, NULL, x, y, w, h);
        qemu_console_update(edpy->dcl.con, x, y, w, h);
        edpy->force_full_update = false;
        return;
    }
#endif

    if (!edpy->guest_fb.texture) {
        return;
    }

    g_auto(EGLHeadlessContext) scope = { 0 };

    if (!egl_headless_context_bind(&scope)) {
        return;
    }

    if (edpy->cursor_fb.texture) {
        /* have cursor -> render using textures */
        egl_texture_blit(edpy->gls, &edpy->blit_fb, &edpy->guest_fb,
                         !edpy->y_0_top);
        egl_texture_blend(edpy->gls, &edpy->blit_fb, &edpy->cursor_fb,
                          !edpy->y_0_top, edpy->pos_x, edpy->pos_y,
                          1.0, 1.0);
    } else {
        /* no cursor -> use simple framebuffer blit */
        egl_fb_blit(&edpy->blit_fb, &edpy->guest_fb, edpy->y_0_top);
    }

    egl_fb_read(edpy->ds, &edpy->blit_fb);
    qemu_console_update(edpy->dcl.con, x, y, w, h);
#if defined(CONFIG_GBM) && defined(CONFIG_LINUX)
    edpy->force_full_update = false;
#endif
}

static const DisplayChangeListenerOps egl_ops = {
    .dpy_name                = "egl-headless",
    .dpy_refresh             = egl_refresh,
    .dpy_gfx_update          = egl_gfx_update,
    .dpy_gfx_switch          = egl_gfx_switch,

    .dpy_gl_scanout_disable  = egl_scanout_disable,
    .dpy_gl_scanout_texture  = egl_scanout_texture,
#ifdef CONFIG_GBM
    .dpy_gl_scanout_dmabuf   = egl_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = egl_cursor_dmabuf,
    .dpy_gl_release_dmabuf   = egl_release_dmabuf,
#endif
    .dpy_gl_cursor_position  = egl_cursor_position,
    .dpy_gl_update           = egl_scanout_flush,
};

static bool
egl_is_compatible_dcl(DisplayGLCtx *dgc,
                      DisplayChangeListener *dcl)
{
    if (!dcl->ops->dpy_gl_update) {
        /*
         * egl-headless is compatible with all 2d listeners, as it blits the GL
         * updates on the 2d console surface.
         */
        return true;
    }

    return dcl->ops == &egl_ops;
}

static const DisplayGLCtxOps eglctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = egl_is_compatible_dcl,
    .dpy_gl_ctx_create       = egl_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,
};

static void early_egl_headless_init(DisplayOptions *opts)
{
    DisplayGLMode mode = DISPLAY_GL_MODE_ON;

    if (opts->has_gl) {
        mode = opts->gl;
    }

    egl_init(opts->u.egl_headless.rendernode, mode, &error_fatal);
}

static void egl_headless_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    egl_dpy *edpy;
    int idx;

    egl_dpys = g_ptr_array_new();

    for (idx = 0;; idx++) {
        DisplayGLCtx *ctx;

        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        edpy = g_new0(egl_dpy, 1);
        edpy->gls = qemu_gl_init_shader();
#if defined(CONFIG_GBM) && defined(CONFIG_LINUX)
        edpy->vk_publish_timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                              egl_vulkan_publish_timer,
                                              edpy);
#endif
        ctx = g_new0(DisplayGLCtx, 1);
        ctx->ops = &eglctx_ops;
        edpy->ctx = ctx;
        qemu_console_set_display_gl_ctx(con, ctx);
        qemu_console_register_listener(con, &edpy->dcl, &egl_ops);
        g_ptr_array_add(egl_dpys, edpy);
    }
}

static void egl_headless_cleanup(void)
{
    bool have_context;

    if (!egl_dpys) {
        return;
    }

    have_context = qemu_egl_make_context_current(NULL, qemu_egl_rn_ctx) == 0;
    for (guint i = 0; i < egl_dpys->len; i++) {
        egl_dpy *edpy = g_ptr_array_index(egl_dpys, i);

        qemu_console_unregister_listener(&edpy->dcl);
        qemu_console_set_display_gl_ctx(edpy->dcl.con, NULL);
        if (have_context) {
            egl_fb_destroy(&edpy->guest_fb);
            egl_fb_destroy(&edpy->cursor_fb);
            egl_fb_destroy(&edpy->blit_fb);
            qemu_gl_fini_shader(edpy->gls);
        }
        g_free(edpy->ctx);
        g_free(edpy);
    }
    g_clear_pointer(&egl_dpys, g_ptr_array_unref);

    egl_cleanup();
}

static QemuDisplay qemu_display_egl = {
    .type       = DISPLAY_TYPE_EGL_HEADLESS,
    .early_init = early_egl_headless_init,
    .init       = egl_headless_init,
    .cleanup    = egl_headless_cleanup,
};

static void register_egl(void)
{
    qemu_display_register(&qemu_display_egl);
}

type_init(register_egl);

module_dep("ui-opengl");

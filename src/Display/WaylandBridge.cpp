#include "WaylandBridge.h"
#include "Utils/Util.h"
#include "virtual-keyboard-unstable-v1.h"
#include "wayland-client.h"
#include "wlr-data-control-unstable-v1.h"
#include "wlr-screencopy-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <map>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
namespace zero {
struct WaylandBridge::Impl {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    wl_output *output = nullptr;
    int outputWidth = 0, outputHeight = 0, outputScale = 1, outputTransform = 0;
    bool outputDone = false, lostGlobal = false;
    std::map<uint32_t, std::string> boundGlobals;
    static void outputGeometry(void *d, wl_output *, int32_t, int32_t, int32_t, int32_t,
                               int32_t, const char *, const char *, int32_t transform) {
        static_cast<Impl *>(d)->outputTransform = transform;
    }
    static void outputMode(void *d, wl_output *, uint32_t flags, int32_t w, int32_t h, int32_t) {
        if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
        auto &s = *static_cast<Impl *>(d);
        s.outputWidth = w; s.outputHeight = h;
        s.outputDone = false;
    }
    static void outputComplete(void *d, wl_output *) { static_cast<Impl *>(d)->outputDone = true; }
    static void outputScaleEvent(void *d, wl_output *, int32_t scale) { static_cast<Impl *>(d)->outputScale = scale; }

    wl_seat *seat = nullptr;
    zwlr_screencopy_manager_v1 *captureManager = nullptr;
    zwlr_screencopy_frame_v1 *frame = nullptr;
    zwp_virtual_keyboard_manager_v1 *keyboardManager = nullptr;
    zwp_virtual_keyboard_v1 *keyboard = nullptr;
    zwlr_virtual_pointer_manager_v1 *pointerManager = nullptr;
    zwlr_virtual_pointer_v1 *mouse = nullptr;
    zwlr_data_control_manager_v1 *dataManager = nullptr;
    zwlr_data_control_device_v1 *dataDevice = nullptr;
    zwlr_data_control_source_v1 *source = nullptr;
    zwlr_data_control_offer_v1 *selection = nullptr;
    std::map<zwlr_data_control_offer_v1 *, std::string> offers;
    std::string sourceText, clipboard, problem;
    bool clipboardDone = false;
    int clipboardFd = -1;
    double clipboardDeadline = 0;
    wl_buffer *buffer = nullptr;
    void *mapping = MAP_FAILED;
    size_t size = 0;
    uint32_t width = 0, height = 0, stride = 0, format = 0, flags = 0;
    double captureStarted = 0;
    uint64_t capturedFrames = 0;
    double lastFrameAt = 0;
    int contentTop = 0;
    std::vector<uint32_t> rgb = std::vector<uint32_t>(320 * 170, 0x0b111a);
    std::set<uint16_t> held;
    zwlr_foreign_toplevel_manager_v1 *toplevelManager = nullptr;
    struct Window {
        Impl *owner;
        zwlr_foreign_toplevel_handle_v1 *handle;
        Toplevel pending, current;
    };
    uint64_t nextWindow = 0;
    std::map<zwlr_foreign_toplevel_handle_v1 *, std::unique_ptr<Window>> windows;
    static void windowTitle(void *d, zwlr_foreign_toplevel_handle_v1 *, const char *s) {
        static_cast<Window *>(d)->pending.title = s;
    }
    static void windowApp(void *d, zwlr_foreign_toplevel_handle_v1 *, const char *s) {
        static_cast<Window *>(d)->pending.appId = s;
    }
    static void windowEnter(void *d, zwlr_foreign_toplevel_handle_v1 *, wl_output *output) {
        auto &w = *static_cast<Window *>(d);
        if (output == w.owner->output) w.pending.mapped = true;
    }
    static void windowLeave(void *d, zwlr_foreign_toplevel_handle_v1 *, wl_output *output) {
        auto &w = *static_cast<Window *>(d);
        if (output == w.owner->output) w.pending.mapped = false;
    }
    static void windowState(void *d, zwlr_foreign_toplevel_handle_v1 *, wl_array *states) {
        auto &w = *static_cast<Window *>(d);
        w.pending.active = false;
        auto values = static_cast<const uint32_t *>(states->data);
        for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i)
            if (values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
                w.pending.active = true;
    }
    static void windowDone(void *d, zwlr_foreign_toplevel_handle_v1 *) {
        auto &w = *static_cast<Window *>(d);
        if (!w.current.complete)
            log("wayland.window", "id=" + std::to_string(w.pending.id) + " app=" + w.pending.appId);
        w.pending.complete = true;
        w.current = w.pending;
    }
    static void windowClosed(void *d, zwlr_foreign_toplevel_handle_v1 *handle) {
        auto owner = static_cast<Window *>(d)->owner;
        log("wayland.window_closed", std::to_string(static_cast<Window *>(d)->current.id));
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        owner->windows.erase(handle);
    }
    static void windowParent(void *d, zwlr_foreign_toplevel_handle_v1 *, zwlr_foreign_toplevel_handle_v1 *p) {
        auto &w = *static_cast<Window *>(d);
        auto found = w.owner->windows.find(p);
        w.pending.parent = found == w.owner->windows.end() ? 0 : found->second->pending.id;
    }
    static void newWindow(void *d, zwlr_foreign_toplevel_manager_v1 *, zwlr_foreign_toplevel_handle_v1 *h) {
        auto &s = *static_cast<Impl *>(d);
        auto w = std::make_unique<Window>();
        w->owner = &s;
        w->handle = h;
        w->pending.id = ++s.nextWindow;
        static const zwlr_foreign_toplevel_handle_v1_listener listener = {
            windowTitle, windowApp, windowEnter, windowLeave, windowState, windowDone, windowClosed, windowParent};
        zwlr_foreign_toplevel_handle_v1_add_listener(h, &listener, w.get());
        s.windows.emplace(h, std::move(w));
    }
    static void windowsFinished(void *d, zwlr_foreign_toplevel_manager_v1 *manager) {
        auto &s = *static_cast<Impl *>(d);
        zwlr_foreign_toplevel_manager_v1_destroy(manager);
        s.toplevelManager = nullptr;
        s.lostGlobal = true;
        s.problem = "Wayland window monitoring ended";
        for (auto &[h, w] : s.windows) zwlr_foreign_toplevel_handle_v1_destroy(h);
        s.windows.clear();
    }
    static void global(void *data, wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
        auto &s = *static_cast<Impl *>(data);
        if (std::strcmp(interface, "zwlr_foreign_toplevel_manager_v1") == 0 && !s.toplevelManager) {
            s.boundGlobals[name] = "toplevel";
            s.toplevelManager = static_cast<zwlr_foreign_toplevel_manager_v1 *>(wl_registry_bind(
                r, name, &zwlr_foreign_toplevel_manager_v1_interface, std::min(version, 3u)));
            static const zwlr_foreign_toplevel_manager_v1_listener listener = {newWindow, windowsFinished};
            zwlr_foreign_toplevel_manager_v1_add_listener(s.toplevelManager, &listener, &s);
            return;
        }
        if (std::strcmp(interface, "wl_output") == 0 && !s.output && version >= 2) {
            s.boundGlobals[name] = "output";
            s.output = static_cast<wl_output *>(wl_registry_bind(r, name, &wl_output_interface, 2));
            static const wl_output_listener listener = {outputGeometry, outputMode, outputComplete, outputScaleEvent, nullptr, nullptr};
            wl_output_add_listener(s.output, &listener, &s);
            return;
        }
#define BIND(n, member, v)                                                                                   \
    if (std::strcmp(interface, #n) == 0 && !s.member) {                                                      \
        s.boundGlobals[name] = #n;                                                                  \
        s.member = static_cast<n *>(wl_registry_bind(r, name, &n##_interface, v));                           \
        return;                                                                                              \
    }
        BIND(wl_shm, shm, 1)
        BIND(wl_seat, seat, 1) BIND(zwlr_screencopy_manager_v1, captureManager, 1)
            BIND(zwp_virtual_keyboard_manager_v1, keyboardManager, 1)
                BIND(zwlr_virtual_pointer_manager_v1, pointerManager, 1)
                    BIND(zwlr_data_control_manager_v1, dataManager, 1)
#undef BIND
    }
    static void globalRemove(void *d, wl_registry *, uint32_t name) {
        auto &s = *static_cast<Impl *>(d);
        auto it = s.boundGlobals.find(name);
        if (it != s.boundGlobals.end()) {
            s.lostGlobal = true;
            s.problem = "Wayland removed required protocol: " + it->second;
        }
    }
    std::string protocols(std::map<std::string, bool> &previous) const {
        std::string missing;
        for (const auto &[name, present] : std::map<std::string, bool>{
                 {"output", output != nullptr}, {"shm", shm != nullptr}, {"seat", seat != nullptr},
                 {"screencopy", captureManager != nullptr}, {"virtual_keyboard", keyboardManager != nullptr},
                 {"virtual_pointer", pointerManager != nullptr}, {"clipboard", dataManager != nullptr},
                 {"toplevel", toplevelManager != nullptr}}) {
            if (!previous.count(name) || previous[name] != present)
                log("wayland." + name + (present ? "=OK" : "=MISSING"));
            previous[name] = present;
            if (!present) missing += (missing.empty() ? "" : ",") + name;
        }
        return missing;
    }
    void clearBuffer() {
        if (buffer)
            wl_buffer_destroy(buffer);
        buffer = nullptr;
        if (mapping != MAP_FAILED)
            munmap(mapping, size);
        mapping = MAP_FAILED;
        size = 0;
    }
    void finishCapture() {
        if (frame)
            zwlr_screencopy_frame_v1_destroy(frame);
        frame = nullptr;
    }
    static void frameBuffer(void *data, zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h,
                            uint32_t pitch) {
        auto &s = *static_cast<Impl *>(data);
        bool supported = fmt == WL_SHM_FORMAT_XRGB8888 || fmt == WL_SHM_FORMAT_ARGB8888 ||
                         fmt == WL_SHM_FORMAT_XBGR8888 || fmt == WL_SHM_FORMAT_ABGR8888 ||
                         fmt == WL_SHM_FORMAT_RGB565;
        unsigned bpp = fmt == WL_SHM_FORMAT_RGB565 ? 2 : 4;
        if (!supported || w == 0 || h == 0 || w > 4096 || h > 2160 || pitch < w * bpp ||
            uint64_t(pitch) * h > 64 * 1024 * 1024) {
            s.problem = "Unsupported capture geometry or format";
            s.finishCapture();
            return;
        }
        if (!s.buffer || w != s.width || h != s.height || pitch != s.stride || fmt != s.format) {
            s.clearBuffer();
            s.width = w;
            s.height = h;
            s.stride = pitch;
            s.format = fmt;
            s.size = static_cast<size_t>(pitch) * h;
            int fd = memfd_create("zero-browser-frame", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, s.size) < 0) {
                if (fd >= 0)
                    close(fd);
                s.problem = "Cannot allocate capture buffer";
                s.finishCapture();
                return;
            }
            s.mapping = mmap(nullptr, s.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (s.mapping == MAP_FAILED) {
                close(fd);
                s.problem = "Cannot map capture buffer";
                s.finishCapture();
                return;
            }
            auto pool = wl_shm_create_pool(s.shm, fd, s.size);
            s.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, pitch, fmt);
            wl_shm_pool_destroy(pool);
            close(fd);
        }
        zwlr_screencopy_frame_v1_copy(f, s.buffer);
    }
    static void frameFlags(void *d, zwlr_screencopy_frame_v1 *, uint32_t flags) {
        static_cast<Impl *>(d)->flags = flags;
    }
    static void frameReady(void *data, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {
        auto &s = *static_cast<Impl *>(data);
        const unsigned height = 170 - s.contentTop;
        for (unsigned y = 0; y < height; ++y) {
            unsigned sy = y * s.height / height;
            if (s.flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT)
                sy = s.height - 1 - sy;
            auto row = static_cast<const uint8_t *>(s.mapping) + sy * s.stride;
            for (unsigned x = 0; x < 320; ++x) {
                unsigned sx = x * s.width / 320;
                uint32_t pixel;
                if (s.format == WL_SHM_FORMAT_RGB565) {
                    uint16_t p;
                    std::memcpy(&p, row + sx * 2, 2);
                    pixel = ((p >> 11) * 255 / 31 << 16) | (((p >> 5) & 63) * 255 / 63 << 8) |
                            ((p & 31) * 255 / 31);
                } else {
                    std::memcpy(&pixel, row + sx * 4, 4);
                    if (s.format == WL_SHM_FORMAT_XBGR8888 || s.format == WL_SHM_FORMAT_ABGR8888)
                        pixel = ((pixel & 255) << 16) | (pixel & 0xff00) | ((pixel >> 16) & 255);
                }
                s.rgb[(y + s.contentTop) * 320 + x] = pixel & 0xffffff;
            }
        }
        if (++s.capturedFrames == 1)
            log("wayland.first_frame", std::to_string(s.width) + "x" + std::to_string(s.height));
        s.lastFrameAt = now();
        s.problem.clear();
        s.finishCapture();
    }
    static void frameFailed(void *data, zwlr_screencopy_frame_v1 *) {
        auto &s = *static_cast<Impl *>(data);
        s.problem = "Wayland capture failed";
        s.finishCapture();
    }
    static void mime(void *d, zwlr_data_control_offer_v1 *o, const char *type) {
        auto &s = *static_cast<Impl *>(d);
        if (std::strcmp(type, "text/plain;charset=utf-8") == 0 ||
            (s.offers[o].empty() && std::strcmp(type, "text/plain") == 0))
            s.offers[o] = type;
    }
    static void offer(void *d, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *o) {
        auto &s = *static_cast<Impl *>(d);
        s.offers[o] = "";
        static const zwlr_data_control_offer_v1_listener listener = {mime};
        zwlr_data_control_offer_v1_add_listener(o, &listener, d);
    }
    static void select(void *d, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *o) {
        auto &s = *static_cast<Impl *>(d);
        if (s.selection && s.selection != o) {
            s.offers.erase(s.selection);
            zwlr_data_control_offer_v1_destroy(s.selection);
        }
        s.selection = o;
    }
    static void finished(void *d, zwlr_data_control_device_v1 *) {
        static_cast<Impl *>(d)->problem = "Clipboard connection closed";
    }
    static void primary(void *, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *) {}
    static void sourceSend(void *d, zwlr_data_control_source_v1 *, const char *, int fd) {
        auto &s = *static_cast<Impl *>(d);
        size_t n = 0;
        while (n < s.sourceText.size()) {
            auto rc = write(fd, s.sourceText.data() + n, s.sourceText.size() - n);
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc <= 0)
                break;
            n += rc;
        }
        close(fd);
    }
    static void cancelled(void *d, zwlr_data_control_source_v1 *src) {
        auto &s = *static_cast<Impl *>(d);
        if (s.source == src)
            s.source = nullptr;
        zwlr_data_control_source_v1_destroy(src);
    }
    ~Impl() {
        for (auto &[h, w] : windows) zwlr_foreign_toplevel_handle_v1_destroy(h);
        if (toplevelManager) zwlr_foreign_toplevel_manager_v1_destroy(toplevelManager);
        if (clipboardFd >= 0)
            close(clipboardFd);
        finishCapture();
        clearBuffer();
        if (source)
            zwlr_data_control_source_v1_destroy(source);
        for (auto &[o, m] : offers)
            zwlr_data_control_offer_v1_destroy(o);
        if (dataDevice)
            zwlr_data_control_device_v1_destroy(dataDevice);
        if (dataManager)
            zwlr_data_control_manager_v1_destroy(dataManager);
        if (mouse)
            zwlr_virtual_pointer_v1_destroy(mouse);
        if (pointerManager)
            zwlr_virtual_pointer_manager_v1_destroy(pointerManager);
        if (keyboard)
            zwp_virtual_keyboard_v1_destroy(keyboard);
        if (keyboardManager)
            zwp_virtual_keyboard_manager_v1_destroy(keyboardManager);
        if (captureManager)
            zwlr_screencopy_manager_v1_destroy(captureManager);
        if (seat)
            wl_seat_destroy(seat);
        if (output)
            wl_output_destroy(output);
        if (shm)
            wl_shm_destroy(shm);
        if (registry)
            wl_registry_destroy(registry);
        if (display)
            wl_display_disconnect(display);
    }
};
WaylandBridge::WaylandBridge() : impl_(std::make_unique<Impl>()) {}
WaylandBridge::~WaylandBridge() = default;
void WaylandBridge::disconnect() { impl_ = std::make_unique<Impl>(); }
void WaylandBridge::connect(const std::string &name, const std::function<void()> &heartbeat) {
    disconnect();
    try {
        auto &s = *impl_;
        s.display = wl_display_connect(name.empty() ? nullptr : name.c_str());
        if (!s.display)
            throw std::runtime_error("Cannot connect to private Wayland display");
        s.registry = wl_display_get_registry(s.display);
        static const wl_registry_listener listener = {Impl::global, Impl::globalRemove};
        wl_registry_add_listener(s.registry, &listener, &s);
        // Registry globals (especially wl_output) may be advertised after the
        // first sync. Keep dispatching until every interface is present.
        std::map<std::string, bool> previous;
        std::string missing;
        const auto deadline = now() + 8;
        for (;;) {
            if (heartbeat) heartbeat();
            pump();
            missing = s.protocols(previous);
            if (missing.empty()) break;
            if (now() >= deadline)
                throw std::runtime_error("Private Wayland missing protocols: " + missing);
            pollfd fd{wl_display_get_fd(s.display), POLLIN, 0};
            poll(&fd, 1, 10);
        }
        s.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(s.keyboardManager, s.seat);
        const char keymap[] =
            "xkb_keymap { xkb_keycodes { include \"evdev+aliases(qwerty)\" }; xkb_types { include \"complete\" "
            "}; xkb_compatibility { include \"complete\" }; xkb_symbols { include \"pc+us+inet(evdev)\" }; };";
        int fd = memfd_create("zero-browser-keymap", MFD_CLOEXEC);
        if (fd < 0)
            throw std::runtime_error("Cannot create keyboard map");
        if (write(fd, keymap, sizeof keymap) != sizeof keymap) {
            close(fd);
            throw std::runtime_error("Cannot write keyboard map");
        }
        zwp_virtual_keyboard_v1_keymap(s.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, sizeof keymap);
        close(fd);
        s.mouse = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(s.pointerManager, s.seat);
        s.dataDevice = zwlr_data_control_manager_v1_get_data_device(s.dataManager, s.seat);
        static const zwlr_data_control_device_v1_listener dl = {Impl::offer, Impl::select, Impl::finished,
                                                                Impl::primary};
        zwlr_data_control_device_v1_add_listener(s.dataDevice, &dl, &s);
        // A cancellable sync also confirms that bind/input requests succeeded.
        bool done = false;
        auto callback = wl_display_sync(s.display);
        static const wl_callback_listener syncListener = {
            [](void *d, wl_callback *, uint32_t) { *static_cast<bool *>(d) = true; }};
        wl_callback_add_listener(callback, &syncListener, &done);
        try {
            const auto end = now() + 5;
            while (!done) {
                if (heartbeat) heartbeat();
                pump();
                if (now() >= end) throw std::runtime_error("Wayland input handshake timed out");
                if (!done) {
                    pollfd fd{wl_display_get_fd(s.display), POLLIN, 0};
                    poll(&fd, 1, 10);
                }
            }
        } catch (...) { wl_callback_destroy(callback); throw; }
        wl_callback_destroy(callback);
        log("wayland.protocols=READY");
    } catch (...) {
        // A failed registry/input handshake is not a usable connection. Destroy
        // every proxy before a subsequent attempt can connect to a fresh server.
        disconnect();
        throw;
    }
}
void WaylandBridge::pump() {
    auto &s = *impl_;
    if (!s.display)
        return;
    // Never call blocking dispatch/roundtrip: even a socket accepting clients
    // can belong to a stuck compositor. ESC and all deadlines must keep ticking.
    while (wl_display_prepare_read(s.display) != 0)
        if (wl_display_dispatch_pending(s.display) < 0)
            throw std::runtime_error("Wayland disconnected");
    if (wl_display_flush(s.display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(s.display);
        throw std::runtime_error("Wayland write failed");
    }
    pollfd p{wl_display_get_fd(s.display), POLLIN, 0};
    const auto rc = poll(&p, 1, 0);
    if (rc > 0 && (p.revents & POLLIN)) {
        if (wl_display_read_events(s.display) < 0)
            throw std::runtime_error("Wayland connection lost");
    } else wl_display_cancel_read(s.display);
    if ((rc > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) ||
        wl_display_dispatch_pending(s.display) < 0)
        throw std::runtime_error("Wayland connection lost");
    if (s.lostGlobal) throw std::runtime_error(s.problem);
    if (s.frame && now() - s.captureStarted > 3) {
        s.finishCapture();
        s.problem = "Capture timed out";
    }
    if (s.clipboardFd >= 0) {
        char b[4096];
        ssize_t n;
        while ((n = read(s.clipboardFd, b, sizeof b)) > 0) {
            if (s.clipboard.size() + n <= 8192)
                s.clipboard.append(b, n);
            else {
                s.clipboard.clear();
                break;
            }
        }
        if (n == 0 || now() > s.clipboardDeadline || s.clipboard.size() > 4096) {
            close(s.clipboardFd);
            s.clipboardFd = -1;
            s.clipboardDone = true;
        }
    }
}
void WaylandBridge::capture() {
    auto &s = *impl_;
    if (!s.display || s.frame)
        return;
    s.flags = 0;
    s.captureStarted = now();
    s.frame = zwlr_screencopy_manager_v1_capture_output(s.captureManager, 0, s.output);
    static const zwlr_screencopy_frame_v1_listener listener = {
        Impl::frameBuffer, Impl::frameFlags, Impl::frameReady, Impl::frameFailed, nullptr, nullptr, nullptr};
    zwlr_screencopy_frame_v1_add_listener(s.frame, &listener, &s);
    wl_display_flush(s.display);
}
bool WaylandBridge::connected() const {
    return impl_->display != nullptr;
}
uint64_t WaylandBridge::capturedFrames() const { return impl_->capturedFrames; }
double WaylandBridge::lastFrameAt() const { return impl_->lastFrameAt; }
bool WaylandBridge::outputReady(int w, int h) const {
    const auto &s = *impl_;
    return s.outputDone && s.outputWidth == w && s.outputHeight == h &&
           s.outputScale == 1 && s.outputTransform == WL_OUTPUT_TRANSFORM_NORMAL;
}
bool WaylandBridge::captureSize(int w, int h) const { return impl_->width == unsigned(w) && impl_->height == unsigned(h); }
void WaylandBridge::resetCapture() {
    impl_->finishCapture();
    impl_->problem.clear();
}
const std::vector<uint32_t> &WaylandBridge::pixels() const {
    return impl_->rgb;
}
void WaylandBridge::key(uint16_t code, bool down, uint32_t mods) {
    auto &s = *impl_;
    if (!s.keyboard)
        return;
    if (down)
        s.held.insert(code);
    else
        s.held.erase(code);
    zwp_virtual_keyboard_v1_modifiers(s.keyboard, mods, 0, 0, 0);
    zwp_virtual_keyboard_v1_key(s.keyboard, static_cast<uint32_t>(now() * 1000), code, down ? 1 : 0);
}
void WaylandBridge::tap(uint16_t code, uint32_t mods) {
    key(code, true, mods);
    key(code, false, mods);
    if (impl_->keyboard)
        zwp_virtual_keyboard_v1_modifiers(impl_->keyboard, 0, 0, 0, 0);
}
void WaylandBridge::releaseKeys() {
    auto held = impl_->held;
    for (auto k : held)
        key(k, false);
}
void WaylandBridge::paste(const std::string &text) {
    auto &s = *impl_;
    if (!s.dataDevice)
        return;
    if (s.source) {
        zwlr_data_control_source_v1_destroy(s.source);
        s.source = nullptr;
    }
    s.sourceText = text.substr(0, 4096);
    s.source = zwlr_data_control_manager_v1_create_data_source(s.dataManager);
    static const zwlr_data_control_source_v1_listener sl = {Impl::sourceSend, Impl::cancelled};
    zwlr_data_control_source_v1_add_listener(s.source, &sl, &s);
    zwlr_data_control_source_v1_offer(s.source, "text/plain;charset=utf-8");
    zwlr_data_control_source_v1_offer(s.source, "text/plain");
    zwlr_data_control_device_v1_set_selection(s.dataDevice, s.source);
    tap(KEY_V, 4);
}
void WaylandBridge::requestClipboard() {
    auto &s = *impl_;
    if (s.clipboardFd >= 0)
        close(s.clipboardFd);
    s.clipboardFd = -1;
    s.clipboard.clear();
    s.clipboardDone = false;
    if (!s.selection || s.offers[s.selection].empty()) {
        s.clipboardDone = true;
        return;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        s.clipboardDone = true;
        return;
    }
    s.clipboardFd = fds[0];
    s.clipboardDeadline = now() + 1;
    zwlr_data_control_offer_v1_receive(s.selection, s.offers[s.selection].c_str(), fds[1]);
    close(fds[1]);
}
bool WaylandBridge::clipboardResult(std::string &text) {
    if (!impl_->clipboardDone)
        return false;
    impl_->clipboardDone = false;
    text = impl_->clipboard;
    return true;
}
void WaylandBridge::contentTop(int pixels) {
    impl_->contentTop = std::clamp(pixels, 0, 40);
    std::fill(impl_->rgb.begin(), impl_->rgb.end(), 0x0b111a);
}
int WaylandBridge::contentHeight() const { return 170 - impl_->contentTop; }
void WaylandBridge::pointer(int x, int y) {
    auto &s = *impl_;
    if (!s.mouse || y < s.contentTop)
        return;
    zwlr_virtual_pointer_v1_motion_absolute(s.mouse, static_cast<uint32_t>(now() * 1000),
                                          x, y - s.contentTop, 320, contentHeight());
    zwlr_virtual_pointer_v1_frame(s.mouse);
}
void WaylandBridge::scroll(int horizontal, int vertical) {
    auto &s = *impl_;
    if (!s.mouse)
        return;
    auto t = static_cast<uint32_t>(now() * 1000);
    if (vertical)
        zwlr_virtual_pointer_v1_axis(s.mouse, t, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(vertical));
    if (horizontal)
        zwlr_virtual_pointer_v1_axis(s.mouse, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_int(horizontal));
    zwlr_virtual_pointer_v1_frame(s.mouse);
}
void WaylandBridge::mouseButton(bool down) {
    auto &s = *impl_;
    if (!s.mouse) return;
    zwlr_virtual_pointer_v1_button(s.mouse, static_cast<uint32_t>(now() * 1000), BTN_LEFT, down ? 1 : 0);
    zwlr_virtual_pointer_v1_frame(s.mouse);
}
void WaylandBridge::click(bool right) {
    auto &s = *impl_;
    if (!s.mouse)
        return;
    auto t = static_cast<uint32_t>(now() * 1000);
    const uint32_t button = right ? BTN_RIGHT : BTN_LEFT;
    zwlr_virtual_pointer_v1_button(s.mouse, t, button, 1);
    zwlr_virtual_pointer_v1_button(s.mouse, t + 1, button, 0);
    zwlr_virtual_pointer_v1_frame(s.mouse);
}
std::string WaylandBridge::error() const {
    return impl_->problem;
}
std::vector<Toplevel> WaylandBridge::toplevels() const {
    std::vector<Toplevel> result;
    for (const auto &[h, w] : impl_->windows) result.push_back(w->current);
    std::sort(result.begin(), result.end(), [](const Toplevel &a, const Toplevel &b) { return a.id < b.id; });
    return result;
}
bool WaylandBridge::closeToplevel(uint64_t id, const std::string &appId) {
    for (const auto &[h, w] : impl_->windows) {
        if (w->current.id == id && w->current.complete && w->current.active && w->current.appId == appId) {
            zwlr_foreign_toplevel_handle_v1_close(h);
            wl_display_flush(impl_->display);
            return true;
        }
    }
    return false;
}
bool WaylandBridge::activateToplevel(const std::string &appId) {
    if (appId.empty()) return false;
    for (const auto &[h, w] : impl_->windows) {
        if (w->current.complete && w->current.appId == appId) {
            zwlr_foreign_toplevel_handle_v1_activate(h, impl_->seat);
            wl_display_flush(impl_->display);
            return true;
        }
    }
    return false;
}
} // namespace zero

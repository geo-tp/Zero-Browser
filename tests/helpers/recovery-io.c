// Permit the test FIFO to act as an explicitly requested evdev keyboard.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

int ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    struct stat st;
    if (request == EVIOCGRAB && fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode)) return 0;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 320*170*4) {
        if (request == FBIOGET_VSCREENINFO) {
            struct fb_var_screeninfo *v = arg;
            memset(v, 0, sizeof(*v)); v->xres=320; v->yres=170; v->bits_per_pixel=32;
            v->red.offset=16; v->green.offset=8; v->blue.offset=0;
            v->red.length=v->green.length=v->blue.length=8;
            return 0;
        }
        if (request == FBIOGET_FSCREENINFO) {
            struct fb_fix_screeninfo *f = arg;
            memset(f, 0, sizeof(*f)); f->type=FB_TYPE_PACKED_PIXELS; f->visual=FB_VISUAL_TRUECOLOR;
            f->line_length=320*4; f->smem_len=320*170*4;
            return 0;
        }
    }
    int (*real_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
    return real_ioctl(fd, request, arg);
}

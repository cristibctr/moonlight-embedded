/* VIDAA firmware capture probe. Only capture handles are changed. */
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#ifdef CAPTURE_INNER_TIMING
/* Diagnostic build only. Interpose this process's existing capture call,
 * forward every argument unchanged, and record a narrower call interval.
 * The three-argument ABI is verified in the installed libHui/libmi code.
 * No preload, module, driver, stream or persistent setting is changed. */
static int (*real_capture_one)(uint32_t, const void *, void *);
int MI_CAP_CaptureOne(uint32_t handle, const void *input, void *output) {
    if (!real_capture_one) return 3;
    uint64_t start = now_us();
    int result = real_capture_one(handle, input, output);
    uint64_t end = now_us();
    fprintf(stderr, "inner_capture=%d start_us=%llu end_us=%llu\n", result,
            (unsigned long long)start, (unsigned long long)end);
    return result;
}
#endif

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s type width height output.raw|-\n", argv[0]);
        return 2;
    }
    unsigned type = strtoul(argv[1], NULL, 10);
    unsigned width = strtoul(argv[2], NULL, 10);
    unsigned height = strtoul(argv[3], NULL, 10);
    if ((type != 2 && type != 16) || width < 64 || width > 1920 ||
        height < 64 || height > 1080) return 2;
    int stream = strcmp(argv[4], "-") == 0;
    FILE *protocol = NULL;
    if (stream) {
        protocol = fdopen(dup(STDOUT_FILENO), "wb");
        if (!protocol || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return 3;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(15); /* Bound a stuck probe; never signal the stream process. */
    void *mi = dlopen("libmi.so", RTLD_NOW | RTLD_GLOBAL);
    if (!mi) { fprintf(stderr, "MI: %s\n", dlerror()); return 3; }
#ifdef CAPTURE_INNER_TIMING
    real_capture_one = dlsym(mi, "MI_CAP_CaptureOne");
    if (!real_capture_one || real_capture_one == MI_CAP_CaptureOne) return 3;
#endif
    int (*sys_init)(void *) = dlsym(mi, "MI_SYS_Init");
    int (*disp_init)(void *) = dlsym(mi, "MI_DISP_Init");
    if (!sys_init || !disp_init) return 3;
    struct { const char *name; uint32_t reserved[23]; } init = {"moonlight-capture", {0}};
    int rc = sys_init(&init);
    fprintf(stderr, "sys_init=%d\n", rc);
    if (rc) return 4;
    rc = disp_init(NULL);
    fprintf(stderr, "disp_init=%d\n", rc);
    if (rc && rc != 2) return 4;
    void *hui = dlopen("libHui.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!hui) { fprintf(stderr, "HUI: %s\n", dlerror()); return 3; }
    int (*available)(uint32_t) = dlsym(hui, "HUI_ScreenCaptureAvaliable");
    int (*capture)(uint32_t, void *) = dlsym(hui, "HUI_ScreenCapture");
    if (!available || !capture) return 3;
    rc = available(type);
    fprintf(stderr, "available=%d\n", rc);
    if (rc) return 5;
    const size_t capacity = 1920 * 1080 * 4 + 4096;
    uint8_t *pixels = malloc(capacity);
    if (!pixels) return 6;
    memset(pixels, 0xa5, capacity);
    /* ABI recovered from the installed libHui; 32-bit process only. */
    struct { uint8_t *pixels; uint32_t length, width, height, reserved[12]; }
        bitmap = {pixels, capacity, width, height, {0}};
    if (stream) { fprintf(protocol, "READY 1\n"); fflush(protocol); }
    alarm(0);
    for (;;) {
        unsigned id = 0;
        if (stream) {
            char command[80];
            if (!fgets(command, sizeof(command), stdin)) break;
            uint64_t received = now_us();
            if (sscanf(command, "PING %u", &id) == 1) {
                fprintf(protocol, "PONG %u %llu %llu\n", id,
                    (unsigned long long)received, (unsigned long long)now_us());
                fflush(protocol);
                continue;
            }
            if (!strncmp(command, "QUIT", 4)) break;
            if (sscanf(command, "CAP %u", &id) != 1) return 9;
        }
        alarm(15);
        uint64_t start = now_us();
        rc = capture(type, &bitmap);
        uint64_t end = now_us();
        alarm(0);
        /* This firmware writes BGRA pixels, but leaves length unchanged. */
        size_t length = (size_t)bitmap.width * bitmap.height * 4;
        fprintf(stderr, "capture=%d start_us=%llu end_us=%llu duration_us=%llu width=%u height=%u\n",
            rc, (unsigned long long)start, (unsigned long long)end,
            (unsigned long long)(end-start), bitmap.width, bitmap.height);
        if (rc || bitmap.pixels != pixels || length > capacity ||
            bitmap.width != width || bitmap.height != height) return 7;
        if (stream) {
            fprintf(protocol, "FRAME %u %llu %llu %u %u %zu\n", id,
                (unsigned long long)start, (unsigned long long)end,
                bitmap.width, bitmap.height, length);
            if (fwrite(pixels, 1, length, protocol) != length || fflush(protocol)) return 8;
        } else {
            FILE *out = fopen(argv[4], "wb");
            if (!out) { perror(argv[4]); return 8; }
            int good = fwrite(pixels, 1, length, out) == length;
            if (fclose(out)) good = 0;
            if (!good) return 8;
            break;
        }
    }
    free(pixels);
    if (protocol) fclose(protocol);
    return 0;
}

/* Diagnostics; --test-window-unmute clears only SET_WINDOW for ten seconds,
 * only for the named, frame-ready Moonlight input, then restores the flag. */
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

typedef struct {
  uint8_t is_input;
  uint8_t reserved[3];
  uint32_t module;
  uint32_t show_type;
} ConnectedConditions;
static volatile sig_atomic_t interrupted;
static void on_signal(int number) { interrupted = number; }
static unsigned long long monotonic_us(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (unsigned long long)value.tv_sec * 1000000 + value.tv_nsec / 1000;
}

int main(int argc, char **argv) {
  int test_unmute = argc == 3 && !strcmp(argv[2], "--test-window-unmute");
  int cycle_game = argc == 3 && !strcmp(argv[2], "--cycle-game-mode");
  if ((argc != 2 && !test_unmute && !cycle_game) || strncmp(argv[1], "moonlight-", 10)) return 64;
  setvbuf(stdout, NULL, _IOLBF, 0);
  alarm(30);
  void *mi = dlopen("libmi.so", RTLD_NOW | RTLD_GLOBAL);
  if (!mi) { fprintf(stderr, "%s\n", dlerror()); return 1; }
  int (*sys_init)(void *) = dlsym(mi, "MI_SYS_Init");
  int (*disp_init)(void *) = dlsym(mi, "MI_DISP_Init");
  int (*video_init)(void *) = dlsym(mi, "MI_VIDEO_Init");
  int (*disp_handle)(const void *, uint32_t *) = dlsym(mi, "MI_DISP_GetHandle");
  int (*video_handle)(const void *, uint32_t *) = dlsym(mi, "MI_VIDEO_GetHandle");
  int (*video_attr)(uint32_t, uint32_t, const void *, void *) = dlsym(mi, "MI_VIDEO_GetAttr");
  int (*mute)(uint32_t, uint32_t, uint8_t *) = dlsym(mi, "MI_DISP_GetMuteStatus");
  int (*window)(uint32_t, uint8_t *) = dlsym(mi, "MI_DISP_GetWindowEnable");
  if (!sys_init || !disp_init || !video_init || !disp_handle || !video_handle || !video_attr || !mute || !window) return 2;
  struct { const char *name; uint32_t reserved[23]; } init = {"moonlight-pipeline-probe", {0}};
  int result = sys_init(&init);
  printf("sys_init=%d\n", result);
  if (result) return 3;
  printf("disp_init=%d video_init=%d\n", disp_init(NULL), video_init(NULL));
  const char *display_name = "MI_DISP_HD0";
  uint32_t display = 0;
  result = disp_handle(&display_name, &display);
  printf("display=%08x result=%d\n", display, result);
  if (result) return 4;
  const char *rect_names[] = {"MI_DISP_GetInputRect", "MI_DISP_GetOutputRect"};
  for (unsigned i = 0; i < 2; ++i) {
    int (*get_rect)(uint32_t, void *) = dlsym(mi, rect_names[i]);
    uint64_t storage[64];
    memset(storage, 0xa5, sizeof(storage));
    if (!get_rect) continue;
    result = get_rect(display, storage);
    printf("%s result=%d data=", rect_names[i], result);
    for (unsigned j = 0; j < 24; ++j) printf("%02x", ((uint8_t *)storage)[j]);
    putchar('\n');
  }
  uint8_t enabled = 0xa5;
  result = window(display, &enabled);
  printf("window result=%d enabled=%u\n", result, enabled);
  const char *stat_names[] = {"mi_disp_GetWaitSyncNum", "mi_disp_GetMaxFrameNum",
                             "mi_disp_GetFrameBufferMode"};
  for (unsigned i = 0; i < 3; ++i) {
    int (*get_stat)(uint32_t, void *) = dlsym(mi, stat_names[i]);
    uint64_t storage[64] = {0};
    if (!get_stat) continue;
    result = get_stat(display, storage);
    printf("%s result=%d value=%llu\n", stat_names[i], result,
           (unsigned long long)storage[0]);
  }
  int (*get_disp_attr)(uint32_t, uint32_t, const void *, void *) =
      dlsym(mi, "MI_DISP_GetAttr");
  /* Read-only cases verified in this firmware's MI_DISP_GetAttr dispatch:
   * 2 is PQ frame latency, 6 includes video path delays, 7 DMS window timing. */
  const uint32_t disp_attrs[] = {2, 6, 7};
  if (get_disp_attr) for (unsigned i = 0; i < 3; ++i) {
    uint64_t storage[64];
    memset(storage, 0xa5, sizeof(storage));
    result = get_disp_attr(display, disp_attrs[i], NULL, storage);
    printf("display attr=%u result=%d data=", disp_attrs[i], result);
    for (unsigned j = 0; j < 40; ++j) printf("%02x", ((uint8_t *)storage)[j]);
    putchar('\n');
  }
  int (*get_picture)(uint32_t, uint32_t, const void *, void *) =
      dlsym(mi, "MI_DISP_GetPictureParam");
  const uint32_t picture_attrs[] = {1, 8, 9, 12, 13, 14};
  if (get_picture) for (unsigned i = 0; i < 6; ++i) {
    uint64_t storage[64];
    memset(storage, 0xa5, sizeof(storage));
    result = get_picture(display, picture_attrs[i], NULL, storage);
    printf("picture attr=%u result=%d data=", picture_attrs[i], result);
    for (unsigned j = 0; j < 16; ++j) printf("%02x", ((uint8_t *)storage)[j]);
    putchar('\n');
  }
  for (unsigned i=0; i<13; ++i) {
    uint32_t flag = i == 12 ? UINT32_C(0x80000000) : 1u << i;
    uint8_t state = 0xa5;
    result = mute(display, flag, &state);
    printf("mute flag=%08x result=%d state=%u\n", flag, result, state);
  }
  const char *decoder_name = argv[1];
  int (*sync_init)(void *) = dlsym(mi, "MI_SYNC_Init");
  int (*sync_handle)(const void *, uint32_t *) = dlsym(mi, "MI_SYNC_GetHandle");
  int (*sync_mode)(uint32_t, uint32_t *, void *) = dlsym(mi, "MI_SYNC_GetMode");
  if (sync_init && sync_handle && sync_mode && sync_init(NULL) == 0) {
    uint32_t sync = 0, mode = UINT32_MAX;
    // Verified GetMode output is an enum plus a 24-byte parameter union.
    uint64_t parameters[3] = {0};
    int found = sync_handle(&decoder_name, &sync);
    printf("sync handle=%08x result=%d\n", sync, found);
    if (found == 0) {
      int got = sync_mode(sync, &mode, parameters);
      printf("sync mode result=%d mode=%u\n", got, mode);
    }
  }
  uint32_t decoder = 0;
  result = video_handle(&decoder_name, &decoder);
  printf("decoder=%08x result=%d\n", decoder, result);
  if (result) return 5;
  const uint32_t attrs[] = {0x101, 0x104, 0x105, 0x106, 0x107, 0x108};
  for (unsigned i=0; i<sizeof(attrs)/sizeof(attrs[0]); ++i) {
    uint64_t buf[64];
    memset(buf, 0xa5, sizeof(buf));
    result = video_attr(decoder, attrs[i], NULL, buf);
    printf("video attr=%03x result=%d data=", attrs[i], result);
    for (unsigned j=0; j<48; ++j) printf("%02x", ((uint8_t *)buf)[j]);
    putchar('\n');
  }
  if (cycle_game) {
    int (*get_num)(uint32_t, const ConnectedConditions *, uint32_t *) =
        dlsym(mi, "MI_DISP_GetConnectedNum");
    int (*get_connected)(uint32_t, const ConnectedConditions *, uint32_t, uint32_t *) =
        dlsym(mi, "MI_DISP_GetConnected");
    int (*set_picture)(uint32_t, uint32_t, const void *) = dlsym(mi, "MI_DISP_SetPictureParam");
    if (!get_num || !get_connected || !get_picture || !set_picture) return 11;
    const ConnectedConditions conditions = {1, {0}, 199, 1};
    uint32_t count = 0, input = 0;
    uint8_t ready = 0, original = 0;
    if (get_num(display, &conditions, &count) || count != 1 ||
        get_connected(display, &conditions, 1, &input) || input != decoder ||
        video_attr(decoder, 0x105, NULL, &ready) || ready != 1 ||
        get_picture(display, 9, NULL, &original) || original != 1) {
      fprintf(stderr, "Game cycle refused: count=%u input=%08x ready=%u original=%u\n",
              count, input, ready, original);
      return 12;
    }
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGALRM, &action, NULL);
    uint32_t disabled = 0, restore = original;
    unsigned long long off_time = monotonic_us();
    result = set_picture(display, 9, &disabled);
    printf("game_cycle_off result=%d tv_us=%llu\n", result, off_time);
    if (!result && !interrupted) usleep(500000);
    count = 0; input = 0;
    if (get_num(display, &conditions, &count) || count != 1 ||
        get_connected(display, &conditions, 1, &input) || input != decoder) {
      fprintf(stderr, "Ownership changed; do not change another input's game mode.\n");
      return 13;
    }
    /* Restore even when the first setter reports an error. */
    unsigned long long on_time = monotonic_us();
    int restored = set_picture(display, 9, &restore);
    uint8_t actual = 0;
    int verify = -1;
    for (unsigned i = 0; i < 20; ++i) {
      verify = get_picture(display, 9, NULL, &actual);
      if (!verify && actual == original) break;
      usleep(50000);
    }
    printf("game_cycle_restore result=%d verify=%d actual=%u tv_us=%llu\n",
           restored, verify, actual, on_time);
    if (result || restored || verify || actual != original) return 14;
  }
  if (test_unmute) {
    int (*get_num)(uint32_t, const ConnectedConditions *, uint32_t *) =
        dlsym(mi, "MI_DISP_GetConnectedNum");
    int (*get_connected)(uint32_t, const ConnectedConditions *, uint32_t, uint32_t *) =
        dlsym(mi, "MI_DISP_GetConnected");
    int (*set_unmute)(uint32_t, uint32_t) = dlsym(mi, "MI_DISP_SetUnMute");
    int (*set_mute)(uint32_t, uint32_t) = dlsym(mi, "MI_DISP_SetMute");
    if (!get_num || !get_connected || !set_unmute || !set_mute) return 6;
    const ConnectedConditions conditions = {1, {0}, 199, 1};
    uint32_t count = 0, input = 0;
    uint8_t ready = 0, original = 0;
    if (get_num(display, &conditions, &count) || count != 1 ||
        get_connected(display, &conditions, 1, &input) || input != decoder ||
        video_attr(decoder, 0x105, NULL, &ready) || ready != 1 ||
        mute(display, 4, &original) || original != 1) {
      fprintf(stderr, "Unmute refused: input_count=%u input=%08x ready=%u original=%u\n",
              count, input, ready, original);
      return 7;
    }
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGALRM, &action, NULL);
    result = set_unmute(display, 4);
    printf("temporary SET_WINDOW unmute=%d original=%u\n", result, original);
    if (result) return 8;
    for (unsigned i = 0; i < 10 && !interrupted; ++i) sleep(1);
    count = 0; input = 0;
    if (get_num(display, &conditions, &count) || count != 1 ||
        get_connected(display, &conditions, 1, &input) || input != decoder) {
      fprintf(stderr, "Ownership changed; do not mute another display input.\n");
      return 9;
    }
    result = set_mute(display, 4);
    uint8_t restored = 0;
    int verify = mute(display, 4, &restored);
    printf("restore SET_WINDOW mute=%d verify=%d state=%u\n", result, verify, restored);
    if (result || verify || restored != original) return 10;
  }
  return 0;
}

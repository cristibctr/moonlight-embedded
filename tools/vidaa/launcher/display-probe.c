/* Read the active display's game-processing flag. Writes need --set-game 0|1. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int set_game = -1;
  if (argc == 3 && !strcmp(argv[1], "--set-game") &&
      (!strcmp(argv[2], "0") || !strcmp(argv[2], "1")))
    set_game = argv[2][0] - '0';
  else if (argc != 1) return 64;
  void *mi = dlopen("libmi.so", RTLD_NOW | RTLD_GLOBAL);
  if (!mi) { fprintf(stderr, "%s\n", dlerror()); return 1; }
  int (*sys_init)(void *) = dlsym(mi, "MI_SYS_Init");
  int (*disp_init)(void *) = dlsym(mi, "MI_DISP_Init");
  int (*get_handle)(const void *, uint32_t *) = dlsym(mi, "MI_DISP_GetHandle");
  int (*get_picture)(uint32_t, uint32_t, const void *, void *) =
      dlsym(mi, "MI_DISP_GetPictureParam");
  if (!sys_init || !disp_init || !get_handle || !get_picture) return 2;
  struct { const char *name; uint32_t reserved[23]; } init = {"moonlight-display-check", {0}};
  int result = sys_init(&init);
  printf("sys_init=%d\n", result);
  if (result) return 3;
  result = disp_init(NULL);
  printf("disp_init=%d\n", result);
  if (result) return 4;
  const char *name = "MI_DISP_HD0";
  uint32_t display = 0;
  result = get_handle(&name, &display);
  printf("display=%08x result=%d\n", display, result);
  if (result) return 5;
  if (set_game >= 0) {
    int (*set_picture)(uint32_t, uint32_t, const void *) =
        dlsym(mi, "MI_DISP_SetPictureParam");
    if (!set_picture) return 7;
    /* The vendor HUI caller supplies a full zero-extended 32-bit value. */
    uint32_t value = (uint32_t)set_game;
    result = set_picture(display, 9, &value);
    printf("game_mode_set=%d value=%u\n", result, value);
    if (result) return 8;
    usleep(500000);
  }
  uint64_t storage[8];
  memset(storage, 0xa5, sizeof(storage));
  result = get_picture(display, 9, NULL, storage);
  printf("game_mode_get=%d bytes=", result);
  for (unsigned i = 0; i < 16; i++) printf("%02x ", ((uint8_t *)storage)[i]);
  putchar('\n');
  return result ? 6 : 0;
}

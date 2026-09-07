#include "../../../src/video/vidaa_bitstream.h"
#include <assert.h>
#include <stdio.h>

size_t append_filler(uint8_t *packet, size_t length, size_t capacity) {
  return vidaa_append_hevc_filler(packet, length, capacity);
}

#ifndef VIDAA_TEST_LIBRARY
int main(void) {
  uint8_t data[40], original[40];
  memset(data, 0xa5, sizeof(data));
  memcpy(original, data, sizeof(data));
  assert(append_filler(NULL, 1, 40) == 0);
  assert(append_filler(data, 0, 40) == 0);
  assert(append_filler(data, SIZE_MAX, 40) == 0);
  assert(append_filler(data, 34, 40) == 0);
  assert(memcmp(data, original, sizeof(data)) == 0);
  assert(append_filler(data + 8, 17, 24) == 24);
  assert(memcmp(data, original, 25) == 0);
  assert(memcmp(data + 25, "\0\0\0\1\x4c\1\x80", 7) == 0);
  assert(memcmp(data + 32, original + 32, 8) == 0);
  puts("VIDAA HEVC filler: bounds and guard checks passed");
  return 0;
}
#endif

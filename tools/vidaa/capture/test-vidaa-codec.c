#include "../../../src/video/vidaa_codec.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static void check(uint32_t codec, uint16_t width, uint16_t height) {
  struct {
    unsigned char before[16];
    MIVideoCodecParams params;
    unsigned char after[16];
  } guarded;
  memset(&guarded, 0xa5, sizeof(guarded));
  MIInjplayVideoCodecData input = {0};
  input.codec_type = codec;
  uint32_t fps = 60, base = 1;
  memcpy(input.codec_attributes, &width, 2);
  memcpy(input.codec_attributes + 2, &height, 2);
  memcpy(input.codec_attributes + 4, &fps, 4);
  memcpy(input.codec_attributes + 8, &base, 4);
  vidaa_video_codec_params(&guarded.params, &input);
  assert(offsetof(MIVideoCodecParams, codec_attributes) == 4);
  assert(memcmp(&guarded.params, &input, sizeof(input)) == 0);
  for (size_t i = sizeof(input.codec_attributes);
       i < sizeof(guarded.params.codec_attributes); ++i)
    assert(guarded.params.codec_attributes[i] == 0);
  for (size_t i = 0; i < sizeof(guarded.before); ++i) {
    assert(guarded.before[i] == 0xa5);
    assert(guarded.after[i] == 0xa5);
  }
}

int main(void) {
  check(0x0b, 1920, 1080);
  check(0x10, 1920, 1080);
  check(0x0b, 3840, 2160);
  check(0x10, 3840, 2160);
  puts("VIDAA codec ABI: four cases passed");
  return 0;
}

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sps.h>
#include <h264_stream.h>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  FILE* input = fopen(argv[1], "rb");
  assert(input);
  fseek(input, 0, SEEK_END);
  long length = ftell(input);
  rewind(input);
  unsigned char* data = malloc((size_t)length);
  assert(data && fread(data, 1, (size_t)length, input) == (size_t)length);
  fclose(input);
  assert(length > 8 && !memcmp(data, "\0\0\0\1", 4) && (data[4]&31) == 7);
  int end = 5;
  while (end+4 < length && memcmp(data+end, "\0\0\0\1", 4)) end++;
  assert(end < 256);
  LENTRY entry = {.data = (char*)data, .length = end, .bufferType = BUFFER_TYPE_SPS};
  unsigned char fixed[512], again[512];
  assert(gs_sps_add_timing(&entry, 0, fixed, sizeof(fixed)) < 0);
  assert(gs_sps_add_timing(&entry, 60, fixed, 4) < 0);
  int result = gs_sps_add_timing(&entry, 60, fixed, sizeof(fixed));
  assert(result > 0);
  h264_stream_t* before = h264_new();
  h264_stream_t* after = h264_new();
  assert(read_nal_unit(before, data+4, end-4) > 0);
  assert(read_nal_unit(after, fixed+4, result-4) > 0);
  assert(!before->sps->vui.timing_info_present_flag);
  assert(after->sps->vui.timing_info_present_flag);
  assert(after->sps->vui.num_units_in_tick == 1 && after->sps->vui.time_scale == 120);
  /* The original reference count and other SPS semantics must stay intact. */
  before->sps->vui.timing_info_present_flag = 1;
  before->sps->vui.num_units_in_tick = 1;
  before->sps->vui.time_scale = 120;
  before->sps->vui.fixed_frame_rate_flag = 1;
  assert(!memcmp(before->sps, after->sps, sizeof(sps_t)));
  entry.data = (char*)fixed;
  entry.length = result;
  assert(gs_sps_add_timing(&entry, 120, again, sizeof(again)) == result);
  assert(!memcmp(fixed, again, (size_t)result));
  FILE* output = fopen(argv[2], "wb");
  assert(output);
  assert(fwrite(fixed, 1, (size_t)result, output) == (size_t)result);
  assert(fwrite(data+end, 1, (size_t)length-end, output) == (size_t)length-end);
  assert(!fclose(output));
  h264_free(before);
  h264_free(after);
  free(data);
  printf("SPS timing tests passed; %d -> %d bytes; all other SPS fields preserved\n", end, result);
  return 0;
}

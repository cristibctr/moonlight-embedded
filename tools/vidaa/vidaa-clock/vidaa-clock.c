#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*HuiOpenFn)(void* open_info, uint32_t* handle);
typedef int (*HuiSetCodecFn)(uint32_t handle, uint32_t codec,
                             uint32_t stream_type, void* codec_info);
typedef int (*HuiWriteDataFn)(uint32_t handle, const void* data,
                              uint32_t length, uint64_t pts);
typedef int (*HuiStartFn)(uint32_t handle);
typedef int (*HuiPauseFn)(uint32_t handle);
typedef int (*MiMmStartFn)(int handle, void* start_params);
typedef int (*MiMmSetAttrFn)(int handle, uint32_t attr_type,
                             const void* params);
typedef int (*MiGetHandleFn)(const void* query_params, uint32_t* handle);
typedef int (*MiOpenHandleFn)(uint32_t handle, const void* open_params);

static void dump_query(const char* name, const void* query_params,
                       uint32_t handle, int result) {
  if (getenv("MOONLIGHT_VIDAA_DEBUG") == NULL)
    return;

  const unsigned char* bytes = (const unsigned char*)query_params;
  fprintf(stderr, "VIDAA clock: %s query=", name);
  if (bytes == NULL) {
    fprintf(stderr, "null");
  } else {
    for (unsigned int i = 0; i < 32; i++)
      fprintf(stderr, "%02x%s", bytes[i], i == 31 ? "" : " ");
    const char* query_name = NULL;
    memcpy(&query_name, bytes, sizeof(query_name));
    if (query_name != NULL)
      fprintf(stderr, " name=%.48s", query_name);
  }
  fprintf(stderr, " handle=0x%08x result=%d\n", handle, result);
}

int MI_DISP_GetHandle(const void* query_params, uint32_t* handle) {
  static MiGetHandleFn real_get_handle;
  if (real_get_handle == NULL)
    real_get_handle =
        (MiGetHandleFn)dlsym(RTLD_NEXT, "MI_DISP_GetHandle");
  if (real_get_handle == NULL)
    return 1;

  int result = real_get_handle(query_params, handle);
  dump_query("MI_DISP_GetHandle", query_params,
             handle != NULL ? *handle : 0, result);
  return result;
}

int MI_DISP_Open(uint32_t handle, const void* open_params) {
  static MiOpenHandleFn real_open;
  if (real_open == NULL)
    real_open = (MiOpenHandleFn)dlsym(RTLD_NEXT, "MI_DISP_Open");
  if (real_open == NULL)
    return 1;

  int result = real_open(handle, open_params);
  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL) {
    const unsigned char* bytes = (const unsigned char*)open_params;
    fprintf(stderr, "VIDAA clock: MI_DISP_Open handle=0x%08x params=", handle);
    if (bytes == NULL) {
      fprintf(stderr, "null");
    } else {
      for (unsigned int i = 0; i < 64; i++)
        fprintf(stderr, "%02x%s", bytes[i], i == 63 ? "" : " ");
    }
    fprintf(stderr, " result=%d\n", result);
  }
  return result;
}

int MI_AOUT_GetHandle(const void* query_params, uint32_t* handle) {
  static MiGetHandleFn real_get_handle;
  if (real_get_handle == NULL)
    real_get_handle =
        (MiGetHandleFn)dlsym(RTLD_NEXT, "MI_AOUT_GetHandle");
  if (real_get_handle == NULL)
    return 1;

  int result = real_get_handle(query_params, handle);
  dump_query("MI_AOUT_GetHandle", query_params,
             handle != NULL ? *handle : 0, result);
  return result;
}

int MI_MM_SetAttr(int handle, uint32_t attr_type, const void* params) {
  static MiMmSetAttrFn real_set_attr;
  if (real_set_attr == NULL)
    real_set_attr = (MiMmSetAttrFn)dlsym(RTLD_NEXT, "MI_MM_SetAttr");
  if (real_set_attr == NULL)
    return 1;

  const char* mode = getenv("MOONLIGHT_VIDAA_MODE");
  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL && params != NULL &&
      attr_type == UINT32_C(0x1013))
    fprintf(stderr,
            "VIDAA clock: MI_MM_SetAttr handle=%d type=0x%x words=%u,%u\n",
            handle, attr_type, ((const uint32_t*)params)[0],
            ((const uint32_t*)params)[1]);
  if (mode != NULL && strcmp(mode, "direct-apstream") == 0 &&
      attr_type == UINT32_C(0x1013) && params != NULL) {
    /* E_MI_MM_AP_TYPE_ANDROID_STREAMING is 7. It keeps ES injection but asks
     * the media service to use its streaming buffer policy. */
    ((uint32_t*)params)[0] = 7;
    fprintf(stderr, "VIDAA clock: selected Android streaming AP profile\n");
  }
  return real_set_attr(handle, attr_type, params);
}

int MI_MM_Start(int handle, void* start_params) {
  static MiMmStartFn real_start;
  if (real_start == NULL)
    real_start = (MiMmStartFn)dlsym(RTLD_NEXT, "MI_MM_Start");
  if (real_start == NULL)
    return 1;

  unsigned char* bytes = (unsigned char*)start_params;
  const char* mode = getenv("MOONLIGHT_VIDAA_MODE");
  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL && bytes != NULL) {
    fprintf(stderr, "VIDAA clock: MI_MM_Start before handle=%d bytes=", handle);
    for (unsigned int i = 0; i < 56; i++)
      fprintf(stderr, "%02x%s", bytes[i], i == 55 ? "" : " ");
    fprintf(stderr, "\n");
  }
  if (mode != NULL && strcmp(mode, "direct-lowlatency") == 0 &&
      bytes != NULL) {
    /* MI_MM_StartParams_t byte 44 is bIsLowLatencyMode. Set only this
     * dedicated flag and retain the normal ES application type. */
    bytes[44] = 1;
    fprintf(stderr, "VIDAA clock: enabled MI_MM low-latency mode\n");
  }
  return real_start(handle, start_params);
}

int HUI_StreamInjectOpen(void* open_info, uint32_t* handle) {
  static HuiOpenFn real_open;
  if (real_open == NULL)
    real_open = (HuiOpenFn)dlsym(RTLD_NEXT, "HUI_StreamInjectOpen");
  if (real_open == NULL)
    return 1;

  uint32_t* fields = (uint32_t*)open_info;
  const char* mode = getenv("MOONLIGHT_VIDAA_MODE");
  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL) {
    fprintf(stderr,
            "VIDAA clock: open before stream=%u drm=%u index=%u "
            "app=%u only_init=%u media_sync=%u fields=",
            fields[1], fields[2], fields[4], fields[6], fields[7], fields[8]);
    for (unsigned int i = 0; i < 9; i++)
      fprintf(stderr, "%s%u", i == 0 ? "" : ",", fields[i]);
    fprintf(stderr, "\n");
  }
  if (mode != NULL && strcmp(mode, "direct-miracast") == 0 &&
      fields[1] == 2) {
    /* App type 1 selects the firmware's Miracast start parameters. This is
     * the interactive, low-latency hardware decoder path. */
    fields[6] = 1;
    fprintf(stderr, "VIDAA clock: selected Miracast app type for video\n");
  }
  if (mode != NULL && strcmp(mode, "mediasync") == 0 && fields[1] == 2) {
    /* HUI_StreamInjectOpenInfo offset 0x20 is bMediaSync. The HMP MSE
     * adapter always clears it, which leaves this firmware's video clock at
     * its first PTS. Enable it only for the video injector. */
    fields[8] = 1;
    fprintf(stderr, "VIDAA clock: enabled HUI media sync for video\n");
  }
  return real_open(open_info, handle);
}

int HUI_StreamInjectSetCodec(uint32_t handle, uint32_t codec,
                             uint32_t stream_type, void* codec_info) {
  static HuiSetCodecFn real_set_codec;
  if (real_set_codec == NULL)
    real_set_codec =
        (HuiSetCodecFn)dlsym(RTLD_NEXT, "HUI_StreamInjectSetCodec");
  if (real_set_codec == NULL)
    return 1;

  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL) {
    const unsigned char* info = (const unsigned char*)codec_info;
    fprintf(stderr,
            "VIDAA clock: set codec handle=%u codec=0x%x stream=%u "
            "info=%p",
            handle, codec, stream_type, codec_info);
    if (info != NULL) {
      unsigned int i;
      fprintf(stderr, " bytes=");
      for (i = 0; i < 24; i++)
        fprintf(stderr, "%02x%s", info[i], i == 23 ? "" : " ");
    }
    fprintf(stderr, "\n");
  }

  int result = real_set_codec(handle, codec, stream_type, codec_info);
  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL)
    fprintf(stderr, "VIDAA clock: set codec result=%d\n", result);
  return result;
}

int HUI_StreamInjectWriteData(uint32_t handle, const void* data,
                              uint32_t length, uint64_t pts) {
  static HuiWriteDataFn real_write;
  static unsigned int audio_writes;
  static unsigned int video_writes;
  if (real_write == NULL)
    real_write =
        (HuiWriteDataFn)dlsym(RTLD_NEXT, "HUI_StreamInjectWriteData");
  if (real_write == NULL)
    return 1;

  unsigned int* count = handle == UINT32_C(0xfffffe00)
                            ? &audio_writes : &video_writes;
  (*count)++;
  int result = real_write(handle, data, length, pts);
  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL &&
      (*count <= 5 || result != 0)) {
    const unsigned char* bytes = (const unsigned char*)data;
    fprintf(stderr,
            "VIDAA clock: write handle=%u count=%u length=%u pts=%llu "
            "head=%02x %02x %02x %02x result=%d\n",
            handle, *count, length, (unsigned long long)pts,
            length > 0 ? bytes[0] : 0, length > 1 ? bytes[1] : 0,
            length > 2 ? bytes[2] : 0, length > 3 ? bytes[3] : 0,
            result);
  }
  return result;
}

int HUI_StreamInjectStart(uint32_t handle) {
  static HuiStartFn real_start;
  static unsigned int audio_starts;
  static unsigned int video_starts;
  if (real_start == NULL)
    real_start = (HuiStartFn)dlsym(RTLD_NEXT, "HUI_StreamInjectStart");
  if (real_start == NULL)
    return 1;

  unsigned int* count = handle == UINT32_C(0xfffffe00)
                            ? &audio_starts : &video_starts;
  (*count)++;
  int result = real_start(handle);
  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL &&
      (*count <= 10 || result != 0))
    fprintf(stderr,
            "VIDAA clock: start handle=%u count=%u result=%d\n",
            handle, *count, result);
  return result;
}

int HUI_StreamInjectPause(uint32_t handle) {
  static HuiPauseFn real_pause;
  static unsigned int pauses;
  if (real_pause == NULL)
    real_pause = (HuiPauseFn)dlsym(RTLD_NEXT, "HUI_StreamInjectPause");
  if (real_pause == NULL)
    return 1;

  pauses++;
  if (getenv("MOONLIGHT_VIDAA_NOPAUSE") != NULL) {
    fprintf(stderr,
            "VIDAA clock: blocked automatic pause handle=%u count=%u\n",
            handle, pauses);
    return 0;
  }

  int result = real_pause(handle);
  if (getenv("MOONLIGHT_VIDAA_DEBUG") != NULL)
    fprintf(stderr, "VIDAA clock: pause handle=%u count=%u result=%d\n",
            handle, pauses, result);
  return result;
}

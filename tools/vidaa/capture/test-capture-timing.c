#define CAPTURE_INNER_TIMING 1
#define main capture_program_main
#include "capture.c"
#undef main
#include <assert.h>

static const uint32_t input_fixture = 0x87654321;
static uint32_t output_fixture;
static int fake_capture(uint32_t handle, const void *input, void *output) {
    assert(handle == 0x12345678);
    assert(input == &input_fixture);
    assert(output == &output_fixture);
    assert(*(const uint32_t *)input == 0x87654321);
    *(uint32_t *)output = 0xabcdef01;
    return 5;
}
int main(void) {
    assert(MI_CAP_CaptureOne(0, NULL, NULL) == 3);
    real_capture_one = fake_capture;
    assert(MI_CAP_CaptureOne(0x12345678, &input_fixture, &output_fixture) == 5);
    assert(output_fixture == 0xabcdef01);
    assert(input_fixture == 0x87654321);
    return 0;
}

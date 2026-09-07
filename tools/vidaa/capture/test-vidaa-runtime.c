#include "../../../src/vidaa_runtime.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
  for (unsigned int bits=0;bits<256;bits++) {
    unsigned int result=vidaa_openssl_armcap(bits);
    assert((result & 2u) == 0);
    assert((result ^ bits) == (bits & 2u));
  }
  assert(vidaa_openssl_armcap(UINT_MAX) == (UINT_MAX ^ 2u));
  puts("VIDAA runtime capability tests passed");
  return 0;
}

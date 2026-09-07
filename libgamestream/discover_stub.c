#include "discover.h"

void gs_discover_server(char* dest, unsigned short* port) {
  (void)port;
  if (dest != 0)
    dest[0] = 0;
}

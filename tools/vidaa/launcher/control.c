/* Local-only control client for the Moonlight bridge. */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2 || argv[1][0] != '/' || strpbrk(argv[1], "\r\n"))
    return 2;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return 3;
  struct timeval timeout = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_port = htons(47985);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    perror("connect");
    close(fd);
    return 4;
  }
  char request[2048];
  int size = snprintf(request, sizeof(request),
      "GET %s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n", argv[1]);
  if (size < 0 || size >= (int)sizeof(request) || send(fd, request, size, 0) != size) {
    close(fd);
    return 5;
  }
  ssize_t count;
  while ((count = recv(fd, request, sizeof(request), 0)) > 0)
    fwrite(request, 1, (size_t)count, stdout);
  close(fd);
  return count < 0 ? 6 : 0;
}

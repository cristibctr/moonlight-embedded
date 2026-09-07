/* Temporary, fixed-route benchmark server. No command or file-browsing API.
 * The marker clock is the TV CLOCK_MONOTONIC used by capture and frame traces.
 * Usage: bind client observer port seconds token page-file new-log-file
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_server(int signal_number) { (void)signal_number; stopped = 1; }
static double now_ms(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return value.tv_sec * 1000.0 + value.tv_nsec / 1000000.0;
}
static int send_all(int socket_fd, const char *data, size_t size) {
  while (size) {
    ssize_t count = send(socket_fd, data, size, 0);
    if (count <= 0) return -1;
    data += count; size -= (size_t)count;
  }
  return 0;
}
static void response(int socket_fd, int status, const char *type,
                     const char *body, size_t size) {
  char header[512];
  int length = snprintf(header, sizeof(header),
      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
      "Connection: close\r\n\r\n", status, status == 200 ? "OK" : "Rejected", type, size);
  if (length > 0 && (size_t)length < sizeof(header) && !send_all(socket_fd, header, length))
    send_all(socket_fd, body, size);
}
static int parse_number(const char *text, long minimum, long maximum, long *value) {
  char *end; errno = 0;
  *value = strtol(text, &end, 10);
  return !errno && *text && !*end && *value >= minimum && *value <= maximum;
}
int main(int argc, char **argv) {
  struct in_addr bind_ip, client_ip, observer_ip;
  long port, seconds;
  if (argc != 9 || !inet_pton(AF_INET, argv[1], &bind_ip) ||
      !inet_pton(AF_INET, argv[2], &client_ip) || !inet_pton(AF_INET, argv[3], &observer_ip) ||
      !parse_number(argv[4], 0, 65535, &port) || !parse_number(argv[5], 1, 3600, &seconds) ||
      strlen(argv[6]) < 32 || strlen(argv[6]) > 64) return 64;
  for (const unsigned char *p = (const unsigned char *)argv[6]; *p; ++p)
    if (!isalnum(*p) && *p != '-' && *p != '_') return 64;
  FILE *page_file = fopen(argv[7], "rb");
  if (!page_file) return 1;
  char page[65537];
  size_t page_size = fread(page, 1, sizeof(page), page_file);
  int page_error = ferror(page_file);
  fclose(page_file);
  if (page_error || !page_size || page_size == sizeof(page)) return 1;
  int log_fd = open(argv[8], O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (log_fd < 0) return 2;
  FILE *log_file = fdopen(log_fd, "w");
  if (!log_file) { close(log_fd); return 2; }
  setvbuf(log_file, NULL, _IOLBF, 0);
  int listener = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
  if (listener < 0) return 3;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET; address.sin_addr = bind_ip; address.sin_port = htons((uint16_t)port);
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 8)) return 3;
  socklen_t address_size = sizeof(address);
  getsockname(listener, (struct sockaddr *)&address, &address_size);
  signal(SIGPIPE, SIG_IGN); signal(SIGINT, stop_server); signal(SIGTERM, stop_server);
  double deadline = now_ms() + seconds * 1000.0;
  unsigned saved = 0;
  printf("{\"url\":\"http://%s:%u/%s/marker\",\"source_clock\":\"tv\"}\n",
         argv[1], ntohs(address.sin_port), argv[6]); fflush(stdout);
  while (!stopped && now_ms() < deadline) {
    struct pollfd ready = {listener, POLLIN, 0};
    if (poll(&ready, 1, 500) <= 0 || !(ready.revents & POLLIN)) continue;
    struct sockaddr_in peer; socklen_t peer_size = sizeof(peer);
    int connection = accept(listener, (struct sockaddr *)&peer, &peer_size);
    if (connection < 0) continue;
    if (peer.sin_addr.s_addr != client_ip.s_addr && peer.sin_addr.s_addr != observer_ip.s_addr) {
      close(connection); continue;
    }
    struct timeval timeout = {2, 0};
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    char request[24577] = {0}; size_t used = 0; char *body = NULL;
    while (used < 8192 && !body) {
      ssize_t count = recv(connection, request + used, 8192 - used, 0);
      if (count <= 0) break;
      used += (size_t)count; request[used] = 0;
      body = strstr(request, "\r\n\r\n");
    }
    double received = now_ms();
    char method[8] = {0}, path[256] = {0};
    int valid = body && sscanf(request, "%7s %255s", method, path) == 2;
    long content_length = 0; unsigned length_count = 0;
    if (valid) {
      *body = 0; body += 4;
      for (char *line = strstr(request, "\r\n"); line; ) {
        line += 2;
        char *end = strstr(line, "\r\n");
        if (end) *end = 0;
        if (!strncasecmp(line, "Content-Length:", 15)) {
          const char *value = line + 15; while (*value == ' ') ++value;
          if (++length_count > 1 || !parse_number(value, 0, 16384, &content_length)) valid = 0;
        }
        if (!strncasecmp(line, "Transfer-Encoding:", 18)) valid = 0;
        line = end;
      }
    }
    size_t header_size = body ? (size_t)(body - request) : 0;
    while (valid && used < header_size + (size_t)content_length) {
      ssize_t count = recv(connection, request + used, header_size + content_length - used, 0);
      if (count <= 0) { valid = 0; break; }
      used += (size_t)count;
    }
    char prefix[80]; snprintf(prefix, sizeof(prefix), "/%s/", argv[6]);
    const char *route = path + strlen(prefix);
    if (!valid) response(connection, 400, "application/json", "{}", 2);
    else if (strncmp(path, prefix, strlen(prefix))) response(connection, 404, "application/json", "{}", 2);
    else if (!strcmp(method, "GET") && !strcmp(route, "marker"))
      response(connection, 200, "text/html; charset=utf-8", page, page_size);
    else if (!strcmp(method, "GET") && (!strcmp(route, "clock") || !strcmp(route, "status"))) {
      char result[180];
      int size = snprintf(result, sizeof(result),
          "{\"receive_ms\":%.6f,\"send_ms\":%.6f,\"source_clock\":\"tv\",\"saved\":%u}",
          received, now_ms(), saved);
      response(connection, 200, "application/json", result, size);
    } else if (!strcmp(method, "POST") &&
               (!strcmp(route, "calibration") || !strcmp(route, "report")) &&
               content_length >= 2 && length_count == 1 && saved < 180 &&
               body[0] == '{' && body[content_length-1] == '}' &&
               !memchr(body, '\n', content_length) && !memchr(body, '\r', content_length)) {
      char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
      fprintf(log_file, "{\"server_time_ms\":%.6f,\"source_ip\":\"%s\",\"route\":\"%s\",\"data\":%.*s}\n",
              now_ms(), ip, route, (int)content_length, body);
      ++saved;
      response(connection, 200, "application/json", "{\"ok\":true}", 11);
    } else response(connection, 404, "application/json", "{}", 2);
    close(connection);
  }
  close(listener); fclose(log_file); return 0;
}

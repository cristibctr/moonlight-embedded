#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PORT 47985
#define REQUEST_SIZE 4096

static volatile sig_atomic_t moonlight_pid = -1;
static volatile sig_atomic_t last_child_pid;
static volatile sig_atomic_t last_child_status;
static char active_host[64];
static char active_resolution[8];
static int active_bitrate;

static const char launcher_html[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta name='theme-color' content='#080b12'><title>Moonlight</title>"
"<style>"
"*{box-sizing:border-box}html,body{width:100%;height:100%;margin:0;background:transparent;color:#f4f7fb;font-family:Arial,sans-serif;overflow:hidden}"
"body.menu{background:radial-gradient(circle at 78% 18%,#17355b 0,#0b1424 38%,#070a11 78%)}"
"main{height:100%;padding:64px 76px;transition:opacity .18s}body.streaming main{opacity:0;pointer-events:none}"
"header{display:flex;align-items:center;justify-content:space-between;margin-bottom:42px}"
".brand{font-size:44px;font-weight:700;letter-spacing:-1px}.brand i{display:inline-block;width:16px;height:16px;border-radius:50%;background:#77e56f;box-shadow:0 0 22px #77e56f;margin-right:18px}"
".hint{color:#9bacbf;font-size:20px}"
".card{width:680px;padding:34px;border:1px solid #30445e;border-radius:22px;background:rgba(15,24,38,.94);box-shadow:0 28px 80px rgba(0,0,0,.35)}"
"label{display:block;color:#9fb0c3;font-size:18px;margin:0 0 10px}"
"select,input,button{width:100%;height:64px;border-radius:12px;border:2px solid #344b67;background:#101b2b;color:#fff;font-size:23px;padding:0 20px;margin:0 0 24px;outline:none}"
"select:focus,input:focus,button:focus{border-color:#77e56f;box-shadow:0 0 0 4px rgba(119,229,111,.18)}"
"button{background:#77e56f;color:#071007;border-color:#77e56f;font-weight:700;cursor:pointer;margin-bottom:10px}"
"#status{min-height:28px;margin-top:15px;color:#9fb0c3;font-size:18px}.error{color:#ff9b94!important}"
"</style></head><body class='menu'><main>"
"<header><div class='brand'><i></i>Moonlight</div><div class='hint'>Select a computer and start</div></header>"
"<section class='card'>"
"<label for='host'>Computer</label><select id='host'><option value='192.168.1.139'>Windows gaming PC</option><option value='192.168.1.132'>Example Mac</option><option value='custom'>Another computer...</option></select>"
"<div id='customRow' style='display:none'><label for='customHost'>Computer IP address</label><input id='customHost' inputmode='numeric' placeholder='192.168.1.100'></div>"
"<label for='app'>Application</label><select id='app'><option value='Desktop'>Desktop</option><option value='Steam Big Picture'>Steam Big Picture</option></select>"
"<label for='profile'>Video</label><select id='profile'><option value='4k'>4K / 60 fps / HEVC / 120 Mbps</option><option value='4k80'>4K / 60 fps / HEVC / 80 Mbps</option><option value='1080p'>1080p / 60 fps / HEVC / 30 Mbps</option></select>"
"<button id='play'>Start streaming</button><div id='status'>Ready</div>"
"</section></main>"
"<script>"
"(function(){var host=document.getElementById('host'),custom=document.getElementById('customHost'),row=document.getElementById('customRow'),app=document.getElementById('app'),profile=document.getElementById('profile'),play=document.getElementById('play'),status=document.getElementById('status');"
"var saved=localStorage.getItem('moonlightCustomHost');if(saved){custom.value=saved;}"
"host.onchange=function(){row.style.display=host.value==='custom'?'block':'none';if(host.value==='custom')custom.focus();};"
"function message(text,bad){status.innerHTML=text;status.className=bad?'error':'';}"
"function request(path,done){var x=new XMLHttpRequest();x.open('GET',path,true);x.onreadystatechange=function(){if(x.readyState===4)done(x.status>=200&&x.status<300,x.responseText);};x.onerror=function(){done(false,'');};x.send();}"
"play.onclick=function(){var ip=host.value==='custom'?custom.value:host.value;if(host.value==='custom')localStorage.setItem('moonlightCustomHost',ip);var resolution=profile.value==='4k80'?'4k':profile.value,bitrate=profile.value==='4k'?'120000':profile.value==='4k80'?'80000':'30000';message('Starting '+app.value+'...',false);play.disabled=true;request('/start?host='+encodeURIComponent(ip)+'&app='+encodeURIComponent(app.value)+'&codec=h265&mode=injplay&resolution='+resolution+'&bitrate='+bitrate,function(ok,text){play.disabled=false;if(!ok){message('Could not start. Check the computer IP address.',true);return;}document.body.className='streaming';message('Streaming',false);});};"
"function stop(){request('/stop',function(){document.body.className='menu';message('Stopped',false);play.focus();});}"
"function controls(){return row.style.display==='none'?[host,app,profile,play]:[host,custom,app,profile,play];}"
"function move(step){var list=controls(),at=list.indexOf(document.activeElement);if(at<0)at=0;else at=(at+step+list.length)%list.length;list[at].focus();}"
"function changeSelect(select,step){var next=(select.selectedIndex+step+select.options.length)%select.options.length;select.selectedIndex=next;if(select.onchange)select.onchange();}"
"document.addEventListener('keydown',function(e){var code=e.keyCode;if(document.body.className==='streaming'){if(code===27||code===461||code===8){e.preventDefault();stop();}return;}"
"if(code===38){e.preventDefault();move(-1);return;}if(code===40){e.preventDefault();move(1);return;}"
"if((code===37||code===39)&&document.activeElement.tagName==='SELECT'){e.preventDefault();changeSelect(document.activeElement,code===37?-1:1);return;}"
"if(code===13&&document.activeElement.tagName==='SELECT'){e.preventDefault();changeSelect(document.activeElement,1);return;}"
"if(code===13&&document.activeElement===play){e.preventDefault();play.click();return;}"
"});"
"function syncState(){request('/status',function(ok,text){if(!ok||play.disabled)return;var state;try{state=JSON.parse(text);}catch(e){return;}if(state.running){document.body.className='streaming';}else if(document.body.className==='streaming'){document.body.className='menu';message('Stopped',false);play.focus();}});}"
"host.focus();syncState();setInterval(syncState,1000);"
"})();"
"</script></body></html>";

static void reap_children(int signal_number) {
  (void)signal_number;
  int saved_errno = errno;
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    if (pid == moonlight_pid) {
      last_child_pid = pid;
      last_child_status = status;
      moonlight_pid = -1;
    }
  }
  errno = saved_errno;
}

static void stop_moonlight(void) {
  pid_t pid = moonlight_pid;
  if (pid <= 0)
    return;

  kill(pid, SIGTERM);
  for (int i = 0; i < 20; i++) {
    int status;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      last_child_pid = pid;
      last_child_status = status;
    }
    if (waited == pid || (waited < 0 && errno == ECHILD) || moonlight_pid != pid) {
      moonlight_pid = -1;
      return;
    }
    usleep(100000);
  }
  if (moonlight_pid != pid)
    return;
  kill(pid, SIGKILL);
  int status;
  if (waitpid(pid, &status, 0) == pid) {
    last_child_pid = pid;
    last_child_status = status;
  }
  moonlight_pid = -1;
}

static bool valid_host(const char* host) {
  if (host == NULL || *host == '\0' || strlen(host) > 63)
    return false;
  for (const char* p = host; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != ':')
      return false;
  }
  return true;
}

static void url_decode(char* value) {
  char* source = value;
  char* target = value;
  while (*source) {
    if (*source == '%' && isxdigit((unsigned char)source[1]) &&
        isxdigit((unsigned char)source[2])) {
      char hex[3] = {source[1], source[2], 0};
      *target++ = (char)strtol(hex, NULL, 16);
      source += 3;
    } else {
      *target++ = *source == '+' ? ' ' : *source;
      source++;
    }
  }
  *target = '\0';
}

static bool query_value(const char* path, const char* key, char* output,
                        size_t output_size) {
  const char* query = strchr(path, '?');
  if (query == NULL)
    return false;
  query++;
  size_t key_length = strlen(key);
  while (*query) {
    if (strncmp(query, key, key_length) == 0 && query[key_length] == '=') {
      const char* value = query + key_length + 1;
      const char* end = strchr(value, '&');
      size_t length = end ? (size_t)(end - value) : strlen(value);
      if (length >= output_size)
        length = output_size - 1;
      memcpy(output, value, length);
      output[length] = '\0';
      url_decode(output);
      return true;
    }
    query = strchr(query, '&');
    if (query == NULL)
      break;
    query++;
  }
  return false;
}

static bool stream_profile(const char* path, char* resolution, int* bitrate) {
  char text[16] = "30000";
  strcpy(resolution, "1080p");
  query_value(path, "resolution", resolution, 8);
  query_value(path, "bitrate", text, sizeof(text));
  if (strcmp(resolution, "1080p") != 0 && strcmp(resolution, "4k") != 0)
    return false;
  char* end;
  long value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 10000 || value > 150000)
    return false;
  *bitrate = (int)value;
  return true;
}

static bool start_moonlight(const char* host, const char* app,
                            const char* codec, const char* mode,
                            const char* resolution, int bitrate) {
  stop_moonlight();
  // A fast child exit must not arrive before its PID is registered.
  sigset_t blocked, original_mask;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGCHLD);
  if (sigprocmask(SIG_BLOCK, &blocked, &original_mask) != 0)
    return false;
  pid_t pid = fork();
  if (pid < 0) {
    sigprocmask(SIG_SETMASK, &original_mask, NULL);
    return false;
  }
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL);
    sigprocmask(SIG_SETMASK, &original_mask, NULL);
    int log_fd = open("/var/local/moonlight-launcher.log",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      close(null_fd);
    }
    if (log_fd >= 0) {
      dup2(log_fd, STDOUT_FILENO);
      dup2(log_fd, STDERR_FILENO);
      close(log_fd);
    }
    setenv("MOONLIGHT_VIDAA_DEBUG", "1", 1);
    setenv("MOONLIGHT_VIDAA_MODE", mode, 1);
    char bitrate_text[16];
    snprintf(bitrate_text, sizeof(bitrate_text), "%d", bitrate);
    const char* size_option = strcmp(resolution, "4k") == 0 ? "-4k" : "-1080";
    fprintf(stderr, "Moonlight profile: host=%s resolution=%s fps=60 bitrate=%d codec=%s mode=%s\n",
            host, resolution, bitrate, codec, mode);
    execl("/var/local/moonlight-vidaa", "moonlight-vidaa", "stream",
          size_option, "-fps", "60", "-bitrate", bitrate_text, "-codec", codec,
          "-app", app, "-platform", "vidaa", "-viewonly", "-audio", "hw:0,0",
          "-remote", "no", host, (char*)NULL);
    perror("Cannot execute Moonlight wrapper");
    _exit(127);
  }
  moonlight_pid = pid;
  last_child_pid = 0;
  last_child_status = 0;
  sigprocmask(SIG_SETMASK, &original_mask, NULL);
  snprintf(active_host, sizeof(active_host), "%s", host);
  snprintf(active_resolution, sizeof(active_resolution), "%s", resolution);
  active_bitrate = bitrate;
  return true;
}

static void send_response(int client, int status, const char* content_type,
                          const char* body) {
  char header[512];
  const char* reason = status == 200 ? "OK" : "Bad Request";
  size_t body_length = strlen(body);
  int header_length = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
      "Connection: close\r\n\r\n",
      status, reason, content_type, body_length);
  send(client, header, (size_t)header_length, 0);
  send(client, body, body_length, 0);
}

static void handle_client(int client) {
  char request[REQUEST_SIZE];
  ssize_t length = recv(client, request, sizeof(request) - 1, 0);
  if (length <= 0)
    return;
  request[length] = '\0';

  char method[12] = {0};
  char path[1024] = {0};
  if (sscanf(request, "%11s %1023s", method, path) != 2) {
    send_response(client, 400, "application/json", "{\"ok\":false}");
    return;
  }
  if (strcmp(method, "OPTIONS") == 0) {
    send_response(client, 200, "text/plain", "");
    return;
  }
  if (strncmp(path, "/start?", 7) == 0) {
    char host[64] = {0};
    char app[64] = "Desktop";
    char codec[8] = "h265";
    char mode[16] = "injplay";
    char resolution[8];
    int bitrate;
    query_value(path, "host", host, sizeof(host));
    query_value(path, "app", app, sizeof(app));
    query_value(path, "codec", codec, sizeof(codec));
    query_value(path, "mode", mode, sizeof(mode));
    if (!valid_host(host) || !stream_profile(path, resolution, &bitrate) ||
        (strcmp(app, "Desktop") != 0 && strcmp(app, "Steam Big Picture") != 0 &&
         strcmp(app, "Moonlight Benchmark") != 0) ||
        (strcmp(codec, "h264") != 0 && strcmp(codec, "h265") != 0) ||
        (strcmp(mode, "normal") != 0 && strcmp(mode, "resume") != 0 &&
         strcmp(mode, "direct") != 0 &&
         strcmp(mode, "injplay") != 0 &&
         strcmp(mode, "direct-nopts") != 0 && strcmp(mode, "av") != 0)) {
      send_response(client, 400, "application/json", "{\"ok\":false}");
      return;
    }
    bool started = start_moonlight(host, app, codec, mode, resolution, bitrate);
    send_response(client, started ? 200 : 400, "application/json",
                  started ? "{\"ok\":true}" : "{\"ok\":false}");
    return;
  }
  if (strcmp(path, "/stop") == 0) {
    stop_moonlight();
    send_response(client, 200, "application/json", "{\"ok\":true}");
    return;
  }
  if (strcmp(path, "/status") == 0) {
    char body[256];
    snprintf(body, sizeof(body),
             "{\"running\":%s,\"host\":\"%s\",\"resolution\":\"%s\",\"fps\":60,\"bitrate\":%d,\"last_exit_code\":%d,\"last_signal\":%d}",
             moonlight_pid > 0 ? "true" : "false", active_host,
             active_resolution, active_bitrate,
             last_child_pid && WIFEXITED(last_child_status) ? WEXITSTATUS(last_child_status) : -1,
             last_child_pid && WIFSIGNALED(last_child_status) ? WTERMSIG(last_child_status) : 0);
    send_response(client, 200, "application/json",
                  body);
    return;
  }
  if (strcmp(path, "/") == 0 || strncmp(path, "/index.html", 11) == 0) {
    send_response(client, 200, "text/html; charset=utf-8", launcher_html);
    return;
  }
  send_response(client, 400, "application/json", "{\"ok\":false}");
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, reap_children);

  int server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0)
    return 1;
  fcntl(server, F_SETFD, FD_CLOEXEC);
  int enabled = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(PORT);
  if (bind(server, (struct sockaddr*)&address, sizeof(address)) != 0)
    return 2;
  if (listen(server, 8) != 0)
    return 3;

  for (;;) {
    int client = accept(server, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    /* A streaming child must not keep an HTTP response open until it exits. */
    fcntl(client, F_SETFD, FD_CLOEXEC);
    handle_client(client);
    close(client);
  }
  close(server);
  stop_moonlight();
  return 0;
}

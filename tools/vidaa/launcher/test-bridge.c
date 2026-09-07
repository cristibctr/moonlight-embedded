#define main bridge_program_main
#include "bridge.c"
#undef main
#include <assert.h>

int main(void) {
  char resolution[8];
  int bitrate;
  assert(stream_profile("/start?host=192.168.1.139", resolution, &bitrate));
  assert(strcmp(resolution, "1080p") == 0 && bitrate == 30000);
  assert(stream_profile("/start?resolution=4k&bitrate=80000", resolution, &bitrate));
  assert(strcmp(resolution, "4k") == 0 && bitrate == 80000);
  assert(!stream_profile("/start?resolution=8k", resolution, &bitrate));
  assert(!stream_profile("/start?bitrate=0", resolution, &bitrate));
  assert(!stream_profile("/start?bitrate=150001", resolution, &bitrate));
  assert(!stream_profile("/start?bitrate=80000bad", resolution, &bitrate));
  assert(!stream_profile("/start?bitrate=", resolution, &bitrate));

  // An already-exited child is reaped after PID registration, not before.
  sigset_t mask, original;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  assert(sigprocmask(SIG_BLOCK, &mask, &original) == 0);
  signal(SIGCHLD, reap_children);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) _exit(73);
  moonlight_pid = child;
  siginfo_t info;
  assert(waitid(P_PID, child, &info, WEXITED | WNOWAIT) == 0);
  assert(sigprocmask(SIG_SETMASK, &original, NULL) == 0);
  for (int i=0;i<100 && moonlight_pid>0;i++) usleep(1000);
  assert(moonlight_pid == -1 && last_child_pid == child);
  assert(WIFEXITED(last_child_status) && WEXITSTATUS(last_child_status) == 73);
  errno = EDOM;
  reap_children(SIGCHLD);
  assert(errno == EDOM);

  child = fork();
  assert(child >= 0);
  if (child == 0) { for (;;) pause(); }
  moonlight_pid = child;
  stop_moonlight();
  assert(moonlight_pid == -1 && last_child_pid == child);
  assert(WIFSIGNALED(last_child_status) && WTERMSIG(last_child_status) == SIGTERM);
  puts("Bridge profile tests passed");
  return 0;
}

// Project 1C: rule checks for stride scheduling.
//
//   stridetest [scheduler-id]
//
// The scheduler ID defaults to 4; use 5 for the own design (stride
// with targeted latency), which keeps ID 4's pass, wakeup and fork
// rules. In every part only two processes compete, so ID 5's slice is
// max(2, 10 / 2) = 5 ticks, the same as ID 4, and the limits below hold.
//
// Three parts, each started with set_scheduler(id), which sets every
// pass and the global pass back to 0. Each part runs a CPU-bound
// helper H next to a test child and prints a number with PASS/FAIL.
// Every other process (init, sh, the parent) is asleep, so H and the
// test child are the only competitors.
//
//   1. A long sleeper is pulled up to the min pass when it wakes.
//      S sleeps 30 ticks while H runs, then spins 20. Correct: S and H
//      alternate slices (longest run about one 5-tick slice). Broken
//      (no clamp, or a min that skips the RUNNING H): S runs 20 in a row.
//   2. A pass never moves backward. S (nice 10) spins 3 ticks, which
//      puts its pass about 28 nice-0 ticks ahead of H, then sleeps
//      1 tick inside its slice. Correct: S keeps its pass and H runs
//      until it catches up (pause(1) takes about 20-40 ticks). Broken
//      (pass = min with no max): S is moved back and runs within a slice.
//   3. A forked child starts at the global pass. H runs alone for
//      20 ticks, then the parent forks C, which spins 20. Correct: C and
//      H alternate. Broken (pass 0 for a new process): C runs 20 in a row.

#include "kernel/types.h"
#include "user/user.h"

#define MAX_RUN 10  // parts 1 and 3: PASS if the longest run is at most this
#define MIN_GAP 15  // part 2: PASS if pause(1) took at least this many ticks

// Keep the CPU until uptime() has changed n times. Return the longest
// run of changes seen without being descheduled: a jump of more than
// 1 between two reads means another process ran in between.
static int
spin(int n)
{
  int t = uptime(), run = 0, best = 0;

  while (n > 0) {
    int u = uptime();
    if (u != t) {
      if (u - t > 1)
        run = 0;
      run++;
      if (run > best)
        best = run;
      t = u;
      n--;
    }
  }
  return best;
}

static int
start(void (*fn)(void))
{
  int pid = fork();

  if (pid < 0) {
    printf("stridetest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    fn();
    exit(0);
  }
  return pid;
}

static int sched_id = 4;

static void
reset(void)
{
  if (set_scheduler(sched_id) < 0) {
    printf("stridetest: set_scheduler(%d) failed\n", sched_id);
    exit(1);
  }
}

static void h70(void) { spin(70); }
static void h80(void) { spin(80); }
static void h60(void) { spin(60); }

static void
sleeper(void)
{
  int best;

  pause(30);
  best = spin(20);
  printf("part 1 (wakeup pulls a sleeper up): longest run %d, expect <= %d"
         " (20 = no clamp or RUNNING skipped): %s\n",
         best, MAX_RUN, best <= MAX_RUN ? "PASS" : "FAIL");
}

static void
ahead(void)
{
  int t0, t1;

  nice(getpid(), 10);
  spin(3);
  t0 = uptime();
  pause(1);
  t1 = uptime();
  printf("part 2 (pass never moves backward): pause(1) took %d ticks,"
         " expect >= %d (<= 6 = moved back to the min): %s\n",
         t1 - t0, MIN_GAP, t1 - t0 >= MIN_GAP ? "PASS" : "FAIL");
}

static void
forked(void)
{
  int best = spin(20);

  printf("part 3 (fork starts at the global pass): longest run %d,"
         " expect <= %d (20 = child started at pass 0): %s\n",
         best, MAX_RUN, best <= MAX_RUN ? "PASS" : "FAIL");
}

int
main(int argc, char *argv[])
{
  if (argc > 2) {
    printf("usage: stridetest [scheduler-id]\n");
    exit(1);
  }
  if (argc == 2)
    sched_id = atoi(argv[1]);
  printf("stridetest: scheduler %d\n", sched_id);

  reset();
  start(h70);
  start(sleeper);
  wait(0);
  wait(0);

  reset();
  start(h80);
  start(ahead);
  wait(0);
  wait(0);

  reset();
  start(h60);
  pause(20);
  start(forked);
  wait(0);
  wait(0);

  printf("stridetest: done (scheduler %d is still selected)\n", sched_id);
  exit(0);
}

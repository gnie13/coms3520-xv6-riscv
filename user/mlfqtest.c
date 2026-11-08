// Project 1C: rule check for MLFQ Rule 6 (nice changes), scheduler 3.
//
//   mlfqtest
//
// Runs four CPU-bound children one at a time, with logging on, and
// prints what the kernel log should show for each. Before each fork
// the parent calls set_scheduler(3), which resets the MLFQ state
// (including the 60-tick boost clock and the parent's own level), so
// each child gets a fresh boost window and no "boost at" line should
// appear. Each child runs alone while the parent waits, so elapsed
// ticks are about equal to the child's charged CPU ticks.
//
//   A: nice 0 -> -10 moves the job up to Q2 and resets used;
//      then 2->1 after 2 ticks and 1->0 after 10 more.
//   B: nice 0 -> 5 keeps the starting queue (Q1): no move, used is
//      not reset, so 1->0 comes 10 ticks after B started.
//   C: demoted to Q0, then nice 0 -> 3 (start queue still Q1): stays
//      in Q0, so nice() cannot undo a demotion.
//   D: nice 0 -> 11 moves the job down to Q0 right away.

#include "kernel/types.h"
#include "user/user.h"

// Keep the CPU until uptime() has advanced n times, so the process
// is running (and charged) at each of those ticks.
static void
spin(int n)
{
  int t = uptime();

  while (n > 0) {
    int u = uptime();
    if (u != t) {
      t = u;
      n--;
    }
  }
}

static void
child_a(void)
{
  int pid = getpid();
  printf("A pid %d: nice 0 (Q1), spin 4, then nice -10\n", pid);
  spin(4);
  nice(pid, -10);
  printf("A pid %d: expect 1->2 now, 2->1 after 2 ticks, 1->0 after 10 more\n", pid);
  spin(14);
}

static void
child_b(void)
{
  int pid = getpid();
  printf("B pid %d: nice 0 (Q1), spin 6, then nice +5 (start queue unchanged)\n", pid);
  spin(6);
  nice(pid, 5);
  printf("B pid %d: expect no level line now; 1->0 about 4 ticks later\n", pid);
  spin(8);
}

static void
child_c(void)
{
  int pid = getpid();
  printf("C pid %d: nice 0 (Q1), spin 12 (demoted to Q0 at ~10), then nice +3\n", pid);
  spin(12);
  nice(pid, 3);
  printf("C pid %d: expect no 0->1 line: still in Q0\n", pid);
  spin(5);
}

static void
child_d(void)
{
  int pid = getpid();
  printf("D pid %d: nice 0 (Q1), spin 3, then nice +11\n", pid);
  spin(3);
  nice(pid, 11);
  printf("D pid %d: expect 1->0 at the nice call, then no more level lines\n", pid);
  spin(5);
}

// Reset the MLFQ state, fork one child that runs fn, and wait for it.
static void
run(void (*fn)(void))
{
  int pid;

  if (set_scheduler(3) < 0) {
    printf("mlfqtest: set_scheduler(3) failed\n");
    stopLogging();
    exit(1);
  }
  pid = fork();
  if (pid < 0) {
    printf("mlfqtest: fork failed\n");
    stopLogging();
    exit(1);
  }
  if (pid == 0) {
    fn();
    exit(0);
  }
  wait(0);
}

int
main(void)
{
  startLogging();
  run(child_a);
  run(child_b);
  run(child_c);
  run(child_d);
  stopLogging();
  printf("mlfqtest: done (scheduler 3 is still selected)\n");
  exit(0);
}

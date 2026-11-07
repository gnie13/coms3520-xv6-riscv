#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// startLogging() and stopLogging() are declared void in user.h, but the
// kernel handlers return the new value of the logging flag in a0.
// Calling the stubs through int-returning pointers lets this test read it.
static int (*start)(void) = (int (*)(void))startLogging;
static int (*stop)(void) = (int (*)(void))stopLogging;

static int passed = 0;
static int total = 0;

static void
check(char *step, int expected, int actual)
{
  total++;
  if (actual == expected) {
    passed++;
    printf("logtest: PASS %s (flag=%d)\n", step, actual);
  } else {
    printf("logtest: FAIL %s (expected %d, got %d)\n", step, expected, actual);
  }
}

// Bounded CPU-bound work that lasts several ticks.
static void
work(void)
{
  volatile int calc = 0;
  for (int i = 0; i < 6000; i++)
    for (int j = 0; j < 100000; j++)
      calc += i;
}

// Child body for the priority test.
static void
prioritychild(void)
{
  int me = getpid();
  int start;

  printf("priority: child %d (nice %d) starting work\n", me, nice(me, 0));
  start = uptime();
  work();
  printf("priority: child %d (nice %d) finished work after %d ticks\n", me,
         nice(me, 0), uptime() - start);
  exit(0);
}

// Parent at -20 creates the +10 child first, then the +5 child.
// Both are RUNNABLE before the parent sleeps in wait(), so the
// +5 child should finish before the +10 child starts.
static void
priority(void)
{
  int me = getpid();
  int pid10, pid5;

  printf("priority: parent %d nice set to %d\n", me, nice(me, -20));

  pid10 = fork();
  if (pid10 == 0)
    prioritychild();
  printf("priority: child %d nice set to %d\n", pid10, nice(pid10, 10));

  pid5 = fork();
  if (pid5 == 0)
    prioritychild();
  printf("priority: child %d nice set to %d\n", pid5, nice(pid5, 5));

  wait(0);
  wait(0);
  printf("priority: parent %d done\n", me);
  exit(0);
}

// Two CPU-bound children at the same nice value, run with logging on.
// The trace should show them taking turns.
static void
equal(void)
{
  int me = getpid();
  int pid1, pid2;

  nice(me, -20);
  pid1 = fork();
  if (pid1 == 0) {
    work();
    exit(0);
  }
  pid2 = fork();
  if (pid2 == 0) {
    work();
    exit(0);
  }
  printf("equal: children %d and %d set to nice %d and %d\n", pid1, pid2,
         nice(pid1, 5), nice(pid2, 5));

  startLogging();
  wait(0);
  wait(0);
  stopLogging();
  exit(0);
}

// Default test: log a child that lowers its priority and does
// bounded work, then stop logging once the child has exited.
static void
basic(void)
{
  int pid;

  startLogging();
  pid = fork();
  if (pid < 0) {
    stopLogging();
    exit(1);
  }
  if (pid == 0) {
    nice(getpid(), 10);
    work();
    exit(0);
  }
  wait(0);
  stopLogging();
  exit(0);
}

// PASS/FAIL checks for the logging flag and nice().
static void
checks(void)
{
  int me = getpid();
  int status;

  // logging flag transitions
  check("stop before any start", 0, stop());
  check("start", 1, start());
  check("start again", 1, start());
  check("stop", 0, stop());

  // nice: default, increments, clamps
  check("nice default", 0, nice(me, 0));
  check("nice +10", 10, nice(me, 10));
  check("nice -3", 7, nice(me, -3));
  check("nice +12 reaches 19", 19, nice(me, 12));
  check("nice +1 stays 19", 19, nice(me, 1));
  check("nice -39 reaches -20", -20, nice(me, -39));
  check("nice -1 stays -20", -20, nice(me, -1));

  // nice: huge increments must not overflow and wrap around
  check("nice +2147483647 from -20", 19, nice(me, 2147483647));
  check("nice -2147483648 from 19", -20, nice(me, -2147483647 - 1));

  // nice: a fork child starts at 0, not the parent's -20
  if (fork() == 0)
    exit(nice(getpid(), 0));
  wait(&status);
  check("fork child starts at 0", 0, status);

  // nice: invalid pids return 0 and must not match empty slots
  check("nice invalid pid 9999", 0, nice(9999, 5));
  check("nice pid 0", 0, nice(0, 5));
  check("own nice unchanged", -20, nice(me, 0));

  // nice: message is printed only while logging is on
  startLogging();
  check("nice +1 while logging", -19, nice(me, 1));
  stopLogging();

  printf("logtest: %d/%d passed\n", passed, total);
  exit(0);
}

int
main(int argc, char *argv[])
{
  if (argc > 1 && strcmp(argv[1], "priority") == 0)
    priority();
  if (argc > 1 && strcmp(argv[1], "checks") == 0)
    checks();
  if (argc > 1 && strcmp(argv[1], "equal") == 0)
    equal();
  basic();
}

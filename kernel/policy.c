// Project 1C: scheduling policies.
//
// This is the file you modify. The scheduler framework in proc.c
// calls the functions below; it already handles locking and the
// context switch, so a policy only decides which process runs next
// and when the running process should be preempted.
//
// Each policy has an ID. Select one at run time with the
// set_scheduler(id) system call (for example: wl 2 1). To try a
// different setting, such as another quantum, give it its own ID.
//
// Scheduler IDs added for Project 1C (Feature 7):
//   3  SCHED_MLFQ  MLFQ, 1C rules (Section 3.5): 3 queues, start queue by nice,
//                  allotment kept across sleeps, boost every 60 ticks
//   4  SCHED_STRIDE stride scheduling (Section 3.6): pass in vrt, stride from
//                  the nice weight, 5-tick slice, wakeup clamp to the min pass
//   6  SCHED_RR5   round robin with a 5-tick (50 ms) quantum
//
// Rules for policy code (Section 2.2 of the project document):
//  1. The system has one CPU. These functions run with interrupts off
//     and must not sleep or acquire p->lock (the framework holds the
//     locks it needs).
//  2. policy_pick_next_proc() must only return a RUNNABLE process, or
//     0 if there is none.
//  3. A scheduler meant to be fair must not starve a job: every ready
//     job eventually runs. (Strict priority, ID 2, is the one
//     deliberate exception.)
//  4. A scheduler must not recognize a workload: do not read
//     p->wl_slot, workload IDs, process names, or pids to decide how
//     to treat a process.
//  5. policy_init(id) may be called more than once for the same ID,
//     including at the start of every workload run. Each call must
//     reset all of the scheduler's global state.
//  6. Do not modify the protected files (kernel/workload.c,
//     kernel/workload.h, user/wl.c).

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define SCHED_RR     0 // round robin, 10-tick (100 ms) quantum: stock xv6
#define SCHED_RR1    1 // round robin, 1-tick (10 ms) quantum
#define SCHED_PRIO   2 // strict priority by nice (Project 1B), 10-tick quantum
#define SCHED_MLFQ   3 // multi-level feedback queue, 1C rules
#define SCHED_STRIDE 4 // stride scheduling, 5-tick slice
#define SCHED_RR5    6 // round robin, 5-tick (50 ms) quantum

// Linux CFS weights for nice -20 .. 19, indexed by nice + 20
// (nice 0 has weight 1024). This is the same table user/wl.c uses
// for the fairness metric. For example, a fair-share scheduler can
// advance a process's vrt by (1 << 24) / nice_weight[p->nice + 20]
// for each tick it runs.
const int nice_weight[40] = {
  88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
  9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
  1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
  110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static int quantum = 10; // ticks a process may run before preemption
static int last = -1;    // index in proc[] of the last process chosen

// ---- MLFQ state (ID 3, and later MLFQ tuning variants) ----
// Queue levels: 2 = Q2 (highest), 1 = Q1, 0 = Q0 (lowest).
// The settings are globals set in policy_init(), so a tuning variant
// is just another case there with different numbers.
#define MLFQ_LEVELS 3
static int mlfq_on;                    // 1 while an MLFQ policy is selected
static int mlfq_quantum[MLFQ_LEVELS];  // quantum per level, in ticks
static int mlfq_allot[MLFQ_LEVELS];    // allotment per level (Q0: unused)
static int boost_interval;             // ticks between priority boosts
static uint last_boost;                // value of ticks at the last boost
static int last_at[MLFQ_LEVELS];       // round-robin index per level

// ---- Stride state (ID 4, and later stride tuning variants) ----
// p->vrt is the pass; the stride is computed from p->nice when needed
// (stride_of()), so a nice change takes effect at the next tick.
static int stride_on;       // 1 while a stride policy is selected
static int stride_slice;    // ticks a process runs before re-picking
static uint64 stride_base;  // global pass: min pass at the last pick,
                            // never decreases; the pass for new processes
static int stride_last;     // index in proc[] of the last process chosen

// Called by set_scheduler(id), and again when a workload starts.
// Set up global state for policy id. Every call resets all of the
// policy's global state, so every run starts the same way.
// Return 0 if id is a known policy, -1 otherwise.
int
policy_init(int id)
{
  mlfq_on = 0;
  stride_on = 0;
  switch (id) {
  case SCHED_RR:
  case SCHED_PRIO:
    quantum = 10;
    break;
  case SCHED_RR1:
    quantum = 1;
    break;
  case SCHED_RR5:
    quantum = 5;
    break;
  case SCHED_MLFQ:
    // Queue table, Section 3.5.
    mlfq_on = 1;
    mlfq_quantum[2] = 2;  mlfq_allot[2] = 2;
    mlfq_quantum[1] = 10; mlfq_allot[1] = 10;
    mlfq_quantum[0] = 15; mlfq_allot[0] = 0; // Q0: no further demotion
    boost_interval = 60;
    break;
  case SCHED_STRIDE:
    stride_on = 1;
    stride_slice = 5; // same as ID 6, so equal-nice results are comparable
    break;
  default:
    return -1;
  }
  last = -1;
  // Rule 5: the boost clock starts now, not at tick 0, so the first
  // boost comes boost_interval ticks after the scheduler is selected
  // or the workload starts.
  last_boost = ticks;
  for (int i = 0; i < MLFQ_LEVELS; i++)
    last_at[i] = -1;
  // Stride: everyone starts level. sched_reset() calls
  // policy_proc_init() for every process right after this, so every
  // pass becomes 0.
  stride_base = 0;
  stride_last = -1;
  return 0;
}

// MLFQ Rule 3: the starting queue for a nice value (Section 3.5 table).
static int
start_level(int nice)
{
  if (nice <= -10)
    return 2;
  if (nice <= 10)
    return 1;
  return 0;
}

// Move p to queue level nl. Every queue change (demotion, boost, nice)
// goes through here, so each one is logged the same way, but only
// while logging is on and only if the level actually changes.
// Does not touch p->used or p->qticks; callers decide that.
static void
set_level(struct proc *p, int nl)
{
  if (nl != p->level && logging)
    printk("level %d %d->%d at %d\n", p->pid, p->level, nl, ticks);
  p->level = nl;
}

// Called when a process is created, and for every process when the
// policy changes or a workload starts. Initialize any per-process
// policy fields.
void
policy_proc_init(struct proc *p)
{
  p->qticks = 0;
  // MLFQ Rule 3: start in the queue chosen by nice. workload_start()
  // calls this for every process after the jobs' nice values are set.
  // Other policies ignore start and level.
  p->start = start_level(p->nice);
  p->level = p->start;
  p->used = 0;
  // Stride: a new process starts at the global pass, not 0. fork()
  // makes the child RUNNABLE without calling policy_wakeup(), and a
  // reused proc[] slot may hold a dead process's pass, so this is the
  // only place a forked child gets a fair pass. stride_base is 0
  // under every other policy and right after policy_init().
  p->vrt = stride_base;
}

// Stride: pass added per tick of CPU. (1 << 24) / weight gives 16384
// at nice 0, 189 at nice -20 and 1118481 at nice 19; never 0.
static uint64
stride_of(struct proc *p)
{
  return (1 << 24) / nice_weight[p->nice + 20];
}

// Round robin: the next RUNNABLE process after the last one chosen,
// scanning proc[] in a circle so no process is starved.
static struct proc *
pick_rr(void)
{
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE) {
      last = p - proc;
      return p;
    }
  }
  return 0;
}

// Strict priority: a RUNNABLE process with the lowest nice value;
// processes with equal nice take turns in round-robin order.
static struct proc *
pick_prio(void)
{
  int best = 100;

  for (struct proc *p = proc; p < &proc[NPROC]; p++)
    if (p->state == RUNNABLE && p->nice < best)
      best = p->nice;
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE && p->nice == best) {
      last = p - proc;
      return p;
    }
  }
  return 0;
}

// MLFQ Rule 5: priority boost. Once boost_interval ticks have passed
// since the last boost, move every process to its starting queue or
// Q1, whichever is higher, and reset its used time even if its queue
// does not change. Called from both policy_tick() and
// policy_pick_next_proc(), so the boost is on time even when every
// process is asleep. Writes other processes' fields without p->lock:
// one CPU, interrupts off (policy-code rule 1).
static void
maybe_boost(void)
{
  if (ticks - last_boost < boost_interval)
    return;
  if (logging)
    printk("boost at %d\n", ticks);
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    set_level(p, p->start > 1 ? p->start : 1);
    p->used = 0;
  }
  last_boost = ticks;
}

// MLFQ selection. Rule 1: only the highest level that has a RUNNABLE
// process is considered. Rule 2: round robin within that level, with
// its own last index, so a higher-level job running in between does
// not move the scan position of a lower level (no starvation within
// a level).
static struct proc *
pick_mlfq(void)
{
  int top = -1;

  maybe_boost();
  for (struct proc *p = proc; p < &proc[NPROC]; p++)
    if (p->state == RUNNABLE && p->level > top)
      top = p->level;
  if (top < 0)
    return 0;
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last_at[top] + i) % NPROC];
    if (p->state == RUNNABLE && p->level == top) {
      last_at[top] = p - proc;
      return p;
    }
  }
  return 0;
}

// Stride selection: the RUNNABLE process with the smallest pass.
// The scan starts after the last process chosen and uses a strict <,
// so among equal passes the first one after stride_last wins and tied
// processes take turns instead of the lowest index always winning.
static struct proc *
pick_stride(void)
{
  struct proc *best = 0;

  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(stride_last + i) % NPROC];
    if (p->state == RUNNABLE && (best == 0 || p->vrt < best->vrt))
      best = p;
  }
  if (best == 0)
    return 0;
  stride_last = best - proc;
  if (best->vrt > stride_base)
    stride_base = best->vrt; // the global pass only moves forward
  return best;
}

// Choose the next process to run, or return 0 if none is RUNNABLE.
struct proc *
policy_pick_next_proc(void)
{
  if (stride_on)
    return pick_stride();
  if (mlfq_on)
    return pick_mlfq();
  switch (sched_current) {
  case SCHED_PRIO:
    return pick_prio();
  default:
    return pick_rr();
  }
}

// Called just before the framework switches to p.
void
policy_dispatch(struct proc *p)
{
  p->qticks = 0; // a new quantum starts
}

// Called on every timer tick while p is running.
// Return 1 to preempt p, or 0 to let it keep running.
// A process is charged only for ticks that arrive while it is running.
// A loop of pause(1) wakes at a tick, runs briefly, and sleeps again
// before the next one, so it is never charged and cannot test a rule
// such as an allotment kept across sleeps. A test that is charged must
// keep the CPU across a tick, for example by spinning until uptime()
// changes before each pause(1); workload jobs are charged correctly.
int
policy_tick(struct proc *p)
{
  if (stride_on) {
    // Charge this tick at p's current weight; re-pick after a slice.
    // No early preemption when a lower-pass job wakes: it waits for
    // the end of the current slice (at most stride_slice ticks).
    p->vrt += stride_of(p);
    p->qticks++;
    return p->qticks >= stride_slice;
  }
  if (!mlfq_on) {
    p->qticks++;
    return p->qticks >= quantum;
  }

  // MLFQ, in the order Section 3.5 specifies:
  // 1. Charge the tick to the allotment and the quantum.
  p->used++;
  p->qticks++;

  // 2. Rule 5: boost if it is due (may reset p->used and raise p->level).
  maybe_boost();

  // 3. Rule 4: allotment used up at this level -> move down one queue.
  //    Q0 has no allotment. Only a queue change resets used; sleeping
  //    does not (policy_wakeup() leaves it alone).
  if (p->level > 0 && p->used >= mlfq_allot[p->level]) {
    set_level(p, p->level - 1);
    p->used = 0;
    return 1;
  }
  //    Rule 2: quantum of the current level used up. A queue change
  //    does not restart the quantum; only policy_dispatch() does.
  if (p->qticks >= mlfq_quantum[p->level])
    return 1;

  // 4. Rule 1: a RUNNABLE job in a higher queue preempts p at this tick.
  for (struct proc *q = proc; q < &proc[NPROC]; q++)
    if (q->state == RUNNABLE && q->level > p->level)
      return 1;
  return 0;
}

// Called when p wakes from sleep (for example, when its simulated I/O
// or a pause() ends) and becomes RUNNABLE again. Use it to decide how
// a waking process re-enters the policy; for example, a fair-share or
// stride policy may move a process that slept a long time up to the
// current minimum, so it cannot claim the CPU for the time it slept.
// MLFQ: deliberately nothing. The level and the used time carry
// across sleeps (Rule 4), so a job cannot game its allotment.
// Stride: pass = max(own pass, min pass of the other RUNNING and
// RUNNABLE processes). RUNNING counts because wakeup() is often called
// from the clock interrupt while another process is running; p itself
// is skipped because wakeup() has already made it RUNNABLE. With no
// other competitor the pass is kept. A pass never moves backward, so
// sleeping can neither bank CPU time nor lower a pass.
void
policy_wakeup(struct proc *p)
{
  int found = 0;
  uint64 min = 0;

  if (!stride_on)
    return;
  for (struct proc *q = proc; q < &proc[NPROC]; q++) {
    if (q == p || (q->state != RUNNING && q->state != RUNNABLE))
      continue;
    if (!found || q->vrt < min)
      min = q->vrt;
    found = 1;
  }
  if (found && p->vrt < min)
    p->vrt = min;
}

// Called whenever p's nice value changes (with p->lock held).
// MLFQ Rule 6: move p only if its new nice value has a different
// starting queue. Compare with p->start, not p->level: a job that was
// demoted and calls nice() with the same starting queue stays put.
// Stride: deliberately nothing. The pass already charged at the old
// weight stays; stride_of() uses the new nice from the next tick on.
void
policy_nice(struct proc *p)
{
  int ns;

  if (!mlfq_on)
    return;
  ns = start_level(p->nice);
  if (ns == p->start)
    return;
  p->start = ns;
  set_level(p, ns);
  p->used = 0;
}

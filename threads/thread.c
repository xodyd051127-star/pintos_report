#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
  {
    void *eip;            /* Return address. */
    thread_func *function; /* Function to call. */
    void *aux;            /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* [추가] Project 1: 우선순위 비교 함수 */
bool
thread_cmp_priority (const struct list_elem *a, const struct list_elem *b,
                     void *aux UNUSED)
{
    struct thread *t1 = list_entry (a, struct thread, elem);
    struct thread *t2 = list_entry (b, struct thread, elem);
    return t1->priority > t2->priority;
}

/* [추가] Project 1: Priority Donation 헬퍼 함수 */

/* Lock을 기다리는 스레드를 donation 리스트에서 제거합니다. */
void
remove_with_lock (struct lock *lock)
{
    struct list_elem *e;
    struct thread *cur = thread_current ();

    // 현재 스레드가 기다리는 모든 락에서 자신을 제거 (Donation 철회)
    for (e = list_begin (&lock->donators); e != list_end (&lock->donators);
         e = list_next (e))
    {
        struct thread *t = list_entry (e, struct thread, donation_elem);
        if (t == cur)
        {
            list_remove (e);
            return;
        }
    }
}

/* 현재 스레드의 우선순위를 재계산합니다. (Priority Donation 정리 시 사용) */
void
refresh_priority (void)
{
    struct thread *cur = thread_current ();
    cur->priority = cur->original_priority; // 기본적으로 원래 우선순위로 복구

    // 자신이 보유한 락을 확인하여, 기부된 우선순위가 있다면 적용
    if (cur->wait_on_lock != NULL && !list_empty(&cur->wait_on_lock->donators))
    {
        // 락의 최고 우선순위가 현재 우선순위보다 높으면 기부 받음
        int donated_priority = list_entry (list_front(&cur->wait_on_lock->donators),
                                           struct thread, donation_elem)->priority;
        if (donated_priority > cur->priority) {
            cur->priority = donated_priority;
        }
    }
}

/* 현재 스레드가 기다리는 락의 holder에게 우선순위를 기부합니다. (연쇄 기부) */
void
donate_priority (void)
{
    struct thread *cur = thread_current ();
    struct lock *l = cur->wait_on_lock;

    while (l != NULL)
    {
        struct thread *holder = l->holder;
        if (holder == NULL || holder->priority >= cur->priority) {
            break; // 더 이상 기부할 필요 없음
        }

        // holder에게 우선순위 기부
        holder->priority = cur->priority;
        
        // 연쇄 기부: holder가 또 다른 락을 기다리는지 확인
        l = holder->wait_on_lock;
    }
}


/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  
  // [추가] Priority Scheduling 초기화
  initial_thread->original_priority = PRI_DEFAULT;
  if (thread_mlfqs) {
      initial_thread->mlfqs_queue_level = 0;
      initial_thread->priority = PRI_MAX; // MLFQS Q0
  }
  
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void)
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void)
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

    // [수정] 스케줄링 로직을 MLFQS 모드와 Priority/Aging 모드로 명확히 분리
    if (thread_mlfqs) {
        /* Simplified MLFQS Logic */

        struct list_elem *e;
        
        // 1. 에이징 및 승급 (Ready List의 스레드만)
        e = list_begin (&ready_list);
        while (e != list_end (&ready_list)) {
            struct thread *ready_t = list_entry(e, struct thread, elem);
            struct list_elem *next_e = list_next(e);
            
            ready_t->age++;
            
            if (ready_t->age >= 20 && ready_t->mlfqs_queue_level > 0) {
                // 승급
                ready_t->mlfqs_queue_level--;
                ready_t->priority = PRI_MAX - 3 * ready_t->mlfqs_queue_level;
                
                // 재정렬
                list_remove(e);
                list_insert_ordered(&ready_list, e, thread_cmp_priority, NULL);
                
                ready_t->age = 0;
            }
            e = next_e;
        }

        // 2. 강등 (Time Slice 소모 시)
        int time_slice;
        if (t->mlfqs_queue_level == 0)      time_slice = TIME_SLICE / 2; // Q0 (2 ticks)
        else if (t->mlfqs_queue_level == 2) time_slice = TIME_SLICE * 2; // Q2 (8 ticks)
        else                                time_slice = TIME_SLICE;     // Q1 (4 ticks)

        if (++thread_ticks >= time_slice) {
            
            // 강등 (Q2 이상으로는 강등하지 않음)
            if (t->mlfqs_queue_level < 2) {
                t->mlfqs_queue_level++;
                t->priority = PRI_MAX - 3 * t->mlfqs_queue_level;
            }

            // 타임 슬라이스 소모에 따른 선점 (yield)
            intr_yield_on_return ();
            return; 
        }

        // 3. Q0 선점 (Q1/Q2 실행 중 Q0에 스레드가 있는 경우)
        if (t->mlfqs_queue_level > 0 && !list_empty(&ready_list)) {
            struct thread *highest = list_entry(list_front(&ready_list), struct thread, elem);
            if (highest->mlfqs_queue_level == 0) {
                intr_yield_on_return ();
            }
        }
    }
    else {
        /* Priority Scheduling / Aging Logic */

        struct list_elem *e;
        
        // 1. 에이징 및 승급 (Ready List의 스레드만)
        e = list_begin (&ready_list);
        while (e != list_end (&ready_list)) {
            struct thread *ready_t = list_entry(e, struct thread, elem);
            struct list_elem *next_e = list_next(e);
            
            ready_t->age++;
            
            if (ready_t->age >= 20) {
                int new_priority = ready_t->priority;
                // 우선순위가 최대값(PRI_MAX)이 아니면 승급
                if (new_priority < PRI_MAX) {
                    new_priority = MIN(ready_t->priority + 1, PRI_MAX);
                    ready_t->priority = new_priority;
                    
                    // 우선순위 변경 시 재정렬: 리스트에서 제거 후 재삽입
                    list_remove(e);
                    list_insert_ordered(&ready_list, e, thread_cmp_priority, NULL);
                }
                ready_t->age = 0;
            }
            e = next_e;
        }

        // 2. Round-Robin Fallback (기존 로직) 및 선점 확인
        if (++thread_ticks >= TIME_SLICE) {
            intr_yield_on_return ();
        }
    }
}

/* Prints thread statistics. */
void
thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.
*/
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();
  
  // [추가] Project 1: Priority Scheduling 초기화
  t->original_priority = priority;
  if (thread_mlfqs) {
      t->mlfqs_queue_level = 0;
      t->priority = PRI_MAX; // MLFQS Q0
  }
  
  // Prepare thread for first run by initializing its stack.
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.) */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  
  // [수정] list_push_back 대신 우선순위 순으로 삽입
  list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL);
  
  t->status = THREAD_READY;
  intr_set_level (old_level);
  
  // [추가] 선점 검사: Unblock된 스레드의 우선순위가 현재 스레드보다 높으면 선점
  if (t->priority > thread_current()->priority) {
      thread_yield();
  }
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void)
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void)
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
    // [수정] list_push_back 대신 우선순위 순으로 삽입
    list_insert_ordered (&ready_list, &cur->elem, thread_cmp_priority, NULL);
    
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority)
{
    // [수정] MLFQS 모드에서는 우선순위 설정 무시
    if (thread_mlfqs) {
        return; 
    }

    struct thread *cur = thread_current ();
    int old_priority = cur->priority;

    // Priority Scheduling 모드에서만 Donation 로직 실행
    cur->original_priority = new_priority; // 원래 우선순위 저장
    refresh_priority ();                   // 기부된 우선순위가 있다면 재계산

    // 우선순위가 낮아지거나, ready_list의 최고 스레드가 더 높으면 선점
    if (cur->priority < old_priority || 
        (!list_empty(&ready_list) && cur->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority))
    {
        thread_yield();
    }
}

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED)
{
  /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void)
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void)
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void)
{
  /* Not yet implemented. */
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.
*/
static void
idle (void *idle_started_ UNUSED)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;)
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.
       */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux)
{
  ASSERT (function != NULL);

  intr_enable (); /* The scheduler runs with interrupts off. */
  function (aux); /* Execute the thread function. */
  thread_exit (); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;
  
  // [추가] Project 1: 초기화
  t->original_priority = priority;
  t->wait_on_lock = NULL;
  t->age = 0;
  t->mlfqs_queue_level = 0;

  list_push_back (&all_list, &t->allelem);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size)
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.
*/
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();

  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.
*/
static void
schedule (void)
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void)
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

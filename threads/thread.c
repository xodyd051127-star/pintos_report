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
#include "devices/timer.h" 
#include "threads/fixed_point.h" // Fixed-Point 헤더 추가

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of all processes. */
static struct list all_list;

/* List of processes in THREAD_READY state, that is, processes
   ready to run but not actually running. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Saved instruction pointer. */
    thread_func *function;      /* Function to start running. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks spent in kernel threads. */
static long long user_ticks;    /* # of timer ticks spent in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduling.
   If true, use multi-level feedback queue scheduler (MLFQS). */
bool thread_mlfqs;

/* Load average for MLFQS. Initialized to 0 when kernel starts (0 in 17.14 format). */
int load_avg; 

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

/* 함수 프로토타입 선언 (MLFQS & Helper) */
static void thread_calculate_priority (struct thread *t);
static void thread_calculate_recent_cpu (struct thread *t);
static void thread_calculate_load_avg (void);
static void init_mlfqs_fields (struct thread *t);

/* 현재 실행 중인 스레드보다 ready_list의 선두 스레드의 우선순위가 높으면 yield */
void test_max_priority (void) {
    if (intr_context ()) return; // 인터럽트 컨텍스트에서는 yield 불가
    
    if (!list_empty (&ready_list)) {
        struct thread *highest_priority_thread = list_entry (list_front (&ready_list), struct thread, elem);
        if (highest_priority_thread->priority > thread_current()->priority) {
            thread_yield();
        }
    }
}

/* thread_create() 시 MLFQS 관련 필드 초기화 */
static void init_mlfqs_fields (struct thread *t) {
    if (t == initial_thread) {
        // 초기 스레드는 nice=0, recent_cpu=0으로 시작
        t->nice = 0;
        t->recent_cpu = INT_TO_FP(0);
        // load_avg도 thread_init에서 0으로 초기화
        load_avg = INT_TO_FP(0); 
    } else {
        // 새로 생성된 스레드는 부모 스레드의 nice 값을 상속받음
        t->nice = thread_current()->nice;
        t->recent_cpu = thread_current()->recent_cpu;
    }
    
    // priority는 MLFQS 계산을 통해 갱신
    thread_calculate_priority(t);
}

/* Initializes the threading system by transforming the code
   that's currently running into a thread. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up a thread structure for the running code. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
  
  if (thread_mlfqs) {
    init_mlfqs_fields(initial_thread);
  }
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  idle_thread = thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt every HZ times per second. */
void 
thread_tick (void) 
{
    struct thread *t = thread_current ();
    
    // Statistics.
    if (t == idle_thread)
      idle_ticks++;
#ifdef USERPROG
    if (t->pagedir != NULL)
      user_ticks++;
#endif
    else
      kernel_ticks++;
    
    // MLFQS 모드일 때 recent_cpu 증가 및 4틱/60틱 로직
    if (thread_mlfqs) {
        // 1. 현재 스레드의 recent_cpu 1 증가
        if (t != idle_thread) {
            t->recent_cpu = FP_ADD(t->recent_cpu, INT_TO_FP(1));
        }

        // 2. 60틱마다 load_avg와 모든 스레드의 recent_cpu 갱신
        if (timer_ticks() % 60 == 0) {
            thread_calculate_load_avg();
            struct list_elem *e;
            for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e)) {
                struct thread *t_mlfqs = list_entry (e, struct thread, allelem);
                if (t_mlfqs != idle_thread) {
                    thread_calculate_recent_cpu(t_mlfqs);
                }
            }
        }
        
        // 3. 4틱마다 모든 스레드의 우선순위 갱신
        if (timer_ticks() % 4 == 0) {
            struct list_elem *e;
            for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e)) {
                struct thread *t_mlfqs = list_entry (e, struct thread, allelem);
                if (t_mlfqs != idle_thread) {
                    thread_calculate_priority(t_mlfqs);
                }
            }
            // 우선순위가 변경되었으므로 ready_list를 정렬합니다. (선점 검사는 아래에서 수행)
            list_sort(&ready_list, thread_cmp_priority, NULL);
        }
    }
    
    // 일반 우선순위 선점: 매 틱마다 ready_list 최상위와 비교하여 선점
    if (!list_empty(&ready_list) && 
        list_entry(list_front(&ready_list), struct thread, elem)->priority > t->priority) {
        intr_yield_on_return (); 
    }

    // Schedule: A call to thread_yield() lets other threads run.
    if (++thread_ticks >= TIME_SLICE)
      intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given PRIORITY and
   executes FUNCTION_ with AUX passed to it.  Returns the new thread's
   thread id, or TID_ERROR if creation fails. */
tid_t
thread_create (const char *name, int priority,
               thread_func func, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (priority >= PRI_MIN && priority <= PRI_MAX);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = func;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  // [Project 1: MLFQS / Priority Donation] 필드 초기화
  if (thread_mlfqs) {
    init_mlfqs_fields(t);
  } else {
    list_init(&t->locks);
  }

  /* Add to run queue. */
  thread_unblock (t);
  
  // [Project 1: Priority Scheduling] 새로 생성된 스레드가 현재 스레드보다 우선순위가 높으면 선점
  test_max_priority();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock(). */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an all-purpose wake-up function. */
void thread_unblock (struct thread *t) {
    enum intr_level old_level;

    ASSERT (t != NULL);
    ASSERT (is_thread (t));
    ASSERT (t->status == THREAD_BLOCKED);

    old_level = intr_disable ();
    
    // list_insert_ordered를 사용하여 ready_list에 우선순위 순으로 삽입
    list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL);

    t->status = THREAD_READY;
    
    // 선점 로직: 새로 깨어난 스레드가 현재 스레드보다 우선순위가 높으면 즉시 yield
    // (intr_yield_on_return을 사용하여 인터럽트 종료 시 선점되도록 함)
    if (t->priority > thread_get_priority ()) {
        intr_yield_on_return (); 
    }

    intr_set_level (old_level);
}


/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is an inline function to avoid a function call context switch. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* ESP is the stack pointer for the current thread.
     Retrieve it and return the thread structure. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) ((unsigned) esp & 0xfffff000);
}

/* Returns the thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never returns to
   the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set necessary status and
     schedule away. */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and may be
   scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) {
    // list_insert_ordered를 사용하여 ready_list에 우선순위 순으로 삽입
    list_insert_ordered (&ready_list, &cur->elem, thread_cmp_priority, NULL);
  }
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

  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* [Priority Scheduling] list_insert_ordered에서 사용할 비교 함수 */
bool thread_cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
    struct thread *thread_a = list_entry(a, struct thread, elem);
    struct thread *thread_b = list_entry(b, struct thread, elem);
    
    // 우선순위가 높은(값이 큰) 스레드가 앞으로 와야 하므로 > 연산자를 사용
    // **주의: priority-fifo 테스트 통과를 위해, 우선순위가 같을 경우 무조건 false를 반환해야 합니다.**
    return thread_a->priority > thread_b->priority;
}


/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* 현재 스레드의 우선순위를 설정합니다. (priority-change, priority-donate에 사용) */
void thread_set_priority (int new_priority) {
    enum intr_level old_level = intr_disable();

    if (thread_mlfqs) {
        // MLFQS 모드에서는 nice 값만 변경합니다.
        // 이 함수는 MLFQS 모드에서 thread_set_nice()의 내부 구현으로 사용되지 않으므로, 
        // 실제 MLFQS에서는 thread_set_nice()를 통해 nice와 priority가 갱신됩니다.
        // 현재 Pintos 구조상 thread_set_priority를 호출하면 단순히 base_priority만 바꿉니다.
        // MLFQS는 base_priority를 사용하지 않으므로, 이 함수는 MLFQS 모드에서 효과가 없습니다.
        thread_current()->priority = new_priority;
    } else {
        // Priority Donation 모드일 때 base_priority를 설정합니다.
        thread_current()->base_priority = new_priority;
        
        // 우선순위가 바뀌면 donation을 고려하여 실제 priority를 갱신합니다.
        thread_update_priority();
        
        // 우선순위 변경 후 즉시 선점 검사
        test_max_priority();
    }

    intr_set_level(old_level);
}


/* [Priority Donation] 락 대기 시, 락 소유자에게 기부된 우선순위를 회수하고 갱신 */
void thread_update_priority (void) {
    struct thread *t = thread_current ();
    
    // 1. base_priority로 초기화
    t->priority = t->base_priority;

    // 2. 획득한 락 리스트(locks)를 순회하며 가장 높은 max_priority를 찾음
    if (!list_empty (&t->locks)) {
        struct list_elem *e;
        int max_donated_priority = t->base_priority;
        
        // locks 리스트를 순회하며 획득한 모든 락의 max_priority를 검사
        for (e = list_begin (&t->locks); e != list_end (&t->locks); e = list_next (e)) {
            struct lock *l = list_entry (e, struct lock, lock_elem);
            if (l->max_priority > max_donated_priority) {
                max_donated_priority = l->max_priority;
            }
        }
        
        // 가장 높은 기부 우선순위를 최종 우선순위에 적용
        if (max_donated_priority > t->priority) {
             t->priority = max_donated_priority;
        }
    }
    
    // 3. 우선순위가 바뀌었다면 즉시 선점 검사
    test_max_priority();
}

/* [Priority Donation] 락 소유자에게 우선순위를 기부 (재귀적 호출) */
void thread_donate_priority (struct thread *t, int priority) {
    // 1. 락 소유자의 현재 우선순위보다 기부할 우선순위가 높을 경우 갱신
    if (t->priority < priority) {
        t->priority = priority;
    }
    
    // 2. 락 소유자가 다른 락을 기다리고 있다면, 재귀적으로 기부를 전파합니다.
    if (t->wait_on_lock != NULL) {
        struct lock *next_lock = t->wait_on_lock;
        if (next_lock->max_priority < priority) {
            // 기다리는 락의 max_priority를 갱신
            next_lock->max_priority = priority;
            // 해당 락의 소유자에게 기부를 전파
            thread_donate_priority(next_lock->holder, priority);
        }
    }
}


/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
    enum intr_level old_level = intr_disable();

    // nice 값 범위 제한
    if (nice > 20) nice = 20;
    if (nice < -20) nice = -20;
    
    thread_current()->nice = nice;

    // nice 값 변경 후, priority 재계산 및 선점 검사
    thread_calculate_priority(thread_current());
    test_max_priority();
    
    intr_set_level(old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
    enum intr_level old_level = intr_disable();
    int nice = thread_current()->nice;
    intr_set_level(old_level);
    return nice;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
    enum intr_level old_level = intr_disable();
    // recent_cpu (Fixed-Point) * 100을 정수로 반올림하여 반환
    int recent_cpu = FP_TO_INT_ROUND(FP_MUL_INT(thread_current()->recent_cpu, 100));
    intr_set_level(old_level);
    return recent_cpu;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
    enum intr_level old_level = intr_disable();
    // load_avg (Fixed-Point) * 100을 정수로 반올림하여 반환
    int load_avg_100 = FP_TO_INT_ROUND(FP_MUL_INT(load_avg, 100));
    intr_set_level(old_level);
    return load_avg_100;
}

/* [MLFQS] thread_calculate_priority 구현 */
static void thread_calculate_priority (struct thread *t) {
    if (t == idle_thread) return;

    // priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
    int recent_cpu_term = FP_DIV_INT (t->recent_cpu, 4);
    int nice_term = INT_TO_FP (t->nice * 2);

    int new_priority_fp = FP_SUB (INT_TO_FP (PRI_MAX), FP_ADD (recent_cpu_term, nice_term));
    int new_priority = FP_TO_INT_ROUND (new_priority_fp);

    // 우선순위 범위 제한
    if (new_priority > PRI_MAX) new_priority = PRI_MAX;
    if (new_priority < PRI_MIN) new_priority = PRI_MIN;

    t->priority = new_priority;
}

/* [MLFQS] thread_calculate_recent_cpu 구현 */
static void thread_calculate_recent_cpu (struct thread *t) {
    if (t == idle_thread) return;
    
    // coefficient = 2 * load_avg / (2 * load_avg + 1)
    int term1 = FP_MUL_INT (load_avg, 2);
    int term2 = FP_ADD_INT (term1, 1);
    int coefficient = FP_DIV (term1, term2);
    
    // recent_cpu = coefficient * recent_cpu + nice
    t->recent_cpu = FP_ADD_INT (FP_MUL (coefficient, t->recent_cpu), t->nice);
}

/* [MLFQS] thread_calculate_load_avg 구현 */
static void thread_calculate_load_avg (void) {
    // ready_list와 running thread를 포함한 실행 가능한 스레드의 개수를 계산
    int ready_threads = list_size (&ready_list);
    if (thread_current () != idle_thread) {
        ready_threads++; 
    }
    
    // load_avg = (59/60) * load_avg + (1/60) * ready_threads
    int term1_coef = FP_DIV_INT (INT_TO_FP (59), 60);
    int term2_coef = FP_DIV_INT (INT_TO_FP (1), 60);
    
    int term1 = FP_MUL (term1_coef, load_avg);
    int term2 = FP_MUL_INT (term2_coef, ready_threads);
    
    load_avg = FP_ADD (term1, term2);
}

/* Idle thread.  Executes when no other thread is ready to run. */
static void
idle (void *aux UNUSED) 
{
  struct semaphore *idle_started = aux;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* The scheduler may decide to switch threads at any point. */
      intr_disable ();
      schedule ();
      intr_enable ();
    }
}

/* Initializes a `struct thread' by setting its default priority,
   providing a stack, and adding it to the all threads list. */
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
  
  // [Project 1: Priority Donation] 필드 초기화
  t->base_priority = priority;
  t->wait_on_lock = NULL;
  list_init(&t->locks);

  // [Project 1: MLFQS] 필드 초기화 (MLFQS 모드에서만 사용)
  t->nice = 0;
  t->recent_cpu = INT_TO_FP(0);


  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

/* Chooses and returns the next thread to be scheduled.  It is
   called from schedule() and is only guaranteed to return a thread
   if at least one thread is ready. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread was not idle, giving a ready
   thread a chance to run. */
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
  /* Activate the new thread's page tables. */
  process_activate ();
#endif

  /* If the thread we switched from is not the idle thread, now is a
     good time to destroy it. */
  if (prev != idle_thread)
    palloc_free_page (prev);
}

/* Schedules a new process.  At the time this is called, we want to
   switch away from the current thread, which is the one running this
   code.  next_thread_to_run() finds the next thread to run, and
   switch_threads() performs the context switch. */
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

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named NAME. */
static void
*alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data grows downward. */
  t->stack -= size;
  return t->stack;
}

/* Chooses a name for a new thread from our tid sequence. */
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

/* Offset of 'stack' member within 'struct thread'.
   Used in assembly language. */
#define THREAD_STACK_OFS (offsetof (struct thread, stack))

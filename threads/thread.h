#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stddef.h>
#include <stdint.h>
#include "threads/synch.h"

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to the type you prefer. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or process.

   Each thread structure is stored in its own 4 kB page.  The
   thread control block (TCB) is always at the beginning of the
   page.

   The first member is the thread's saved stack pointer, which
   is used to switch between threads.

   It's essential to understand the representation of a thread's
   stack:

         +-----------------+
         | Switch frames   |
         |-----------------|
         | Thread structure|
         |-----------------|
         | page start      |
         +-----------------+

   The original thread's stack looks like this:

         +-----------------+
         | Switch frames   |
         |-----------------|
         | (kernel/thread) |
         |-----------------|
         | (kernel/thread) |
         +-----------------+
 
   The `stack' member points to the bottommost of the stack
   frames, ready to be popped off.
*/
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                      /* Thread identifier. */
    enum thread_status status;      /* Thread state. */
    char name[16];                  /* Name (for debugging purposes). */
    uint8_t *stack;                 /* Saved stack pointer. */
    int priority;                   /* Priority. */
    struct list_elem allelem;       /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;          /* List element. */

    /* [추가] Project 1: Priority Scheduling & Donation 관련 */
   int base_priority;           /* 원래의 우선순위 (기부받기 전 값). */
    struct lock *wait_on_lock;   /* 현재 기다리고 있는 Lock 포인터. */
    struct list_elem donation_elem;/* 자신이 락의 donators 리스트에 사용될 리스트 엘리먼트. */
    struct list locks;           /* 이 스레드가 획득한 Lock 리스트. */

    /* [추가] Project 1: Aging 및 MLFQS 관련 */
    int age;                        /* 에이징을 위한 틱 카운터입니다. */
    int mlfqs_queue_level;          /* Simplified MLFQS 큐 레벨입니다 (0: Q0, 1: Q1, 2: Q2). */
    int nice;                    /* MLFQS의 nice 값 (정수). */
    int recent_cpu;              /* 최근 CPU 사용량 (Fixed-Point). */
    

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;              /* Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic;                 /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *aux);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* [추가] Project 1: 스케줄링 및 동기화 헬퍼 함수 선언 */

/* 우선순위 비교 함수 (ready_list 및 synch 대기열 정렬에 사용) */
bool thread_cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux);

/* Priority Donation 헬퍼 함수 */
void donate_priority (void);
void remove_with_lock (struct lock *lock);
void refresh_priority (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int new_priority);

int thread_get_nice (void);
void thread_set_nice (int nice);
int thread_get_load_avg (void);
int thread_get_recent_cpu (void);

#endif /* threads/thread.h */

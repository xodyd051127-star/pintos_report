#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"

#define F (1 << 14) // 1.0을 의미하는 값 (16384)

/* Fixed-Point 값을 정수로 반올림 변환 */
#define FP_TO_INT_ROUND(x) ((x) >= 0 ? ((x) + F / 2) / F : ((x) - F / 2) / F)

/* 정수를 Fixed-Point로 변환 */
#define INT_TO_FP(n) ((n) * F)

/* Fixed-Point 덧셈 */
#define FP_ADD(x, y) ((x) + (y))

/* Fixed-Point와 정수 덧셈 */
#define FP_ADD_INT(x, n) ((x) + (n) * F)

/* Fixed-Point 뺄셈 */
#define FP_SUB(x, y) ((x) - (y))

/* Fixed-Point 곱셈 (오버플로우 방지를 위해 int64_t 사용) */
#define FP_MUL(x, y) (((int64_t)(x)) * (y) / F)

/* Fixed-Point와 정수 곱셈 */
#define FP_MUL_INT(x, n) ((x) * (n))

/* Fixed-Point 나눗셈 (오버플로우 방지를 위해 int64_t 사용) */
#define FP_DIV(x, y) (((int64_t)(x)) * F / (y))

/* Fixed-Point와 정수 나눗셈 */
#define FP_DIV_INT(x, n) ((x) / (n))

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to happen. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* A kernel thread or user process. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Priority. */
    struct list_elem allelem;           /* List element for all threads list. */
    
    /* [Project 1: Priority Scheduling & Donation] */
    int base_priority;                  /* 원래의 우선순위 (기부받기 전 값). */
    struct lock *wait_on_lock;          /* 현재 기다리고 있는 Lock 포인터. */
    struct list_elem donation_elem;     /* 자신이 락의 donators 리스트에 사용될 리스트 엘리먼트. */
    struct list locks;                  /* 이 스레드가 획득한 Lock 리스트. */
    struct list_elem lock_elem;         /* Lock의 holder가 가진 locks 리스트에 사용될 엘리먼트 */


    /* [Project 1: MLFQS] */
    int nice;                           /* MLFQS의 nice 값 (정수). */
    int recent_cpu;                     /* 최근 CPU 사용량 (Fixed-Point). */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduling.
   If true, use multi-level feedback queue scheduler (MLFQS). */
extern bool thread_mlfqs;

/* Load average for MLFQS */
extern int load_avg; // thread.c에서 전역으로 선언됨

/*  (기존 함수 프로토타입)  */

int thread_get_priority (void);
void thread_set_priority (int new_priority);
void thread_update_priority (void); // Priority Donation 갱신 함수 선언

/*  (MLFQS 관련 함수 프로토타입) */
int thread_get_nice (void);
void thread_set_nice (int nice);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

bool thread_cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux); // 우선순위 비교 함수 선언
void thread_donate_priority (struct thread *t, int priority); // 우선순위 기부 함수 선언

#endif /* threads/thread.h */

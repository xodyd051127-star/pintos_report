#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/* Forward declaration to avoid circular dependency */
struct thread; 

/* A counting semaphore. */
struct semaphore
{
    unsigned value;       /* Current value. */
    struct list waiters;  /* List of waiting threads. */
};

void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);

/* Lock. */
struct lock
{
    struct thread *holder;         /* Thread holding lock (for debugging). */
    struct semaphore semaphore;    /* Binary semaphore controlling access. */
    
    /* [추가] Project 1: Priority Donation 관련 */
    struct list donators;          /* 이 Lock을 기다리는 스레드 리스트 (우선순위 순). */
    int max_priority;              /* donators 리스트에서 가장 높은 우선순위입니다. */
};

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

/* Condition variable. */
struct condition
{
    struct list waiters; /* List of waiting threads. */
};

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

/* Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /* threads/synch.h */

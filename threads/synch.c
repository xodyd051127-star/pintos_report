/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.
   ... (Copyright notice omitted for brevity) ...
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE. 
   ... (Comments omitted for brevity) ...
*/
void
sema_init (struct semaphore *sema, unsigned value)
{
    ASSERT (sema != NULL);

    sema->value = value;
    list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore. 
   ... (Comments omitted for brevity) ...
*/
void
sema_down (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);
    ASSERT (!intr_context ());

    old_level = intr_disable ();
    while (sema->value == 0)
        {
            // [수정] list_push_back 대신 우선순위 순으로 삽입
            list_insert_ordered (&sema->waiters, &thread_current ()->elem, thread_cmp_priority, NULL);
            thread_block ();
        }
    sema->value--;
    intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0. 
   ... (Comments omitted for brevity) ...
*/
bool
sema_try_down (struct semaphore *sema)
{
    enum intr_level old_level;
    bool success;

    ASSERT (sema != NULL);

    old_level = intr_disable ();
    if (sema->value > 0)
        {
            sema->value--;
            success = true;
        }
    else
        success = false;
    intr_set_level (old_level);

    return success;
}

/* Up or "V" operation on a semaphore. 
   ... (Comments omitted for brevity) ...
*/
void
sema_up (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);

    old_level = intr_disable ();
    if (!list_empty (&sema->waiters)) {
        // [수정] waiters 리스트는 우선순위 순으로 정렬되어 있으므로, pop_front로 최고 우선순위 스레드 선택
        struct thread *t = list_entry (list_pop_front (&sema->waiters),
                                       struct thread, elem);
        thread_unblock (t);
    }
    sema->value++;
    intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads. 
   ... (Comments omitted for brevity) ...
*/
void
sema_self_test (void)
{
    struct semaphore sema[2];
    int i;

    printf ("Testing semaphores...");
    sema_init (&sema[0], 0);
    sema_init (&sema[1], 0);
    thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
    for (i = 0; i < 10; i++)
        {
            sema_up (&sema[0]);
            sema_down (&sema[1]);
        }
    printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_)
{
    struct semaphore *sema = sema_;
    int i;

    for (i = 0; i < 10; i++)
        {
            sema_down (&sema[0]);
            sema_up (&sema[1]);
        }
}

/* Initializes LOCK. 
   ... (Comments omitted for brevity) ...
*/
void
lock_init (struct lock *lock)
{
    ASSERT (lock != NULL);

    lock->holder = NULL;
    sema_init (&lock->semaphore, 1);
    
    // [추가] Donation 관련 초기화
    list_init (&lock->donators);
    lock->max_priority = PRI_MIN;
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary. 
   ... (Comments omitted for brevity) ...
*/
void
lock_acquire (struct lock *lock)
{
    struct thread *cur = thread_current();
    
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (!lock_held_by_current_thread (lock));

    enum intr_level old_level = intr_disable();

    if (lock->holder != NULL)
    {
        // [수정] Priority Scheduling 모드에서만 Donation 로직 실행
        if (!thread_mlfqs) {
            // Donation 시작
            cur->wait_on_lock = lock;
            // donation_elem을 사용하여 donators 리스트에 우선순위 순으로 삽입
            list_insert_ordered (&lock->donators, &cur->donation_elem, thread_cmp_priority, NULL);
            
            // 락의 max_priority 업데이트
            if (lock->max_priority < cur->priority) {
                lock->max_priority = cur->priority;
            }
            donate_priority(); // 연쇄 기부
        }

        sema_down (&lock->semaphore);
        
        // 블록에서 풀리면
        if (!thread_mlfqs) {
            cur->wait_on_lock = NULL;
            // donators 리스트에서 자신을 제거 (lock_acquire 완료)
            remove_with_lock(lock);
        }
    }
    else 
    {
        sema_down (&lock->semaphore); // value=1이므로 바로 통과
    }

    lock->holder = cur;
    intr_set_level(old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure. 
   ... (Comments omitted for brevity) ...
*/
bool
lock_try_acquire (struct lock *lock)
{
    bool success;

    ASSERT (lock != NULL);
    ASSERT (!lock_held_by_current_thread (lock));

    success = sema_try_down (&lock->semaphore);
    if (success)
        lock->holder = thread_current ();
    return success;
}

/* Releases LOCK, which must be owned by the current thread.
   ... (Comments omitted for brevity) ...
*/
void
lock_release (struct lock *lock)
{
    struct thread *cur = thread_current();
    
    ASSERT (lock != NULL);
    ASSERT (lock_held_by_current_thread (lock));

    enum intr_level old_level = intr_disable();
    
    lock->holder = NULL;
    sema_up (&lock->semaphore);
    
    // [수정] Priority Scheduling 모드에서만 Donation 해제 로직 실행
    if (!thread_mlfqs) {
        // 기부 효과 제거
        refresh_priority(); 
        
        // 락 해제 후 선점 여부 확인
        if (!list_empty(&ready_list) && thread_current()->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority) {
            thread_yield();
        }
    }

    intr_set_level(old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise. 
   ... (Comments omitted for brevity) ...
*/
bool
lock_held_by_current_thread (const struct lock *lock)
{
    ASSERT (lock != NULL);

    return lock->holder == thread_current ();
}

/* One semaphore in a list. 
   ... (Comments omitted for brevity) ...
*/
struct semaphore_elem
{
    struct list_elem elem;      /* List element. */
    struct semaphore semaphore; /* This semaphore. */
};

/* Initializes condition variable COND. 
   ... (Comments omitted for brevity) ...
*/
void
cond_init (struct condition *cond)
{
    ASSERT (cond != NULL);

    list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code. 
   ... (Comments omitted for brevity) ...
*/
void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);
    // [수정] cond_waiters는 thread_elem을 직접 사용하지 않고 semaphore_elem을 사용. 
    // semaphore_elem은 우선순위 정보가 없으므로 정렬하지 않음.
    list_push_back (&cond->waiters, &waiter.elem);
    
    // Lock release는 lock_release()가 알아서 Donation 해제 로직을 처리함
    lock_release (lock); 
    sema_down (&waiter.semaphore);
    lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   ... (Comments omitted for brevity) ...
*/
void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    if (!list_empty (&cond->waiters))
        // [수정] list_pop_front로 가장 오래 기다린(FIFO) 스레드를 깨움
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                             ->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).
   ... (Comments omitted for brevity) ...
*/
void
cond_broadcast (struct condition *cond, struct lock *lock)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);

    while (!list_empty (&cond->waiters))
        cond_signal (cond, lock);
}

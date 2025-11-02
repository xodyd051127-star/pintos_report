#include "threads/synch.h"
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/list.h"

/* Returns true if LOCK is held by the current thread, false otherwise. */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);
  return lock->holder == thread_current ();
}

/* [Project 1] lock_init 함수 수정 */
void lock_init (struct lock *lock) {
  ASSERT (lock != NULL);

  sema_init (&lock->semaphore, 1);
  lock->holder = NULL;
  
  /* Priority Donation을 위한 필드 초기화 */
  list_init (&lock->donators);
  lock->max_priority = PRI_MIN; // 초기값은 최소 우선순위
}

/* [Project 1] lock_acquire 함수 수정 (기부 로직) */
void lock_acquire (struct lock *lock) {
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  struct thread *curr = thread_current ();

  intr_disable ();
  
  // 락을 기다려야 하는 경우 (lock->semaphore.value == 0)
  if (lock->semaphore.value == 0) {
      // 1. 현재 기다리는 락을 저장하고, 락의 donators 리스트에 우선순위 순으로 삽입
      curr->wait_on_lock = lock;
      // donation_elem을 사용하여 donators 리스트에 삽입
      list_insert_ordered (&lock->donators, &curr->donation_elem, thread_cmp_priority, NULL);

      // 2. donators 리스트에서 가장 높은 우선순위를 갱신하고 holder에게 기부
      int highest_priority = list_entry(list_front(&lock->donators), struct thread, donation_elem)->priority;
      lock->max_priority = highest_priority;
      thread_donate_priority (lock->holder, lock->max_priority); 
      
      // 3. semaphore down (대기)
      sema_down (&lock->semaphore);

      // 락 획득 후, donators 리스트에서 자신을 제거
      list_remove (&curr->donation_elem);
      curr->wait_on_lock = NULL;
  } 
  
  // 락 획득 (lock->semaphore.value == 1 이었거나, sema_down 후 락을 획득했거나)
  lock->holder = curr;
  list_push_back(&curr->locks, &lock->lock_elem);
  
  // 락 획득 후 우선순위 갱신 (획득한 락에 대한 기부 우선순위 반영)
  thread_update_priority(); 
  
  intr_enable ();
}

/* [Project 1] lock_release 함수 수정 (기부 회수 로직) */
void lock_release (struct lock *lock) {
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  intr_disable ();
  
  // 1. 스레드의 locks 리스트에서 락 제거
  list_remove(&lock->lock_elem);

  // 2. 락 소유자 해제 및 max_priority 초기화
  lock->holder = NULL;
  lock->max_priority = PRI_MIN; // 최소 우선순위로 리셋
  
  // 3. 우선순위 재조정 (획득한 다른 락이 있다면 그 락의 max_priority로 갱신)
  thread_update_priority ();

  // 4. semaphore up (락 대기 중인 스레드 중 가장 높은 우선순위 스레드가 깨어남)
  sema_up (&lock->semaphore);
  
  intr_enable ();
}

/* Initializes semaphore S to VALUE. */
void sema_init (struct semaphore *sema, int value) {
  ASSERT (sema != NULL);
  ASSERT (value >= 0);

  list_init (&sema->waiters);
  sema->value = value;
}

/* Waits for semaphore S to be signaled.  If S's value is positive,
   decrements it and returns immediately.  Otherwise, adds the current
   thread to S's waiters list and blocks. */
void sema_down (struct semaphore *sema) {
  enum intr_level old_level;
  
  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      // [Project 1: Priority Scheduling] waiters 리스트에 우선순위 순으로 삽입
      list_insert_ordered (&sema->waiters, &thread_current()->elem, thread_cmp_priority, NULL);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Tries to acquire semaphore S without blocking.  Returns true on
   success, false on failure. */
bool sema_try_down (struct semaphore *sema) {
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

/* Signals semaphore S, waking up one waiting thread, if any. */
void sema_up (struct semaphore *sema) {
  enum intr_level old_level;
  
  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters)) 
    {
      // [Project 1: Priority Scheduling] 가장 높은 우선순위 스레드를 unblock
      struct list_elem *e = list_pop_front (&sema->waiters);
      thread_unblock (list_entry (e, struct thread, elem));
    }
  sema->value++;
  intr_set_level (old_level);
}

/* ... (나머지 cond_init, cond_wait, cond_signal, cond_broadcast 등은 Pintos 기본 구현과 동일) ... */

/* Initializes condition variable COND. */
void cond_init (struct condition *cond) {
  list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled.
   When signaled, reacquires LOCK before returning. */
void cond_wait (struct condition *cond, struct lock *lock) {
  struct thread *curr = thread_current();
  
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  intr_disable ();
  // [Project 1: Priority Scheduling] waiters 리스트에 우선순위 순으로 삽입
  list_insert_ordered (&cond->waiters, &curr->elem, thread_cmp_priority, NULL);
  
  lock_release (lock);
  thread_block ();
  lock_acquire (lock);
  intr_enable ();
}

/* Wakes up one thread, if any, waiting on COND. */
void cond_signal (struct condition *cond, struct lock *lock UNUSED) {
  enum intr_level old_level;
  
  ASSERT (cond != NULL);
  
  old_level = intr_disable ();
  if (!list_empty (&cond->waiters)) {
    // [Project 1: Priority Scheduling] 가장 높은 우선순위 스레드를 unblock
    struct list_elem *e = list_pop_front (&cond->waiters);
    thread_unblock (list_entry (e, struct thread, elem));
  }
  intr_set_level (old_level);
}

/* Wakes up all threads waiting on COND. */
void cond_broadcast (struct condition *cond, struct lock *lock UNUSED) {
  enum intr_level old_level;

  ASSERT (cond != NULL);

  old_level = intr_disable ();
  while (!list_empty (&cond->waiters))
    {
      struct list_elem *e = list_pop_front (&cond->waiters);
      thread_unblock (list_entry (e, struct thread, elem));
    }
  intr_set_level (old_level);
}

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <windows.h>
#include "ThreadHandler.h"

#define MB(x) (((ULONG64)(x)) * 1024 * 1024)
#define KB(x) ((ULONG64)(x) * 1024)
#define GB(x) (MB(x) * 1024)

#define WORK_ITERATIONS (GB(1) * 1)

int num_threads;
DWORD num_cores;
HANDLE threads[MAX_THREADS];
THREAD_CONTROLLER thread_metadata_array[MAX_THREADS];
LOCKED_LIST wait_list;
LOCKED_LIST ready_pool[NUM_PRIORITIES];
HANDLE wait_event;
SIM_LOCK locks[NUM_LOCKS];
CRITICAL_SECTION scheduler_lock; // Only one thread can edit thread metadata at a time

// So DoBusyWork can trap into the scheduler when its time slice runs out
VOID EnterKernelMode(PTHREAD_CONTROLLER self);

// One distinct printable symbol per thread index so interleaving is visible in the output
// There are 62 symbols, but MAX_THREADS is 64 so collisions would only show up there.
static const char THREAD_SYMBOLS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

VOID DoBusyWork(PTHREAD_CONTROLLER self) {
    volatile ULONG64 count = 0;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    char symbol = THREAD_SYMBOLS[(self - thread_metadata_array) % (sizeof(THREAD_SYMBOLS) - 1)];

    while (count <= WORK_ITERATIONS) {
        count++;
        if (count % (WORK_ITERATIONS / 100) == 0) {
            printf("%c", symbol);
        }

        // Simulating what a real CPU does at trap points
        if (count % WORK_CHECK_INTERVAL == 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double running_ms = (double)(now.QuadPart - self->dispatch_time.QuadPart) * 1000.0 / freq.QuadPart;
            if ((ULONG64)(running_ms / TICK_MS) >= self->running_amount) {
                EnterKernelMode(self);
            }
        }
    }

    LARGE_INTEGER end_time;
    QueryPerformanceCounter(&end_time);
    double elapsed_ms = (double)(end_time.QuadPart - self->start_time.QuadPart) * 1000.0 / freq.QuadPart;
    printf("\nThread %d finished in %.3f ms\n", (int)(self - thread_metadata_array), elapsed_ms);
}

// Pass in a bitmask of which locks to acquire
// Ex. 0001 = lock1, 0010 = lock 2, 0001 | 0010 = 0011 = lock1 AND lock2
VOID AcquireLocks(PTHREAD_CONTROLLER self, ULONG lock_bit_mask) {
    self->pending_locks = lock_bit_mask;
    WaitForSingleObject(self->lock_grant_event, INFINITE);
}

// Works the same way as acquire
VOID ReleaseLocks(PTHREAD_CONTROLLER self, ULONG lock_bit_mask) {
    self->releasing_locks = lock_bit_mask;
}

// Holds lock1 (locks[0]) for the duration of its busy work.
DWORD WINAPI perform_work_A(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK1_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK1_BIT);

    return 0;
}

// Holds lock2 (locks[1]) for the duration of its busy work.
DWORD WINAPI perform_work_B(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK2_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK2_BIT);

    return 0;
}

// Holds both lock1 and lock2 for the duration of its busy work.
DWORD WINAPI perform_work_C(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK1_BIT | LOCK2_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK1_BIT | LOCK2_BIT);

    return 0;
}

// Holds lock3 for the duration of its busy work.
DWORD WINAPI perform_work_D(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK3_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK3_BIT);

    return 0;
}

// Holds lock4 for the duration of its busy work.
DWORD WINAPI perform_work_E(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK4_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK4_BIT);

    return 0;
}

// Holds both lock3 and lock4
DWORD WINAPI perform_work_F(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK3_BIT | LOCK4_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK3_BIT | LOCK4_BIT);

    return 0;
}

// Ready pool is an array of flink blink lists. There is one list per priority.
int FindHighestReadyPriority(void) {
    for (int i = NUM_PRIORITIES - 1; i >= 0; i--) {
        if (!IsListEmpty(&ready_pool[i].head)) {
            return i;
        }
    }
    return -1;
}

// Picks whoever should run next on `core`, and makes sure they're ACTUALLY eligible to: priority
// always wins first, but within the single highest non-empty bucket, a candidate already pinned
// to `core` is preferred over one that isn't, purely to avoid a pointless SetThreadAffinityMask
// reassignment when it doesn't change the outcome. If the winner isn't already on `core`, we
// retarget its affinity here and update its core field to match. Threads are never given a
// multi-core mask, just a reassignable single-core one, so we always know exactly which core a
// running thread occupies because we're the ones who just set it.
PTHREAD_CONTROLLER RemoveNextForCore(int core) {
    for (int i = NUM_PRIORITIES - 1; i >= 0; i--) {
        PLOCKED_LIST list = &ready_pool[i];
        EnterCriticalSection(&list->lock);
        if (IsListEmpty(&list->head)) {
            LeaveCriticalSection(&list->lock);
            continue;
        }

        // Prefer a same-core candidate within this top priority bucket; otherwise FIFO head.
        PLIST_ENTRY chosen = NULL;
        // Traverse the list
        for (PLIST_ENTRY e = list->head.Flink; e != &list->head; e = e->Flink) {
            PTHREAD_CONTROLLER candidate = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
            // If same core then choose this guy
            if (candidate->core == core) {
                chosen = e;
                break;
            }
        }
        // Else take first guy
        if (chosen == NULL) {
            chosen = list->head.Flink;
        }

        // Remove from list
        RemoveEntryList(chosen);
        list->count--;
        chosen->Flink = NULL;
        chosen->Blink = NULL;
        LeaveCriticalSection(&list->lock);

        // Update core for the chosen guy
        PTHREAD_CONTROLLER next = CONTAINING_RECORD(chosen, THREAD_CONTROLLER, entry);
        if (next->core != core) {
            next->core = core;
            SetThreadAffinityMask(next->handle, 1ULL << core);
        }
        return next;
    }
    return NULL;
}

// Higher-priority threads earn a longer slice before they have to trap back in and yield.
ULONG64 ComputeRunningAmount(PTHREAD_CONTROLLER thread) {
    return BASE_RUNNING_AMOUNT + (ULONG64)thread->current_priority * RUNNING_AMOUNT_STEP;
}

// Single place that stamps everything a freshly-scheduled thread needs including state, decay/aging
// counters, and the slice (running_amount + dispatch_time) it gets to run this time around.
VOID InitializeThread(PTHREAD_CONTROLLER next) {
    next->state = RUNNING;
    next->ticks_at_priority = 0;
    next->ticks_waiting = 0;
    next->running_amount = ComputeRunningAmount(next);
    QueryPerformanceCounter(&next->dispatch_time);
}

VOID Reschedule(void) {
    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_metadata_array[i];

        // Only possibly need to kick out the running guys
        if (curr_thread->state != RUNNING) {
            continue;
        }

        int highest = FindHighestReadyPriority();
        // Nobody ready anywhere so no running thread on any core could be preempted
        if (highest < 0) {
            break;
        }

        // Only a strictly higher priority candidate is worth preempting for
        if (curr_thread->current_priority < highest) {
            // Suspend and send to ready
            SuspendThread(curr_thread->handle);
            curr_thread->state = READY;
            LockedInsertTail(&ready_pool[curr_thread->current_priority], &curr_thread->entry);

            // Get the next guy to run on this same core
            PTHREAD_CONTROLLER next = RemoveNextForCore(curr_thread->core);
            InitializeThread(next);
            ResumeThread(next->handle);
        }
    }
}

// Ages every ready/waiting thread
// The longer one sits un-run, the higher its current_priority climbs, so it eventually catches up to and can preempt
// whatever's been hogging a core. Replaces the old periodic full-reset boost. This is continuous and per-thread instead
// of a global sweep back to base priorities.
VOID AgeWaiting(void) {
    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_metadata_array[i];
        // We don't want to age threads that are running or finished
        if (curr_thread->state != READY && curr_thread->state != WAITING) {
            continue;
        }

        // Increase waiting times
        curr_thread->ticks_waiting++;
        if (curr_thread->ticks_waiting < AGE_THRESHOLD) {
            continue;
        }
        curr_thread->ticks_waiting = 0;

        // If we are at highest priority we cannot increase it
        if (curr_thread->current_priority >= NUM_PRIORITIES - 1) {
            continue;
        }

        if (curr_thread->state == READY) {
            // Priority is changing, so it has to hop to the new bucket
            LockedTryRemoveEntry(&ready_pool[curr_thread->current_priority], &curr_thread->entry);
            curr_thread->current_priority++;
            LockedInsertTail(&ready_pool[curr_thread->current_priority], &curr_thread->entry);
        } else {
            // Waiting threads sit in one flat wait_list so we can just increase priority
            // ExitWait queues it into ready_pool at whatever this field says later
            curr_thread->current_priority++;
        }
    }
}

// Moves a thread onto wait_list
// Current_priority is left untouched, so it re-enters ready_pool at the same level once ExitWait runs.
// Since a run slot just freed up, hands it to whoever's next in line.
VOID EnterWaitAndResumeNext(PTHREAD_CONTROLLER curr_thread) {
    SuspendThread(curr_thread->handle);
    curr_thread->state = WAITING;
    LockedInsertTail(&wait_list, &curr_thread->entry);

    // Make sure we are resuming on the right core
    PTHREAD_CONTROLLER next = RemoveNextForCore(curr_thread->core);
    if (next != NULL) {
        InitializeThread(next);
        ResumeThread(next->handle);
    }
}

// Moves a thread off wait_list and back into ready, queued at whatever current_priority it already had.
VOID ExitWait(PTHREAD_CONTROLLER curr_thread) {
    curr_thread->state = READY;
    LockedInsertTail(&ready_pool[curr_thread->current_priority], &curr_thread->entry);
}

// Checks for wait_event. When it's signaled, drains everyone currently on wait_list and puts back to READY.
VOID CheckWaiting(void) {
    if (WaitForSingleObject(wait_event, 0) != WAIT_OBJECT_0) {
        return;
    }

    PLIST_ENTRY e;
    while ((e = LockedRemoveHead(&wait_list)) != NULL) {
        PTHREAD_CONTROLLER curr_thread = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
        ExitWait(curr_thread);
    }
}

// Without this, a thread stays marked running forever
VOID HandleFinished(void) {
    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_metadata_array[i];
        if (curr_thread->state != RUNNING) {
            continue;
        }

        // Still running
        if (WaitForSingleObject(curr_thread->handle, 0) != WAIT_OBJECT_0) {
            continue;
        }

        curr_thread->state = DONE;

        // Hand free slot to next person in line for this same core
        PTHREAD_CONTROLLER next = RemoveNextForCore(curr_thread->core);
        if (next != NULL) {
            InitializeThread(next);
            ResumeThread(next->handle);
        }
    }
}

// Walks through threads starting at `waiter` (about to block)
// Finds the lock that thread is waiting on -> finds the owner -> finds what lock that guy is waiting for -> repeat.
// Boosts every thread as it runs until it gets to someone not waiting
VOID BoostChain(PTHREAD_CONTROLLER waiter) {
    int required = waiter->current_priority;
    PTHREAD_CONTROLLER visited[MAX_THREADS];
    int visited_count = 0;
    PTHREAD_CONTROLLER curr = waiter;

    for (;;) {
        if (curr->waiting_on_lock == NULL) {
            break;
        }
        PTHREAD_CONTROLLER owner = curr->waiting_on_lock->owner;
        if (owner == NULL) {
            break;
        }

        for (int i = 0; i < visited_count; i++) {
            if (visited[i] == owner) {
                return; // A real deadlock between lock requests, nothing more to boost
            }
        }
        visited[visited_count++] = owner;

        if (owner->current_priority < required) {
            owner->current_priority = required;
            owner->canBeDecremented = 0;
        }

        curr = owner;
    }
}

// Tries to satisfy whatever locks a thread still needs, one at a time in ascending lock order.
// Grabs every immediately-free lock in the mask and the moment it hits a held one, it gives the
// owning thread its base priority then goes on the wait list for that lock. (a thread can only be linked
// into one waiter list at a time). The rest of pending_locks waits for a later release to resume this walk.
VOID TryAcquireNextPendingLock(PTHREAD_CONTROLLER curr_thread) {
    while (curr_thread->pending_locks != 0) {
        int curr_lock = 0;
        // 1u is 1 in 32 bits. 1u << curr_lock creates the bit mask for curr_lock
        // pending_locks & 1u << curr_lock checks to see if we have already accounted for the curr_lock in our mask
        while (!(curr_thread->pending_locks & (1u << curr_lock))) {
            curr_lock++;
        }

        // New lock to account for
        ULONG bit = 1u << curr_lock;

        // If the lock is free then take it and keep looking for new locks
        if (locks[curr_lock].owner == NULL) {
            locks[curr_lock].owner = curr_thread;
            curr_thread->owned_locks |= bit; // Put this in our mask
            curr_thread->pending_locks &= ~bit; // Make sure it is accounted for in pending
            continue;
        }

        // The lock is held so record that we are waiting and boost the whole chain behind it, not
        // just this lock's direct owner (see BoostChain)
        curr_thread->waiting_on_lock = &locks[curr_lock];
        BoostChain(curr_thread);

        // If we are running we do not want to take up a core as we wait for a lock
        if (curr_thread->state == RUNNING) {
            // Hand free spot to whoever is next in line
            SuspendThread(curr_thread->handle);
            curr_thread->state = WAITING;

            PTHREAD_CONTROLLER next = RemoveNextForCore(curr_thread->core);
            if (next != NULL) {
                InitializeThread(next);
                ResumeThread(next->handle);
            }
        }
        // Else we are already waiting so no need to do anything

        // Put ourselves on the wait list for this lock
        LockedInsertTail(&locks[curr_lock].waiters, &curr_thread->entry);
        return;
    }

    // Here pending_locks == 0 so we got everything this AcquireLocks() call asked for
    curr_thread->waiting_on_lock = NULL;
    if (curr_thread->state == WAITING) {
        curr_thread->state = READY;
        LockedInsertTail(&ready_pool[curr_thread->current_priority], &curr_thread->entry);
    }
    SetEvent(curr_thread->lock_grant_event);
}

// Pulls the highest current_priority waiter out of a lock's waiter list instead of the oldest
// one.
PLIST_ENTRY LockedRemoveHighestPriorityWaiter(PLOCKED_LIST list) {
    EnterCriticalSection(&list->lock);

    PLIST_ENTRY best = NULL;
    int best_priority = -1;
    // Winner algorithm or whatever it's called
    for (PLIST_ENTRY e = list->head.Flink; e != &list->head; e = e->Flink) {
        PTHREAD_CONTROLLER waiter = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
        if (waiter->current_priority > best_priority) {
            best_priority = waiter->current_priority;
            best = e;
        }
    }

    // Remove from list
    if (best != NULL) {
        RemoveEntryList(best);
        list->count--;
        best->Flink = NULL;
        best->Blink = NULL;
    }

    LeaveCriticalSection(&list->lock);
    return best;
}

// For a given thread, boost them if someone higher is waiting on a lock they own
VOID RecomputeOwnerPriority(PTHREAD_CONTROLLER thread) {
    int required = thread->base_priority;

    for (int i = 0; i < NUM_LOCKS; i++) {
        // If we do not own this lock go to next iteration
        if (!(thread->owned_locks & (1u << i))) {
            continue;
        }

        // Boost myself if someone higher is waiting on a lock I own
        LOCKED_LIST* waiters = &locks[i].waiters;
        EnterCriticalSection(&waiters->lock);
        // If someone of higher priority is waiting then we need to bump up required priority
        for (PLIST_ENTRY e = waiters->head.Flink; e != &waiters->head; e = e->Flink) {
            PTHREAD_CONTROLLER waiter = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
            if (waiter->current_priority > required) {
                required = waiter->current_priority;
            }
        }
        LeaveCriticalSection(&waiters->lock);
    }

    thread->current_priority = required;
    thread->canBeDecremented = (required == thread->base_priority);
}

// Releases locks that need to be released and then checks for new requests for locks using the func above
VOID ReleaseAndCheckLockRequests(void) {
    // Go through threads
    for (int i = 0; i < num_threads; i++) {
        // If we are not releasing locks then skip
        PTHREAD_CONTROLLER curr_thread = &thread_metadata_array[i];
        if (curr_thread->releasing_locks == 0) {
            continue;
        }

        for (int j = 0; j < NUM_LOCKS; j++) {
            ULONG bit = 1u << j; // The mask associated with this lock
            // If we are not releasing this lock then skip
            if (!(curr_thread->releasing_locks & bit)) {
                continue;
            }

            // Add this lock to the releasing locks mask and the owned locks mask
            curr_thread->releasing_locks &= ~bit;
            curr_thread->owned_locks &= ~bit;
            locks[j].owner = NULL;

            // Boosted if someone is waiting on a lock that curr thread owns
            RecomputeOwnerPriority(curr_thread);

            // Update metadata to grant to whoever's been boosted highest, not whoever waited longest
            PLIST_ENTRY e = LockedRemoveHighestPriorityWaiter(&locks[j].waiters);
            if (e != NULL) {
                PTHREAD_CONTROLLER waiter = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
                locks[j].owner = waiter;
                waiter->owned_locks |= bit;
                waiter->pending_locks &= ~bit;
                waiter->waiting_on_lock = NULL;
                TryAcquireNextPendingLock(waiter);
            }
        }
    }

    // Go through threads to check for if we need another lock to run
    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_metadata_array[i];
        if (curr_thread->state == RUNNING && curr_thread->pending_locks != 0) {
            TryAcquireNextPendingLock(curr_thread);
        }
    }
}

// Sweeps every physical core and fills any that are idle. This is a per-core check now instead
// of a global running_count-vs-num_cores target: a global count could hit num_cores while two
// threads are doubled up on one core and another core sits empty, since nothing pinned to that
// empty core happened to get picked. We can fill cores without relying on transitions like if a
// thread needs to be descheduled.
VOID FillIdleSlots(void) {
    BOOL core_occupied[MAX_CORES] = { FALSE };
    for (int i = 0; i < num_threads; i++) {
        if (thread_metadata_array[i].state == RUNNING) {
            core_occupied[thread_metadata_array[i].core] = TRUE;
        }
    }

    for (int i = 0; i < (int)num_cores; i++) {
        if (core_occupied[i]) {
            continue;
        }

        PTHREAD_CONTROLLER next = RemoveNextForCore(i);
        if (next != NULL) {
            InitializeThread(next);
            ResumeThread(next->handle);
        }
    }
}

// A worker thread's own trap into "kernel mode" that's called by itself once DoBusyWork notices its
// running_amount has elapsed. It does its own scheduling bookkeeping (requeue itself, pick who's
// next) the same way the ticker would, then performs the actual OS handoff itself via
// SuspendThread/ResumeThread
VOID EnterKernelMode(PTHREAD_CONTROLLER self) {
    // SuspendThread on yourself doesn't take effect synchronously so we can keep executing a
    // few more instructions past the call below before the OS actually kills us.
    if (self->state != RUNNING) {
        return;
    }

    EnterCriticalSection(&scheduler_lock);

    if (self->state != RUNNING) {
        LeaveCriticalSection(&scheduler_lock);
        return;
    }

    // Put ourselves on ready list
    self->state = READY;
    self->ticks_at_priority = 0;
    self->ticks_waiting = 0;
    LockedInsertTail(&ready_pool[self->current_priority], &self->entry);

    // Find next guy to run on THIS core
    // We're always a valid candidate for our own core (we just inserted ourselves above), so this can never come back NULL.
    PTHREAD_CONTROLLER next = RemoveNextForCore(self->core);
    InitializeThread(next);

    LeaveCriticalSection(&scheduler_lock);

    // Nobody else was runnable so we just continue
    if (next == self) {
        return;
    }

    // Real context switch, performed by the outgoing thread itself
    // Bring the next thread up, then take ourselves down. This only returns once some future dispatch elsewhere calls
    // ResumeThread(self->handle). InitializeThread on that path has already refreshed our
    // running_amount and dispatch_time before we're woken.
    ResumeThread(next->handle);

    // Re-check before actually suspending ourselves
    // It's possible for another core to have already dequeued and re-dispatched us (state flipped back to RUNNING)
    // in the window between us enqueuing ourselves and reaching this line. If that happened we'd kill ourselves
    // right after being  resumed, with nothing left to wake us up again.
    EnterCriticalSection(&scheduler_lock);
    BOOL still_yielding = (self->state != RUNNING);
    LeaveCriticalSection(&scheduler_lock);

    if (still_yielding) {
        SuspendThread(self->handle);
    }
}

VOID SchedulerTick(void) {
    EnterCriticalSection(&scheduler_lock);

    // Check to see if anyone is done
    HandleFinished();

    // Check to see if anyone doesn't need to be waiting
    CheckWaiting();

    // Check to see if anyone wants/is releasing a lock
    ReleaseAndCheckLockRequests();

    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_metadata_array[i];

        // Only check running guys
        if (curr_thread->state != RUNNING) {
            continue;
        }

        // Increase ticks
        curr_thread->ticks_at_priority++;

        // If we do not have to kick this guy off
        if (curr_thread->ticks_at_priority < SUSPEND_THRESHOLD) {
            continue;
        }

        // Cannot decrease special boost from auto boost
        if (curr_thread->canBeDecremented == 0) {
            continue;
        }

        // Decrement only boosts
        int old_priority = curr_thread->current_priority;
        int new_priority;

        if (old_priority > curr_thread->base_priority) {
            new_priority = old_priority - 1;
        }
        else {
            new_priority = curr_thread->base_priority;
            continue;
        }

        // Still running
        curr_thread->current_priority = new_priority;
        curr_thread->ticks_at_priority = 0;
    }

    // Long-waiting ready/waiting threads earn a priority bump here, before Reschedule decides who should be running
    AgeWaiting();

    Reschedule();

    // HandleFinished/Reschedule/CheckLockRequests only ever hand a slot to a
    // ready thread as a side effect of some OTHER thread leaving running. If a thread
    // becomes ready (got a lock) at a moment when nothing is currently running,
    // none of those have anything to react to and the slot sits idle forever. This checks
    // the actual state of who is running and how many on ready directly instead of only
    // reacting to specific transitions.
    FillIdleSlots();

    LeaveCriticalSection(&scheduler_lock);
}

DWORD WINAPI ClockTickSimulator(LPVOID lpParameter) {
    // A timer you can treat like an event
    HANDLE timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    LARGE_INTEGER due = {.QuadPart = -TICK_MS * 10000LL}; // 100ns units, negative = relative
    SetWaitableTimer(timer, &due, TICK_MS, NULL, NULL, FALSE);

    for (;;) {
        WaitForSingleObject(timer, INFINITE);   // sleeps until next tick, no busy-wait
        SchedulerTick();
    }
}

int main(void) {
    // num_cores has to be known before we can decide how many threads to make because we want at
    // least 2 threads per core so every core is actually shared and EnterKernelMode's voluntary
    // yield has somebody to hand off to everywhere, not just on the handful of cores that
    // happened to get doubled up by leftover wraparound.
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    num_cores = sys_info.dwNumberOfProcessors;
    num_threads = (int)min(2 * num_cores, (DWORD)MAX_THREADS);

    InitializeCriticalSectionAndSpinCount(&scheduler_lock, 4000);

    // Set up the lists
    InitializeLockedList(&wait_list);
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        InitializeLockedList(&ready_pool[i]);
    }

    // Set up lock metadata
    for (int i = 0; i < NUM_LOCKS; i++) {
        locks[i].owner = NULL;
        InitializeLockedList(&locks[i].waiters);
    }

    // Round-robin across six worker functions, split into two independent contention pairs
    // (lock1/lock2 and lock3/lock4). Threads on different pairs never touch each other's locks,
    // so they can be productive at the same time
    LPTHREAD_START_ROUTINE worker_functions[] = {
        perform_work_A, perform_work_B, perform_work_C,
        perform_work_D, perform_work_E, perform_work_F,
    };

    for (int i = 0; i < num_threads; i++) {
        LPTHREAD_START_ROUTINE worker_function = worker_functions[i % 6];

        // Create suspended so we can fill in the thread controls and assign a CPU
        threads[i] = CreateThread(NULL, 0, worker_function, &thread_metadata_array[i], CREATE_SUSPENDED, NULL);
        SetThreadPriority(threads[i], 16 + i);

        QueryPerformanceCounter(&thread_metadata_array[i].start_time);

        thread_metadata_array[i].handle = threads[i];
        thread_metadata_array[i].entry.Flink = NULL;
        thread_metadata_array[i].entry.Blink = NULL;
        // Wrapped so this stays a valid ready_pool[] index no matter how large
        // num_threads gets. ready_pool only has NUM_PRIORITIES valid slots.
        thread_metadata_array[i].base_priority = (i + 16) % NUM_PRIORITIES;
        thread_metadata_array[i].current_priority = (i + 16) % NUM_PRIORITIES;
        thread_metadata_array[i].ticks_at_priority = 0;
        thread_metadata_array[i].ticks_waiting = 0;
        thread_metadata_array[i].canBeDecremented = 1;
        thread_metadata_array[i].owned_locks = 0;
        thread_metadata_array[i].pending_locks = 0;
        thread_metadata_array[i].releasing_locks = 0;
        thread_metadata_array[i].lock_grant_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        thread_metadata_array[i].waiting_on_lock = NULL;

        // Assign the processor
        // 1ULL equals 1 but with 64 bits.
        // 1ULL << i % num_cores gives you the bit mask for whichever core you will run on
        thread_metadata_array[i].core = (int)(i % num_cores);
        SetThreadAffinityMask(threads[i], 1ULL << (i % num_cores));

        if ((DWORD)i < num_cores) {
            // One running slot per core so executing immediately
            InitializeThread(&thread_metadata_array[i]);
            ResumeThread(threads[i]);
        } else {
            // More threads than cores so the rest wait their turn in the ready queues
            thread_metadata_array[i].state = READY;
            LockedInsertTail(&ready_pool[thread_metadata_array[i].current_priority], &thread_metadata_array[i].entry);
        }
    }

    wait_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, ClockTickSimulator, NULL, 0, NULL);

    LARGE_INTEGER frequency, start_time, end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);

    WaitForMultipleObjects(num_threads, &threads[0], TRUE, INFINITE);

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;
    printf("Finished in %f", elapsed_ms);

    CloseHandle(wait_event);
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <windows.h>
#include "ThreadHandler.h"

#define MB(x) (((ULONG64)(x)) * 1024 * 1024)
#define KB(x) ((ULONG64)(x) * 1024)
#define GB(x) (MB(x) * 1024)


int num_threads;
DWORD num_cores;
HANDLE threads[MAX_THREADS];
THREAD_CONTROLLER thread_controls[MAX_THREADS];
LOCKED_LIST wait_list;
LOCKED_LIST ready_pool[NUM_PRIORITIES];
HANDLE wait_event;
SIM_LOCK locks[NUM_LOCKS];

VOID DoBusyWork(PTHREAD_CONTROLLER self) {
    volatile ULONG64 count = 0;
    while (count <= GB(1)) {
        count++;
        if (count % (GB(1) / 100) == 0) {
            printf(".");
        }
    }

    LARGE_INTEGER end_time, freq;
    QueryPerformanceCounter(&end_time);
    QueryPerformanceFrequency(&freq);
    double elapsed_ms = (double)(end_time.QuadPart - self->start_time.QuadPart) * 1000.0 / freq.QuadPart;
    printf("\nThread %d finished in %.3f ms\n", (int)(self - thread_controls), elapsed_ms);
}

// Pass in a bitmask of which locks to aquire
// Ex. 0001 = lock1, 0010 = lock 2, 0001 | 0010 = 0011 = lock1 AND lock2
VOID AcquireLocks(PTHREAD_CONTROLLER self, ULONG lock_bit_mask) {
    self->pending_locks = lock_bit_mask;
    WaitForSingleObject(self->lock_grant_event, INFINITE);
}

// Works the same way as aquire
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

// Ready pool is an array of flink blink lists. There is one list per priority.
int FindHighestReadyPriority(void) {
    for (int i = NUM_PRIORITIES - 1; i >= 0; i--) {
        if (!IsListEmpty(&ready_pool[i].head)) {
            return i;
        }
    }
    return -1;
}

VOID Reschedule(void) {
    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_controls[i];

        // Only possibly need to kick out the running guys
        if (curr_thread->state != RUNNING) {
            continue;
        }

        int highest = FindHighestReadyPriority();
        // This guy is still the highest
        if (highest < 0) {
            break;
        }

        // If we aren't the highest priority
        if (curr_thread->current_priority < highest) {
            // Suspend and send to ready
            SuspendThread(curr_thread->handle);
            curr_thread->state = READY;
            LockedInsertTail(&ready_pool[curr_thread->current_priority], &curr_thread->entry);

            // Get the next guy to run
            PLIST_ENTRY e = LockedRemoveHead(&ready_pool[highest]);
            // Walk back up from the entry to get to the address of the entire struct
            PTHREAD_CONTROLLER next = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
            next->state = RUNNING;
            next->ticks_at_priority = 0;
            next->ticks_waiting = 0;
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
        PTHREAD_CONTROLLER curr_thread = &thread_controls[i];
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
VOID EnterWait(PTHREAD_CONTROLLER curr_thread) {
    SuspendThread(curr_thread->handle);
    curr_thread->state = WAITING;
    LockedInsertTail(&wait_list, &curr_thread->entry);

    int highest = FindHighestReadyPriority();
    if (highest >= 0) {
        PLIST_ENTRY e = LockedRemoveHead(&ready_pool[highest]);
        PTHREAD_CONTROLLER next = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
        next->state = RUNNING;
        next->ticks_at_priority = 0;
        next->ticks_waiting = 0;
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
        PTHREAD_CONTROLLER curr_thread = &thread_controls[i];
        if (curr_thread->state != RUNNING) {
            continue;
        }

        // Still running
        if (WaitForSingleObject(curr_thread->handle, 0) != WAIT_OBJECT_0) {
            continue;
        }

        curr_thread->state = DONE;

        // Hand free slot to next person in line
        int highest = FindHighestReadyPriority();
        if (highest >= 0) {
            PLIST_ENTRY e = LockedRemoveHead(&ready_pool[highest]);
            PTHREAD_CONTROLLER next = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
            next->state = RUNNING;
            next->ticks_at_priority = 0;
            next->ticks_waiting = 0;
            ResumeThread(next->handle);
        }
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

        // The lock is held so boost the owner of the lock to our base priority if we are higher
        PTHREAD_CONTROLLER owner = locks[curr_lock].owner;
        if (curr_thread->base_priority > owner->current_priority) {
            owner->current_priority = curr_thread->base_priority;
            owner->canBeDecremented = 0;
        }

        // If we are running we do not want to take up a core as we wait for a lock
        if (curr_thread->state == RUNNING) {
            // Hand free spot to whoever is next in line
            SuspendThread(curr_thread->handle);
            curr_thread->state = WAITING;

            int highest = FindHighestReadyPriority();
            if (highest >= 0) {
                PLIST_ENTRY e = LockedRemoveHead(&ready_pool[highest]);
                PTHREAD_CONTROLLER next = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
                next->state = RUNNING;
                next->ticks_at_priority = 0;
                next->ticks_waiting = 0;
                ResumeThread(next->handle);
            }
        }
        // Else we are already waiting so no need to do anything

        // Put ourselves on the wait list for this lock
        LockedInsertTail(&locks[curr_lock].waiters, &curr_thread->entry);
        return;
    }

    // Here pending_locks == 0 so we got everything this AcquireLocks() call asked for
    if (curr_thread->state == WAITING) {
        curr_thread->state = READY;
        LockedInsertTail(&ready_pool[curr_thread->current_priority], &curr_thread->entry);
    }
    SetEvent(curr_thread->lock_grant_event);
}

// Releases locks that need to be released and then checks for new requests for locks using the func above
VOID ReleaseAndCheckLockRequests(void) {
    // Go through threads
    for (int i = 0; i < num_threads; i++) {
        // If we are not releasing locks then skip
        PTHREAD_CONTROLLER curr_thread = &thread_controls[i];
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

            // Simple version for now where we set our priority to base priority when we release a lock
            // A resource-graph version will reset to whatever priority is needed for another lock we may be waiting for
            curr_thread->current_priority = curr_thread->base_priority;
            curr_thread->canBeDecremented = 1;

            // Update metadata
            PLIST_ENTRY e = LockedRemoveHead(&locks[j].waiters);
            if (e != NULL) {
                PTHREAD_CONTROLLER waiter = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
                locks[j].owner = waiter;
                waiter->owned_locks |= bit;
                waiter->pending_locks &= ~bit;
                TryAcquireNextPendingLock(waiter);
            }
        }
    }

    // Go through threads to check for if we need another lock to run
    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_controls[i];
        if (curr_thread->state == RUNNING && curr_thread->pending_locks != 0) {
            TryAcquireNextPendingLock(curr_thread);
        }
    }
}

// Dispatches highest-priority ready threads until every core is occupied or ready_pool runs dry.
// We can fill cores without relying on transitions like if a thread needs to be descheduled
VOID FillIdleSlots(void) {
    int running_count = 0;
    for (int i = 0; i < num_threads; i++) {
        if (thread_controls[i].state == RUNNING) {
            running_count++;
        }
    }

    // If we can schedule someone
    while ((DWORD)running_count < num_cores) {
        // Schedule the highest priority
        int highest = FindHighestReadyPriority();
        if (highest < 0) {
            break;
        }

        PLIST_ENTRY e = LockedRemoveHead(&ready_pool[highest]);
        PTHREAD_CONTROLLER next = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
        next->state = RUNNING;
        next->ticks_at_priority = 0;
        next->ticks_waiting = 0;
        ResumeThread(next->handle);
        running_count++;
    }
}

VOID SchedulerTick(void) {
    // Check to see if anyone is done
    HandleFinished();

    // Check to see if anyone doesn't need to be waiting
    CheckWaiting();

    // Check to see if anyone wants/is releasing a lock
    ReleaseAndCheckLockRequests();

    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_controls[i];

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

        // Pinned by lock priority inheritance -- don't erode the borrowed priority
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
    num_threads = 16;

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

    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    num_cores = sys_info.dwNumberOfProcessors;

    for (int i = 0; i < num_threads; i++) {
        // Even threads want lock1 and use worker function A, odd threads want lock2 and use worker function B
        int worker_function_decider = i % 2;
        LPTHREAD_START_ROUTINE worker_function;
        if (worker_function_decider == 0) {
             worker_function = perform_work_A;
        }
        else {
             worker_function = perform_work_B;
        }

        // Create suspended so we can fill in the thread controls and assign a CPU
        threads[i] = CreateThread(NULL, 0, worker_function, &thread_controls[i], CREATE_SUSPENDED, NULL);
        SetThreadPriority(threads[i], 16 + i);

        QueryPerformanceCounter(&thread_controls[i].start_time);

        thread_controls[i].handle = threads[i];
        thread_controls[i].entry.Flink = NULL;
        thread_controls[i].entry.Blink = NULL;
        // Wrapped so this stays a valid ready_pool[] index no matter how large
        // num_threads gets -- ready_pool only has NUM_PRIORITIES valid slots.
        thread_controls[i].base_priority = (i + 16) % NUM_PRIORITIES;
        thread_controls[i].current_priority = (i + 16) % NUM_PRIORITIES;
        thread_controls[i].ticks_at_priority = 0;
        thread_controls[i].ticks_waiting = 0;
        thread_controls[i].canBeDecremented = 1;
        thread_controls[i].owned_locks = 0;
        thread_controls[i].pending_locks = 0;
        thread_controls[i].releasing_locks = 0;
        thread_controls[i].lock_grant_event = CreateEvent(NULL, FALSE, FALSE, NULL);

        // Assign the processor
        // 1ULL equals 1 but with 64 bits.
        // 1ULL << i % num_cores gives you the bit mask for whichever core you will run on
        SetThreadAffinityMask(threads[i], 1ULL << (i % num_cores));

        if ((DWORD)i < num_cores) {
            // One running slot per core so executing immediately
            thread_controls[i].state = RUNNING;
            ResumeThread(threads[i]);
        } else {
            // More threads than cores so the rest wait their turn in the ready queues
            thread_controls[i].state = READY;
            LockedInsertTail(&ready_pool[thread_controls[i].current_priority], &thread_controls[i].entry);
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
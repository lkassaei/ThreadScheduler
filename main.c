#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <windows.h>
#include "ThreadHandler.h"

#define MB(x) (((ULONG64)(x)) * 1024 * 1024)
#define KB(x) ((ULONG64)(x) * 1024)
#define GB(x) (MB(x) * 1024)

// Locks are held for the whole busy-work call, so this is really "how long does each lock hold
// last"
// Shrinking it is what makes lock hand-offs (and therefore visible thread swaps) happen
// more often.
#define WORK_ITERATIONS (GB(1) / 1)

int num_threads;
DWORD num_cores;
HANDLE threads[MAX_THREADS];
THREAD_CONTROLLER thread_metadata_array[MAX_THREADS];
LOCKED_LIST ready_pool[MAX_CORES][NUM_PRIORITIES]; // Ready pools per core
HANDLE wait_event;
SIM_LOCK locks[NUM_LOCKS];

// One distinct printable symbol per thread index so interleaving is visible in the output.
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
    }

    LARGE_INTEGER end_time;
    QueryPerformanceCounter(&end_time);
    double elapsed_ms = (double)(end_time.QuadPart - self->start_time.QuadPart) * 1000.0 / freq.QuadPart;
    printf("\nThread %d finished in %.3f ms\n", (int)(self - thread_metadata_array), elapsed_ms);
}

// Sets pending_locks and wakes this thread's own core scheduler, which is the only thing that
// ever touches locks[] now. Blocking here is a real, synchronous OS wait. There is no race, since the
// worker isn't the one suspending itself.
VOID AcquireLocks(PTHREAD_CONTROLLER self, ULONG lock_bit_mask) {
    self->pending_locks = lock_bit_mask;
    SetEvent(self->notify_event); // Tell the per-core scheduler we are waiting
    WaitForSingleObject(self->lock_grant_event, INFINITE);
}

// Fire and forget. The scheduler picks this up the next time it wakes for this thread.
VOID ReleaseLocks(PTHREAD_CONTROLLER self, ULONG lock_bit_mask) {
    self->releasing_locks = lock_bit_mask;
    SetEvent(self->notify_event);
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

// Holds both lock1 and lock2 for the duration of its busy work. This is what actually exercises
// BoostChain's multi-hop walk: this thread can be sitting mid-chain (owns lock1, blocked on
// lock2 held by someone else), so a thread that only wants lock1 has to boost through it.
DWORD WINAPI perform_work_C(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK1_BIT | LOCK2_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK1_BIT | LOCK2_BIT);

    return 0;
}

// Holds lock3 (locks[2]) for the duration of its busy work. Lock3/lock4 are a second,
// independent contention pair. A thread here can run at the same time as an A/B/C thread
// since they never touch each other's locks.
DWORD WINAPI perform_work_D(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK3_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK3_BIT);

    return 0;
}

// Holds lock4 (locks[3]) for the duration of its busy work.
DWORD WINAPI perform_work_E(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK4_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK4_BIT);

    return 0;
}

// Holds both lock3 and lock4.
DWORD WINAPI perform_work_F(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
    AcquireLocks(self, LOCK3_BIT | LOCK4_BIT);
    DoBusyWork(self);
    ReleaseLocks(self, LOCK3_BIT | LOCK4_BIT);

    return 0;
}

// Higher-priority threads earn a longer quantum before their scheduler cuts them off.
ULONG64 ComputeRunningAmount(PTHREAD_CONTROLLER thread) {
    return BASE_RUNNING_AMOUNT + (ULONG64)thread->current_priority * RUNNING_AMOUNT_STEP;
}

// Insert onto the ready list of a given core
VOID InsertReady(int core, PTHREAD_CONTROLLER thread) {
    thread->state = READY;
    LockedInsertTail(&ready_pool[core][thread->current_priority], &thread->entry);
}

// Picks who a core's scheduler should run next. Checks that core's own ready buckets first (highest priority down).
// Only if its own list is completely empty does it poach the globally highest-priority ready thread from another
// core's list, retargeting its affinity to the new core and updating its `core` field to match.
// Threads are never given a multi-core mask, just a reassignable single-core one,
// so we always know exactly which core a running thread occupies.
PTHREAD_CONTROLLER PickNextForCore(int core) {
    // Iterate through highest priorities down
    for (int i = NUM_PRIORITIES - 1; i >= 0; i--) {
        PLIST_ENTRY e = LockedRemoveHead(&ready_pool[core][i]);
        if (e != NULL) {
            return CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
        }
    }

    int best_priority = -1;
    int best_core = -1;
    for (int i = 0; i < (int)num_cores; i++) {
        // We already know our core is empty
        if (i == core) {
            continue;
        }
        // Get highest priority from next core until we have a candidate
        for (int j = NUM_PRIORITIES - 1; j > best_priority; j--) {
            if (!IsListEmpty(&ready_pool[i][j].head)) {
                best_priority = j;
                best_core = i;
                break;
            }
        }
    }

    // No options
    if (best_core < 0) {
        return NULL;
    }


    PLIST_ENTRY e = LockedRemoveHead(&ready_pool[best_core][best_priority]);
    if (e == NULL) {
        return NULL; // lost the race to another poacher so caller just retries
    }

    // Set the core metadata and affinity mask to match new core
    PTHREAD_CONTROLLER next = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
    next->core = core;
    SetThreadAffinityMask(next->handle, 1ULL << core);
    return next;
}

// "Self Boost" myself before I give up the CPU if I am holding a resource
VOID RecomputeOwnerPriority(PTHREAD_CONTROLLER thread) {
    int required = thread->base_priority;

    // Go through all locks
    for (int i = 0; i < NUM_LOCKS; i++) {
        // If I don't own it then go to next lock
        if (!(thread->owned_locks & (1u << i))) {
            continue;
        }

        // Get a list of waiters for my lock
        LOCKED_LIST* waiters = &locks[i].waiters;
        AcquireSRWLockExclusive(&waiters->lock);
        // Traverse waiters list
        for (PLIST_ENTRY e = waiters->head.Flink; e != &waiters->head; e = e->Flink) {
            // If someone waiting is higher, I need to boost myself
            PTHREAD_CONTROLLER waiter = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
            if (waiter->current_priority > required) {
                required = waiter->current_priority;
            }
        }
        ReleaseSRWLockExclusive(&waiters->lock);
    }

    // Set my metadata
    thread->current_priority = required;
    thread->canBeDecremented = (required == thread->base_priority);
}

// A waiter goes to the lock it wants -> checks owner thread -> goes to what the owner is waiting on -> goes to that lock -> repeat
VOID BoostChain(PTHREAD_CONTROLLER waiter) {
    int required = waiter->current_priority;
    PTHREAD_CONTROLLER visited[MAX_THREADS];
    int visited_count = 0;
    PTHREAD_CONTROLLER curr = waiter;

    // Walk the metadata
    for (;;) {
        // Get lock I am waiting for
        PSIM_LOCK lock = curr->waiting_on_lock;
        if (lock == NULL) {
            break;
        }

        // Get the owner
        AcquireSRWLockExclusive(&lock->owner_lock);
        PTHREAD_CONTROLLER owner = lock->owner;
        ReleaseSRWLockExclusive(&lock->owner_lock);

        if (owner == NULL) {
            break;
        }


        for (int i = 0; i < visited_count; i++) {
            if (visited[i] == owner) {
                return; // Loop means a real deadlock between lock requests, nothing more to boost
            }
        }

        // Mark that we saw this thread
        visited[visited_count++] = owner;

        // Boost threads lower than me that have the resource I want
        if (owner->current_priority < required) {
            owner->current_priority = required;
            owner->canBeDecremented = 0;
        }

        // Walk down
        curr = owner;
    }
}

// Pulls the highest current_priority waiter out of a lock's waiter list instead of the oldest
// one.
PLIST_ENTRY LockedRemoveHighestPriorityWaiter(PLOCKED_LIST list) {
    // Lock the wait list for this lock
    AcquireSRWLockExclusive(&list->lock);

    // Find the best candidate
    PLIST_ENTRY best = NULL;
    int best_priority = -1;
    // Iterate through the wait list
    for (PLIST_ENTRY e = list->head.Flink; e != &list->head; e = e->Flink) {
        PTHREAD_CONTROLLER waiter = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
        // Keep updating best until we fully iterate
        if (waiter->current_priority > best_priority) {
            best_priority = waiter->current_priority;
            best = e;
        }
    }

    // Remove the best candidate from the wait list
    if (best != NULL) {
        RemoveEntryList(best);
        list->count--;
        best->Flink = NULL;
        best->Blink = NULL;
    }

    ReleaseSRWLockExclusive(&list->lock);
    return best;
}

// Tries to satisfy whatever locks `thread` still needs, one at a time in ascending lock order.
// Grabs every immediately-free lock in the mask.
// The moment it hits a held one, it records that its waiting, boosts itself (RecomputeOwnerPriority) if it is blocking someone,
// and boosts whoever it now depends on (BoostChain) from the lock it is waiting on, and enqueues on that lock's waiters.
BOOL TryAcquirePendingLocks(PTHREAD_CONTROLLER thread) {
    // Go through the locks we are waiting on
    while (thread->pending_locks != 0) {
        int curr_lock = 0;
        // If we do not own this
        while (!(thread->pending_locks & (1u << curr_lock))) {
            curr_lock++;
        }
        ULONG bit = 1u << curr_lock;
        PSIM_LOCK lock = &locks[curr_lock];

        // Get the lock over the metadata for the lock we want
        AcquireSRWLockExclusive(&lock->owner_lock);
        if (lock->owner == NULL) {
            // Grab it if its free
            lock->owner = thread;
            ReleaseSRWLockExclusive(&lock->owner_lock);
            thread->owned_locks |= bit;
            thread->pending_locks &= ~bit;
            continue;
        }
        ReleaseSRWLockExclusive(&lock->owner_lock);

        // The lock is held so record that we are waiting and boost both directions before actually blocking
        thread->waiting_on_lock = lock;
        RecomputeOwnerPriority(thread); // Self boost: Boosts based on if higher priority threads are waiting for the locks I own
        BoostChain(thread); // Boosts whoever currently owns the locks I need
        thread->state = WAITING;
        LockedInsertTail(&lock->waiters, &thread->entry);
        return FALSE;
    }

    thread->waiting_on_lock = NULL;
    return TRUE;
}

// Releases whatever locks `thread` has flagged in releasing_locks, and grants each one to its
// highest-priority waiter (if any). Only ever called by that thread's own core scheduler.
VOID ProcessRelease(PTHREAD_CONTROLLER thread) {
    // Go through locks
    for (int i = 0; i < NUM_LOCKS; i++) {
        ULONG bit = 1u << i;
        // If we are not releasing this than go to next
        if (!(thread->releasing_locks & bit)) {
            continue;
        }
        // Mark that we are not in the process of releasing this and we do not own
        thread->releasing_locks &= ~bit;
        thread->owned_locks &= ~bit;

        // Make sure lock owner is updated to none
        PSIM_LOCK lock = &locks[i];
        AcquireSRWLockExclusive(&lock->owner_lock);
        lock->owner = NULL;
        ReleaseSRWLockExclusive(&lock->owner_lock);

        // Recompute rather than drop straight to base because if we still own another lock,
        // a waiter on THAT lock still needs it boosted until that lock is released too.
        RecomputeOwnerPriority(thread);

        // Find new guy to own this lock
        PLIST_ENTRY e = LockedRemoveHighestPriorityWaiter(&lock->waiters);
        if (e == NULL) {
            continue;
        }

        // Update lock owner to this new guy
        PTHREAD_CONTROLLER new_owner = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
        AcquireSRWLockExclusive(&lock->owner_lock);
        lock->owner = new_owner;
        ReleaseSRWLockExclusive(&lock->owner_lock);
        new_owner->owned_locks |= bit;
        new_owner->pending_locks &= ~bit;
        new_owner->waiting_on_lock = NULL;

        // new_owner might still need MORE locks.
        // If TryAcquirePendingLocks can't finish satisfying it, it has
        // already re-enqueued it as a waiter on the next lock, so there's nothing more to do
        // here. If it DID finish, new_owner was never actually dispatched while waiting (it's
        // been suspended this whole time), so just make it READY. Its own core's scheduler
        // will pick it up and resume it on its own next cycle.
        if (TryAcquirePendingLocks(new_owner)) {
            SetEvent(new_owner->lock_grant_event);
            InsertReady(new_owner->core, new_owner);
        }
    }
}

// Every tick, every READY/WAITING thread's ticks_waiting climbs. When it reaches past AGE_THRESHOLD it earns a priority bump.
// READY threads hop to a new bucket on their own core.
// WAITING threads (lock waiters) just bump current_priority in place.
DWORD WINAPI AgingThread(LPVOID lpParameter) {
    // Timer that functions as an event (like the clock tick)
    HANDLE timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due = {.QuadPart = -TICK_MS * 10000LL};
    SetWaitableTimer(timer, &due, TICK_MS, NULL, NULL, FALSE);

    for (;;) {
        WaitForSingleObject(timer, INFINITE);

        // Go through cores
        for (int i = 0; i < (int)num_cores; i++) {
            // Go from highest priority to lowest
            for (int j = 0; j < NUM_PRIORITIES - 1; j++) {
                PLOCKED_LIST list = &ready_pool[i][j];
                // Enter lock
                AcquireSRWLockExclusive(&list->lock);
                PLIST_ENTRY e = list->head.Flink;
                // Go through ready list
                while (e != &list->head) {
                    PLIST_ENTRY next_e = e->Flink;
                    PTHREAD_CONTROLLER thread = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
                    thread->ticks_waiting++;
                    // Boost
                    if (thread->ticks_waiting >= AGE_THRESHOLD) {
                        thread->ticks_waiting = 0;
                        // Remove from old bucket in list
                        RemoveEntryList(e);
                        list->count--;
                        thread->current_priority++;

                        // Put the new bucket's lock instead of releasing list->lock. This is
                        // the only thread that ever double-locks two buckets, so there's no
                        // ordering conflict to deadlock on, and it keeps next_e valid the whole
                        // time (nobody else can touch this bucket while we hold its lock).
                        PLOCKED_LIST new_list = &ready_pool[i][j + 1];
                        AcquireSRWLockExclusive(&new_list->lock);
                        InsertTailList(&new_list->head, e);
                        new_list->count++;
                        ReleaseSRWLockExclusive(&new_list->lock);
                    }
                    e = next_e;
                }
                ReleaseSRWLockExclusive(&list->lock);
            }
        }

        // Go through locks
        for (int j = 0; j < NUM_LOCKS; j++) {
            // List of waiters for given lock
            PLOCKED_LIST waiters = &locks[j].waiters;
            AcquireSRWLockExclusive(&waiters->lock);
            // Go through list
            for (PLIST_ENTRY e = waiters->head.Flink; e != &waiters->head; e = e->Flink) {
                PTHREAD_CONTROLLER t = CONTAINING_RECORD(e, THREAD_CONTROLLER, entry);
                t->ticks_waiting++;
                // Increase boost if they have waited long enough
                if (t->ticks_waiting >= AGE_THRESHOLD && t->current_priority < NUM_PRIORITIES - 1) {
                    t->ticks_waiting = 0;
                    t->current_priority++;
                }
            }
            ReleaseSRWLockExclusive(&waiters->lock);
        }
    }
}

// One dedicated scheduler thread per physical core. Pinned to its core, elevated to
// THREAD_PRIORITY_TIME_CRITICAL so it always trumps the normal-priority worker it shares that
// core with the instant it's signaled. This is the ONLY thing that ever calls SuspendThread or
// ResumeThread.
DWORD WINAPI CoreScheduler(LPVOID lpParameter) {
    // Get the core and set the affinity mask
    int core = (int)(ULONG_PTR)lpParameter;

    SetThreadAffinityMask(GetCurrentThread(), 1ULL << core);
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        printf("Warning: scheduler for core %d failed to elevate priority (error %lu)\n", core, GetLastError());
    }

    // Timer to schedule threads on and off
    HANDLE quantum_timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    for (;;) {
        // Get next candidate
        PTHREAD_CONTROLLER next = PickNextForCore(core);
        if (next == NULL) {
            Sleep(1); // Nothing ready anywhere for this core right now so brief retry
            continue;
        }

        // Get him running
        next->state = RUNNING;
        ResumeThread(next->handle);

        // We come here when it actually stops running (finishes, blocks, or times out).
        // A lock grant loops back here instead of falling through to picking someone new.
        for (;;) {
            next->running_amount = ComputeRunningAmount(next);
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(next->running_amount * TICK_MS) * 10000LL;
            SetWaitableTimer(quantum_timer, &due, 0, NULL, NULL, FALSE);

            HANDLE wait_handles[3] = { next->handle, next->notify_event, quantum_timer };
            DWORD result = WaitForMultipleObjects(3, wait_handles, FALSE, INFINITE);

            if (result == WAIT_OBJECT_0) {
                // Thread handle signaled
                if (next->releasing_locks != 0) {
                    ProcessRelease(next);
                }
                next->state = DONE;
                break;
            }

            if (result == WAIT_OBJECT_0 + 1) {
                // Notify_event (an acquire and/or release request)
                if (next->releasing_locks != 0) {
                    ProcessRelease(next);
                }
                if (next->pending_locks != 0) {
                    if (TryAcquirePendingLocks(next)) {
                        SetEvent(next->lock_grant_event);
                        continue; // Same thread, fresh quantum
                    }
                    SuspendThread(next->handle);
                    break;
                }
                continue; // Pure release with nothing pending so same thread keeps going
            }

            // Quantum timer expired
            RecomputeOwnerPriority(next); // Self boost after quantum
            InsertReady(core, next);
            SuspendThread(next->handle);
            break;
        }
    }
}

int main(void) {
    // Levels 16-31 are the real-time priority range. Reaching that range at all requires the PROCESS to be real-time class first.
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        printf("Warning: failed to set REALTIME_PRIORITY_CLASS (error %lu) -- scheduler priority elevation may not hold\n", GetLastError());
    }

    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    num_cores = sys_info.dwNumberOfProcessors;

    num_threads = (int)min(2 * num_cores, (DWORD)MAX_THREADS);

    for (int i = 0; i < (int)num_cores; i++) {
        for (int j = 0; j < NUM_PRIORITIES; j++) {
            InitializeLockedList(&ready_pool[i][j]);
        }
    }

    for (int i = 0; i < NUM_LOCKS; i++) {
        locks[i].owner = NULL;
        InitializeSRWLock(&locks[i].owner_lock);
        InitializeLockedList(&locks[i].waiters);
    }

    // Round-robin across six worker functions, split into two independent contention pairs
    // (lock1/lock2 and lock3/lock4). Threads on different pairs never touch each other's locks,
    // so they can be productive at the same time.
    LPTHREAD_START_ROUTINE worker_functions[] = {
        perform_work_A, perform_work_B, perform_work_C,
        perform_work_D, perform_work_E, perform_work_F,
    };

    for (int i = 0; i < num_threads; i++) {
        LPTHREAD_START_ROUTINE worker_function = worker_functions[i % 6];

        // Create suspended so we can fill in the thread controls and assign a CPU
        threads[i] = CreateThread(NULL, 0, worker_function, &thread_metadata_array[i], CREATE_SUSPENDED, NULL);

        // Real OS priority no longer needs to vary among workers. Our own current_priority
        // field is what drives simulated dispatch order. The only thing real priority needs to
        // guarantee now is that a core's scheduler always outranks the worker it's managing,
        // which THREAD_PRIORITY_TIME_CRITICAL on the scheduler side already covers.
        if (!SetThreadPriority(threads[i], THREAD_PRIORITY_NORMAL)) {
            printf("Warning: SetThreadPriority failed for thread %d (error %lu)\n", i, GetLastError());
        }

        QueryPerformanceCounter(&thread_metadata_array[i].start_time);

        thread_metadata_array[i].handle = threads[i];
        thread_metadata_array[i].entry.Flink = NULL;
        thread_metadata_array[i].entry.Blink = NULL;
        // Wrapped so this stays a valid ready_pool[][] index no matter how large num_threads
        // gets. Ready_pool only has NUM_PRIORITIES valid slots per core.
        thread_metadata_array[i].base_priority = (i + 16) % NUM_PRIORITIES;
        thread_metadata_array[i].current_priority = thread_metadata_array[i].base_priority;
        thread_metadata_array[i].ticks_waiting = 0;
        thread_metadata_array[i].canBeDecremented = 1;
        thread_metadata_array[i].owned_locks = 0;
        thread_metadata_array[i].pending_locks = 0;
        thread_metadata_array[i].releasing_locks = 0;
        thread_metadata_array[i].waiting_on_lock = NULL;
        thread_metadata_array[i].notify_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        thread_metadata_array[i].lock_grant_event = CreateEvent(NULL, FALSE, FALSE, NULL);

        int core = (int)(i % num_cores);
        thread_metadata_array[i].core = core;
        SetThreadAffinityMask(threads[i], 1ULL << core);

        // Every thread starts READY. There's no more special-cased "first num_cores threads
        // run immediately"; each core's scheduler picks up whatever's in its own list naturally.
        InsertReady(core, &thread_metadata_array[i]);
    }

    wait_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    for (DWORD c = 0; c < num_cores; c++) {
        CreateThread(NULL, 0, CoreScheduler, (LPVOID)(ULONG_PTR)c, 0, NULL);
    }
    CreateThread(NULL, 0, AgingThread, NULL, 0, NULL);

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

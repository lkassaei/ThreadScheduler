#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <windows.h>
#include "ThreadHandler.h"

#define MB(x) (((ULONG64)(x)) * 1024 * 1024)
#define KB(x) ((ULONG64)(x) * 1024)
#define GB(x) (MB(x) * 1024)


int num_threads;
HANDLE threads[MAX_THREADS];
THREAD_CONTROLLER thread_controls[MAX_THREADS];
LOCKED_LIST ready_pool[NUM_PRIORITIES];

DWORD WINAPI perform_busy_work(LPVOID lpParameter) {
    volatile ULONG64 count = 0;
    while (count <= GB(1)) {
        count++;
        if (count % (GB(1) / 100) == 0) {
            printf(".");
        }
    }
    return TRUE;
}

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
            ResumeThread(next->handle);
        }
    }
}

// Undo the decay so we don't have everyone at priority 0
VOID PriorityBoost(void) {
    for (int i = 0; i < num_threads; i++) {
        PTHREAD_CONTROLLER curr_thread = &thread_controls[i];
        if (curr_thread->current_priority == curr_thread->base_priority) {
            continue;
        }

        if (curr_thread->state == READY) {
            // Has to move to a different priority's queue, not just change the field
            LockedTryRemoveEntry(&ready_pool[curr_thread->current_priority], &curr_thread->entry);
            curr_thread->current_priority = curr_thread->base_priority;
            LockedInsertTail(&ready_pool[curr_thread->current_priority], &curr_thread->entry);
        } else {
            // Running so not queued anywhere, just correct the field
            curr_thread->current_priority = curr_thread->base_priority;
        }
        curr_thread->ticks_at_priority = 0;
    }
}

// A RUNNING thread whose OS handle has signaled already finished perform_busy_work
// and returned -- without this, it stays marked RUNNING forever and permanently
// occupies a running slot that a READY thread could otherwise use.
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
            ResumeThread(next->handle);
        }
    }
}

VOID SchedulerTick(void) {
    static ULONG64 tick_count = 0;

    // Check to see if anyone is done
    HandleFinished();

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

        // Decrement priority
        int old_priority = curr_thread->current_priority;
        int new_priority;

        if (old_priority > 0) {
            new_priority = old_priority - 1;
        }
        else {
            new_priority = 0;
            continue;
        }

        // Still RUNNING -- not queued anywhere, so just correct the fields.
        // Reschedule() is what links this into ready_pool, and only once it
        // actually suspends the thread.
        curr_thread->current_priority = new_priority;
        curr_thread->ticks_at_priority = 0;
    }

    tick_count++;
    if (tick_count >= BOOST_INTERVAL_TICKS) {
        tick_count = 0;
        PriorityBoost();
    }

    Reschedule();
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
    num_threads = 12;

    for (int i = 0; i < NUM_PRIORITIES; i++) {
        InitializeLockedList(&ready_pool[i]);
    }

    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    DWORD num_cores = sys_info.dwNumberOfProcessors;

    for (int i = 0; i < num_threads; i++) {
        threads[i] = CreateThread(NULL, 0, perform_busy_work, NULL, CREATE_SUSPENDED, NULL);

        thread_controls[i].handle = threads[i];
        thread_controls[i].entry.Flink = NULL;
        thread_controls[i].entry.Blink = NULL;
        thread_controls[i].base_priority = i;
        thread_controls[i].current_priority = i;
        thread_controls[i].ticks_at_priority = 0;

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

    CreateThread(NULL, 0, ClockTickSimulator, NULL, 0, NULL);

    LARGE_INTEGER frequency, start_time, end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);

    WaitForMultipleObjects(num_threads, &threads[0], TRUE, INFINITE);

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;
    printf("Finished in %f", elapsed_ms);

    return 0;
}


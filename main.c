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
LOCKED_LIST wait_list;
LOCKED_LIST ready_pool[NUM_PRIORITIES];
HANDLE wait_event;

DWORD WINAPI perform_busy_work(LPVOID lpParameter) {
    PTHREAD_CONTROLLER self = (PTHREAD_CONTROLLER)lpParameter;

    WaitForSingleObject(wait_event, 10);
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
            ResumeThread(next->handle);
        }
    }
}

VOID SchedulerTick(void) {
    static ULONG64 tick_count = 0;

    // Check to see if anyone is done
    HandleFinished();

    // Check to see if anyone doesn't need to be waiting
    CheckWaiting();

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

        // Still running
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

// Simulate event (homemade critsecs)
// What happens when I have two at same priority?
// Thread termination (cleanup) shutdown event
// Timers per thread to check if they finish at same time
int main(void) {
    num_threads = 16;

    InitializeLockedList(&wait_list);
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        InitializeLockedList(&ready_pool[i]);
    }

    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    DWORD num_cores = sys_info.dwNumberOfProcessors;

    for (int i = 0; i < num_threads; i++) {
        // Create suspended so we can fill in the thread controls and assign a CPU
        threads[i] = CreateThread(NULL, 0, perform_busy_work, &thread_controls[i], CREATE_SUSPENDED, NULL);
        SetThreadPriority(threads[i], 16 + i);

        QueryPerformanceCounter(&thread_controls[i].start_time);

        thread_controls[i].handle = threads[i];
        thread_controls[i].entry.Flink = NULL;
        thread_controls[i].entry.Blink = NULL;
        thread_controls[i].base_priority = i + 16;
        thread_controls[i].current_priority = i + 16;
        thread_controls[i].ticks_at_priority = 0;

        // Assign the processor
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
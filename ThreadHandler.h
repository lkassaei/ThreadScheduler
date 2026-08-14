//
// Created by lilyk on 8/5/2026.
//

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <windows.h>

#ifndef THREADHANDLER_THREADHANDLER_H
#define THREADHANDLER_THREADHANDLER_H

#define NUM_PRIORITIES 32
#define WAITING 0
#define READY 1
#define RUNNING 2
#define DONE 3

#define SUSPEND_THRESHOLD 100
#define AGE_THRESHOLD 50 // ticks a READY/WAITING thread sits un-run before it earns a priority bump
#define TICK_MS 5

#define MAX_THREADS 64
#define MAX_CORES 64 // upper bound for FillIdleSlots' per-core occupancy scan
#define NUM_LOCKS 4
#define LOCK1_BIT (1u << 0)
#define LOCK2_BIT (1u << 1)
#define LOCK3_BIT (1u << 2)
#define LOCK4_BIT (1u << 3)

#define BASE_RUNNING_AMOUNT 50      // ticks (TICK_MS each) a thread runs before EnterKernelMode() traps it back in
#define RUNNING_AMOUNT_STEP 5       // extra ticks granted per priority level
#define WORK_CHECK_INTERVAL 200000  // how often DoBusyWork checks running_amount, in loop iterations

typedef struct _SIM_LOCK SIM_LOCK, *PSIM_LOCK; // forward decl: THREAD_CONTROLLER needs to point at one

typedef struct THREAD_CONTROLLER {
    LIST_ENTRY  entry;            // Links into ready_queue[priority]
    HANDLE      handle;           // The thread itself
    int         core;             // Which core this thread is affinity-pinned to (i % num_cores)
    int         base_priority;    // What prioriy it started with
    int         current_priority; // Current priority
    ULONG64     ticks_at_priority;// Resets on demotion or on getting scheduled
    ULONG64     ticks_waiting;    // Resets on getting scheduled and counts up while ready/waiting
    int         state;            // Ready, waiting, running or finished
    ULONG64     canBeDecremented; // If the thread received a boost from another thread this is 0, else it is 1
    LARGE_INTEGER start_time;     // QueryPerformanceCounter value at CreateThread

    ULONG64     running_amount;   // Ticks this dispatch is allowed to run before EnterKernelMode() traps in
    LARGE_INTEGER dispatch_time;  // QueryPerformanceCounter value from the moment this dispatch started running

    ULONG       owned_locks;      // Bitmask of locks currently held (bit i = locks[i])
    ULONG       pending_locks;    // Bitmask of locks still needed to satisfy the current AcquireLocks() call
    ULONG       releasing_locks;  // Bitmask the worker wants released; ticker consumes and clears it
    HANDLE      lock_grant_event; // Signaled by the ticker once pending_locks hits 0
    PSIM_LOCK   waiting_on_lock;  // Lock currently enqueued on in locks[i].waiters, NULL if not blocked
} THREAD_CONTROLLER, *PTHREAD_CONTROLLER;

// Flink Blink list with its own critical section
typedef struct _LOCKED_LIST {
    LIST_ENTRY        head;
    ULONG64           count;
    CRITICAL_SECTION  lock;
} LOCKED_LIST, *PLOCKED_LIST;

// Metadata for our simulated locks where we know the owning thread and who is waiting on it
struct _SIM_LOCK {
    PTHREAD_CONTROLLER owner;
    LOCKED_LIST         waiters;
};

extern int num_threads;
extern DWORD num_cores;
extern HANDLE threads[MAX_THREADS];
extern THREAD_CONTROLLER thread_metadata_array[MAX_THREADS];
extern LOCKED_LIST wait_list;
extern LOCKED_LIST ready_pool[NUM_PRIORITIES];
extern HANDLE wait_event;
extern SIM_LOCK locks[NUM_LOCKS];
extern CRITICAL_SECTION scheduler_lock; // Serializes worker self-scheduling (EnterKernelMode) against SchedulerTick

VOID        InitializeLockedList(PLOCKED_LIST list);
VOID        LockedInsertTail(PLOCKED_LIST list, PLIST_ENTRY entry);
VOID        LockedInsertTailBatch(PLOCKED_LIST list, PLIST_ENTRY* entries, int n);
PLIST_ENTRY LockedRemoveHead(PLOCKED_LIST list);
BOOL        LockedTryRemoveEntry(PLOCKED_LIST list, PLIST_ENTRY entry);

VOID        InitializeListHead(PLIST_ENTRY ListHead);
BOOLEAN     IsListEmpty(PLIST_ENTRY ListHead);
VOID        InsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
PLIST_ENTRY RemoveHeadList(PLIST_ENTRY ListHead);
BOOLEAN     RemoveEntryList(PLIST_ENTRY Entry);

#endif //THREADHANDLER_THREADHANDLER_H

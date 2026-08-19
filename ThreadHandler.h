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

#define AGE_THRESHOLD 50 // ticks a READY/WAITING thread sits un-run before it earns a priority bump
#define TICK_MS 5         // cadence of the aging sweep -- the only periodic thread left in the system

#define MAX_THREADS 64
#define MAX_CORES 64
#define NUM_LOCKS 4
#define LOCK1_BIT (1u << 0)
#define LOCK2_BIT (1u << 1)
#define LOCK3_BIT (1u << 2)
#define LOCK4_BIT (1u << 3)

#define BASE_RUNNING_AMOUNT 50      // ticks (TICK_MS each) a thread's quantum lasts before its scheduler cuts it off
#define RUNNING_AMOUNT_STEP 5       // extra ticks granted per priority level

typedef struct _SIM_LOCK SIM_LOCK, *PSIM_LOCK; // forward decl: THREAD_CONTROLLER needs to point at one

typedef struct THREAD_CONTROLLER {
    LIST_ENTRY  entry;             // Links into ready_pool[core][priority] or locks[i].waiters
    HANDLE      handle;            // The thread itself -- also waited on directly to detect natural exit
    HANDLE      notify_event;      // Signaled by this thread to wake its own core's scheduler: "come look at me"
    HANDLE      lock_grant_event;  // Signaled by the scheduler once pending_locks hits 0
    int         core;              // Which core this thread is currently pinned to (reassignable via poaching)
    int         base_priority;     // What priority it started with
    int         current_priority;  // Current priority
    ULONG64     ticks_waiting;     // Aging counter -- counts up while READY or WAITING, resets on dispatch or bump
    int         state;             // Ready, waiting, running or finished
    ULONG64     canBeDecremented;  // If the thread received a boost from another thread this is 0, else it is 1
    LARGE_INTEGER start_time;      // QueryPerformanceCounter value at CreateThread

    ULONG64     running_amount;    // Ticks this dispatch's quantum timer is armed for

    ULONG       owned_locks;       // Bitmask of locks currently held (bit i = locks[i])
    ULONG       pending_locks;     // Bitmask of locks still needed to satisfy the current AcquireLocks() call
    ULONG       releasing_locks;   // Bitmask the worker wants released; its scheduler consumes and clears it
    PSIM_LOCK   waiting_on_lock;   // Lock currently enqueued on in locks[i].waiters, NULL if not blocked
} THREAD_CONTROLLER, *PTHREAD_CONTROLLER;

// Flink Blink list with its own SRWLOCK -- lighter weight than a CRITICAL_SECTION, no recursion needed
typedef struct _LOCKED_LIST {
    LIST_ENTRY  head;
    ULONG64     count;
    SRWLOCK     lock;
} LOCKED_LIST, *PLOCKED_LIST;

// Metadata for our simulated locks. `owner` is protected by its own owner_lock (separate from
// waiters' internal lock) since multiple core schedulers can now race to claim/release the same
// lock concurrently, not just one central ticker.
struct _SIM_LOCK {
    PTHREAD_CONTROLLER owner;
    SRWLOCK             owner_lock;
    LOCKED_LIST         waiters;
};

extern int num_threads;
extern DWORD num_cores;
extern HANDLE threads[MAX_THREADS];
extern THREAD_CONTROLLER thread_metadata_array[MAX_THREADS];
extern LOCKED_LIST ready_pool[MAX_CORES][NUM_PRIORITIES]; // one full priority-bucket set per core
extern HANDLE wait_event;
extern SIM_LOCK locks[NUM_LOCKS];

VOID        InitializeLockedList(PLOCKED_LIST list);
VOID        LockedInsertTail(PLOCKED_LIST list, PLIST_ENTRY entry);
PLIST_ENTRY LockedRemoveHead(PLOCKED_LIST list);
BOOL        LockedTryRemoveEntry(PLOCKED_LIST list, PLIST_ENTRY entry);

VOID        InitializeListHead(PLIST_ENTRY ListHead);
BOOLEAN     IsListEmpty(PLIST_ENTRY ListHead);
VOID        InsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
PLIST_ENTRY RemoveHeadList(PLIST_ENTRY ListHead);
BOOLEAN     RemoveEntryList(PLIST_ENTRY Entry);

#endif //THREADHANDLER_THREADHANDLER_H

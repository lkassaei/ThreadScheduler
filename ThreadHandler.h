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
#define NUM_LOCKS 2
#define LOCK1_BIT (1u << 0)
#define LOCK2_BIT (1u << 1)

typedef struct THREAD_CONTROLLER {
    LIST_ENTRY  entry;            // Links into ready_queue[priority]
    HANDLE      handle;           // The thread itself
    int         base_priority;    // What prioriy it started with
    int         current_priority; // Current priority
    ULONG64     ticks_at_priority;// Resets on demotion or on getting scheduled
    ULONG64     ticks_waiting;    // Resets on getting scheduled and counts up while ready/waiting
    int         state;            // Ready, waiting, running or finished
    ULONG64     canBeDecremented; // If the thread received a boost from another thread this is 0, else it is 1
    LARGE_INTEGER start_time;     // QueryPerformanceCounter value at CreateThread

    ULONG       owned_locks;      // Bitmask of locks currently held (bit i = locks[i])
    ULONG       pending_locks;    // Bitmask of locks still needed to satisfy the current AcquireLocks() call
    ULONG       releasing_locks;  // Bitmask the worker wants released; ticker consumes and clears it
    HANDLE      lock_grant_event; // Signaled by the ticker once pending_locks hits 0
} THREAD_CONTROLLER, *PTHREAD_CONTROLLER;

// Flink Blink list with its own critical section
typedef struct _LOCKED_LIST {
    LIST_ENTRY        head;
    ULONG64           count;
    CRITICAL_SECTION  lock;
} LOCKED_LIST, *PLOCKED_LIST;

// Metadata for our simulated locks where we know the owning thread and who is waiting on it
typedef struct _SIM_LOCK {
    PTHREAD_CONTROLLER owner;
    LOCKED_LIST         waiters;
} SIM_LOCK, *PSIM_LOCK;

extern int num_threads;
extern DWORD num_cores;
extern HANDLE threads[MAX_THREADS];
extern THREAD_CONTROLLER thread_controls[MAX_THREADS];
extern LOCKED_LIST wait_list;
extern LOCKED_LIST ready_pool[NUM_PRIORITIES];
extern HANDLE wait_event;
extern SIM_LOCK locks[NUM_LOCKS];

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

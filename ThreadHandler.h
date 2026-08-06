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
#define TICK_MS 5
#define BOOST_INTERVAL_TICKS 1000 // ~5s at TICK_MS=5 -- how often everyone gets reset to base_priority

#define MAX_THREADS 16

typedef struct THREAD_CONTROLLER {
    LIST_ENTRY  entry;            // link into ready_queue[priority]
    HANDLE      handle;
    int         base_priority;
    int         current_priority;
    ULONG64     ticks_at_priority; // resets on demotion or on getting scheduled
    int         state;
} THREAD_CONTROLLER, *PTHREAD_CONTROLLER;

// Flink Blink list with its own critical section
typedef struct _LOCKED_LIST {
    LIST_ENTRY        head;
    ULONG64           count;
    CRITICAL_SECTION  lock;
} LOCKED_LIST, *PLOCKED_LIST;

extern int num_threads;
extern HANDLE threads[MAX_THREADS];
extern THREAD_CONTROLLER thread_controls[MAX_THREADS];
extern LOCKED_LIST ready_pool[NUM_PRIORITIES];

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

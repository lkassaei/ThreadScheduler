#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <windows.h>

#define MB(x) (((ULONG64)(x)) * 1024 * 1024)
#define KB(x) ((ULONG64)(x) * 1024)
#define GB(x) (MB(x) * 1024)


DWORD WINAPI perform_busy_work() {
    volatile ULONG64 count = 0;
    while (count <= GB(1)) {
        count++;
    }
    return TRUE;
}

int main(void) {
    LARGE_INTEGER frequency, start_time, end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    // Which parameter do we have to change to be able to set the mask?
    HANDLE thread1 = CreateThread(NULL, 0, perform_busy_work, NULL, 0, NULL);
    SetThreadAffinityMask(thread1, 1); // How can we set the affinity mask if the thread is already running?

    // Extra stuff we can use
    //GetThreadGroupAffinity()
    //SetThreadGroupAffinity()

    // Puts on the "ready to run" I think
    SuspendThread(thread1);

    // Change to highest priority
    SetThreadPriority(thread1, THREAD_PRIORITY_HIGHEST); // Would this constant be 16 or 31?
    ResumeThread(thread1);

    WaitForSingleObject(thread1, INFINITE);

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;
    printf("Finished in %f", elapsed_ms);

    return 0;
}


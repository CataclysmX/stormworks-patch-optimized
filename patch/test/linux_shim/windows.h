/* Just enough of windows.h to compile pure-logic stwpatch modules natively on Linux (tests). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
typedef uint64_t ULONGLONG;
typedef int64_t LONG64;
typedef long LONG;
typedef uint32_t DWORD;
typedef int BOOL;
typedef void *HMODULE, *HANDLE, *LPVOID;
typedef struct { void *p; } SRWLOCK;
#define SRWLOCK_INIT { 0 }
#define WINAPI
static inline int  TryAcquireSRWLockExclusive(SRWLOCK *l) { (void)l; return 1; }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }
static inline void AcquireSRWLockShared(SRWLOCK *l) { (void)l; }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { (void)l; }
static inline LONG64 InterlockedIncrement64(volatile LONG64 *p) { return ++*p; }
ULONGLONG GetTickCount64(void);

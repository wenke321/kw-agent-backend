#ifndef BUILTINS_H
#define BUILTINS_H

#define _load_relaxed(ptr) __atomic_load_n(ptr, __ATOMIC_RELAXED)
#define _load_acquire(ptr) __atomic_load_n(ptr, __ATOMIC_ACQUIRE)
#define _load_seqcst(ptr)  __atomic_load_n(ptr, __ATOMIC_SEQ_CST)

#define _store_relaxed(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELAXED)
#define _store_release(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_RELEASE)
#define _store_seqcst(ptr, val)  __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)

#define _fetch_add_relaxed(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED)

#define _fetch_add_seqcst(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_SEQ_CST)

#define _fetch_or_acquire(ptr, val) __atomic_fetch_or(ptr, val, __ATOMIC_ACQUIRE)

#define _fetch_sub_acqrel(ptr, val) __atomic_fetch_sub(ptr, val, __ATOMIC_ACQ_REL)

#define _fetch_sub_release(ptr, val) __atomic_fetch_sub(ptr, val, __ATOMIC_RELEASE)

#define _fetch_sub_relaxed(ptr, val) __atomic_fetch_sub(ptr, val, __ATOMIC_RELAXED)

#define _sub_fetch_relaxed(ptr, val) __atomic_sub_fetch(ptr, val, __ATOMIC_RELAXED)

#define _add_fetch_seqcst(ptr, val) __atomic_add_fetch(ptr, val, __ATOMIC_SEQ_CST)

#define _sub_fetch_seqcst(ptr, val) __atomic_sub_fetch(ptr, val, __ATOMIC_SEQ_CST)

#define _CAS_weak_relaxed(ptr, expected, desired) __atomic_compare_exchange_n(ptr, expected, desired, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)

#define _CAS_weak_release(ptr, expected, desired) __atomic_compare_exchange_n(ptr, expected, desired, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)

#define _CAS_strong_relaxed(ptr, expected, desired) __atomic_compare_exchange_n(ptr, expected, desired, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)

#define _CAS_strong_release(ptr, expected, desired) __atomic_compare_exchange_n(ptr, expected, desired, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)

#define _thread_fence_relaxed __atomic_thread_fence(__ATOMIC_RELAXED)

#define _addressof(val) __builtin_addressof(val)

#endif  // BUILTIN_H

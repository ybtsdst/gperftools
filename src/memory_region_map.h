// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/* Copyright (c) 2006, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ---
 * Author: Maxim Lifantsev
 */

#ifndef BASE_MEMORY_REGION_MAP_H_
#define BASE_MEMORY_REGION_MAP_H_

#include <config.h>

#include <stddef.h>
#include <set>
#include "base/stl_allocator.h"
#include "base/spinlock.h"
#include "base/threading.h"
#include "base/thread_annotations.h"
#include "base/low_level_alloc.h"
#include "heap-profile-stats.h"
#include "mmap_hook.h"

// TODO(maxim): add a unittest:
//  execute a bunch of mmaps and compare memory map what strace logs
//  execute a bunch of mmap/munmup and compare memory map with
//  own accounting of what those mmaps generated

// Thread-safe class to collect and query the map of all memory regions
// in a process that have been created with mmap, munmap, mremap, sbrk.
// For each memory region, we keep track of (and provide to users)
// the stack trace that allocated that memory region.
// The recorded stack trace depth is bounded by
// a user-supplied max_stack_depth parameter of Init().
// After initialization with Init()
// (which can happened even before global object constructor execution)
// we collect the map by installing and monitoring MallocHook-s
// to mmap, munmap, mremap, sbrk.
// At any time one can query this map via provided interface.
// For more details on the design of MemoryRegionMap
// see the comment at the top of our .cc file.
class MemoryRegionMap {
 private:
  // Max call stack recording depth supported by Init().  Set it to be
  // high enough for all our clients.  Note: we do not define storage
  // for this (doing that requires special handling in windows), so
  // don't take the address of it!
  static const int kMaxStackDepth = 32;

  // Size of the hash table of buckets.  A structure of the bucket table is
  // described in heap-profile-stats.h.
  static const int kHashTableSize = 179999;

 public:
  // interface ================================================================

  // Every client of MemoryRegionMap must call Init() before first use,
  // and Shutdown() after last use.  This allows us to reference count
  // this (singleton) class properly.

  // Initialize this module to record memory allocation stack traces.
  // Stack traces that have more than "max_stack_depth" frames
  // are automatically shrunk to "max_stack_depth" when they are recorded.
  // Init() can be called more than once w/o harm, largest max_stack_depth
  // will be the effective one.
  // When "use_buckets" is true, then counts of mmap and munmap sizes will be
  // recorded with each stack trace.  If Init() is called more than once, then
  // counting will be effective after any call contained "use_buckets" of true.
  // It will install mmap, munmap, mremap, sbrk hooks
  // and initialize arena_ and our hook and locks, hence one can use
  // MemoryRegionMap::Lock()/Unlock() to manage the locks.
  // Uses Lock/Unlock inside.
  static void Init(int max_stack_depth, bool use_buckets);

  // Try to shutdown this module undoing what Init() did.
  // Returns true iff could do full shutdown (or it was not attempted).
  // Full shutdown is attempted when the number of Shutdown() calls equals
  // the number of Init() calls.
  static bool Shutdown();

  // Return true if MemoryRegionMap is initialized and recording, i.e. when
  // then number of Init() calls are more than the number of Shutdown() calls.
  static bool IsRecordingLocked();

  // Locks to protect our internal data structures.
  // These also protect use of arena_ if our Init() has been done.
  // The lock is recursive.
  static void Lock() EXCLUSIVE_LOCK_FUNCTION(lock_);
  static void Unlock() UNLOCK_FUNCTION(lock_);

  // Returns true when the lock is held by this thread (for use in RAW_CHECK-s).
  static bool LockIsHeld();

  // Locker object that acquires the MemoryRegionMap::Lock
  // for the duration of its lifetime (a C++ scope).
  class SCOPED_LOCKABLE LockHolder {
   public:
     LockHolder() EXCLUSIVE_LOCK_FUNCTION(lock_) { Lock(); }
     ~LockHolder() UNLOCK_FUNCTION(lock_) { Unlock(); }
   private:
    DISALLOW_COPY_AND_ASSIGN(LockHolder);
  };

  // A memory region that we know about through mmap hooks.
  // This is essentially an interface through which MemoryRegionMap
  // exports the collected data to its clients.  Thread-compatible.
  struct Region {
    uintptr_t start_addr;  // region start address
    uintptr_t end_addr;  // region end address
    int call_stack_depth;  // number of caller stack frames that we saved
    const void* call_stack[kMaxStackDepth];  // caller address stack array
                                             // filled to call_stack_depth size
    bool is_stack;  // does this region contain a thread's stack:
                    // a user of MemoryRegionMap supplies this info

    // Convenience accessor for call_stack[0],
    // i.e. (the program counter of) the immediate caller
    // of this region's allocation function,
    // but it also returns NULL when call_stack_depth is 0,
    // i.e whe we weren't able to get the call stack.
    // This usually happens in recursive calls, when the stack-unwinder
    // calls mmap() which in turn calls the stack-unwinder.
    uintptr_t caller() const {
      return reinterpret_cast<uintptr_t>(call_stack_depth >= 1
                                         ? call_stack[0] : NULL);
    }

    // Return true iff this region overlaps region x.
    bool Overlaps(const Region& x) const {
      return start_addr < x.end_addr  &&  end_addr > x.start_addr;
    }

   private:  // helpers for MemoryRegionMap
    friend class MemoryRegionMap;

    // The ways we create Region-s:
    void Create(const void* start, size_t size) {
      start_addr = reinterpret_cast<uintptr_t>(start);
      end_addr = start_addr + size;
      is_stack = false;  // not a stack till marked such
      call_stack_depth = 0;
      AssertIsConsistent();
    }
    void set_call_stack_depth(int depth) {
      RAW_DCHECK(call_stack_depth == 0, "");  // only one such set is allowed
      call_stack_depth = depth;
      AssertIsConsistent();
    }

    // The ways we modify Region-s:
    void set_is_stack() { is_stack = true; }
    void set_start_addr(uintptr_t addr) {
      start_addr = addr;
      AssertIsConsistent();
    }
    void set_end_addr(uintptr_t addr) {
      end_addr = addr;
      AssertIsConsistent();
    }

    // Verifies that *this contains consistent data, crashes if not the case.
    void AssertIsConsistent() const {
      RAW_DCHECK(start_addr < end_addr, "");
      RAW_DCHECK(call_stack_depth >= 0  &&
                 call_stack_depth <= kMaxStackDepth, "");
    }

    // Post-default construction helper to make a Region suitable
    // for searching in RegionSet regions_.
    void SetRegionSetKey(uintptr_t addr) {
      // make sure *this has no usable data:
      if (DEBUG_MODE) memset(this, 0xFF, sizeof(*this));
      end_addr = addr;
    }

    // Note: call_stack[kMaxStackDepth] as a member lets us make Region
    // a simple self-contained struct with correctly behaving bit-vise copying.
    // This simplifies the code of this module but wastes some memory:
    // in most-often use case of this module (leak checking)
    // only one call_stack element out of kMaxStackDepth is actually needed.
    // Making the storage for call_stack variable-sized,
    // substantially complicates memory management for the Region-s:
    // as they need to be created and manipulated for some time
    // w/o any memory allocations, yet are also given out to the users.
  };

  // Find the region that covers addr and write its data into *result if found,
  // in which case *result gets filled so that it stays fully functional
  // even when the underlying region gets removed from MemoryRegionMap.
  // Returns success. Uses Lock/Unlock inside.
  static bool FindRegion(uintptr_t addr, Region* result);

  // Find the region that contains stack_top, mark that region as
  // a stack region, and write its data into *result if found,
  // in which case *result gets filled so that it stays fully functional
  // even when the underlying region gets removed from MemoryRegionMap.
  // Returns success. Uses Lock/Unlock inside.
  static bool FindAndMarkStackRegion(uintptr_t stack_top, Region* result);

  // Iterate over the buckets which store mmap and munmap counts per stack
  // trace.  It calls "callback" for each bucket, and passes "arg" to it.
  template<class Body>
  static void IterateBuckets(Body body) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Get the bucket whose caller stack trace is "key".  The stack trace is
  // used to a depth of "depth" at most.  The requested bucket is created if
  // needed.
  // The bucket table is described in heap-profile-stats.h.
  static HeapProfileBucket* GetBucket(int depth, const void* const key[]) EXCLUSIVE_LOCKS_REQUIRED(lock_);

 private:  // our internal types ==============================================

  // Region comparator for sorting with STL
  struct RegionCmp {
    bool operator()(const Region& x, const Region& y) const {
      return x.end_addr < y.end_addr;
    }
  };

  // We allocate STL objects in our own arena.
  struct MyAllocator {
    static void *Allocate(size_t n) {
      return LowLevelAlloc::AllocWithArena(n, arena_);
    }
    static void Free(const void *p, size_t /* n */) {
      LowLevelAlloc::Free(const_cast<void*>(p));
    }
  };

  // Set of the memory regions
  typedef std::set<Region, RegionCmp,
              STL_Allocator<Region, MyAllocator> > RegionSet;

  // The bytes where MemoryRegionMap::regions_ will point to.  We use
  // StaticStorage with noop c-tor so that global construction does
  // not interfere.
  static inline tcmalloc::StaticStorage<RegionSet> regions_rep_;

 public:  // more in-depth interface ==========================================

  // STL iterator with values of Region
  typedef RegionSet::const_iterator RegionIterator;

  // Return the begin/end iterators to all the regions.
  // These need Lock/Unlock protection around their whole usage (loop).
  // Even when the same thread causes modifications during such a loop
  // (which are permitted due to recursive locking)
  // the loop iterator will still be valid as long as its region
  // has not been deleted, but EndRegionLocked should be
  // re-evaluated whenever the set of regions has changed.
  static RegionIterator BeginRegionLocked();
  static RegionIterator EndRegionLocked();

  // Return the accumulated sizes of mapped and unmapped regions.
  static int64_t MapSize() { return map_size_; }
  static int64_t UnmapSize() { return unmap_size_; }
 private:
  // representation ===========================================================

  // Counter of clients of this module that have called Init().
  static int client_count_;

  // Maximal number of caller stack frames to save (>= 0).
  static int max_stack_depth_;

  // Arena used for our allocations in regions_.
  static LowLevelAlloc::Arena* arena_;

  // Set of the mmap/sbrk/mremap-ed memory regions
  // To be accessed *only* when Lock() is held.
  // Hence we protect the non-recursive lock used inside of arena_
  // with our recursive Lock(). This lets a user prevent deadlocks
  // when threads are stopped by TCMalloc_ListAllProcessThreads at random spots
  // simply by acquiring our recursive Lock() before that.
  static RegionSet* regions_;

  // Lock to protect regions_ and buckets_ variables and the data behind.
  static SpinLock lock_;
  // Lock to protect the recursive lock itself.
  static SpinLock owner_lock_;

  // Recursion count for the recursive lock.
  static int recursion_count_;
  // The thread id of the thread that's inside the recursive lock.
  static uintptr_t lock_owner_tid_;

  // Total size of all mapped pages so far
  static int64_t map_size_;
  // Total size of all unmapped pages so far
  static int64_t unmap_size_;

  // Bucket hash table which is described in heap-profile-stats.h.
  static HeapProfileBucket** bucket_table_ GUARDED_BY(lock_);
  static int num_buckets_ GUARDED_BY(lock_);

  // The following members are local to MemoryRegionMap::GetBucket()
  // and MemoryRegionMap::HandleSavedBucketsLocked()
  // and are file-level to ensure that they are initialized at load time.
  //
  // These are used as temporary storage to break the infinite cycle of mmap
  // calling our hook which (sometimes) causes mmap.  It must be a static
  // fixed-size array.  The size 20 is just an expected value for safety.
  // The details are described in memory_region_map.cc.

  // Number of unprocessed bucket inserts.
  static int saved_buckets_count_ GUARDED_BY(lock_);

  // Unprocessed inserts (must be big enough to hold all mmaps that can be
  // caused by a GetBucket call).
  // Bucket has no constructor, so that c-tor execution does not interfere
  // with the any-time use of the static memory behind saved_buckets.
  static HeapProfileBucket saved_buckets_[20] GUARDED_BY(lock_);

  static const void* saved_buckets_keys_[20][kMaxStackDepth] GUARDED_BY(lock_);

  static tcmalloc::MappingHookSpace mapping_hook_space_;

  // helpers ==================================================================

  // Helper for FindRegion and FindAndMarkStackRegion:
  // returns the region covering 'addr' or NULL; assumes our lock_ is held.
  static const Region* DoFindRegionLocked(uintptr_t addr);

  // Verifying wrapper around regions_->insert(region)
  // To be called to do InsertRegionLocked's work only!
  inline static void DoInsertRegionLocked(const Region& region);
  // Handle regions saved by InsertRegionLocked into a tmp static array
  // by calling insert_func on them.
  inline static void HandleSavedRegionsLocked(
                       void (*insert_func)(const Region& region));

  // Restore buckets saved in a tmp static array by GetBucket to the bucket
  // table where all buckets eventually should be.
  static void RestoreSavedBucketsLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Initialize RegionSet regions_.
  inline static void InitRegionSetLocked();

  // Wrapper around DoInsertRegionLocked
  // that handles the case of recursive allocator calls.
  inline static void InsertRegionLocked(const Region& region);

  // Record addition of a memory region at address "start" of size "size"
  // (called from our mmap/mremap/sbrk hook).
  static void RecordRegionAddition(const void* start, size_t size);
  // Record deletion of a memory region at address "start" of size "size"
  // (called from our munmap/mremap/sbrk hook).
  static void RecordRegionRemoval(const void* start, size_t size);

  // Record deletion of a memory region of size "size" in a bucket whose
  // caller stack trace is "key".  The stack trace is used to a depth of
  // "depth" at most.
  static void RecordRegionRemovalInBucket(int depth,
                                          const void* const key[],
                                          size_t size) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  static void HandleMappingEvent(const tcmalloc::MappingEvent& evt);

  // Log all memory regions; Useful for debugging only.
  // Assumes Lock() is held
  static void LogAllLocked();

  DISALLOW_COPY_AND_ASSIGN(MemoryRegionMap);
};

template <class Body>
void MemoryRegionMap::IterateBuckets(Body body) {
  for (int index = 0; index < kHashTableSize; index++) {
    for (HeapProfileBucket* bucket = bucket_table_[index];
         bucket != NULL;
         bucket = bucket->next) {
      body(bucket);
    }
  }
}

#endif  // BASE_MEMORY_REGION_MAP_H_

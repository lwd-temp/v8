// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_INCREMENTAL_MARKING_H_
#define V8_HEAP_INCREMENTAL_MARKING_H_

#include "src/base/logging.h"
#include "src/base/platform/mutex.h"
#include "src/heap/heap.h"
#include "src/heap/incremental-marking-job.h"
#include "src/heap/mark-compact.h"
#include "src/tasks/cancelable-task.h"

namespace v8 {
namespace internal {

class HeapObject;
class MarkBit;
class Map;
class Object;
class PagedSpace;

// Describes in which context IncrementalMarking::Step() is used in. This
// information is used when marking finishes and for marking progress
// heuristics.
enum class StepOrigin {
  // The caller of Step() is not allowed to complete marking right away. A task
  // is scheduled to complete the GC. When the task isn't
  // run soon enough, the stack guard mechanism will be used.
  kV8,

  // The caller of Step() will complete marking by running the GC right
  // afterwards.
  kTask
};

enum class StepResult {
  kNoImmediateWork,
  kMoreWorkRemaining,
  kWaitingForFinalization
};

class V8_EXPORT_PRIVATE IncrementalMarking final {
 public:
  enum State : uint8_t { STOPPED, MARKING, COMPLETE };

  class V8_NODISCARD PauseBlackAllocationScope {
   public:
    explicit PauseBlackAllocationScope(IncrementalMarking* marking)
        : marking_(marking) {
      if (marking_->black_allocation()) {
        paused_ = true;
        marking_->PauseBlackAllocation();
      }
    }

    ~PauseBlackAllocationScope() {
      if (paused_) {
        marking_->StartBlackAllocation();
      }
    }

   private:
    IncrementalMarking* marking_;
    bool paused_ = false;
  };

  // It's hard to know how much work the incremental marker should do to make
  // progress in the face of the mutator creating new work for it.  We start
  // of at a moderate rate of work and gradually increase the speed of the
  // incremental marker until it completes.
  // Do some marking every time this much memory has been allocated or that many
  // heavy (color-checking) write barriers have been invoked.
  static const size_t kYoungGenerationAllocatedThreshold = 64 * KB;
  static const size_t kOldGenerationAllocatedThreshold = 256 * KB;
  static const size_t kMinStepSizeInBytes = 64 * KB;

  static constexpr double kStepSizeInMs = 1;
  static constexpr double kMaxStepSizeInMs = 5;

#ifndef DEBUG
  static constexpr size_t kV8ActivationThreshold = 8 * MB;
  static constexpr size_t kEmbedderActivationThreshold = 8 * MB;
#else
  static constexpr size_t kV8ActivationThreshold = 0;
  static constexpr size_t kEmbedderActivationThreshold = 0;
#endif

  V8_INLINE void TransferColor(HeapObject from, HeapObject to);

  V8_INLINE void RestartIfNotMarking();

  IncrementalMarking(Heap* heap, WeakObjects* weak_objects);

  MarkingState* marking_state() { return &marking_state_; }
  AtomicMarkingState* atomic_marking_state() { return &atomic_marking_state_; }
  NonAtomicMarkingState* non_atomic_marking_state() {
    return &non_atomic_marking_state_;
  }

  void NotifyLeftTrimming(HeapObject from, HeapObject to);

  bool IsStopped() const { return state() == STOPPED; }
  bool IsRunning() const { return !IsStopped(); }
  bool IsMarking() const { return state() >= MARKING; }
  bool IsComplete() const { return state() == COMPLETE; }

  bool CollectionRequested() const { return collection_requested_; }

  bool CanBeStarted() const;

  void Start(GarbageCollectionReason gc_reason);
  // Returns true if incremental marking was running and false otherwise.
  bool Stop();

  void UpdateMarkingWorklistAfterYoungGenGC();
  void UpdateMarkedBytesAfterScavenge(size_t dead_bytes_in_new_space);

  // Performs incremental marking steps and returns before the deadline_in_ms is
  // reached. It may return earlier if the marker is already ahead of the
  // marking schedule, which is indicated with StepResult::kDone.
  StepResult AdvanceWithDeadline(double deadline_in_ms, StepOrigin step_origin);

  StepResult Step(double max_step_size_in_ms, StepOrigin step_origin);

  // This function is used to color the object black before it undergoes an
  // unsafe layout change. This is a part of synchronization protocol with
  // the concurrent marker.
  void MarkBlackAndVisitObjectDueToLayoutChange(HeapObject obj);

  void MarkBlackBackground(HeapObject obj, int object_size);

  bool IsCompacting() { return IsMarking() && is_compacting_; }

  Heap* heap() const { return heap_; }

  IncrementalMarkingJob* incremental_marking_job() {
    return &incremental_marking_job_;
  }

  bool black_allocation() { return black_allocation_; }

  void StartBlackAllocationForTesting() {
    if (!black_allocation_) {
      StartBlackAllocation();
    }
  }

  MarkingWorklists::Local* local_marking_worklists() const {
    return collector_->local_marking_worklists();
  }

  bool IsBelowActivationThresholds() const;

  void IncrementLiveBytesBackground(MemoryChunk* chunk, intptr_t by) {
    base::MutexGuard guard(&background_live_bytes_mutex_);
    background_live_bytes_[chunk] += by;
  }

  void MarkRootsForTesting();

 private:
  class IncrementalMarkingRootMarkingVisitor;

  class Observer : public AllocationObserver {
   public:
    Observer(IncrementalMarking* incremental_marking, intptr_t step_size)
        : AllocationObserver(step_size),
          incremental_marking_(incremental_marking) {}

    void Step(int bytes_allocated, Address, size_t) override;

   private:
    IncrementalMarking* incremental_marking_;
  };

  void StartMarking();

  bool ShouldDoEmbedderStep();
  StepResult EmbedderStep(double expected_duration_ms, double* duration_ms);

  void StartBlackAllocation();
  void PauseBlackAllocation();
  void FinishBlackAllocation();

  void PublishWriteBarrierWorklists();

  // Updates scheduled_bytes_to_mark_ to ensure marking progress based on
  // time.
  void ScheduleBytesToMarkBasedOnTime(double time_ms);
  // Updates scheduled_bytes_to_mark_ to ensure marking progress based on
  // allocations.
  void ScheduleBytesToMarkBasedOnAllocation();
  // Helper functions for ScheduleBytesToMarkBasedOnAllocation.
  size_t StepSizeToKeepUpWithAllocations();
  size_t StepSizeToMakeProgress();
  void AddScheduledBytesToMark(size_t bytes_to_mark);

  // Schedules more bytes to mark so that the marker is no longer ahead
  // of schedule.
  void FastForwardSchedule();
  void FastForwardScheduleIfCloseToFinalization();

  // Fetches marked byte counters from the concurrent marker.
  void FetchBytesMarkedConcurrently();

  // Returns the bytes to mark in the current step based on the scheduled
  // bytes and already marked bytes.
  size_t ComputeStepSizeInBytes(StepOrigin step_origin);

  void TryMarkingComplete(StepOrigin step_origin);
  void MarkRoots();

  void AdvanceOnAllocation();

  // Returns true if the function succeeds in transitioning the object
  // from white to grey.
  bool WhiteToGreyAndPush(HeapObject obj);

  State state() const {
    DCHECK_IMPLIES(state_ != STOPPED, FLAG_incremental_marking);
    return state_;
  }

  void SetState(State s) {
    state_ = s;
    heap_->SetIsMarkingFlag(s >= MARKING);
  }

  double CurrentTimeToMarkingTask() const;

  Heap* const heap_;
  MarkCompactCollector* const collector_;
  WeakObjects* weak_objects_;

  double start_time_ms_ = 0.0;
  double time_to_force_completion_ = 0.0;
  size_t initial_old_generation_size_ = 0;
  size_t old_generation_allocation_counter_ = 0;
  size_t bytes_marked_ = 0;
  size_t scheduled_bytes_to_mark_ = 0;
  double schedule_update_time_ms_ = 0.0;
  // A sample of concurrent_marking()->TotalMarkedBytes() at the last
  // incremental marking step. It is used for updating
  // bytes_marked_ahead_of_schedule_ with contribution of concurrent marking.
  size_t bytes_marked_concurrently_ = 0;

  // Must use `SetState()` above to update `state_`.
  // Atomic since main thread can complete marking while a background thread's
  // slow allocation path will check whether incremental marking is currently
  // running.
  std::atomic<State> state_;

  bool is_compacting_ = false;
  bool black_allocation_ = false;
  bool collection_requested_ = false;
  IncrementalMarkingJob incremental_marking_job_;

  Observer new_generation_observer_;
  Observer old_generation_observer_;

  MarkingState marking_state_;
  AtomicMarkingState atomic_marking_state_;
  NonAtomicMarkingState non_atomic_marking_state_;

  base::Mutex background_live_bytes_mutex_;
  std::unordered_map<MemoryChunk*, intptr_t> background_live_bytes_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(IncrementalMarking);
};
}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_INCREMENTAL_MARKING_H_

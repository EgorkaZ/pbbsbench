#pragma once
#include "modes.h"
// #define EIGEN_MODE EIGEN_TIMESPAN_GRAINSIZE

#include "eigen_pool.h"
#include "intrusive_ptr.h"
#include "num_threads.h"
#include "thread_index.h"
#include "util.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace EigenPartitioner {

struct Range {
  size_t From;
  size_t To;

  size_t Size() { return To - From; }
};

struct SplitData {
  static constexpr size_t K_SPLIT = 2;
  Range Threads;
  size_t GrainSize = 1;
  size_t Depth = 0;
};

namespace detail {

class TaskStack {
public:
  TaskStack() {}

  void Add(TaskStack& ts) noexcept {
    ts.prev = prev;
    prev = &ts;
  }

  void Pop() noexcept {
    assert(!IsEmpty());
    prev = prev->prev;
  }

  bool IsEmpty() const noexcept {
    return prev == nullptr;
  }
private:
  TaskStack* prev = nullptr;
};

inline TaskStack& ThreadLocalTaskStack() {
  static thread_local TaskStack stack;
  return stack;
}
}

namespace RapidStart {

class DistributionFunc {
  virtual ~DistributionFunc() noexcept = default;

  virtual void operator()(size_t from, size_t to) = 0;
  void Distribute(int threadId, uint64_t mask) {
    //
  }
};

constexpr size_t CacheLine = 64;

// class alignas(CacheLine) Distributor {
// public:
//   bool TryDistribute(DistributionFunc* func) {
//     uint64_t epoch = Epoch_;
//     // something
//     // func->Distribute(ThreadIdx)
//   }

// private:
//   alignas(CacheLine) std::atomic_uint64_t StartMask_ = 0;
//   alignas(CacheLine) std::atomic_uint64_t FinishMask_ = 0;
//   alignas(CacheLine) std::atomic_uint64_t RunMask_ = 0;
//   alignas(CacheLine) std::atomic<DistributionFunc*> Func_ = nullptr;
//   std::atomic_uint64_t Epoch_ = 0;
//   volatile int Mode_ = 0; // 0 - stopping, 1 - rebalance, 2 - trapped
// };
}

struct TaskNode : intrusive_ref_counter<TaskNode> {
  using NodePtr = IntrusivePtr<TaskNode>;

  TaskNode(NodePtr parent = nullptr) : Parent(std::move(parent)) {}

  void SpawnChild(size_t count = 1) {
    ChildWaitingSteal_.fetch_add(count, std::memory_order_relaxed);
  }

  void OnStolen() {
    Parent->ChildWaitingSteal_.fetch_sub(1, std::memory_order_relaxed);
  }

  bool AllStolen() {
    return ChildWaitingSteal_.load(std::memory_order_relaxed) == 0;
  }

  NodePtr Parent;

  std::atomic<size_t> ChildWaitingSteal_{0};
};

enum class Balance { OFF, SIMPLE, DELAYED };

enum class Initial { TRUE, FALSE };

enum class GrainSize { DEFAULT, AUTO };

template <typename Func, Balance balance,
          GrainSize grainSizeMode, Initial initial = Initial::FALSE>
struct Task {
  using Scheduler = EigenPoolWrapper;

  static inline const uint64_t INIT_TIME = [] {
  // should be calculated using timespan_tuner with EIGEN_SIMPLE
  // currently 0.99 percentile for maximums is used: 99% of iterations should
  // fit scheduling in timespan
#if defined(__x86_64__)
    if (Eigen::internal::GetNumThreads() == 48) {
      return 16500;
    }
    // return 13500;
    return 75000000;
#elif defined(__aarch64__)
    return 1800;
#else
#error "Unsupported architecture"
#endif
  }();

  using StolenFlag = std::atomic<bool>;

  Task(Scheduler &sched, TaskNode::NodePtr node, size_t from, size_t to,
       Func func, SplitData split)
      : Sched_(sched), CurrentNode_(std::move(node)), Current_(from), End_(to),
        Func_(std::move(func)), Split_(split) {
  }

  bool IsDivisible() const {
    return (Current_ + Split_.GrainSize < End_) && !is_stack_half_full();
  }

  void DistributeWork() {
    if (Split_.Threads.Size() != 1 && IsDivisible()) {
      // take 1/parts of iterations for current thread
      Range otherData{Current_ + (End_ - Current_ + Split_.Threads.Size() - 1) /
                                     Split_.Threads.Size(),
                      End_};
      if (otherData.From + Split_.GrainSize < otherData.To) {
        End_ = otherData.From;
        Range otherThreads{Split_.Threads.From + 1, Split_.Threads.To};
        size_t parts = std::min(std::min(Split_.K_SPLIT, otherThreads.Size()),
                                otherData.Size());
        auto threadStep = otherThreads.Size() / parts;
        auto threadsMod = otherThreads.Size() % parts;
        auto dataStep = otherData.Size() / parts;
        auto dataMod = otherData.Size() % parts;
        for (size_t i = 0; i != parts; ++i) {
          auto threadSplit =
              std::min(otherThreads.To,
                       otherThreads.From + threadStep +
                           static_cast<size_t>((parts - 1 - i) < threadsMod));
          // if threads are divided equally, distribute one more task for first
          // parts of threads otherwise distribute one more task for last parts
          // of threads
          auto dataSplit = std::min(
              otherData.To,
              otherData.From + dataStep +
                  static_cast<size_t>((threadsMod == 0 ? i : (parts - 1 - i)) <
                                      dataMod));
          assert(otherData.From < dataSplit);
          assert(otherThreads.From < threadSplit);
          Sched_.run_on_thread(
              Task<Func, balance, grainSizeMode, Initial::TRUE>{
                  Sched_, new TaskNode(CurrentNode_), otherData.From, dataSplit,
                  Func_,
                  SplitData{.Threads = {otherThreads.From, threadSplit},
                            .GrainSize = Split_.GrainSize}},
              otherThreads.From);
          otherThreads.From = threadSplit;
          otherData.From = dataSplit;
        }
        assert(otherData.From == otherData.To);
        assert(otherThreads.From == otherThreads.To ||
               parts < Split_.K_SPLIT &&
                   otherThreads.From + (Split_.K_SPLIT - parts) ==
                       otherThreads.To);
      }
    }
  }

  void operator()() {
    detail::TaskStack ts;
    auto& stack = detail::ThreadLocalTaskStack();
    stack.Add(ts);
    if constexpr (initial == Initial::TRUE) {
      DistributeWork();
    }

    if constexpr (balance == Balance::DELAYED) {
      // at first we are executing job for INIT_TIME
      // and then create balancing task
      auto start = Now();
      while (Current_ < End_) {
        Execute();
        if (Now() - start > INIT_TIME) {
          break;
        }
        if constexpr (grainSizeMode == GrainSize::AUTO) {
          Split_.GrainSize++;
        }
      }
    }

    if constexpr (balance != Balance::OFF) {
      while (Current_ != End_ && IsDivisible()) {
        // make balancing tasks for remaining iterations
        // TODO: check stolen? maybe not each time?
        // if (CurrentNode_->AllStolen()) {
        // TODO: maybe we need to check "depth" - number of being stolen
        // times?
        size_t mid = Current_ + (End_ - Current_) / 2;
        // CurrentNode_->SpawnChild();
        // eigen's scheduler will push task to the current thread queue,
        // then some other thread can steal this
        Sched_.run(Task<Func, Balance::SIMPLE, GrainSize::DEFAULT>{
            Sched_, new TaskNode(CurrentNode_), mid, End_, Func_,
            SplitData{.GrainSize = Split_.GrainSize, .Depth = Split_.Depth + 1}});
        Eigen::Tracing::TaskSplit();
        End_ = mid;
      }
    }

    while (Current_ != End_) {
      Execute();
    }
    CurrentNode_.Reset();
    stack.Pop();
  }

private:
  void Execute() {
    Func_(Current_);
    ++Current_;
  }

  Scheduler &Sched_;
  size_t Current_;
  size_t End_;
  Func Func_;
  SplitData Split_;
  // ThreadId SupposedThread_;

  IntrusivePtr<TaskNode> CurrentNode_;
};

template <Balance balance, GrainSize grainSizeMode, typename F>
auto MakeInitialTask(EigenPoolWrapper &sched, TaskNode::NodePtr node, size_t from,
                     size_t to, F func, size_t threadCount,
                     size_t grainSize = 1) {
  return Task<F, balance, grainSizeMode, Initial::TRUE>{
      sched,
      std::move(node),
      from,
      to,
      std::move(func),
      SplitData{.Threads = {0, threadCount}, .GrainSize = grainSize},
      GetThreadIndex()};
}

template <Balance Balance, GrainSize GrainSizeMode, typename Func>
class RapidStartTask : public Eigen::RapidStart::Task {
public:
  RapidStartTask(Func&& func, EigenPoolWrapper& scheduler, size_t from, size_t to, IntrusivePtr<TaskNode> node)
    : Scheduler_{scheduler}
    , Func_(std::forward<Func>(func))
    , From_{from}
    , To_{to}
    , CurrentNode_{std::move(node)}
  {
    // std::cout << "RapidStartTask(this=" << (const void*)this << ")" << std::endl;
  }

  // ~RapidStartTask() override {
  //   std::cout << "~RapidStartTask(this=" << (const void*)this << ")" << std::endl;
  // }

  void operator()(int part, int totalParts) override {
    // std::cout << "RapidStartTask()(this=" << (const void*)this << ",part=" << part
    //           << ", totalParts=" << totalParts << ")" << std::endl;
    const size_t range = To_ - From_;
    const size_t step = range / totalParts;
    const size_t remainder = range % totalParts;
    const size_t from = From_ + part * step + std::min(remainder, static_cast<size_t>(part));
    part++;
    const size_t to = From_ + part * step + std::min(remainder, static_cast<size_t>(part));

    if (from == to) {
      return;
    }

    // std::cout << "in [" << From_ << ", " << To_ << ") for " << part << " / " << totalParts
    //           << " got [" << from << ", " << to << ")" << std::endl;

    for (size_t i = from; i < to; ++i) {
      Func_(i);
    }

    // IntoTask<Initial::FALSE>(from, to)();
  }

  template <Initial Initial>
  EigenPartitioner::Task<Func, Balance, GrainSizeMode, Initial> IntoTask(size_t from, size_t to) {
    return EigenPartitioner::Task<Func, Balance, GrainSizeMode, Initial>{
      Scheduler_,
      CurrentNode_,
      from,
      to,
      Func_,
      SplitData{.Threads = {.From = 0, .To = 1}}
    };
  }

  Func& TakeFunc() {
    return std::move(Func_);
  }

private:
  EigenPoolWrapper& Scheduler_;
  Func Func_;
  size_t From_ = 0;
  size_t To_ = 0;

  IntrusivePtr<TaskNode> CurrentNode_;
};

template <Balance balance, GrainSize grainSizeMode, typename F>
void ParallelFor(size_t from, size_t to, F func, int64_t grainSize) {
  grainSize = std::max<int64_t>(grainSize, 1);
  EigenPoolWrapper sched;
  // allocating only for top-level nodes
  TaskNode rootNode;
  IntrusivePtrAddRef(&rootNode); // avoid deletion
  if (detail::ThreadLocalTaskStack().IsEmpty()) {
    using RapidTask = RapidStartTask<balance, grainSizeMode, F>;
    auto rapid_task = std::make_unique<RapidTask>(std::move(func), sched, from, to, &rootNode);
    if (auto rejected = sched.try_run_rapid(std::move(rapid_task)); rejected) {
      auto& rejected_rapid = static_cast<RapidTask&>(*rejected);
      rejected_rapid.template IntoTask<Initial::TRUE>(from, to)();
    }
  } else {
    Task<F, balance, grainSizeMode, Initial::FALSE> task{
        sched,
        IntrusivePtr<TaskNode>(&rootNode),
        from,
        to,
        std::move(func),
        SplitData{.Threads = {0, static_cast<size_t>(Eigen::internal::GetNumThreads())},
                  .GrainSize = static_cast<size_t>(grainSize)}};
    task();
  }

  while (IntrusivePtrLoadRef(&rootNode) != 1) {
    sched.execute_something_else();
    // sched.join_main_thread();
  }
}

namespace detail {

template <typename F>
auto WrapAsTask(F&& func, const IntrusivePtr<TaskNode>& node) {
  return [&func, ref = node]() {
    TaskStack ts;
    auto& threadTaskStack = ThreadLocalTaskStack();
    threadTaskStack.Add(ts);

    std::forward<F>(func)();
    threadTaskStack.Pop();
  };
}
}

template <typename F1, typename F2>
void ParallelDo(F1&& fst, F2&& sec) {
  EigenPoolWrapper sched;
  // allocating only for top-level nodes
  TaskNode rootNode;
  IntrusivePtrAddRef(&rootNode); // avoid deletion

  sched.run(detail::WrapAsTask(std::forward<F1>(fst), &rootNode));
  std::forward<F2>(sec)();

  while (IntrusivePtrLoadRef(&rootNode) != 1) {
    sched.execute_something_else();
  }
}

template <GrainSize grainSizeMode, typename F>
void ParallelForTimespan(size_t from, size_t to, F func, int64_t grainsize) {
  ParallelFor<Balance::DELAYED, grainSizeMode, F>(from, to, std::move(func), grainsize);
}

template <typename F>
void ParallelForSimple(size_t from, size_t to, F func, int64_t grainsize) {
  ParallelFor<Balance::SIMPLE, GrainSize::DEFAULT, F>(from, to, std::move(func), grainsize);
}

template <typename F>
void ParallelForStatic(size_t from, size_t to, F func, int64_t grainsize) {
  ParallelFor<Balance::OFF, GrainSize::DEFAULT, F>(from, to, std::move(func), grainsize);
}

} // namespace EigenPartitioner

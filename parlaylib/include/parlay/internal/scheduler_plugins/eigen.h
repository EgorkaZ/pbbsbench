#ifndef PARLAY_INTERNAL_SCHEDULER_PLUGINS_EIGEN_H_
#define PARLAY_INTERNAL_SCHEDULER_PLUGINS_EIGEN_H_

#if !defined(PARLAY_EIGEN) || !defined(EIGEN_MODE)
#error "Undefined eigen"
#endif

#include <thread>

#include "eigen/eigen_pinner.h"
#include "eigen/poor_barrier.h"
#include "eigen/thread_index.h"
#include "eigen/timespan_partitioner.h"
#include "eigen/util.h"

namespace parlay {

inline size_t num_workers() {
  // cache result to avoid calling getenv on every call
  static size_t threads = []() -> size_t {
    if (const char *envThreads = std::getenv("BENCH_NUM_THREADS")) {
      return std::stoul(envThreads);
    }
    // left just for compatibility
    if (const char *envThreads = std::getenv("OMP_NUM_THREADS")) {
      return std::stoul(envThreads);
    }
    // left just for compatibility
    if (const char *envThreads = std::getenv("CILK_NWORKERS")) {
      return std::stoul(envThreads);
    }
    return std::thread::hardware_concurrency();
  }();
  return threads;
}

inline size_t worker_id() {
    return GetThreadIndex();
}

template <typename F>
inline void parallel_for(size_t start, size_t end, F&& f, long grainsize, bool) {
  return EigenPartitioner::ParallelFor(start, end, std::forward<F>(f), static_cast<size_t>(grainsize));
}

template <typename Lf, typename Rf>
inline void par_do(Lf&& left, Rf&& right, bool) {
  EigenPartitioner::ParallelDo(std::forward<Lf>(left), std::forward<Rf>(right));
}

inline void init_plugin_internal() {
    auto threadsNum = num_workers();
    Eigen::ThreadPool& ep = EigenPool();

    #if defined(EIGEN_MODE) and EIGEN_MODE != EIGEN_RAPID
    static EigenPinner pinner(threadsNum);
    #endif
}

template <typename... Fs>
void execute_with_scheduler(Fs...) {
  struct Illegal {};
  static_assert((std::is_same_v<Illegal, Fs> && ...), "parlay::execute_with_scheduler is only available in the Parlay scheduler and is not compatible with OpenMP");
}

}
#endif
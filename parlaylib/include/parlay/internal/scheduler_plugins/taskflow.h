#ifndef PARLAY_INTERNAL_SCHEDULER_PLUGINS_TASKFLOW_H_
#define PARLAY_INTERNAL_SCHEDULER_PLUGINS_TASKFLOW_H_

#include "taskflow/taskflow/taskflow.hpp"
#include "taskflow/taskflow/algorithm/for_each.hpp"
#include "eigen/modes.h"

namespace parlay {

namespace internal {

class NumericIterator {
public:
    using value_type = int;
    using difference_type = std::ptrdiff_t;
    using pointer = int*;
    using reference = int&;
    using iterator_category = std::forward_iterator_tag;

    NumericIterator(int current, int step = 1)
        : current(current), step(step) {}

    // Dereference operator
    reference operator*() {
        return current;
    }

    // Prefix increment
    NumericIterator& operator++() {
        current += step;
        return *this;
    }

    // Postfix increment
    NumericIterator operator++(int) {
        NumericIterator temp = *this;
        ++(*this);
        return temp;
    }

    // Equality operator
    bool operator==(const NumericIterator& other) const {
        return current == other.current;
    }

    // Inequality operator
    bool operator!=(const NumericIterator& other) const {
        return !(*this == other);
    }

private:
    int current;
    int step;
};

}  // namespace internal

static inline tf::Executor exec;

inline size_t num_workers() {
    return exec.num_workers();
}

inline size_t worker_id() {
    return exec.this_worker_id();
}

template <typename F>
inline void parallel_for(size_t start, size_t end, F&& f, long granularity, bool) {
    tf::Taskflow tf;

#if TASKFLOW_MODE == TASKFLOW_GUIDED
    tf::GuidedPartitioner execution_policy(granularity);
#elif TASKFLOW_MODE == TASKFLOW_DYNAMIC
    tf::DynamicPartitioner execution_policy(granularity);
#elif TASKFLOW_MODE == TASKFLOW_STATIC
    tf::StaticPartitioner execution_policy(granularity);
#elif TASKFLOW_MODE == TASKFLOW_RANDOM
    tf::RandomPartitioner execution_policy(granularity);
#else
    static_assert(false, "Wrong TASKFLOW_MODE mode");
#endif  // TASKFLOW_MODE
    
    tf.for_each(internal::NumericIterator(start), internal::NumericIterator(end), std::forward<F>(f), execution_policy);

    exec.run(tf).get();
}

template <typename Lf, typename Rf>
inline void par_do(Lf&& left, Rf&& right, bool) {
    tf::Taskflow tf;

    tf.emplace(std::forward<Lf>(left));
    auto fut = exec.run(tf);

    std::forward<Rf>(right)();

    fut.get();
}

inline void init_plugin_internal() {}

template <typename... Fs>
void execute_with_scheduler(Fs...) {
    struct Illegal {};
    static_assert((std::is_same_v<Illegal, Fs> && ...), "parlay::execute_with_scheduler is only available in the Parlay scheduler and is not compatible with OpenMP");
}

}  // namespace parlay

#endif  // PARLAY_INTERNAL_SCHEDULER_PLUGINS_TASKFLOW_H_

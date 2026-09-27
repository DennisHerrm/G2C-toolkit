// g2/parallel.h - Minimal parallel for loop.
//
// Deliberately uses std::thread instead of std::execution::par: the standard
// library's parallel algorithms strictly require Intel TBB on GCC and Clang
// and only work without an extra library under MSVC. For a tool that should
// build with nothing but a C++20 compiler, that is too high a price for
// saving a few lines.

#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace g2 {

inline unsigned defaultThreadCount() {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4u;
}

// Calls body(i, worker) for i in [0, count), spread across multiple threads.
//
// "worker" is a number from 0 to threads-1 and stays the same for a given
// thread. This allows keeping a separate counter per thread, without a lock
// and without data races.
//
// WHY AS A PARAMETER and not via thread_local:
//
// A thread_local outlives the call. If parallelFor runs serially - on a
// single core or with threads=1 - the executing thread is the main thread,
// and its value is still there on the next call. If fewer threads are then
// used, the stored index points past the end of the counter array. No test
// catches this, because it only strikes when the thread count changes.
//
// Exceptions from body are caught and rethrown after the join, so that an
// error in one file doesn't take the whole program down with it.
template <typename F>
void parallelForWorker(std::size_t count, F&& body, unsigned threads = 0) {
    if (count == 0) return;
    if (threads == 0) threads = defaultThreadCount();
    threads = std::min<unsigned>(threads, static_cast<unsigned>(count));

    if (threads <= 1) {
        for (std::size_t i = 0; i < count; ++i) body(i, 0u);
        return;
    }

    // Hand out work in chunks rather than one item at a time.
    //
    // One fetch_add per element is the bottleneck for short tasks: with
    // 30384 frames of a few microseconds each, eight threads fight over the
    // same cache line. Chunks of about 64 elements still spread the load
    // finely enough - the tasks vary in length, a fixed split would be
    // worse - but cost only a sixty-fourth of the synchronization.
    const std::size_t chunk = std::max<std::size_t>(1, std::min<std::size_t>(64, count / (threads * 8) + 1));

    std::atomic<std::size_t> next{0};
    std::mutex               errMutex;
    std::exception_ptr       firstError;

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (;;) {
                const std::size_t begin = next.fetch_add(chunk, std::memory_order_relaxed);
                if (begin >= count) return;
                const std::size_t end = std::min(begin + chunk, count);
                try {
                    for (std::size_t i = begin; i < end; ++i) body(i, t);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (!firstError) firstError = std::current_exception();
                    return;
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    if (firstError) std::rethrow_exception(firstError);
}

// Without a thread number, for loops that don't need one.
template <typename F>
void parallelFor(std::size_t count, F&& body, unsigned threads = 0) {
    parallelForWorker(
        count, [&](std::size_t i, unsigned) { body(i); }, threads);
}

}  // namespace g2

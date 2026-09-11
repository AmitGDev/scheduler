/*
    Scheduler.hpp
    Copyright (c) 2024-2026, Amit Gefen

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#ifndef AMITGDEV_SCHEDULER_HPP_
#define AMITGDEV_SCHEDULER_HPP_

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>

namespace amitgdev {

// Checked against the decayed types because the stored handler invokes its
// own copy of the callback as an rvalue, with rvalue copies of the arguments,
// not the caller's lvalues. Checking the deduced types (F&, T&) would wrongly
// reject callbacks taking rvalue references and wrongly accept ones that are
// only invocable as lvalues.
template <typename Callback, typename... Args>
concept NothrowTimerCallback =
    std::is_nothrow_invocable_v<std::decay_t<Callback>, std::uint64_t,
                                std::decay_t<Args>...>;

// Timer callbacks execute on a dedicated thread, isolating the scheduler's
// event loop from the caller's thread.
//
// Contract: all callbacks must be noexcept. If a callback throws despite
// satisfying the contract, std::terminate() is called.
//
// Lifetime: the caller must ensure no other thread is inside ScheduleTimer()
// when the scheduler is destroyed (the io_context does not outlive the
// object). Destroying it from another thread while a callback is running is
// fine; the destructor waits for that callback. Destroying it from inside one
// of its own callbacks is not, because the worker would try to join itself.
class Scheduler final {
 public:
  // The work guard keeps the event loop alive while the scheduler exists,
  // even when there are temporarily no outstanding asynchronous operations.
  Scheduler()
      : io_context_(),
        work_guard_(boost::asio::make_work_guard(io_context_)),
        io_context_thread_([this] { Service(); }) {}

  // stop() terminates the event loop.
  // The jthread then joins automatically when it is destroyed.
  // Pending handlers are not guaranteed to execute.
  ~Scheduler() {
    try {
      io_context_.stop();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
      // Intentionally ignore any exception from stop().
      // Destructor must not throw; no logging, no side effects.
    }
  }

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Scheduler(Scheduler&&) = delete;
  Scheduler& operator=(Scheduler&&) = delete;

  // Schedules a one-shot timer. The callback receives the timer ID followed by
  // the arguments supplied to this function.
  //
  // Important: The callback must be noexcept.
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  template <typename Callback, typename... Args>
    requires NothrowTimerCallback<Callback, Args...>
  void ScheduleTimer(const std::uint64_t timer_id, const std::uint32_t duration,
                     Callback&& callback, Args&&... callback_args) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto timer = std::make_shared<boost::asio::steady_timer>(
        io_context_, std::chrono::milliseconds(duration));

    timer->async_wait(
        [timer_id, timer, callback = std::forward<Callback>(callback),
         ... callback_args = std::forward<Args>(callback_args)](
            const boost::system::error_code& error) mutable noexcept {
          if (error) {
            // Cancellation during scheduler shutdown is expected.
            return;
          }

          std::move(callback)(timer_id, std::move(callback_args)...);
        });
  }

 private:
  // Runs the Asio event loop on the dedicated scheduler thread.
  void Service() { io_context_.run(); }

  boost::asio::io_context io_context_{};

  // Prevent the event loop from exiting while there are temporarily no
  // outstanding timers, allowing timers to be scheduled at any time.
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
      work_guard_;

  std::jthread io_context_thread_;
};

}  // namespace amitgdev

#endif  // AMITGDEV_SCHEDULER_HPP_
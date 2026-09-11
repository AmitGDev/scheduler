// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026, Amit Gefen

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>

#include "Scheduler.hpp"

using amitgdev::Scheduler;

// fputs instead of std::println: the error path must not throw again.
static void ReportError(const char* what) noexcept {
  (void)std::fputs(what, stderr);
  (void)std::fputs("\n", stderr);
}

static void TestGenericCallback() {
  std::cout << "* test lambda & functor callbacks\n";

  // Lambda callback
  const auto lambda_callback = [](std::uint64_t timer_id, int value) noexcept {
    try {
      std::osyncstream sync_stream(std::cout);
      sync_stream << "lambda callback for timer " << timer_id
                  << " expired. int value: " << value << "\n";
    } catch (const std::exception& e) {
      ReportError(e.what());
    }
  };

  // Functor callback
  struct MyFunctor {
    void operator()(const std::uint64_t timer_id, const std::string& str,
                    const int value) const noexcept {
      try {
        std::osyncstream sync_stream(std::cout);
        sync_stream << "functor callback for timer " << timer_id
                    << " expired. string data: " << str
                    << " int value: " << value << "\n";
      } catch (const std::exception& e) {
        ReportError(e.what());
      }
    }
  };

  Scheduler scheduler{};

  // Using lambda callback
  scheduler.ScheduleTimer(1, 2000, lambda_callback, 42);  // <--

  // Using functor callback
  const auto functor = MyFunctor{};
  scheduler.ScheduleTimer(2, 4000, functor, std::string("test functor string"),
                          2024);  // <--

  // Sleep for a while to let the timers expire
  std::this_thread::sleep_for(std::chrono::seconds(6));
}

// Function Callback__

static void OnTimer(const std::uint64_t timer_id) noexcept {
  try {
    std::osyncstream sync_stream(std::cout);
    sync_stream << "timer " << timer_id << " expired\n";
  } catch (const std::exception& e) {
    ReportError(e.what());
  }
}

static void TestFunctionCallback2Timers() {
  std::cout << "* test \"traditional\" function callback\n";

  Scheduler scheduler{};

  scheduler.ScheduleTimer(1, 2000, OnTimer);  // <--
  scheduler.ScheduleTimer(2, 4000, OnTimer);  // <--

  // Sleep for a while to let the timers expire
  std::this_thread::sleep_for(std::chrono::seconds(6));
}

// __Function Callback

static void TestMemberFunctionCallbackPlusExtraParameterPlusReschedule() {
  std::cout << "* test member function & instance callback\n";

  class Model final {
   public:
    void Test() {
      std::cout << "* test re-activate the timer 5 times\n";

      scheduler_.ScheduleTimer(1, 1000, std::bind_front(&Model::OnTimer, this),
                               static_cast<std::uint8_t>(5));

      std::this_thread::sleep_for(std::chrono::seconds(6));
    }

   private:
    Scheduler scheduler_;

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void OnTimer(const std::uint64_t timer_id,
                 const std::uint8_t count_down) noexcept {
      if (count_down == 0) {
        return;
      }

      try {
        std::osyncstream sync_stream(std::cout);
        sync_stream << "timer " << timer_id << " expired\n";

        const auto next = static_cast<std::uint8_t>(count_down - 1);
        scheduler_.ScheduleTimer(1, 1000,
                                 std::bind_front(&Model::OnTimer, this), next);
      } catch (const std::exception& e) {
        ReportError(e.what());
      }
    }
  };

  Model model{};
  model.Test();
}

namespace {

// Counts timer firings with a single atomic, so a callback needs no lock or
// allocation and stays trivially noexcept.
class FireCounter final {
 public:
  void Record() noexcept { count_.fetch_add(1); }

  [[nodiscard]] std::size_t Count() const noexcept { return count_.load(); }

 private:
  std::atomic<std::size_t> count_{0};
};

}  // namespace

// Builds a noexcept callback that counts into the given counter.
static auto MakeCountingCallback(FireCounter& counter) {
  return [&counter](std::uint64_t /*timer_id*/) noexcept { counter.Record(); };
}

// Throws instead of asserting so a failure flows through main()'s existing
// error path and produces EXIT_FAILURE, which CI can detect.
static void Expect(const bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

// Polls instead of blocking on a signalling primitive, which keeps the
// noexcept callbacks down to plain atomic operations.
template <typename Predicate>
static bool WaitUntil(const Predicate& predicate,
                      const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

// In the tests below the counter is declared before the scheduler so the
// worker is joined before the counter it writes to is destroyed.

static void TestZeroDelay() {
  std::cout << "* test end case: zero delay\n";

  FireCounter counter;
  Scheduler scheduler{};

  scheduler.ScheduleTimer(1, 0, MakeCountingCallback(counter));

  Expect(WaitUntil([&counter] { return counter.Count() == 1; },
                   std::chrono::seconds(1)),
         "zero-delay timer did not fire");
}

static void TestShutdownWithPendingTimer() {
  std::cout << "* test end case: shutdown drops pending timers\n";

  FireCounter counter;
  const auto start = std::chrono::steady_clock::now();

  {
    Scheduler scheduler{};
    scheduler.ScheduleTimer(1, 60000, MakeCountingCallback(counter));
  }

  // Shutdown must not wait for far-future timers, and must not run them.
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Expect(elapsed < std::chrono::seconds(5),
         "destruction blocked on a pending timer");
  Expect(counter.Count() == 0, "a pending timer ran during shutdown");
}

static void TestEndCases() {
  std::cout << "* test end cases\n";

  TestZeroDelay();
  TestShutdownWithPendingTimer();

  std::cout << "* end cases passed\n";
}

int main() {
  try {
    // False positive in Boost 1.92 Asio win_thread::start_thread: after a
    // failed Win32 call, it calls arg->destroy() and then throw_error(ec, ...).
    // boost::asio::detail::throw_error(ec, "thread.exit_event") throws only
    // when ec is non-empty; if ec is empty, it returns normally. The analyzer
    // cannot prove that the failed Win32 call set one, so it assumes execution
    // continues after arg->destroy() and reports a use-after-free.
    //
    // This suppression is here because TestGenericCallback() makes this
    // program's first Scheduler::ScheduleTimer() call. That call creates a
    // boost::asio::steady_timer and calls async_wait(), which is Asio's first
    // timer operation on this io_context; Asio consequently starts its internal
    // timer thread, reaching the third-party win_thread::start_thread() path
    // reported above.
    TestGenericCallback();  // NOLINT(clang-analyzer-cplusplus.NewDelete)
    TestFunctionCallback2Timers();
    TestMemberFunctionCallbackPlusExtraParameterPlusReschedule();
    TestEndCases();
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    ReportError(e.what());
    return EXIT_FAILURE;
  }
}

// main.cpp : This file contains the 'main' function. Program execution begins
// and ends there.
//

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <syncstream>
#include <system_error>
#include <thread>

#include "Scheduler.hpp"

using amitgdev::Scheduler;

// Logs to std::cerr without ever letting an exception escape. std::cerr's
// operator<< can throw if the stream's exception mask has been set, and this
// is the last line of defense for callback error reporting - an exception
// thrown while already handling an exception here would have nothing left
// to catch it.
// Justification for the swap risk below: prefix/detail are only ever fed
// a literal and a formatted/exception message at each call site, in that
// fixed order; swapping them would reorder log text, not change program
// behavior.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void LogNoThrow(std::string_view prefix,
                       std::string_view detail = {}) noexcept {
  try {
    std::cerr << prefix;
    if (!detail.empty()) {
      std::cerr << detail;
    }
    std::cerr << '\n';
  } catch (...) {  // NOLINT(bugprone-empty-catch): deliberately silent -
                   // this *is* the fallback path for a logging failure, so
                   // there is nothing safe left to do with it.
  }
}

// Reports which callback threw, for which timer, and why. Callers use this
// from inside a catch block that is itself the last line of defense for a
// noexcept callback, so nothing here may risk throwing a second exception:
// std::to_chars reports failure via a return code rather than an exception,
// and the buffer is filled with memcpy instead of any string-building
// facility (ostringstream, std::format, operator+) that could allocate and
// throw.
static void LogCallbackException(std::string_view callback_name,
                                 std::uint64_t timer_id,
                                 std::string_view detail) noexcept {
  std::array<char, 128> buffer{};
  char* cursor = buffer.data();
  char* const end = std::next(buffer.data(), buffer.size());

  const auto append = [&](std::string_view text) noexcept {
    const auto count = std::min(
        text.size(), static_cast<std::size_t>(std::distance(cursor, end)));
    std::memcpy(cursor, text.data(), count);
    std::advance(cursor, count);
  };

  append(callback_name);
  append(" callback threw for timer ");
  if (const auto result = std::to_chars(cursor, end, timer_id);
      result.ec == std::errc{}) {
    cursor = result.ptr;
  }
  append(": ");

  LogNoThrow(std::string_view(buffer.data(),
                              static_cast<std::size_t>(
                                  std::distance(buffer.data(), cursor))),
             detail);
}

static void TestGenericCallback() {
  std::cout << "* test lambda & functor callbacks\n";

  // Lambda callback
  const auto lambda_callback = [](uint64_t timer_id, int value) noexcept {
    try {
      std::osyncstream sync_stream(std::cout);
      sync_stream << "lambda callback for timer " << timer_id
                  << " expired. int value: " << value << "\n";
    } catch (const std::exception& e) {
      LogCallbackException("lambda", timer_id, e.what());
    } catch (...) {
      LogCallbackException("lambda", timer_id, "unknown exception");
    }
  };

  // Functor callback
  struct MyFunctor {
    void operator()(uint64_t timer_id, const std::string& str,
                    int value) const noexcept {
      try {
        std::osyncstream sync_stream(std::cout);
        sync_stream << "functor callback for timer " << timer_id
                    << " expired. string data: " << str
                    << " int value: " << value << "\n";
      } catch (const std::exception& e) {
        LogCallbackException("functor", timer_id, e.what());
      } catch (...) {
        LogCallbackException("functor", timer_id, "unknown exception");
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

static void OnTimer(uint64_t timer_id) noexcept {
  try {
    std::osyncstream sync_stream(std::cout);
    sync_stream << "timer " << timer_id << " expired\n";
  } catch (const std::exception& e) {
    LogCallbackException("OnTimer", timer_id, e.what());
  } catch (...) {
    LogCallbackException("OnTimer", timer_id, "unknown exception");
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

      scheduler_.ScheduleTimer(1, 1000, &Model::OnTimer, this,
                               static_cast<uint8_t>(5));  // <--

      // Sleep for a while to let the timer expire
      std::this_thread::sleep_for(std::chrono::seconds(6));
    }

   private:
    Scheduler scheduler_;

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void OnTimer(uint64_t timer_id,
                 uint8_t count_down) noexcept  // n - Extra parameter
    {
      if (count_down > 0) {
        try {
          std::osyncstream sync_stream(std::cout);
          sync_stream << "timer " << timer_id << " expired\n";

          const auto next = static_cast<uint8_t>(count_down - 1);
          scheduler_.ScheduleTimer(timer_id, 1000, &Model::OnTimer, this,
                                   next);  // <--
        } catch (const std::exception& e) {
          LogCallbackException("Model::OnTimer", timer_id, e.what());
        } catch (...) {
          LogCallbackException("Model::OnTimer", timer_id, "unknown exception");
        }
      }
    }
  };

  Model model{};
  model.Test();
}

static void TestEndCases() {
  std::cout << "* test end cases\n";
}

// Main
int main() {
  // main() must not let exceptions escape; a top-level catch is the
  // deliberate boundary rather than a per-call try/catch at each site.
  try {
    TestGenericCallback();
    TestFunctionCallback2Timers();
    TestMemberFunctionCallbackPlusExtraParameterPlusReschedule();
    TestEndCases();
  } catch (const std::exception& e) {
    LogNoThrow("unhandled exception: ", e.what());
    return 1;
  } catch (...) {
    LogNoThrow("unknown unhandled exception");
    return 1;
  }
}

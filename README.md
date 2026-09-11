# Scheduler

A lightweight header-only timer scheduler built on Boost.Asio. Timers execute callbacks asynchronously on a dedicated thread, keeping scheduling independent from the caller's thread.

## Purpose

`Scheduler` provides a small abstraction for scheduling one-shot callbacks after a specified delay.

It is intended for applications that need delayed or background timer-based work without requiring the rest of the application to manage a Boost.Asio event loop.

The scheduler supports:

- Lambda callbacks
- Function objects
- Free-function callbacks
- Member-function callbacks
- Additional callback arguments
- Self-rescheduling timers

## Interface

```cpp
template <typename Callback, typename... Args>
  requires NothrowTimerCallback<Callback, Args...>
void ScheduleTimer(std::uint64_t timer_id, std::uint32_t duration,
                   Callback&& callback, Args&&... callback_args);
```

- `timer_id`: Application-defined identifier, passed to the callback as its first argument
- `duration`: Delay in milliseconds (`std::uint32_t`, up to about 49.7 days)
- `callback`: Any callable that is `noexcept` when invoked as an rvalue with `(std::uint64_t, Args...)`
- `callback_args`: Extra arguments, stored by value in the timer and passed to the callback as rvalues

`Scheduler` itself is neither copyable nor movable.

## Design Rationale

### Dedicated scheduler thread

The scheduler owns a dedicated `std::jthread` that runs the Boost.Asio event loop.

This keeps timer processing independent from the thread that creates or schedules the timers:

```text
Application thread
       |
       | ScheduleTimer(...)
       |
       | continues working
       |
       v

Scheduler thread
       |
       | waits for timer
       |
       | invokes callback
       v
```

The application therefore does not need to run or integrate an Asio event loop itself.

### Boost.Asio

Boost.Asio provides the asynchronous timer and event-loop infrastructure.

The scheduler uses:

- `boost::asio::io_context`
- `boost::asio::steady_timer`
- `boost::asio::executor_work_guard`

`steady_timer` is used because timer durations should be based on a monotonic clock rather than changes to the system wall clock.

### Work guard

The executor work guard keeps the `io_context` alive even when there are temporarily no timers.

Without it, `io_context::run()` could return when no asynchronous work is pending, causing the scheduler thread to terminate before a later timer is scheduled.

### Timer lifetime

Each timer is created with `std::make_shared` and captured by its asynchronous handler until the wait completes.

This allows `ScheduleTimer()` to return immediately while ensuring that the underlying timer remains alive for the duration of the asynchronous operation.

### Callback exceptions

Timer callbacks execute on the scheduler thread and must be `noexcept`. This requirement is enforced at compile time by the `NothrowTimerCallback` concept, so a callable that is not `noexcept` is rejected by `ScheduleTimer()`. The concept checks the callable the way the scheduler will invoke it: as an rvalue, with rvalue copies of the arguments.

Callbacks are responsible for handling their own errors. If an exception escapes a callback despite the `noexcept` contract, `std::terminate()` is called and the process ends.

This also applies to work done inside a callback: `ScheduleTimer()` is not `noexcept` (for example, it can throw on allocation failure), so a callback that reschedules itself must catch exceptions from it.

## Usage

Include the scheduler header:

```cpp
#include "Scheduler.hpp"
```

Create a scheduler and schedule a timer:

```cpp
amitgdev::Scheduler scheduler;

scheduler.ScheduleTimer(
    1,
    2000,
    [](std::uint64_t timer_id, int value) noexcept {
        std::cout << "Timer " << timer_id
                  << " expired: " << value << '\n';
    },
    42);
```

The callback receives the timer ID as its first argument, followed by any additional arguments supplied to `ScheduleTimer()`. The delay is in milliseconds, so the timer above fires after two seconds.

The examples in this document keep callback bodies short. A callback that performs operations which can throw (such as stream output) should catch exceptions itself; `main.cpp` wraps each callback body in `try`/`catch` and reports failures through a small helper built on `std::fputs`, which cannot throw.

### Function callback

A regular function can be used directly:

```cpp
void OnTimer(std::uint64_t timer_id) noexcept {
    std::cout << "Timer " << timer_id << " expired\n";
}

amitgdev::Scheduler scheduler;

scheduler.ScheduleTimer(1, 2000, OnTimer);
```

### Functor callback

Function objects are also supported:

```cpp
struct TimerHandler {
    void operator()(std::uint64_t timer_id, int value) const noexcept {
        std::cout << "Timer " << timer_id
                  << " expired: " << value << '\n';
    }
};

TimerHandler handler;

scheduler.ScheduleTimer(1, 2000, handler, 42);
```

### Member-function callback

A member function can be scheduled together with its object using `std::bind_front` (from `<functional>`). The scheduler requires a `noexcept` callable, so this relies on `std::bind_front` propagating `noexcept` from the bound member function. libstdc++ does this only from GCC 15; older versions return a callable that is not `noexcept`, and `ScheduleTimer()` rejects it (see Requirements for the workaround):

```cpp
class Model final {
 public:
    void Start() {
        scheduler_.ScheduleTimer(
            1, 1000, std::bind_front(&Model::OnTimer, this), 42);
    }

 private:
    void OnTimer(std::uint64_t timer_id, int value) noexcept {
        // Handle timer expiration.
    }

    // Declared last: members are destroyed in reverse order, so the
    // scheduler (and its thread) is shut down before the other members.
    amitgdev::Scheduler scheduler_;
};
```

The scheduler does not take ownership of the object. The caller must ensure that the object remains alive until the timer callback has completed. Owning the `Scheduler` as the last data member of the target object, as above, ties the two lifetimes together.

## Rescheduling

Timers are one-shot. Recurring behavior can be implemented by scheduling another timer from the callback:

```cpp
void OnTimer(std::uint64_t timer_id, std::uint8_t count_down) noexcept {
    if (count_down == 0) {
        return;
    }

    try {
        std::osyncstream(std::cout) << "timer " << timer_id << " expired\n";

        scheduler_.ScheduleTimer(
            1, 1000, std::bind_front(&Model::OnTimer, this),
            static_cast<std::uint8_t>(count_down - 1));
    } catch (...) {
        // ScheduleTimer() can throw; the callback is noexcept, so the
        // exception must not escape.
    }
}
```

The first timer is scheduled with an initial count (for example, `static_cast<std::uint8_t>(5)`), which is passed as the extra argument.

This keeps the scheduler itself simple while allowing the caller to control the recurrence policy.

For example, the callback can implement:

```text
Timer
  |
  +-- 1 second --> callback(5)
                       |
                       +-- 1 second --> callback(4)
                                          |
                                          +-- 1 second --> callback(3)
                                                             |
                                                             +-- ...
```

## Timer IDs

The `timer_id` is an application-defined identifier passed to the callback.

`Scheduler` does not maintain a timer registry and does not enforce uniqueness.

For example:

```cpp
scheduler.ScheduleTimer(1, 1000, callback);
scheduler.ScheduleTimer(2, 2000, callback);
```

The callback can use the ID to determine which logical timer expired.

If timer IDs must be unique, that policy is the responsibility of the application.

## Threading Model

All timer callbacks are executed by the scheduler's dedicated thread.

Multiple timers therefore share the same `io_context` execution context, and their callbacks run one at a time. A long-running callback delays every timer that expires while it is running.

Timers can be scheduled from the application thread and from inside callbacks.

Callbacks should still synchronize access to application state that is also accessed by other threads. The scheduler does not provide synchronization for application-owned data.

## Shutdown

The scheduler stops its Asio event loop when it is destroyed:

```cpp
{
    amitgdev::Scheduler scheduler;

    scheduler.ScheduleTimer(1, 2000, callback);
}
```

Destruction:

1. Stops the `io_context`.
2. Causes the scheduler thread to leave the event loop.
3. Allows `std::jthread` to join the thread automatically when the members are destroyed.

A callback that is already running completes before the destructor returns. Pending timers are not guaranteed to execute during shutdown.

Do not destroy the scheduler from inside one of its own callbacks: the worker would try to join itself.

Likewise, the caller must ensure no other thread is inside `ScheduleTimer()` when the scheduler is destroyed: the `io_context` does not outlive the object. Destroying the scheduler from another thread while a callback is running is fine; the destructor waits for that callback.

## Examples

The repository includes a complete example demonstrating:

- Lambda callbacks
- Functor callbacks
- Free-function callbacks
- Member-function callbacks (`std::bind_front`)
- Additional callback arguments
- Timer self-rescheduling
- Exception-safe error reporting from `noexcept` callbacks

See [`main.cpp`](https://github.com/AmitGDev/scheduler/blob/main/src/main.cpp).

## Scope

`Scheduler` deliberately provides only timer scheduling. It does not attempt to be a general-purpose task scheduler.

It does not provide:

- Timer cancellation
- Timer ID registration or uniqueness enforcement
- Recurring timers (rescheduling is done by the callback)
- Cron-style scheduling
- Calendar-based scheduling
- A thread pool
- Ownership of callback target objects
- Synchronization of application-owned state

These responsibilities remain with the application.

## Requirements

- A C++23-compatible compiler (GCC 16 or later on Linux) and standard library
- Boost.Asio

Note: The member-function examples use `std::bind_front`, which must propagate `noexcept` to satisfy `NothrowTimerCallback`. Before GCC 15, libstdc++ does not do this. On an older toolchain, schedule a `noexcept` lambda instead:

```cpp
scheduler_.ScheduleTimer(
    1, 1000,
    [this](std::uint64_t timer_id, int value) noexcept {
        OnTimer(timer_id, value);
    },
    42);
```

## Build and Run

- [Build on Windows](https://github.com/AmitGDev/cpp-ci-template#build-on-windows)
- [Build on Linux](https://github.com/AmitGDev/cpp-ci-template#build-on-linux)

## Code Style

- [C++](https://github.com/AmitGDev/cpp-coding-standard)
- [Python](https://github.com/AmitGDev/cpp-ci-template#python)
- [CMake](https://github.com/AmitGDev/cpp-ci-template#cmake)

## License

MIT License. Copyright © 2025-2026 Amit Gefen.

---

**Last Updated:** 2026-10-25  
**Maintainer:** AmitGDev

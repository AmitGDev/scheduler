# Scheduler

A lightweight C++23 timer scheduler built on Boost.Asio. Timers execute callbacks asynchronously on a dedicated thread, keeping scheduling independent from the caller's thread.

## Purpose

`Scheduler` provides a small abstraction for scheduling one-shot callbacks after a specified delay.

It is intended for applications that need delayed or background timer-based work without requiring the rest of the application to manage a Boost.Asio event loop.

The scheduler supports:

* Lambda callbacks
* Function objects
* Free-function callbacks
* Member-function callbacks
* Additional callback arguments
* Self-rescheduling timers

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

* `boost::asio::io_context`
* `boost::asio::steady_timer`
* `boost::asio::executor_work_guard`

`steady_timer` is used because timer durations should be based on a monotonic clock rather than changes to the system wall clock.

### Work guard

The executor work guard keeps the `io_context` alive even when there are temporarily no timers.

Without it, `io_context::run()` could return when no asynchronous work is pending, causing the scheduler thread to terminate before a later timer is scheduled.

### Timer lifetime

Each timer is held by the asynchronous handler until the wait completes.

This allows `ScheduleTimer()` to return immediately while ensuring that the underlying timer remains alive for the duration of the asynchronous operation.

### Callback exceptions

Timer callbacks execute on the scheduler thread and must be `noexcept`. This requirement is enforced at compile time.

Callbacks are responsible for handling their own errors and must not allow exceptions to escape. This ensures that an exception cannot terminate the scheduler thread.

## Usage

Include the scheduler header:

```cpp
#include "Scheduler.h"
```

Create a scheduler and schedule a timer:

```cpp
amitgdev::Scheduler scheduler;

scheduler.ScheduleTimer(
    1,
    2000,
    [](std::uint64_t timer_id, int value) {
        std::cout << "Timer " << timer_id
                  << " expired: " << value << '\n';
    },
    42);
```

The callback receives the timer ID as its first argument, followed by any additional arguments supplied to `ScheduleTimer()`.

### Function callback

A regular function can be used directly:

```cpp
void OnTimer(std::uint64_t timer_id) {
    std::cout << "Timer " << timer_id << " expired\n";
}

amitgdev::Scheduler scheduler;

scheduler.ScheduleTimer(1, 2000, OnTimer);
```

### Functor callback

Function objects are also supported:

```cpp
struct TimerHandler {
    void operator()(std::uint64_t timer_id, int value) const {
        std::cout << "Timer " << timer_id
                  << " expired: " << value << '\n';
    }
};

TimerHandler handler;

scheduler.ScheduleTimer(1, 2000, handler, 42);
```

### Member-function callback

A member function can be scheduled together with its object:

```cpp
class Model {
 public:
    void OnTimer(std::uint64_t timer_id, int value) {
        // Handle timer expiration.
    }
};

Model model;
amitgdev::Scheduler scheduler;

scheduler.ScheduleTimer(
    1,
    2000,
    &Model::OnTimer,
    &model,
    42);
```

The scheduler does **not** take ownership of `model`. The caller must ensure that the object remains alive until the timer callback has completed.

## Rescheduling

Timers are one-shot. Recurring behavior can be implemented by scheduling another timer from the callback:

```cpp
void OnTimer(std::uint64_t timer_id, int remaining) {
    if (remaining == 0) {
        return;
    }

    scheduler_.ScheduleTimer(
        timer_id,
        1000,
        &Model::OnTimer,
        this,
        remaining - 1);
}
```

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

Multiple timers therefore share the same `io_context` execution context.

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
3. Allows `std::jthread` to join the thread automatically.

Pending timers are not guaranteed to execute during shutdown.

## Examples

The repository includes a complete example demonstrating:

* Lambda callbacks
* Functor callbacks
* Free-function callbacks
* Member-function callbacks
* Additional callback arguments
* Timer self-rescheduling

See [`main.cpp`](main.cpp).

## Requirements

* C++23
* Boost.Asio
* A C++23-compatible compiler
* Standard library support for `std::jthread` and `std::osyncstream` when building the example

## Scope

`Scheduler` deliberately provides only timer scheduling. It does not attempt to be a general-purpose task scheduler.

It does not provide:

* Timer cancellation by ID
* Timer ID registration or uniqueness enforcement
* Cron-style scheduling
* Calendar-based scheduling
* A thread pool
* Ownership of callback target objects
* Synchronization of application-owned state

These responsibilities remain with the application.

## Summary

`Scheduler` provides a small, focused interface for delayed asynchronous callbacks:

> Schedule a callback to execute after a specified duration on a dedicated thread.

Boost.Asio handles the asynchronous timer infrastructure, while `Scheduler` provides the application-facing API and manages the event-loop thread and timer lifetime.

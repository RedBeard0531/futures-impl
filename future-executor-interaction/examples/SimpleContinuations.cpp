#include <iostream>
#include <map>

#include "TestHelper.h"
#include <experimental/execution>
#include <futures.h>

namespace execution = std::experimental::execution;

struct inline_executor
{
public:
  friend bool operator==(
    const inline_executor&, const inline_executor&) noexcept { return true; }
  friend bool operator!=(
    const inline_executor&, const inline_executor&) noexcept { return false; }
  template<class Function>
  void execute(Function f) const noexcept { f(); }
  constexpr bool query(execution::oneway_t) { return true; }
  constexpr bool query(execution::twoway_t) { return false; }
  constexpr bool query(execution::single_t) { return true; }
};

struct simple_then_executor
{
public:
  friend bool operator==(
      const simple_then_executor&, const simple_then_executor&) noexcept {
    return true;
  }
  friend bool operator!=(
      const simple_then_executor&, const simple_then_executor&) noexcept {
    return false;
  }
  template<class Function, class Future>
  auto then_execute(Function func, Future fut) const noexcept
      -> std::experimental::standard_future<
          std::result_of_t<Function(std::decay_t<typename Future::value_type>&&)>, simple_then_executor> {

    // Chain by cosntructing new promise/future pair
    std::experimental::standard_promise<
      std::result_of_t<Function(std::decay_t<typename Future::value_type>&&)>>
        chainedPromise;

    // Get future directly because we know this future has that functionality
    // and to call .via would be recursive on this function
    auto returnFuture = chainedPromise.get_future(simple_then_executor{});

    // Then construct a completion token to perform the trivial enqueue action
    // which is all this particular executor type needs as it does not carry
    // chaining state.
    auto ct = execution::future_completion_token<std::decay_t<typename Future::value_type>>(
      [promise = std::move(chainedPromise),
       func = std::move(func)](typename Future::value_type&& val) mutable {
        // Completion token performs a trivial action on the source future's
        // executor.
        // Usually an operation that amounts to triggering execution of the
        // actual completion task on the next executor.
        // As for this executor completion is via a promise/future pair
        // we simulate this by creating a trivial inline_executor to run it on
        inline_executor{}.execute(
          [   promise = std::move(promise),
              val = std::move(val),
              func = std::move(func)]() mutable {
            promise.set_value(func(std::move(val)));
          });
      }
    );
    // End the future chain by setting the action it shold perform
    std::move(fut).set_callback(std::move(ct));
    return returnFuture;
  }
  constexpr bool query(execution::oneway_t) { return false; }
  constexpr bool query(execution::twoway_t) { return false; }
  constexpr bool query(execution::then_t) { return true; }
  constexpr bool query(execution::single_t) { return true; }
};


struct simple_then_executor_with_promise
{
public:

  // Local future and promise type
  // as example, these are used in a specific way by standard_future's
  // via_with_executor_promise.
  // In production code they should be strengthened but the point is to
  // demonstrate that these are not intended to be entirely general future/
  // promise pairs
  template<class T>
  struct LittleSharedState {
    std::function<void(T&&)> func;
  };
  template<class T>
  struct LocalFuture {
    std::shared_ptr<LittleSharedState<T>> state;
  };
  template<class T>
  struct Promise {
    Promise(){
      state = std::make_shared<LittleSharedState<T>>();
    }
    void set_value(T&& val) {
      // Don't do mutex because this future/promise pair is used in a single
      // use case
      auto f = std::move(state->func);
      f(std::move(val));
    }

    LocalFuture<T> get_future() {
      return LocalFuture<T>{state};
    }

    std::shared_ptr<LittleSharedState<T>> state;
  };

  template<class T>
  Promise<T> get_promise() {
    // In general could add shared state here, GPU queue details
    return Promise<T>{};
  }

  friend bool operator==(
      const simple_then_executor_with_promise&, const simple_then_executor_with_promise&) noexcept {
    return true;
  }
  friend bool operator!=(
      const simple_then_executor_with_promise&, const simple_then_executor_with_promise&) noexcept {
    return false;
  }

  // General version for any future type
  // Only implemented to satisfy type trait for now
  template<class Function, class Future>
  auto then_execute(Function /*func*/, Future /*fut*/) const noexcept
      -> std::experimental::standard_future<
          std::result_of_t<Function(std::decay_t<typename Future::value_type>&&)>, simple_then_executor_with_promise> {
    throw std::logic_error("Not used");
  }

  // Specialised version for internal future type
  template<class Function, class T>
  auto then_execute(Function func, LocalFuture<T> fut) const noexcept
      -> std::experimental::standard_future<
          std::result_of_t<Function(T&&)>, simple_then_executor_with_promise> {

    // Chain by cosntructing new promise/future pair
    std::experimental::standard_promise<
      std::result_of_t<Function(T&&)>>
        chainedPromise;

    // Get future directly because we know this future has that functionality
    // and to call .via would be recursive on this function
    auto returnFuture = chainedPromise.get_future(simple_then_executor_with_promise{});

    // Then construct a completion token to perform the trivial enqueue action
    // which is all this particular executor type needs as it does not carry
    // chaining state.
    std::function<void(T&&)> contFunc = [promise = std::move(chainedPromise),
       func = std::move(func)](T&& val) mutable {
        // Completion token performs a trivial action on the source future's
        // executor.
        // Usually an operation that amounts to triggering execution of the
        // actual completion task on the next executor.
        // As for this executor completion is via a promise/future pair
        // we simulate this by creating a trivial inline_executor to run it on
        inline_executor{}.execute(
          [   promise = std::move(promise),
              val = std::move(val),
              func = std::move(func)]() mutable {
            promise.set_value(func(std::move(val)));
          });
      };

    fut.state->func = std::move(contFunc);
    return returnFuture;
  }
  constexpr bool query(execution::oneway_t) { return false; }
  constexpr bool query(execution::twoway_t) { return false; }
  constexpr bool query(execution::then_t) { return true; }
  constexpr bool query(execution::single_t) { return true; }
};


struct stateful_then_executor
{
public:
  stateful_then_executor() {
    impl_ = std::make_shared<stateful_then_executor_impl>();
  }
  stateful_then_executor(const stateful_then_executor&) = default;
  stateful_then_executor& operator=(const stateful_then_executor&) = default;

  friend bool operator==(
      const stateful_then_executor& lhs, const stateful_then_executor& rhs) noexcept {
    return lhs.impl_ == rhs.impl_;
  }
  friend bool operator!=(
      const stateful_then_executor& lhs, const stateful_then_executor& rhs) noexcept {
    return !(lhs==rhs);
  }
  // Overload for when future types match
  template<class Function, class Future>
  auto then_execute(Function func, Future fut) const noexcept
      -> std::experimental::standard_future<
          std::result_of_t<Function(std::decay_t<typename Future::value_type>&&)>, stateful_then_executor> {
    execution::future_completion_token<
      std::decay_t<typename Future::value_type>> token;

    // Chain by constructing new promise/future pair
    std::experimental::standard_promise<
      std::result_of_t<Function(std::decay_t<typename Future::value_type>&&)>>
        chainedPromise;

    // Get future directly because we know this future has that functionality
    // and to call .via would be recursive on this function
    auto returnFuture = chainedPromise.get_future(stateful_then_executor{});

    // Acquire lock and setup map state, task and completion token
    {
      std::lock_guard<std::mutex> lck{impl_->mapMutex};
      auto sharedValue =
        std::make_shared<std::atomic<std::decay_t<typename Future::value_type>>>();
      unsigned key = ++impl_->counter;

      // Task is the work that completes the promise
      // Stored inside the executor in a map
      // The task is a simple worker wrapper that reads from some communication
      // variable (an atomic in shared memory in this simple case), runs the
      // continuation, and puts that result in the chained promise
      std::function<void()> task{
        [sharedValue,
         func = std::move(func),
         promise = std::move(chainedPromise)]() mutable {
          promise.set_value(func(*std::move(sharedValue)));
        }
      };
      impl_->tasks[key] = std::move(task);

      // Completion token is what the future will do to get this executor to work
      // executor that does the dequeue.
      // Still blocking technically (as executor is inline) but a non-blocking
      // executor would remove that problem
      std::function<void(std::decay_t<typename Future::value_type>&&)> ctFunc{
        [sharedValue, impl = std::move(impl_), key](
            std::decay_t<typename Future::value_type>&& val){

          // Completion token will trivially enqueue workTrigger onto the
          // destination executor
          // workTrigger, when run, extracts the task from the pending work map
          // and enqueues is.
          // On a non-blocking executor, all of this work can be achieved
          // entirely non-blocking
          auto workTrigger =
            [sharedValue, impl = std::move(impl), key, val = std::move(val)](){
              std::function<void()> task;
              // Set the shared value
              *sharedValue = std::move(val);
              // Obtain task
              {
                std::lock_guard<std::mutex> lck{impl->mapMutex};
                task = std::move(impl->tasks[key]);
                impl->tasks.erase(key);
              }
              // Enqueue the task as appropriate, for simplicity here in an
              // inline_executor
              inline_executor{}.execute(std::move(task));
            };
          inline_executor{}.execute(std::move(workTrigger));
        }
      };
      token = execution::future_completion_token<
        std::decay_t<typename Future::value_type>>{std::move(ctFunc)};
    }

    // End the future chain by setting the action it shold perform
    std::move(fut).set_callback(std::move(token));
    return returnFuture;
  }
  constexpr bool query(execution::oneway_t) { return false; }
  constexpr bool query(execution::twoway_t) { return false; }
  constexpr bool query(execution::then_t) { return true; }
  constexpr bool query(execution::single_t) { return true; }

private:
  struct stateful_then_executor_impl {
    std::mutex mapMutex;
    unsigned counter = 0;
    std::map<unsigned, std::function<void()>> tasks;
  };

  std::shared_ptr<stateful_then_executor_impl> impl_;
};



namespace std {
namespace experimental {
namespace execution {
template<class T>
struct executor_future<simple_then_executor, T> {
  using type = std::experimental::standard_future<T, simple_then_executor>;
};
template<class T>
struct executor_future<stateful_then_executor, T> {
  using type = std::experimental::standard_future<T, stateful_then_executor>;
};
} // execution
} // experimental
} // std


int main() {
  {
    const std::string TESTNAME = "Simple via then and get with inline executor";
    const int EXPECTED = 5;

    std::experimental::standard_promise<int> promise;

    std::experimental::standard_future<int, inline_executor> f =
      promise.get_semi_future().via(inline_executor{}).then([](auto&& val) {
        return val + 2;
      });

    std::thread setter{[&promise](){
      promise.set_value(3);
    }};

    auto result = f.get();
    setter.join();
    std::cerr << TESTNAME << "\t" << check(result, EXPECTED) << "\n";
  }
  {
    const std::string TESTNAME =
      "Simple via then and get with then_executor";
    const int EXPECTED = 6;

    std::experimental::standard_promise<int> promise;

    std::experimental::standard_future<int, simple_then_executor> f =
      promise.get_semi_future().via(simple_then_executor{}).then(
        [](int&& val) {
          return val + 3;
        });

    std::thread setter{[&promise](){
      promise.set_value(3);
    }};

    auto result = f.get();
    setter.join();
    std::cerr << TESTNAME << "\t" << check(result, EXPECTED) << "\n";
  }
  {
    const std::string TESTNAME =
      "Simple via then and get with stateful then_executor";
    const int EXPECTED = 12;

    std::experimental::standard_promise<int> promise;

    std::experimental::standard_future<int, stateful_then_executor> f =
      promise.get_semi_future().via(stateful_then_executor{}).then(
        [](int&& val) {
          return val + 3;
        });

    std::thread setter{[&promise](){
      promise.set_value(9);
    }};

    auto result = f.get();
    setter.join();
    std::cerr << TESTNAME << "\t" << check(result, EXPECTED) << "\n";
  }

  // Use then_executor with promise.
  // This should the alternative construction that does not rely on a public
  // set_callback on the future
  {
    const std::string TESTNAME =
      "Simple via then and get with simple_then_executor_with_promise";
    const int EXPECTED = 3;

    std::experimental::standard_promise<int> promise;

    // Do via and get
    // This is only for demonstration - to add continuations too we'd want
    // to also special case the executor to have its own future type or it
    // will always have to do then_execute with special chaining.
    auto f =
      promise.get_semi_future().via_with_executor_promise(simple_then_executor_with_promise{});

    std::thread setter{[&promise](){
      promise.set_value(3);
    }};

    auto result = f.get();
    setter.join();
    std::cerr << TESTNAME << "\t" << check(result, EXPECTED) << "\n";
  }

  return 0;
}

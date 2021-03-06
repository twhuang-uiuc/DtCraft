/******************************************************************************
 *                                                                            *
 * Copyright (c) 2018, Tsung-Wei Huang and Martin D. F. Wong,                 *
 * University of Illinois at Urbana-Champaign (UIUC), IL, USA.                *
 *                                                                            *
 * All Rights Reserved.                                                       *
 *                                                                            *
 * This program is free software. You can redistribute and/or modify          *
 * it in accordance with the terms of the accompanying license agreement.     *
 * See LICENSE in the top-level directory for details.                        *
 *                                                                            *
 ******************************************************************************/

#ifndef DTC_CONCURRENT_THREADPOOL_HPP_
#define DTC_CONCURRENT_THREADPOOL_HPP_

#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <deque>
#include <algorithm>
#include <thread>
#include <future>
#include <functional>

namespace dtc {

// Class: Threadpool
class Threadpool {

  enum class Signal {
    STANDARD,
    SHUTDOWN
  };

  template <typename T>
  struct StrongMovable {
  
    StrongMovable(T&& rhs) : object(std::move(rhs)) {}
    StrongMovable(const StrongMovable& other) : object(std::move(other.object)) {}

    T& get() { return object; }
    
    mutable T object; 
  };

  public:

    inline Threadpool(unsigned = 0);
    inline ~Threadpool();
    
    template <typename C>
    auto async(C&&, const Signal = Signal::STANDARD);
    
    inline void shutdown();
    inline void spawn(unsigned);
    
    inline size_t num_tasks() const;
    inline size_t num_workers() const;

    inline bool is_worker() const;
    
  private:

    mutable std::mutex _mutex;

    std::condition_variable _worker_signal;
    std::deque<std::function<Signal()>> _task_queue;
    std::vector<std::thread> _threads;
    std::unordered_set<std::thread::id> _worker_ids;
};

// Constructor
inline Threadpool::Threadpool(unsigned N) {
  spawn(N);
}

// Destructor
inline Threadpool::~Threadpool() {
  shutdown();
}

// Function: num_tasks
// Return the number of "unfinished" tasks. Notice that this value is not necessary equal to
// the size of the task_queue since the task can be popped out from the task queue while 
// not yet finished.
inline size_t Threadpool::num_tasks() const {
  return _task_queue.size();
}

inline size_t Threadpool::num_workers() const {
  return _threads.size();
}

inline bool Threadpool::is_worker() const {
  std::scoped_lock<std::mutex> lock(_mutex);
  return _worker_ids.find(std::this_thread::get_id()) != _worker_ids.end();
}

// Procedure: spawn
// The procedure spawns "n" threads monitoring the task queue and executing each task. After the
// task is finished, the thread reacts to the returned signal.
inline void Threadpool::spawn(unsigned N) {

  if(is_worker()) {
    throw std::runtime_error("Worker cannot spawn threads");
  }

  
  for(size_t i=0; i<N; ++i) {

    _threads.emplace_back([this] () -> void { 

      {  // Acquire lock
        std::scoped_lock<std::mutex> lock(_mutex);
        _worker_ids.insert(std::this_thread::get_id());         
      }

      auto stop = bool {false};
      while(!stop) {
        decltype(_task_queue)::value_type task;

        { // Acquire lock. --------------------------------------------------------------------------
          std::unique_lock<std::mutex> lock(_mutex);
          _worker_signal.wait(lock, [this] () { return _task_queue.size() != 0; });
          task = std::move(_task_queue.front());
          _task_queue.pop_front();
        } // Release lock. --------------------------------------------------------------------------

        // Execute the task and react to the returned signal.
        switch(task()) {
          case Signal::SHUTDOWN:
            stop = true;
          break;      

          default:
          break;
        };

      } // End of worker loop.

      {  // Acquire lock
        std::scoped_lock<std::mutex> lock(_mutex);
        _worker_ids.erase(std::this_thread::get_id());         
      }

    });
  }
}

// Function: async
// Insert a callable task to the end of the queue. The input must be a callable object. A normal
// callable object the task is associated with STANDARD task signal. Sicne each task stored in the 
// task queue must be copy-constructible and copy-assignable, the input callable object is wrapped
// into a strong movable object which replaces the copy constructor with move constructor.
// Notice that the procedure is concurrent-safe.
template<typename C>
auto Threadpool::async(C&& c, const Signal sig) {

  using R = std::result_of_t<C()>;
  
  std::promise<R> p;
  auto fu = p.get_future();
  
  // No worker, do this immediately.
  if(_threads.empty()) {
    if constexpr(std::is_same_v<void, R>) {
      c();
      p.set_value();
    }
    else {
      p.set_value(c());
    }
  }
  // Schedule a thread to do this.
  else {
    {
      std::unique_lock lock(_mutex);
      _task_queue.emplace_back(
        [p=StrongMovable(std::move(p)), c=std::forward<C>(c), ret=sig] () mutable { 
          if constexpr(std::is_same_v<void, R>) {
            c();
            p.get().set_value();
          }
          else {
            p.get().set_value(c());
          }
          return ret;
        }
      );
    }
    _worker_signal.notify_one();
  }
  return fu;

  //-----------------------------------------------------------------------------------------------
  // Using packaged_task to propagate the exception automatically.

  //std::packaged_task<R()> task (std::forward<C>(c));
  //auto fu = task.get_future();
  //{
  //  std::unique_lock<std::mutex> lock(_mutex);
  //  _task_queue.emplace_back(
  //    [sm=StrongMovable<decltype(task)>(std::move(task)), ret=sig] () { 
  //      sm.object();
  //      return ret;
  //    }
  //  );
  //  ++_num_tasks;
  //}
  //_worker_signal.notify_one();
  //return fu;
}

// Procedure: shutdown
// Remove a given number of workers. Notice that only the master can call this procedure.
inline void Threadpool::shutdown() {
  
  if(is_worker()) {
    throw std::runtime_error("Worker cannot shut down thread pool");
  }

  for(size_t i=0; i<_threads.size(); ++i) {
    async([](){}, Signal::SHUTDOWN);
  }
  
  for(auto& t : _threads) {
    t.join();
  }

  _threads.clear();
}

};  // End of namespace dtc. ----------------------------------------------------------------------



#endif




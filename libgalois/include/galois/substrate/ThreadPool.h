/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#ifndef GALOIS_SUBSTRATE_THREADPOOL_H
#define GALOIS_SUBSTRATE_THREADPOOL_H

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

#include "galois/substrate/CacheLineStorage.h"
#include "galois/substrate/HWTopo.h"

namespace galois::substrate::internal {

template <typename tpl, int s, int r>
struct ExecuteTupleImpl {
  static inline void execute(tpl& cmds) {
    std::get<s>(cmds)();
    ExecuteTupleImpl<tpl, s + 1, r - 1>::execute(cmds);
  }
};

template <typename tpl, int s>
struct ExecuteTupleImpl<tpl, s, 0> {
  static inline void execute(tpl&) {}
};

} // namespace galois::substrate::internal

namespace galois::substrate {

class ThreadPool {
  friend class SharedMem;

protected:
  struct shutdown_ty {}; //! type for shutting down thread
  struct fastmode_ty {
    bool mode;
  }; //! type for setting fastmode
  struct dedicated_ty {
    std::function<void(void)> fn;
  }; //! type to switch to dedicated mode

  //! Per-thread mailboxes for notification
  struct per_signal {
    std::condition_variable cv;
    std::mutex m;
    unsigned wbegin, wend;
    std::atomic<int> done;
    std::atomic<int> fastRelease;
    ThreadTopoInfo topo;

    void wakeup(bool fastmode) {
      if (fastmode) {
        done        = 0;
        fastRelease = 1;
      } else {
        std::lock_guard<std::mutex> lg(m);
        done = 0;
        cv.notify_one();
        // start.release();
      }
    }

    void wait(bool fastmode) {
      if (fastmode) {
        while (!fastRelease.load(std::memory_order_relaxed)) {
          asmPause();
        }
        fastRelease = 0;
      } else {
        std::unique_lock<std::mutex> lg(m);
        cv.wait(lg, [=] { return !done; });
        // start.acquire();
      }
    }
  };

  thread_local static per_signal my_box;

  MachineTopoInfo mi;
  std::vector<per_signal*> signals;
  std::vector<std::thread> threads;
  unsigned reserved;
  unsigned masterFastmode;
  bool running;
  std::function<void(void)> work;

  //Added by Chinmay
  int numThreads1;
  int numThreads2;
  std::function<void(void)> work1;
  std::function<void(void)> work2;
  // std::vector<int>work1_tids;
  // std::vector<int>work2_tids;



  //! destroy all threads
  void destroyCommon();

  //! Initialize a thread
  void initThread(unsigned tid);

  //! main thread loop
  void threadLoop(unsigned tid);

  //! spin up for run
  void cascade(bool fastmode);

  //! spin down after run
  void decascade();

  //! execute work on num threads
  void runInternal(unsigned num);
  void runInternal();

  ThreadPool();

public:

  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  //! execute work on all threads
  //! a simple wrapper for run
  template <typename... Args>
  void run(unsigned num, Args&&... args) {
    struct ExecuteTuple {
      //      using Ty = std::tuple<Args...>;
      std::tuple<Args...> cmds;

      void operator()() {
        internal::ExecuteTupleImpl<
            std::tuple<Args...>, 0,
            std::tuple_size<std::tuple<Args...>>::value>::execute(this->cmds);
      }
      ExecuteTuple(Args&&... args) : cmds(std::forward<Args>(args)...) {}
    };
    // paying for an indirection in work allows small-object optimization in
    // std::function to kick in and avoid a heap allocation
    ExecuteTuple lwork(std::forward<Args>(args)...);
    work = std::ref(lwork);
    // work =
    // std::function<void(void)>(ExecuteTuple(std::forward<Args>(args)...));
    assert(num <= getMaxThreads());
    // std::cout << "Calling runInternal" << std::endl;
    // printf("Calling runInternal\n");
    runInternal(num);
    // printf("runInternal complete\n");
    // std::cout << "runInternal complete" << std::endl;
  }

  template <typename... splitArgs>
    struct ExecuteTuple {
      //      using Ty = std::tuple<Args...>;
      std::tuple<splitArgs...> cmds;

      void operator()() {
        internal::ExecuteTupleImpl<
            std::tuple<splitArgs...>, 0,
            std::tuple_size<std::tuple<splitArgs...>>::value>::execute(this->cmds);
      }

      ExecuteTuple(splitArgs&&... ags) : cmds(std::forward<splitArgs>(ags)...) {}
    };

  template <typename... Args>
  void run_do_specified(int num1, int num2, Args&&... args) {
    
    // template <typename... splitArgs>
    // struct ExecuteTuple {
    //   //      using Ty = std::tuple<Args...>;
    //   std::tuple<splitArgs...> cmds;

    //   void operator()() {
    //     internal::ExecuteTupleImpl<
    //         std::tuple<splitArgs...>, 0,
    //         std::tuple_size<std::tuple<splitArgs...>>::value>::execute(this->cmds);
    //   }

    //   ExecuteTuple(splitArgs&&... ags) : cmds(std::forward<splitArgs>(ags)...) {}

      // ExecuteTuple(std::function<void(void)>& w1, std::function<void(void)>& w2 ,Args&&... args) 
      // : cmds(std::forward<Args>(args)...) {
      //   auto tpl1 = std::make_tuple(std::get<0>cmds,std::get<1>cmds,std::get<2>cmds);
      //   auto tpl2 = std::make_tuple(std::get<3>cmds,std::get<4>cmds,std::get<5>cmds);
      //   w1 = std::ref(tpl1);  
      // }

      // void set_work(std::function<void(void)>& w1, std::function<void(void)>& w2){
      //   auto tpl1 = std::make_tuple(std::get<0>cmds,std::get<1>cmds,std::get<2>cmds);
      //   auto tpl2 = std::make_tuple(std::get<3>cmds,std::get<4>cmds,std::get<5>cmds);
      //   // ExecuteTuple lwork1 = 
      //   //  std::apply([](auto&&... args2) { return ExecuteTuple{std::forward<decltype(args2)>(args2)...}; }, args);
      //   ExecuteTuple lwork1 = std::make_from_tuple<ExecuteTuple>(std::forward<decltype(tpl1)>(tpl1));
      //   ExecuteTuple lwork2 = std::make_from_tuple<ExecuteTuple>(std::forward<decltype(tpl2)>(tpl2));
      // }
    // };
    // paying for an indirection in work allows small-object optimization in
    // std::function to kick in and avoid a heap allocation
    auto tpl = std::tuple<Args...>(args...);

    // auto tpl1 = std::make_tuple(std::get<0>(tpl),std::get<1>(tpl),std::get<2>(tpl));
    // ExecuteTuple lwork1 = std::make_from_tuple<ExecuteTuple>(std::forward<decltype(tpl1)>(tpl1)...);
    // ExecuteTuple lwork1 = std::make_from_tuple<ExecuteTuple>(tpl1);
    ExecuteTuple lwork1(std::move<decltype(std::get<0>(tpl))>(std::get<0>(tpl)),
      std::move<decltype(std::get<1>(tpl))>(std::get<1>(tpl)),
      std::move<decltype(std::get<2>(tpl))>(std::get<2>(tpl)));
    work1 = std::ref(lwork1);

    if(num2 > 0){
      // auto tpl2 = std::make_tuple(std::get<3>(tpl),std::get<4>(tpl),std::get<5>(tpl));
      // ExecuteTuple lwork2 = std::make_from_tuple<ExecuteTuple>(std::forward<decltype(tpl2)>(tpl2)...);
      // ExecuteTuple lwork2 = std::make_from_tuple<ExecuteTuple>(tpl2);
      // ExecuteTuple lwork2(std::get<3>(tpl),std::get<4>(tpl),std::get<5>(tpl));
      // printf("ThreadPool.h: num2 > 0 so setting up work2\n");
      ExecuteTuple lwork2(std::move<decltype(std::get<3>(tpl))>(std::get<3>(tpl)),
      std::move<decltype(std::get<4>(tpl))>(std::get<4>(tpl)),
      std::move<decltype(std::get<5>(tpl))>(std::get<5>(tpl)));
      work2 = std::ref(lwork2);
      // printf("ThreadPool.h: work2 has been set up\n");  
    }
  
    numThreads1 = num1;
    numThreads2 = num2;

    // for(int i = 0; i < numThreads1; i++){
    //   // printf("Pushed %d into work1_tids\n",i);
    //   work1_tids.push_back(i);
    // }
    // for(int i = numThreads1; i < numThreads1 + numThreads2; i++){
    //   work2_tids.push_back(i);
    //   // printf("Pushed %d into work2_tids. Printing it: %d\n",i, work2_tids[0]);
    // }
    
    // assert(num <= getMaxThreads());
    // printf("Calling runInternal\n");
    runInternal();
    // printf("runInternal complete\n");
  }



  // template <typename... Args>
  // void setWork(int num, int which, Args&&... args) {

  //   struct ExecuteTuple {
  //     //      using Ty = std::tuple<Args...>;
  //     std::tuple<Args...> cmds;

  //     void operator()() {
  //       internal::ExecuteTupleImpl<
  //           std::tuple<Args...>, 0,
  //           std::tuple_size<std::tuple<Args...>>::value>::execute(this->cmds);
  //     }
  //     ExecuteTuple(Args&&... args) : cmds(std::forward<Args>(args)...) {}
  //   };
    
  //   ExecuteTuple lwork(std::forward<Args>(args)...);
    
  //   if(which == 1){
  //     work1 = std::ref(lwork);
  //     numThreads1 = num;

  //     //For debugging
  //     // runInternal(); 
  //     // printf("ThreadPool.h: DEBUGGING COMPLETE(LINE 214)\n"); 
  //   }
  //   else if(which == 2){
  //     work2 = std::ref(lwork);
  //     numThreads2 = num; 
  //   }
  // }

  /*
  template <typename F2, typename F3, typename F5, typename F6>
  void run_do_specified(int num1, int num2, std::tuple<std::function<void(void)>,F2&,F3&> tuple1,
    std::tuple<std::function<void(void)>, F5&, F6&> tuple2) {
    
    using Ty1 = decltype(tuple1);
    using Ty2 = decltype(tuple2); 

    struct ExecuteTuple {
      //      using Ty = std::tuple<Args...>;
      Ty1 cmds1;
      Ty2 cmds2;
      int which;

      void operator()() {
        if(which == 1){
          internal::ExecuteTupleImpl<Ty1, 0, 3>::execute(this->cmds1);  
        }
        else if(which == 2){
          internal::ExecuteTupleImpl<Ty2, 0, 3>::execute(this->cmds2);   
        }
        
      }

      // ExecuteTuple(std::tuple<Ty1> tpl1, std::tuple<Ty2> tpl2, int type){
      //   cmds1 = tpl1;
      //   cmds2 = tpl2;
      //   which = type;
      // }

      ExecuteTuple(){
        cmds1 = std::make_tuple(nullptr, nullptr, nullptr);
        cmds2 = std::make_tuple(nullptr, nullptr, nullptr);
        which = 0;
      }

      // ExecuteTuple(std::tuple<Ty1> tpl){
      //   cmds1 = tpl;
      //   which = 1;
      // }

      // ExecuteTuple(std::tuple<Ty2> tpl){
      //   cmds2 = tpl;
      //   which = 2;
      // }
    };

    // ExecuteTuple lwork1(tuple1, tuple2, 1);
    // ExecuteTuple lwork2(tuple1, tuple2, 2);

    ExecuteTuple lwork1();
    ExecuteTuple lwork2();

    lwork1.cmds1 = tuple1;
    lwork1.cmds2 = tuple2;
    lwork1.which = 1;

    lwork2.cmds1 = tuple1;
    lwork2.cmds2 = tuple2;
    lwork2.which = 2;

  
    if(num1 > 0){
      work1 = std::ref(lwork1);  
    }
    if(num2 > 0){
      work2 = std::ref(lwork2);  
    }
    
    numThreads1 = num1;  
    numThreads2 = num2; 

    runInternal();
  }
  */

/* Added by Chinmay */
  void run() {
    runInternal();
  }

  //Added by Chinmay
  // int getTIDIndex(int workloadType, unsigned id){
  //   int index = 0;
  //   printf("Inside getTIDIndex for tid = %u, workloadType = %d\n",id,workloadType);
  //   if(workloadType == 1){
  //     for(int i = 0; i < numThreads1; i++){
  //       if(work1_tids[i] == (int)id){
  //         return i;
  //       }
  //     }
  //   }
  //   else if(workloadType == 2){
  //     for(int i = 0; i < numThreads2; i++){
  //       printf("work2_tids[i] = %d, id = %d\n",work2_tids[i],(int)id);
  //       if(work2_tids[i] == (int)id){
  //         return i;
  //       }
  //     }
  //   }
  //   printf("Error:Wrong tid %u passed to getTIDIndex\n",id);
  //   return index;
  // }

  //! run function in a dedicated thread until the threadpool exits
  void runDedicated(std::function<void(void)>& f);

  // experimental: busy wait for work
  void burnPower(unsigned num);
  // experimental: leave busy wait
  void beKind();

  bool isRunning() const { return running; }

  //! return the number of non-reserved threads in the pool
  unsigned getMaxUsableThreads() const { return mi.maxThreads - reserved; }
  //! return the number of threads supported by the thread pool on the current
  //! machine
  unsigned getMaxThreads() const { return mi.maxThreads; }
  unsigned getMaxCores() const { return mi.maxCores; }
  unsigned getMaxSockets() const { return mi.maxSockets; }
  unsigned getMaxNumaNodes() const { return mi.maxNumaNodes; }

  unsigned getLeaderForSocket(unsigned pid) const {
    for (unsigned i = 0; i < getMaxThreads(); ++i)
      if (getSocket(i) == pid && isLeader(i))
        return i;
    abort();
  }

  bool isLeader(unsigned tid) const {
    return signals[tid]->topo.socketLeader == tid;
  }
  unsigned getSocket(unsigned tid) const { return signals[tid]->topo.socket; }
  unsigned getLeader(unsigned tid) const {
    return signals[tid]->topo.socketLeader;
  }
  unsigned getCumulativeMaxSocket(unsigned tid) const {
    return signals[tid]->topo.cumulativeMaxSocket;
  }
  unsigned getNumaNode(unsigned tid) const {
    return signals[tid]->topo.numaNode;
  }

  static unsigned getTID() { return my_box.topo.tid; }
  static bool isLeader() { return my_box.topo.tid == my_box.topo.socketLeader; }
  static unsigned getLeader() { return my_box.topo.socketLeader; }
  static unsigned getSocket() { return my_box.topo.socket; }
  static unsigned getCumulativeMaxSocket() {
    return my_box.topo.cumulativeMaxSocket;
  }
  static unsigned getNumaNode() { return my_box.topo.numaNode; }
};

/**
 * return a reference to system thread pool
 */
ThreadPool& getThreadPool(void);

} // namespace galois::substrate

namespace galois::substrate::internal {

void setThreadPool(ThreadPool* tp);

} // namespace galois::substrate::internal

#endif

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

#include "galois/substrate/ThreadPool.h"
#include "galois/substrate/EnvCheck.h"
#include "galois/substrate/HWTopo.h"
#include "galois/gIO.h"

#include <algorithm>
#include <iostream>


// Forward declare this to avoid including PerThreadStorage.
// We avoid this to stress that the thread Pool MUST NOT depend on PTS.
namespace galois::substrate {

extern void initPTS(unsigned);

}

using galois::substrate::ThreadPool;

thread_local ThreadPool::per_signal ThreadPool::my_box;

ThreadPool::ThreadPool()
    : mi(getHWTopo().machineTopoInfo), reserved(0), masterFastmode(false),
      running(false) {
  signals.resize(mi.maxThreads);
  initThread(0);

  for (unsigned i = 1; i < mi.maxThreads; ++i) {
    std::thread t(&ThreadPool::threadLoop, this, i);
    threads.emplace_back(std::move(t));
  }

  // we don't want signals to have to contain atomics, since they are set once
  while (std::any_of(signals.begin(), signals.end(),
                     [](per_signal* p) { return !p || !p->done; })) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  Time1 = std::chrono::high_resolution_clock::now();
  Time2 = std::chrono::high_resolution_clock::now();

  totalBlockedTimeCompute = 0;
  totalBlockedTimePrefetch = 0;

  numThreads1 = 0;
  numThreads2 = 0;
}

ThreadPool::~ThreadPool() {
  std::cout << "Total blocked time for compute threads = " << totalBlockedTimeCompute << std::endl;
  std::cout << "Total blocked time for prefetch threads = " << totalBlockedTimePrefetch << std::endl;
  destroyCommon();
  for (auto& t : threads) {
    t.join();
  }
}

void ThreadPool::destroyCommon() {
  beKind(); // reset fastmode
  run(mi.maxThreads, []() { throw shutdown_ty(); });
}

void ThreadPool::burnPower(unsigned num) {
  num = std::min(num, getMaxUsableThreads());

  // changing number of threads?  just do a reset
  if (masterFastmode && masterFastmode != num) {
    beKind();
  }
  if (!masterFastmode) {
    run(num, []() { throw fastmode_ty{true}; });
    masterFastmode = num;
  }
}

void ThreadPool::beKind() {
  if (masterFastmode) {
    run(masterFastmode, []() { throw fastmode_ty{false}; });
    masterFastmode = 0;
  }
}

// inefficient append
template <typename T>
static void atomic_append(std::atomic<T*>& headptr, T* newnode) {
  T* n = nullptr;
  if (!headptr.compare_exchange_strong(n, newnode))
    atomic_append(headptr.load()->next, newnode);
}

// find id
template <typename T>
static unsigned findID(std::atomic<T*>& headptr, T* node, unsigned off) {
  T* n = headptr.load();
  assert(n);
  if (n == node) {
    return off;
  }
  return findID(n->next, node, off + 1);
}

template <typename T>
static T* getNth(std::atomic<T*>& headptr, unsigned off) {
  T* n = headptr.load();
  if (!off) {
    return n;
  }
  return getNth(n->next, off - 1);
}

void ThreadPool::initThread(unsigned tid) {
  signals[tid] = &my_box;
  my_box.topo  = getHWTopo().threadTopoInfo[tid];
  // Initialize
  substrate::initPTS(mi.maxThreads);

  if (!EnvCheck("GALOIS_DO_NOT_BIND_THREADS")) {
    if (my_box.topo.tid != 0 || !EnvCheck("GALOIS_DO_NOT_BIND_MAIN_THREAD")) {
      bindThreadSelf(my_box.topo.osContext);
    }
  }
  my_box.done = 1;
}

void ThreadPool::threadLoop(unsigned tid) {
  initThread(tid);
  bool fastmode = false;
  auto& me      = my_box;
  do {
    me.wait(fastmode);
    cascade(fastmode);
    //Modified by Chinmay
    std::chrono::high_resolution_clock::time_point t;
    try {
      if(work != nullptr){
        work();
      }
      else{
        if(getTID() < (unsigned int)numThreads1){
          work1();  
          t = std::chrono::high_resolution_clock::now();
          if(t.time_since_epoch() > Time1.time_since_epoch()){
            Time1 = t;
          }
          // numThreadsThatHaveFinished1.fetch_add(1);
          // if(numThreadsThatHaveFinished2 < numThreads2){
          //   try {
          //     work2();
          //   }catch (const shutdown_ty&) {
          //     return;
          //   } catch (const fastmode_ty& fm) {
          //     fastmode = fm.mode;
          //   } catch (const dedicated_ty dt) {
          //     me.done = 1;
          //     dt.fn();
          //     return;
          //   } catch (const std::exception& exc) {
          //     // catch anything thrown within try block that derives from std::exception
          //     std::cerr << exc.what();
          //     abort();
          //   } catch (...) {
          //     abort();
          //   } 
          // }
        }
        else{
          work2();
          t = std::chrono::high_resolution_clock::now();
          if(t.time_since_epoch() > Time2.time_since_epoch()){
            Time2 = t;
          }
          // numThreadsThatHaveFinished2.fetch_add(1);
          // if(numThreadsThatHaveFinished1 < numThreads1){
          //   try {
          //     work1();
          //   }catch (const shutdown_ty&) {
          //       return;
          //     } catch (const fastmode_ty& fm) {
          //       fastmode = fm.mode;
          //     } catch (const dedicated_ty dt) {
          //       me.done = 1;
          //       dt.fn();
          //       return;
          //     } catch (const std::exception& exc) {
          //       // catch anything thrown within try block that derives from std::exception
          //       std::cerr << exc.what();
          //       abort();
          //     } catch (...) {
          //       abort();
          //     }
          // }
      }
    }
  }
     catch (const shutdown_ty&) {
      return;
    } catch (const fastmode_ty& fm) {
      fastmode = fm.mode;
    } catch (const dedicated_ty dt) {
      me.done = 1;
      dt.fn();
      return;
    } catch (const std::exception& exc) {
      // catch anything thrown within try block that derives from std::exception
      std::cerr << exc.what();
      abort();
    } catch (...) {
      abort();
    }
    decascade();
  } while (true);
}

void ThreadPool::decascade() {
  auto& me = my_box;
  // nothing to wake up
  if (me.wbegin != me.wend) {
    auto midpoint = me.wbegin + (1 + me.wend - me.wbegin) / 2;
    auto& c1done  = signals[me.wbegin]->done;
    while (!c1done) {
      asmPause();
    }
    if (midpoint < me.wend) {
      auto& c2done = signals[midpoint]->done;
      while (!c2done) {
        asmPause();
      }
    }
  }
  me.done = 1;
}

void ThreadPool::cascade(bool fastmode) {
  auto& me = my_box;
  assert(me.wbegin <= me.wend);
  // nothing to wake up
  if (me.wbegin == me.wend) {
    return;
  }

  auto midpoint = me.wbegin + (1 + me.wend - me.wbegin) / 2;

  auto child1    = signals[me.wbegin];
  child1->wbegin = me.wbegin + 1;
  child1->wend   = midpoint;
  child1->wakeup(fastmode);

  if (midpoint < me.wend) {
    auto child2    = signals[midpoint];
    child2->wbegin = midpoint + 1;
    child2->wend   = me.wend;
    child2->wakeup(fastmode);
  }
}

void ThreadPool::runInternal(unsigned num) {
  // sanitize num
  // seq write to starting should make work safe
  GALOIS_ASSERT(!running, "Recursive thread pool execution not supported");
  running = true;
  num     = std::min(std::max(1U, num), getMaxUsableThreads());
  // my_box is tid 0
  auto& me  = my_box;
  me.wbegin = 1;
  me.wend   = num;

  assert(!masterFastmode || masterFastmode == num);
  
  // launch threads
  cascade(masterFastmode);
  // Do master thread work
  try {
    /* 
      Changes made by Chinmay on 8th March 2021

    */
    // for(int z = 0; z < 4; z++){
    //   // std::cout << "Thread id = " << getHWTopo().threadTopoInfo[z].tid << std::endl;
    //   std::cout << "Thread id = " << signals[z]->topo.tid << std::endl;
    // }
    // std::cout << getTID() << std::endl;
    work();
  } catch (const shutdown_ty&) {
    return;
  } catch (const fastmode_ty& fm) {
  }
  // wait for children
  decascade();
  // std::cout << "Descascade complete" << std::endl;
  // Clean up
  work    = nullptr;
  running = false;
}

/* Added by Chinmay */
void ThreadPool::runInternal() {
  // sanitize num
  // seq write to starting should make work safe
  GALOIS_ASSERT(!running, "Recursive thread pool execution not supported");
  running = true;
  // my_box is tid 0
  auto& me  = my_box;
  me.wbegin = 1;
  me.wend   = numThreads1 + numThreads2;

  assert(!masterFastmode || (int)masterFastmode == numThreads1 + numThreads2);
  std::chrono::high_resolution_clock::time_point t;
  // launch threads
  cascade(masterFastmode);

  // Do master thread work
  try {
    work1();
  } catch (const shutdown_ty&) {
    return;
  } catch (const fastmode_ty& fm) {
  }

  t = std::chrono::high_resolution_clock::now();
  if(t.time_since_epoch() > Time1.time_since_epoch()){
      Time1 = t;
  }
  numThreadsThatHaveFinished1.fetch_add(1);
  // if(numThreadsThatHaveFinished2 < numThreads2){
  //   try {
  //     work2();
  //   } catch (const shutdown_ty&) {
  //     return;
  //   } catch (const fastmode_ty& fm) {
  //   }
  // }

  // wait for children
  decascade();
  // std::cout << "Descascade complete" << std::endl;
  // std::cout << "Blocking period = " << 
    // std::chrono::duration_cast<std::chrono::milliseconds>(Time2-Time1).count() << std::endl;
  if(std::chrono::duration_cast<std::chrono::milliseconds>(Time2-Time1).count() > 0){
    totalBlockedTimeCompute += (long int)std::chrono::duration_cast<std::chrono::milliseconds>(Time2-Time1).count();
  }
  else{
    totalBlockedTimePrefetch += (long int)std::chrono::duration_cast<std::chrono::milliseconds>(Time1-Time2).count();
    // std::cout << "Blocking period prefetch = " << 
    // std::chrono::duration_cast<std::chrono::milliseconds>(Time1-Time2).count() << std::endl;
  } 
  // totalBlockedTime += std::abs(std::chrono::duration_cast<std::chrono::milliseconds>(Time2-Time1));
  // Clean up
  work    = nullptr;
  work1    = nullptr;
  work2    = nullptr;
  running = false;
}

void ThreadPool::runDedicated(std::function<void(void)>& f) {
  // TODO(ddn): update galois::runtime::activeThreads to reflect the dedicated
  // thread but we don't want to depend on galois::runtime symbols and too many
  // clients access galois::runtime::activeThreads directly.
  GALOIS_ASSERT(!running,
                "Can't start dedicated thread during parallel section");
  ++reserved;

  GALOIS_ASSERT(reserved < mi.maxThreads, "Too many dedicated threads");
  work          = [&f]() { throw dedicated_ty{f}; };
  auto child    = signals[mi.maxThreads - reserved];
  child->wbegin = 0;
  child->wend   = 0;
  child->done   = 0;
  child->wakeup(masterFastmode);
  while (!child->done) {
    asmPause();
  }
  work = nullptr;
}

static galois::substrate::ThreadPool* TPOOL = nullptr;

void galois::substrate::internal::setThreadPool(ThreadPool* tp) {
  GALOIS_ASSERT(!(TPOOL && tp), "Double initialization of ThreadPool");
  TPOOL = tp;
}

galois::substrate::ThreadPool& galois::substrate::getThreadPool() {
  GALOIS_ASSERT(TPOOL, "ThreadPool not initialized");
  return *TPOOL;
}

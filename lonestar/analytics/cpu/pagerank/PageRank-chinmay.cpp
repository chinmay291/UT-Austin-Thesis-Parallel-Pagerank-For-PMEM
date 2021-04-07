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

#include "Lonestar/BoilerPlate.h"
#include "PageRank-constants.h"
#include "galois/Galois.h"
#include "galois/LargeArray.h"
#include "galois/Timer.h"
#include "galois/config.h"
#include "galois/graphs/LC_CSR_Graph_PM.h"
#include "galois/graphs/ReadGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "galois/gstl.h"
//For debugging
#include "galois/substrate/ThreadPool.h"

const char* desc =
    "Computes page ranks a la Page and Brin. This is a pull-style topology-driven algorithm for PM by Chinmay.";

//! Flag that forces user to be aware that they should be passing in a
//! transposed graph.
static cll::opt<bool>
    transposedGraph("transposedGraph",
                    cll::desc("Specify that the input graph is transposed"),
                    cll::init(false));

constexpr static const unsigned CHUNK_SIZE = 32;
constexpr static const long int EDGE_TILE_SIZE = 128;

static cll::opt<unsigned int> t1(
    "t1",
    cll::desc("Number of threads for first operator of do_specified loop"),
    cll::init(0));

static cll::opt<unsigned int> t2(
    "t2",
    cll::desc("Number of threads for second operator of do_specified loop"),
    cll::init(0));

size_t cacheSize = 38610UL * 2UL;


struct LNode {
  PRTy value;
  uint32_t nout;
};

typedef galois::graphs::LC_CSR_Graph_PM<LNode, void>::with_no_lockable<
    true>::type ::with_numa_alloc<true>::type Graph;
typedef typename Graph::GraphNode GNode;

using iterator = boost::counting_iterator<uint32_t>;
using edge_iterator = boost::counting_iterator<uint64_t>;

std::vector<std::pair<iterator, iterator>>bounds;


//! Initialize nodes for the topological algorithm.
void initNodeDataTopological(Graph& g) {
  PRTy init_value = 1.0f / g.size();
  galois::do_all(
      galois::iterate(g),
      [&](const GNode& n) {
        auto& sdata = g.getData(n, galois::MethodFlag::UNPROTECTED);
        sdata.value = init_value;
        sdata.nout  = 0;
      },
      galois::no_stats(), galois::loopname("initNodeData"));
}

//! Computing outdegrees in the tranpose graph is equivalent to computing the
//! indegrees in the original graph.
void computeOutDeg(Graph& graph) {
  galois::StatTimer outDegreeTimer("computeOutDegFunc");
  outDegreeTimer.start();

  galois::LargeArray<std::atomic<size_t>> vec;
  vec.allocateInterleaved(graph.size());

  galois::do_all(
      galois::iterate(graph),
      [&](const GNode& src) { vec.constructAt(src, 0ul); }, galois::no_stats(),
      galois::loopname("InitDegVec"));

  galois::do_all(
      galois::iterate(graph),
      [&](const GNode& src) {
        for (auto nbr : graph.edges(src)) {
          GNode dst = graph.getEdgeDst(nbr);
          vec[dst].fetch_add(1ul);
        };
      },
      galois::steal(), galois::chunk_size<CHUNK_SIZE>(), galois::no_stats(),
      galois::loopname("computeOutDeg"));

  galois::do_all(
      galois::iterate(graph),
      [&](const GNode& src) {
        auto& srcData = graph.getData(src, galois::MethodFlag::UNPROTECTED);
        srcData.nout  = vec[src];
      },
      galois::no_stats(), galois::loopname("CopyDeg"));

  outDegreeTimer.stop();
}

/*
Calculates out-degree of each node and the range of vertices for each iteration
*/

void preprocess(Graph& graph){
  uint64_t currEdges = 0;
  auto currBegin = graph.begin();
  auto currEnd = graph.begin();
  for(auto i = graph.begin(); i < graph.end(); i++){
    currEdges = currEdges + graph.getDegree(*i);
    currEnd++;
    if(currEdges > cacheSize){
      //push make_pair(currStart, currEnd) into a vector/insertBag
      bounds.push_back(std::make_pair(*currBegin, *currEnd));
      // std::cout << "bound end - begin = " << (*currEnd - *currBegin) << std::endl;
      currBegin = i;
      currEnd = currBegin;
      currEdges = graph.getDegree(*i);
    }
  }

  //Handling case in which currEnd == graph.end(), ie. last pair not yet added to bounds
  bounds.push_back(std::make_pair(*currBegin, *currEnd));
  
}

/*
  Same as prTopological from PageRank-pull.cpp, except it iterates over an InsertBag instead of the input graph
*/
void prTopologicalForComparisonInsertBag(Graph& graph) {

  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  unsigned int iteration = 0;
  galois::GAccumulator<float> accum;

  std::atomic<uint64_t> computeT(0);
  float base_score = (1.0f - ALPHA) / graph.size();

  typedef struct{
    GNode G;
    char label;
  }workItem;

  galois::InsertBag<workItem> currentActiveNodes;
  unsigned int it;

  for(it = 1; it < maxIterations+1; it++){
    
    
      //compute on bounds[i], prefetch data for bounds[i+1](if i+1 != bounds.size())
      /* Step 1: Create the worklist with P and C items */
      galois::do_all(
        galois::iterate(graph),
        [&](GNode src){
          workItem w;
          w.G = src;
          w.label = 'C';
          currentActiveNodes.push(w);
        }, galois::loopname("Push Compute nodes in worklist"));      

      
      /* Step 2: Process the filled worklist */
      galois::do_all(
        galois::iterate(currentActiveNodes),
        [&](workItem& it){
          GNode src = it.G;
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          
          if(it.label == 'C'){

              galois::StatTimer computeTime("Timer_Compute");
              computeTime.start();

                LNode& sdata = graph.getData(src, flag);
                float sum    = 0.0;   
        
                for (auto i = edgeBegin; i < edgeEnd; i++) {
                  GNode dst = graph.getEdgeDst(i);

                  LNode& ddata = graph.getData(dst, flag);
                  sum += ddata.value / ddata.nout;
                }

                //! New value of pagerank after computing contributions from incoming edges in the original graph.
                float value = sum * ALPHA + base_score;
                //! Find the delta in new and old pagerank values.
                float diff = std::fabs(value - sdata.value);

                //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
                sdata.value = value;
                accum += diff;

              computeTime.stop();
              computeT.fetch_add(computeTime.get_usec());
          }
        },
        galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("PageRank P and C"));

        currentActiveNodes.clear();
    

    if (accum.reduce() <= tolerance) {
      break;
    }
    accum.reset();
    
  }

  std::cout << "COMPUTE TIME = "<< computeT << std::endl;
  
  galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
  if (iteration >= maxIterations) {
    std::cerr << "ERROR: failed to converge in " << iteration
              << " iterations\n";
  }

}


/*
Exactly the same as computePRTopological(Graph& graph) in PageRank-pull.cpp
except that execution is divided into rounds such that in each round only the vertices
that can fit in the cache at a time are processed. However, the cache is not actually used. 
Edge adj data is read from PM as in computePRTopological(Graph& graph).
*/
void prTopologicalForComparisonRoundsBasedInsertBag(Graph& graph) {

  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  unsigned int iteration = 0;
  galois::GAccumulator<float> accum;

  std::atomic<uint64_t> computeT(0);
  float base_score = (1.0f - ALPHA) / graph.size();

  typedef struct{
    GNode G;
    char label;
  }workItem;

  galois::InsertBag<workItem> currentActiveNodes;
  unsigned int it;

  for(it = 1; it < maxIterations+1; it++){
    
    for(long unsigned int i = 0; i < bounds.size(); i++){
      //compute on bounds[i], prefetch data for bounds[i+1](if i+1 != bounds.size())
      /* Step 1: Create the worklist with P and C items */
      galois::do_all(
        galois::iterate(bounds[i].first, bounds[i].second),
        [&](GNode src){
          workItem w;
          w.G = src;
          w.label = 'C';
          currentActiveNodes.push(w);
        }, galois::loopname("Push Compute nodes in worklist"));      

      
      /* Step 2: Process the filled worklist */
      galois::do_all(
        galois::iterate(currentActiveNodes),
        [&](workItem& it){
          GNode src = it.G;
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          
          if(it.label == 'C'){

              galois::StatTimer computeTime("Timer_Compute");
              computeTime.start();

                LNode& sdata = graph.getData(src, flag);
                float sum    = 0.0;   
        
                for (auto i = edgeBegin; i < edgeEnd; i++) {
                  GNode dst = graph.getEdgeDst(i);

                  LNode& ddata = graph.getData(dst, flag);
                  sum += ddata.value / ddata.nout;
                }

                //! New value of pagerank after computing contributions from incoming edges in the original graph.
                float value = sum * ALPHA + base_score;
                //! Find the delta in new and old pagerank values.
                float diff = std::fabs(value - sdata.value);

                //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
                sdata.value = value;
                accum += diff;

              computeTime.stop();
              computeT.fetch_add(computeTime.get_usec());
          }
        },
        galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("PageRank P and C"));

        currentActiveNodes.clear();
    }

    if (accum.reduce() <= tolerance) {
      break;
    }
    accum.reset();
    
  }

  std::cout << "COMPUTE TIME = "<< computeT << std::endl;
  
  galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
  if (iteration >= maxIterations) {
    std::cerr << "ERROR: failed to converge in " << iteration
              << " iterations\n";
  }

}

void prTopologicalForComparisonRoundsBasedNoInsertBag(Graph& graph) {

  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  unsigned int iteration = 0;
  galois::GAccumulator<float> accum;

  std::atomic<uint64_t> computeT(0);
  float base_score = (1.0f - ALPHA) / graph.size();

  // galois::LargeArray<char> labels;
  // labels.allocateInterleaved(graph.size());

  // galois::do_all(
  //     galois::iterate(graph),
  //     [&](const GNode& src) { labels.constructAt(src, 'N'); }, galois::no_stats(),
  //     galois::loopname("InitLabelsVec"));

  
  unsigned int it;

  for(it = 1; it < maxIterations+1; it++){
    
    for(long unsigned int i = 0; i < bounds.size(); i++){
      //compute on bounds[i], prefetch data for bounds[i+1](if i+1 != bounds.size())
      /* Step 1: Create the worklist with P and C items */
      // galois::do_all(
      //   galois::iterate(bounds[i].first, bounds[i].second),
      //   [&](GNode src){
      //     labels[src] = 'C';
      //   }, galois::loopname("Push Compute nodes in worklist"));      

      
      /* Step 2: Process the filled worklist */
      galois::do_all(
        galois::iterate(bounds[i].first, bounds[i].second),
        [&](GNode src){
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          
          // if(labels[src] == 'C'){

              galois::StatTimer computeTime("Timer_Compute");
              computeTime.start();

                LNode& sdata = graph.getData(src, flag);
                float sum    = 0.0;   
        
                for (auto i = edgeBegin; i < edgeEnd; i++) {
                  GNode dst = graph.getEdgeDst(i);

                  LNode& ddata = graph.getData(dst, flag);
                  sum += ddata.value / ddata.nout;
                }

                //! New value of pagerank after computing contributions from incoming edges in the original graph.
                float value = sum * ALPHA + base_score;
                //! Find the delta in new and old pagerank values.
                float diff = std::fabs(value - sdata.value);

                //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
                sdata.value = value;
                accum += diff;

              computeTime.stop();
              computeT.fetch_add(computeTime.get_usec());
          // }
        },
        galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("PageRank P and C"));
    }

    if (accum.reduce() <= tolerance) {
      break;
    }
    accum.reset();
    
  }

  std::cout << "COMPUTE TIME = "<< computeT << std::endl;
  
  galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
  if (iteration >= maxIterations) {
    std::cerr << "ERROR: failed to converge in " << iteration
              << " iterations\n";
  }

}

/*
Exact same algo as computePRTopological from PageRank-pull.cpp, except that it executes the do_all
loop on 4 vertices at a time. Also does not use an insertBag.
*/

void prTopologicalForComparisonFourAtATime(Graph& graph) {

  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  unsigned int iteration = 0;
  galois::GAccumulator<float> accum;

  std::atomic<uint64_t> computeT(0);
  float base_score = (1.0f - ALPHA) / graph.size();
  
  unsigned int it;
  

  for(it = 1; it < maxIterations+1; it++){
    
    for(auto i = graph.begin(); i < graph.end(); i = i+4){
      
      /* Step 2: Process the filled worklist */
      auto end = i + 4;
      if(i +4 > graph.end()){
        end = graph.end();
      }
      galois::do_all(
        galois::iterate(i, end),
        [&](GNode src){
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          
          // if(labels[src] == 'C'){

              galois::StatTimer computeTime("Timer_Compute");
              computeTime.start();

                LNode& sdata = graph.getData(src, flag);
                float sum    = 0.0;   
        
                for (auto i = edgeBegin; i < edgeEnd; i++) {
                  GNode dst = graph.getEdgeDst(i);

                  LNode& ddata = graph.getData(dst, flag);
                  sum += ddata.value / ddata.nout;
                }

                //! New value of pagerank after computing contributions from incoming edges in the original graph.
                float value = sum * ALPHA + base_score;
                //! Find the delta in new and old pagerank values.
                float diff = std::fabs(value - sdata.value);

                //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
                sdata.value = value;
                accum += diff;

              computeTime.stop();
              computeT.fetch_add(computeTime.get_usec());
          // }
        },
        galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("PageRank P and C"));
    }

    if (accum.reduce() <= tolerance) {
      break;
    }
    accum.reset();
    
  }

  std::cout << "COMPUTE TIME = "<< computeT << std::endl;
  
  galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
  if (iteration >= maxIterations) {
    std::cerr << "ERROR: failed to converge in " << iteration
              << " iterations\n";
  }

}

void computePRTopologicalEdgeTileChinmay(Graph& graph) {
  //Version with edge tiling enabled for prefetching edge data

  galois::StatTimer swapTime("Timer_Swap");
  galois::StatTimer clearTime("Timer_Clear");
  
  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  unsigned int iteration = 0;
  galois::GAccumulator<float> accum;

  float base_score = (1.0f - ALPHA) / graph.size();

  typedef struct{
    GNode G;
    char label;
    edge_iterator begin;
    edge_iterator end;
  }workItem;

  galois::InsertBag<workItem> currentActiveNodes;
  unsigned int it;

  for(it = 1; it < maxIterations+1; it++){
    galois::do_all(
        galois::iterate(bounds[0].first, bounds[0].second),
        [&](GNode src){
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          auto cacheIndBegin = graph.edge_begin(src, flag) - graph.edge_begin(*bounds[0].first, flag);
          auto j = cacheIndBegin;

          for(auto i = edgeBegin; i < edgeEnd; i++){
            graph.writeToCurrCache(j, graph.getEdgeDst(i));
            j++;
          }
        },
        galois::loopname("Prefetch data for first round"));
    
    for(long unsigned int i = 0; i < bounds.size(); i++){
      //compute on bounds[i], prefetch data for bounds[i+1](if i+1 != bounds.size())
      /* Step 1: Create the worklist with P and C items */
      galois::do_all(
        galois::iterate(bounds[i].first, bounds[i].second),
        [&](GNode src){
          workItem w;
          w.G = src;
          w.label = 'C';
          currentActiveNodes.push(w);
        }, galois::loopname("Push Compute nodes in worklist"));      

      if((i + 1) != bounds.size()){
        galois::do_all(
          galois::iterate(bounds[i+1].first, bounds[i+1].second),
          [&](GNode src){



            auto beg       = graph.edge_begin(src, flag);
            const auto end = graph.edge_end(src, flag);
            //! Edge tiling for large outdegree nodes.
            if ((end - beg) > EDGE_TILE_SIZE) {
              for (; beg + EDGE_TILE_SIZE < end;) {
                  auto ne = beg + EDGE_TILE_SIZE;
                  workItem w;
                  w.G = src;
                  w.label = 'P';
                  w.begin = beg;
                  w.end = ne;
                  currentActiveNodes.push(w);
                  beg = ne;
              }
            }

            if ((end - beg) > 0) {
                workItem w;
                w.G = src;
                w.label = 'P';
                w.begin = beg;
                w.end = end;
                currentActiveNodes.push(w);
            }
          }, galois::loopname("Push Prefetch nodes in worklist"));      
      }
      
      /* Step 2: Process the filled worklist */
      galois::do_all(
        galois::iterate(currentActiveNodes),
        [&](workItem& it){
          GNode src = it.G;
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          auto cacheIndBegin = graph.edge_begin(src, flag);
          auto cacheIndEnd = graph.edge_end(src, flag);
          if(it.label == 'C'){

            //Make sure that edge adj data is read from DRAM and not PM for "Compute" vertices
                LNode& sdata = graph.getData(src, flag);
                float sum    = 0.0;   
                cacheIndBegin = cacheIndBegin - graph.edge_begin(*bounds[i].first, flag);
                cacheIndEnd = cacheIndEnd - graph.edge_begin(*bounds[i].first, flag);
                for (auto i = cacheIndBegin; i < cacheIndEnd; i++) {
                  GNode dst = graph.readFromCurrCache(i);

                  LNode& ddata = graph.getData(dst, flag);
                  sum += ddata.value / ddata.nout;
                }

                //! New value of pagerank after computing contributions from incoming edges in the original graph.
                float value = sum * ALPHA + base_score;
                //! Find the delta in new and old pagerank values.
                float diff = std::fabs(value - sdata.value);

                //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
                sdata.value = value;
                accum += diff;

            // computeTime.stop();    
          }
          else if(it.label == 'P'){

            // galois::StatTimer prefetchTime("Timer_Prefetch");
            // prefetchTime.start();

            edgeBegin = it.begin;
            edgeEnd = it.end;
            cacheIndBegin = edgeBegin - graph.edge_begin(*bounds[i+1].first, flag);
            cacheIndEnd = edgeEnd - graph.edge_begin(*bounds[i+1].first, flag);
            auto k = cacheIndBegin;
            for(auto i = edgeBegin; i < edgeEnd; i++){
              graph.writeToNextCache(k,graph.getEdgeDst(i));
              k++;
            }

            // prefetchTime.stop();
          }
        },
        galois::steal(), 
        galois::loopname("PageRank P and C"));

      clearTime.start();
      currentActiveNodes.clear();
      clearTime.stop();
      swapTime.start();
      graph.swapCache();
      swapTime.stop();
      // std::cout << "clearTime = " << clearTime.get_usec() << std::endl;  
      // std::cout << "swapTime = " << swapTime.get_usec() << std::endl;  
    }

    if (accum.reduce() <= tolerance) {
      break;
    }
    accum.reset();
    // std::cout << "clearTime = " << clearTime.get_usec() << std::endl;  
    // std::cout << "swapTime = " << swapTime.get_usec() << std::endl;  
  }


  
  galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
  if (iteration >= maxIterations) {
    std::cerr << "ERROR: failed to converge in " << iteration
              << " iterations\n";
  }

}


void computePRTopologicalInsertBagAndRounds(Graph& graph) {
  galois::StatTimer swapTime("Timer_Swap");
  galois::StatTimer clearTime("Timer_Clear");
  std::atomic<uint64_t> computeT(0);
  std::atomic<uint64_t> prefetchT(0);

  std::cout << "COMPUTE TIME_INIT = "<< computeT << std::endl;
  std::cout << "PREFETCH TIME_INIT = "<< prefetchT << std::endl;

  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  unsigned int iteration = 0;
  galois::GAccumulator<float> accum;

  float base_score = (1.0f - ALPHA) / graph.size();

  typedef struct{
    GNode G;
    char label;
  }workItem;

  galois::InsertBag<workItem> currentActiveNodes;
  unsigned int it;

  for(it = 1; it < maxIterations+1; it++){
    galois::do_all(
        galois::iterate(bounds[0].first, bounds[0].second),
        [&](GNode src){
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          auto cacheIndBegin = graph.edge_begin(src, flag) - graph.edge_begin(*bounds[0].first, flag);
          auto j = cacheIndBegin;

          for(auto i = edgeBegin; i < edgeEnd; i++){
            graph.writeToCurrCache(j, graph.getEdgeDst(i));
            j++;
          }
        },
        galois::loopname("Prefetch data for first round"));
    
    for(long unsigned int i = 0; i < bounds.size(); i++){
      //compute on bounds[i], prefetch data for bounds[i+1](if i+1 != bounds.size())
      /* Step 1: Create the worklist with P and C items */
      galois::do_all(
        galois::iterate(bounds[i].first, bounds[i].second),
        [&](GNode src){
          workItem w;
          w.G = src;
          w.label = 'C';
          currentActiveNodes.push(w);
        }, galois::loopname("Push Compute nodes in worklist"));      

      if((i + 1) != bounds.size()){
        galois::do_all(
          galois::iterate(bounds[i+1].first, bounds[i+1].second),
          [&](GNode src){
            workItem w;
            w.G = src;
            w.label = 'P';
            currentActiveNodes.push(w);
          }, galois::loopname("Push Prefetch nodes in worklist"));      
      }
      
      /* Step 2: Process the filled worklist */
      galois::do_all(
        galois::iterate(currentActiveNodes),
        [&](workItem& it){
          GNode src = it.G;
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          auto cacheIndBegin = graph.edge_begin(src, flag);
          auto cacheIndEnd = graph.edge_end(src, flag);
          if(it.label == 'C'){
            //Make sure that edge adj data is read from DRAM and not PM for "Compute" vertices

              galois::StatTimer computeTime("Timer_Compute");
              computeTime.start();

                LNode& sdata = graph.getData(src, flag);
                float sum    = 0.0;   
                cacheIndBegin = cacheIndBegin - graph.edge_begin(*bounds[i].first, flag);
                cacheIndEnd = cacheIndEnd - graph.edge_begin(*bounds[i].first, flag);
                for (auto i = cacheIndBegin; i < cacheIndEnd; i++) {
                  GNode dst = graph.readFromCurrCache(i);

                  LNode& ddata = graph.getData(dst, flag);
                  sum += ddata.value / ddata.nout;
                }

                //! New value of pagerank after computing contributions from incoming edges in the original graph.
                float value = sum * ALPHA + base_score;
                //! Find the delta in new and old pagerank values.
                float diff = std::fabs(value - sdata.value);

                //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
                sdata.value = value;
                accum += diff;

              computeTime.stop();
              computeT.fetch_add(computeTime.get_usec());
          }
          else if(it.label == 'P'){
            // std::cout << "Processing prefetch vertex" << std::endl;

            galois::StatTimer prefetchTime("Timer_Prefetch");
            prefetchTime.start();

            cacheIndBegin = cacheIndBegin - graph.edge_begin(*bounds[i+1].first, flag);
            cacheIndEnd = cacheIndEnd - graph.edge_begin(*bounds[i+1].first, flag);
            auto k = cacheIndBegin;
            for(auto i = edgeBegin; i < edgeEnd; i++){
              graph.writeToNextCache(k,graph.getEdgeDst(i));
              k++;
            }

            prefetchTime.stop();
            // std::cout << prefetchTime.get_usec() << std::endl;
            prefetchT.fetch_add(prefetchTime.get_usec());
          }
        },
        galois::steal(),
        galois::loopname("PageRank P and C"));

      clearTime.start();
      currentActiveNodes.clear();
      clearTime.stop();
      swapTime.start();
      graph.swapCache();
      swapTime.stop();
      // std::cout << "clearTime = " << clearTime.get_usec() << std::endl;  
      // std::cout << "swapTime = " << swapTime.get_usec() << std::endl;  
    }

    if (accum.reduce() <= tolerance) {
      break;
    }
    accum.reset();
    // std::cout << "clearTime = " << clearTime.get_usec() << std::endl;  
    // std::cout << "swapTime = " << swapTime.get_usec() << std::endl;  
  }

  std::cout << "COMPUTE TIME = "<< computeT << std::endl;
  std::cout << "PREFETCH TIME = "<< prefetchT << std::endl;
  
  galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
  if (iteration >= maxIterations) {
    std::cerr << "ERROR: failed to converge in " << iteration
              << " iterations\n";
  }

}

void computePRTopologicalNoInsertBag(Graph& graph) {
  std::atomic<uint64_t> computeT(0);
  std::atomic<uint64_t> prefetchT(0);

  std::cout << "COMPUTE TIME_INIT = "<< computeT << std::endl;
  std::cout << "PREFETCH TIME_INIT = "<< prefetchT << std::endl;

  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  galois::LargeArray<char> labels;
  labels.allocateInterleaved(graph.size());

  galois::do_all(
      galois::iterate(graph),
      [&](const GNode& src) { labels.constructAt(src, 'N'); }, galois::no_stats(),
      galois::loopname("InitLabelsVec"));

  unsigned int iteration = 0;
  galois::GAccumulator<float> accum;

  float base_score = (1.0f - ALPHA) / graph.size();

  unsigned int it;

  for(it = 1; it < maxIterations+1; it++){
    galois::do_all(
        galois::iterate(bounds[0].first, bounds[0].second),
        [&](GNode src){
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          auto cacheIndBegin = graph.edge_begin(src, flag) - graph.edge_begin(*bounds[0].first, flag);
          auto j = cacheIndBegin;

          for(auto i = edgeBegin; i < edgeEnd; i++){
            graph.writeToCurrCache(j, graph.getEdgeDst(i));
            j++;
          }
        },
        galois::loopname("Prefetch data for first round"));
    
    for(long unsigned int i = 0; i < bounds.size(); i++){
      //compute on bounds[i], prefetch data for bounds[i+1](if i+1 != bounds.size())
      galois::do_all(
        galois::iterate(bounds[i].first, bounds[i].second),
        [&](GNode src){
          labels[src] = 'C';
        }, galois::loopname("Push Compute nodes in worklist"));      

      if((i + 1) != bounds.size()){
        galois::do_all(
          galois::iterate(bounds[i+1].first, bounds[i+1].second),
          [&](GNode src){
            labels[src] = 'P';
          }, galois::loopname("Push Prefetch nodes in worklist"));      
      }
      auto itStart = bounds[i].first;
      auto itEnd = bounds[i].second;
      if(i + 1 != bounds.size()){
        itEnd = bounds[i+1].second;
      }
      
      
      galois::do_all(
        galois::iterate(itStart, itEnd),
        [&](GNode src){
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          auto cacheIndBegin = graph.edge_begin(src, flag);
          auto cacheIndEnd = graph.edge_end(src, flag);
          
          if(labels[src] == 'C'){
            //Make sure that edge adj data is read from DRAM and not PM for "Compute" vertices

              galois::StatTimer computeTime("Timer_Compute");
              computeTime.start();

                LNode& sdata = graph.getData(src, flag);
                float sum    = 0.0;   
                cacheIndBegin = cacheIndBegin - graph.edge_begin(*bounds[i].first, flag);
                cacheIndEnd = cacheIndEnd - graph.edge_begin(*bounds[i].first, flag);
                for (auto i = cacheIndBegin; i < cacheIndEnd; i++) {
                  GNode dst = graph.readFromCurrCache(i);

                  LNode& ddata = graph.getData(dst, flag);
                  sum += ddata.value / ddata.nout;
                }

                //! New value of pagerank after computing contributions from incoming edges in the original graph.
                float value = sum * ALPHA + base_score;
                //! Find the delta in new and old pagerank values.
                float diff = std::fabs(value - sdata.value);

                //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
                sdata.value = value;
                accum += diff;

              computeTime.stop();
              computeT.fetch_add(computeTime.get_usec());
          }
          else if(labels[src] == 'P'){
            // std::cout << "Processing prefetch vertex" << std::endl;

            galois::StatTimer prefetchTime("Timer_Prefetch");
            prefetchTime.start();

            cacheIndBegin = cacheIndBegin - graph.edge_begin(*bounds[i+1].first, flag);
            cacheIndEnd = cacheIndEnd - graph.edge_begin(*bounds[i+1].first, flag);
            auto k = cacheIndBegin;
            for(auto i = edgeBegin; i < edgeEnd; i++){
              graph.writeToNextCache(k,graph.getEdgeDst(i));
              k++;
            }

            prefetchTime.stop();
            // std::cout << prefetchTime.get_usec() << std::endl;
            prefetchT.fetch_add(prefetchTime.get_usec());
          }
        },
        galois::steal(),
        galois::loopname("PageRank P and C"));

      graph.swapCache();
      
    }

    if (accum.reduce() <= tolerance) {
      break;
    }
    accum.reset();
    // std::cout << "clearTime = " << clearTime.get_usec() << std::endl;  
    // std::cout << "swapTime = " << swapTime.get_usec() << std::endl;  
  }

  std::cout << "COMPUTE TIME = "<< computeT << std::endl;
  std::cout << "PREFETCH TIME = "<< prefetchT << std::endl;
  
  galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
  if (iteration >= maxIterations) {
    std::cerr << "ERROR: failed to converge in " << iteration
              << " iterations\n";
  }

}

void computePRTopologicalUsingDoSpecified(Graph& graph) {
  std::atomic<uint64_t> computeT(0);
  std::atomic<uint64_t> prefetchT(0);

  std::cout << "COMPUTE TIME_INIT = "<< computeT << std::endl;
  std::cout << "PREFETCH TIME_INIT = "<< prefetchT << std::endl;

  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  unsigned int iteration = 0;
  galois::GAccumulator<float> accum;

  float base_score = (1.0f - ALPHA) / graph.size();

  unsigned int it;

  galois::do_all(
        galois::iterate(bounds[0].first, bounds[0].second),
        [&](GNode src){
          auto edgeBegin = graph.edge_begin(src, flag);
          auto edgeEnd = graph.edge_end(src, flag);
          auto cacheIndBegin = graph.edge_begin(src, flag) - graph.edge_begin(*bounds[0].first, flag);
          auto j = cacheIndBegin;

          for(auto i = edgeBegin; i < edgeEnd; i++){
            graph.writeToCurrCache(j, graph.getEdgeDst(i));
            j++;
          }
        },
        galois::steal(),
        galois::loopname("Prefetch data for first round"));

  for(it = 1; it < maxIterations+1; it++){

      for(long unsigned int i = 0; i < bounds.size(); i++){

        auto itStart = bounds[i].first;
        auto itEnd = bounds[i].second;
        auto pfStart = itStart;
        auto pfEnd = itEnd;
        if(i + 1 != bounds.size()){
          pfStart = bounds[i+1].first;
          pfEnd = bounds[i+1].second;
        }
        else{
          pfStart = bounds[0].first;
          pfEnd = bounds[0].second;  
        }
        
        
        galois::do_specified(
          t1, galois::iterate(itStart, itEnd),
          [&](GNode src){
            galois::StatTimer computeTime("Timer_Compute");
            computeTime.start();

            LNode& sdata = graph.getData(src, flag);
            float sum    = 0.0;   
            auto cacheIndBegin = graph.edge_begin(src, flag) - graph.edge_begin(*bounds[i].first, flag);
            auto cacheIndEnd = graph.edge_end(src, flag) - graph.edge_begin(*bounds[i].first, flag);
            
            for (auto i = cacheIndBegin; i < cacheIndEnd; i++) {
                GNode dst = graph.readFromCurrCache(i);

                LNode& ddata = graph.getData(dst, flag);
                sum += ddata.value / ddata.nout;
            }

            //! New value of pagerank after computing contributions from incoming edges in the original graph.
            float value = sum * ALPHA + base_score;
            //! Find the delta in new and old pagerank values.
            float diff = std::fabs(value - sdata.value);

            //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
            sdata.value = value;
            accum += diff;

            computeTime.stop();
            computeT.fetch_add(computeTime.get_usec());
          },
          t2, galois::iterate(pfStart, pfEnd),
          [&](GNode src){
            
            galois::StatTimer prefetchTime("Timer_Prefetch");
            prefetchTime.start();
            // printf("Inside prefetch operator\n");

            auto edgeBegin = graph.edge_begin(src, flag);
            auto edgeEnd = graph.edge_end(src, flag);
            auto cacheIndBegin = graph.edge_begin(src, flag) - graph.edge_begin(*bounds[i+1].first, flag);
            // auto cacheIndEnd = graph.edge_end(src, flag) - graph.edge_begin(*bounds[i+1].first, flag);
            auto k = cacheIndBegin;
            for(auto i = edgeBegin; i < edgeEnd; i++){
              graph.writeToNextCache(k,graph.getEdgeDst(i));
              k++;
            }

            prefetchTime.stop();
            // std::cout << prefetchTime.get_usec() << std::endl;
            prefetchT.fetch_add(prefetchTime.get_usec());
          }
          ,
          galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
          galois::loopname("PageRank P and C"));

          graph.swapCache();
      }

      
      if (accum.reduce() <= tolerance) {
        break;
      }
      accum.reset();
      
  }

    // std::cout << "clearTime = " << clearTime.get_usec() << std::endl;  
    // std::cout << "swapTime = " << swapTime.get_usec() << std::endl;  

  std::cout << "COMPUTE TIME = "<< computeT << std::endl;
  std::cout << "PREFETCH TIME = "<< prefetchT << std::endl;
  
  galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
  if (iteration >= maxIterations) {
    std::cerr << "ERROR: failed to converge in " << iteration
              << " iterations\n";
  }

}


void prTopologicalChinmay(Graph& graph) {
  initNodeDataTopological(graph);
  computeOutDeg(graph);
  preprocess(graph);
  // printf("initNodeDataTopological and computeOutDeg complete\n");

  galois::StatTimer execTime("Timer_0");
  execTime.start();
  computePRTopologicalUsingDoSpecified(graph);
  execTime.stop();
}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url, &inputFile);

  if (!transposedGraph) {
    GALOIS_DIE("This application requires a transposed graph input;"
               " please use the -transposedGraph flag "
               " to indicate the input is a transposed graph.");
  }
  galois::StatTimer totalTime("TimerTotal");
  totalTime.start();

  Graph transposeGraph;
  std::cout << "WARNING: pull style algorithms work on the transpose of the "
               "actual graph\n"
            << "WARNING: this program assumes that " << inputFile
            << " contains transposed representation\n\n"
            << "Reading graph: " << inputFile << "\n";

  galois::graphs::readGraph(transposeGraph, inputFile);
  std::cout << "Read " << transposeGraph.size() << " nodes, "
            << transposeGraph.sizeEdges() << " edges\n";

  galois::preAlloc(2 * numThreads + (3 * transposeGraph.size() *
                                     sizeof(typename Graph::node_data_type)) /
                                        galois::runtime::pagePoolSize());
  galois::reportPageAlloc("MeminfoPre");

  // Graph* chinzptr = &transposeGraph;
  Graph chinzgraphs[2];
  galois::graphs::readGraph(chinzgraphs[0], inputFile);
  galois::graphs::readGraph(chinzgraphs[1], inputFile);

  std::atomic<int>firstInt(0);
  std::atomic<int>secondInt(0);
  // galois::do_all(
  //       galois::iterate(transposeGraph),
  //       [&](GNode src){
  //         transposeGraph.getData(src).value = 0;
  //         firstInt.fetch_add(1);
  //       }, 
  //       galois::steal(),
  //       galois::loopname("Bina Kaam ka loop"));
/*
  printf("Main: calling do_specified\n");
  galois::do_specified(
        t1, galois::iterate(chinzgraphs[0]),
        [&](GNode src){
          chinzgraphs[0].getData(src).value = 0;
          auto edgeBegin = chinzgraphs[0].edge_begin(src, galois::MethodFlag::UNPROTECTED);
          auto edgeEnd = chinzgraphs[0].edge_end(src, galois::MethodFlag::UNPROTECTED);
          for(auto i = edgeBegin; i < edgeEnd; i++){
              GNode dest = chinzgraphs[0].getEdgeDst(i);
              chinzgraphs[0].getData(dest).value = 1;
            }
          firstInt.fetch_add(1);
        }, 
        t2, galois::iterate(chinzgraphs[1]),
        [&](GNode src){
          // printf("Inside operator 2 for tid = %u\n",galois::substrate::ThreadPool::getTID());
          chinzgraphs[1].getData(src).value = 0;
          auto edgeBegin = chinzgraphs[1].edge_begin(src, galois::MethodFlag::UNPROTECTED);
          auto edgeEnd = chinzgraphs[1].edge_end(src, galois::MethodFlag::UNPROTECTED);
          for(auto i = edgeBegin; i < edgeEnd; i++){
              GNode dest = chinzgraphs[1].getEdgeDst(i);
              chinzgraphs[1].getData(dest).value = 1;
            }
          secondInt.fetch_add(1);
        },
        galois::steal(),
        galois::loopname("Kaam ka loop"));
  std::cout << "FIRST INT = " << firstInt << std::endl;
  std::cout << "SECOND INT = " << secondInt << std::endl;
*/
  //Added by Chinmay
  
  
  prTopologicalChinmay(transposeGraph);

  galois::reportPageAlloc("MeminfoPost");

  //! Sanity checking code.
  galois::GReduceMax<PRTy> maxRank;
  galois::GReduceMin<PRTy> minRank;
  galois::GAccumulator<PRTy> distanceSum;
  maxRank.reset();
  minRank.reset();
  distanceSum.reset();

  //! [example of no_stats]
  galois::do_all(
      galois::iterate(transposeGraph),
      [&](uint64_t i) {
        PRTy rank = transposeGraph.getData(i).value;

        maxRank.update(rank);
        minRank.update(rank);
        distanceSum += rank;
      },
      galois::loopname("Sanity check"), galois::no_stats());
  //! [example of no_stats]

  PRTy rMaxRank = maxRank.reduce();
  PRTy rMinRank = minRank.reduce();
  PRTy rSum     = distanceSum.reduce();
  galois::gInfo("Max rank is ", rMaxRank);
  galois::gInfo("Min rank is ", rMinRank);
  galois::gInfo("Sum is ", rSum);

  if (!skipVerify) {
    printTop(transposeGraph);
  }

#if DEBUG
  printPageRank(transposeGraph);
#endif


  totalTime.stop();

  std::cout << "FIRST INT = " << firstInt << std::endl;
  std::cout << "SECOND INT = " << secondInt << std::endl;

  return 0;
}

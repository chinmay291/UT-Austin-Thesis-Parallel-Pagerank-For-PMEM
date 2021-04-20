#include "Lonestar/BoilerPlate.h"
#include "PageRank-constants.h"
#include "galois/Galois.h"
#include "galois/LargeArray.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/config.h"
#include "galois/graphs/ReadGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "galois/gstl.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/serialization/vector.hpp>
//For debugging
#include "galois/substrate/ThreadPool.h"



const char* desc =
    "Computes page ranks a la Page and Brin. This is a pull-style topology-driven algorithm using CSR Segmentation for PM by Chinmay.";

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

static cll::opt<size_t> cacheSize(
    "cacheSize",
    cll::desc("Size of DRAM cache for PM"),
    cll::init(0));

static cll::opt<uint64_t> numSubgraphs(
    "numSubgraphs",
    cll::desc("Number of subgraphs"),
    cll::init(0));

// size_t cacheSize = 38610UL * 2UL;

typedef std::pair<PRTy, uint32_t> LNode;

// typedef struct{
//   PRTy value;
//   uint32_t nout;
// }LNode;

typedef galois::graphs::LC_CSR_Graph<LNode, void>::with_no_lockable<
    true>::type ::with_numa_alloc<true>::type Graph;
typedef typename Graph::GraphNode GNode;

using iterator = boost::counting_iterator<uint32_t>;
using edge_iterator = boost::counting_iterator<uint64_t>;

galois::LargeArray<PRTy> PRvec_curr;
galois::LargeArray<PRTy> PRvec_next;
galois::LargeArray<uint32_t> outDegvec;
galois::LargeArray<uint32_t> currEdgeCache;
galois::LargeArray<uint32_t> nextEdgeCache;

uint64_t numTotalNodes = 0;


void initNodeDataTopological(Graph& g, std::vector<uint64_t>& LtoG) {
  PRTy init_value = 1.0f / numTotalNodes;
  galois::do_all(
      galois::iterate(g),
      [&](const GNode& n) {
        PRvec_curr[LtoG[n]] = init_value;
        PRvec_next[LtoG[n]] = 0;
        outDegvec[LtoG[n]] = 0;
        //Note: n is the local index, so first conver it to global
      },
      galois::no_stats(), galois::loopname("initNodeData"));
}

//! Computing outdegrees in the tranpose graph is equivalent to computing the
//! indegrees in the original graph.
void computeOutDeg(Graph& graph, std::vector<uint64_t>& LtoG) {
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
        outDegvec[LtoG[src]]  = vec[src];
        //Note: src is the local index, so first conver it to global
      },
      galois::no_stats(), galois::loopname("CopyDeg"));

  outDegreeTimer.stop();
}

void computePRTopologicalCsrSeg(Graph graphs[], std::vector<uint64_t> localToGlobal[]) {
      constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
      std::atomic<uint64_t> computeT(0);
      std::atomic<uint64_t> prefetchT(0);
      unsigned int iteration = 0;
      galois::GAccumulator<float> accum;

      float base_score = (1.0f - ALPHA) / numTotalNodes;

      unsigned int it;

      galois::do_all(
            galois::iterate(graphs[0]),
            [&](GNode src){
              auto edgeBegin = graphs[0].edge_begin(src, flag);
              auto edgeEnd = graphs[0].edge_end(src, flag);
              //NOTE: cacheIndBegin is same as edgeBegin now
              // auto cacheIndBegin = graph.edge_begin(src, flag) - graph.edge_begin(*bounds[0].first, flag);
              // auto j = cacheIndBegin;

              for(auto i = edgeBegin; i < edgeEnd; i++){
                // graph.writeToCurrCache(i, graph.getEdgeDst(i));
                currEdgeCache[*i] = graphs[0].getEdgeDst(i);
              }
            },
            galois::steal(),
            galois::loopname("Prefetch data for first round"));

      printf("Prefetched data for first round\n");

      for(it = 1; it < maxIterations+1; it++){

          for(uint64_t i = 0; i < numSubgraphs; i++){
            // printf("calling do_specified\n");
            galois::do_specified(
              t1, galois::iterate(graphs[i%numSubgraphs]),
              [&](GNode src){
                galois::StatTimer computeTime("Timer_Compute");
                computeTime.start();

                // PRTy& sdata = PRvec_curr[src];
                //Note: src is the local index, so first convert it to global 
                float sum    = 0.0;   
                auto cacheIndBegin = graphs[i%numSubgraphs].edge_begin(src, flag);
                auto cacheIndEnd = graphs[i%numSubgraphs].edge_end(src, flag);
                
                for (auto j = cacheIndBegin; j < cacheIndEnd; j++) {
                    GNode dst = currEdgeCache[*j];
                    sum += PRvec_curr[localToGlobal[i][dst]] / outDegvec[localToGlobal[i][dst]];
                    //Note: dst is the local index, so first conver it to global
                }

                //! New value of pagerank after computing contributions from incoming edges in the original graph.
                // float value = sum * ALPHA + base_score;
                //! Find the delta in new and old pagerank values.
                // float diff = std::fabs(value - sdata.first);

                //! Do not update pagerank before the diff is computed since there is a data dependence on the pagerank value.
                PRvec_next[localToGlobal[i][src]] += sum;
                //Note: Within an iteration, the PR of a node may be updated in multiple rounds. 
                // All of these computed values should be fetch_added()
                // accum += diff;

                computeTime.stop();
                computeT.fetch_add(computeTime.get_usec());
              },
              t2, galois::iterate(graphs[(i+1)%numSubgraphs]),
              [&](GNode src){
                
                galois::StatTimer prefetchTime("Timer_Prefetch");
                prefetchTime.start();

                auto edgeBegin = graphs[(i+1)%numSubgraphs].edge_begin(src, flag);
                auto edgeEnd = graphs[(i+1)%numSubgraphs].edge_end(src, flag);
                
                for(auto j = edgeBegin; j < edgeEnd; j++){
                    nextEdgeCache[*j] = graphs[(i+1)%numSubgraphs].getEdgeDst(j);
                }

                prefetchTime.stop();
                // std::cout << prefetchTime.get_usec() << std::endl;
                prefetchT.fetch_add(prefetchTime.get_usec());
              }
              ,
              galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
              galois::loopname("PageRank P and C"));

              // graph.swapCache();
              swap(currEdgeCache, nextEdgeCache);  
          }
          // printf("Iteration complete\n");
          
          galois::do_all(
            galois::iterate(PRvec_next),
            [&](uint64_t i){
                PRvec_next[i] = PRvec_next[i] * ALPHA + base_score; 
                float diff = std::fabs(PRvec_next[i] - PRvec_curr[i]);
                accum += diff;
                PRvec_curr[i] = 0;
            }, galois::steal(), galois::no_stats());

          swap(PRvec_curr, PRvec_next);

          std::cout << "accum.reduce() = " << accum.reduce() << std::endl;
          if (accum.reduce() <= tolerance) {
            break;
          }
          accum.reset();
          
      }

      std::cout << "COMPUTE TIME = "<< computeT << std::endl;
      std::cout << "PREFETCH TIME = "<< prefetchT << std::endl;
      
      galois::runtime::reportStat_Single("PageRank", "ITERATIONS", it);
      if (iteration >= maxIterations) {
        std::cerr << "ERROR: failed to converge in " << iteration
                  << " iterations\n";
      }
}

void prTopologicalCsrSeg(Graph graphs[], std::vector<uint64_t> localToGlobal[]) {
  for(uint64_t i = 0; i < numSubgraphs; i++){
    initNodeDataTopological(graphs[i], localToGlobal[i]);
    computeOutDeg(graphs[i], localToGlobal[i]);  
  }
  
  // preprocess(graph);
  // printf("initNodeDataTopological and computeOutDeg complete\n");

  galois::StatTimer execTime("Timer_0");
  execTime.start();
  computePRTopologicalCsrSeg(graphs, localToGlobal);
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
  if (numSubgraphs == 0) {
    GALOIS_DIE("Number of subgraphs is 0");
  }
  if (cacheSize == 0) {
    GALOIS_DIE("Size of cache is 0");
  }
  galois::StatTimer totalTime("TimerTotal");
  totalTime.start();

  // Graph transposeGraph;
  std::cout << "WARNING: pull style algorithms work on the transpose of the "
               "actual graph\n"
            << "WARNING: this program assumes that " << inputFile
            << " contains transposed representation\n\n"
            << "Reading graph: " << inputFile << "\n";

  
  

  Graph chinzgraphs[(uint64_t)numSubgraphs];
  
  // std::string filename = inputFile + "." + std::to_string(0) + ".of." + std::to_string(numSubgraphs);
  // galois::graphs::readGraph(chinzgraphs[0], filename);
  
  galois::reportPageAlloc("MeminfoPre");

  for(uint64_t i = 0; i < numSubgraphs; i++){
    std::string filename = inputFile + "." + std::to_string(i) + ".of." + std::to_string(numSubgraphs);
    galois::graphs::readGraph(chinzgraphs[i], filename);
    numTotalNodes += chinzgraphs[i].size();
    std::cout << "Read " << chinzgraphs[i].size() << " nodes, "
            << chinzgraphs[i].sizeEdges() << " edges\n";
  }

  // numTotalNodes = chinzgraphs[0].size();
  galois::preAlloc(2 * numThreads + (3 * numTotalNodes *
                                     sizeof(typename Graph::node_data_type)) /
                                        galois::runtime::pagePoolSize());
  //Read local to global vectors
  std::vector<uint64_t> localToGlobal[(uint64_t)numSubgraphs];
  for(uint64_t i = 0; i < numSubgraphs; i++){
    std::string archive_name = inputFile + "-LtoG-" + std::to_string(i);
    std::ifstream ifs(archive_name);
    boost::archive::text_iarchive ia{ifs};
    ia >> localToGlobal[i];
  }
  printf("localToGlobal vectors have been read\n");

  PRvec_curr.allocateInterleaved(numTotalNodes);
  PRvec_next.allocateInterleaved(numTotalNodes);
  outDegvec.allocateInterleaved(numTotalNodes);
  currEdgeCache.allocateInterleaved(cacheSize);
  nextEdgeCache.allocateInterleaved(cacheSize);
  printf("Memory allocation for LargeArrays complete\n");
    
  
  prTopologicalCsrSeg(chinzgraphs, localToGlobal);

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
      galois::iterate(PRvec_curr),
      [&](uint64_t src) {
        PRTy rank = PRvec_curr[src];

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
/*
  if (!skipVerify) {
    printTop(transposeGraph);
  }

#if DEBUG
  printPageRank(transposeGraph);
#endif
*/

  totalTime.stop();

  return 0;
}
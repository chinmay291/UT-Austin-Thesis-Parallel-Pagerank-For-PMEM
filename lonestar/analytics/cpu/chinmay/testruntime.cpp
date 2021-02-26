#include "Lonestar/BoilerPlate.h"
#include "galois/Galois.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/TypeTraits.h"

namespace cll = llvm::cl;

static cll::opt<std::string>
    inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);

const char* desc =
    "Computes runtime a la Chinmay.";
const char* name = "Testruntime";
static const char* url  = nullptr;


constexpr static const unsigned CHUNK_SIZE = 16;

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url, &inputFile);

  using Graph = galois::graphs::LC_CSR_Graph<unsigned, void>::with_no_lockable<true>::type ::with_numa_alloc<true>::type;
  using GNode = Graph::GraphNode;
  constexpr static const unsigned CHUNK_SIZE = 16;

  galois::StatTimer totalTime("TimerTotal");
  totalTime.start();

  Graph graph;
  galois::graphs::readGraph(graph, inputFile);
  std::cout << "Read " << graph.size() << " nodes, " << graph.sizeEdges()
            << " edges\n";


  for(int j = 0; j < 100; j++){
	galois::do_all(
        galois::iterate(graph),
        [&](GNode n) {
        	constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
        	auto& sdata = graph.getData(n, flag);
        	sdata++;
        },
        galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("ReadVertexData"),galois::no_stats());  	
  }

  for(int j = 0; j < 100; j++){
  	int i = 0;
  	galois::do_all(
        galois::iterate(graph),
        [&](GNode n) {
        	graph.getData(n) = i;
        	i++;
        },
        galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("WriteVertexData"),galois::no_stats());	
  }

  for(int j = 0; j < 10; j++){
  	galois::do_all(
        galois::iterate(graph),
        [&](GNode n) {
        	for (auto ii : graph.edges(n, galois::MethodFlag::UNPROTECTED)) {
          		GNode dst   = graph.getEdgeDst(ii);
          		graph.getData(dst) = 0;
          	}
        },
        galois::steal(), galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("ReadEdgeData"), galois::no_stats());	
  }

  totalTime.stop();
  return 0;
}
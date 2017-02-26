/** Common command line processing for benchmarks -*- C++ -*-
 * @file
 * @section License
 *
 * This file is part of Galois.  Galois is a framework to exploit
 * amorphous data-parallelism in irregular programs.
 *
 * Galois is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Galois is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Galois.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * @section Copyright
 *
 * Copyright (C) 2015, The University of Texas at Austin. All rights
 * reserved.
 *
 * @section Description
 *
 * Perform common setup tasks for the lonestar benchmarks
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */

#include "Lonestar/BoilerPlate.h"
#include "Galois/Version.h"
#include "Galois/Runtime/Network.h"

#include <iostream>
#include <sstream>

//! standard global options to the benchmarks
llvm::cl::opt<bool> skipVerify("noverify", llvm::cl::desc("Skip verification step"), llvm::cl::init(false));
llvm::cl::opt<int> numThreads("t", llvm::cl::desc("Number of threads"), llvm::cl::init(1));

llvm::cl::opt<int> numRuns("runs", llvm::cl::desc("Number of runs"), llvm::cl::init(3));

llvm::cl::opt<bool> savegraph("savegraph", llvm::cl::desc("Bool flag to enable save graph"), llvm::cl::init(false));
llvm::cl::opt<std::string> outputfile("outputfile", llvm::cl::desc("Output file name to store the local graph structure"), llvm::cl::init("local_graph"));
llvm::cl::opt<std::string> outputfolder("outputfolder", llvm::cl::desc("Output folder name to store the local graph structure"), llvm::cl::init("."));

llvm::cl::opt<bool> verifyMax("verifyMax", llvm::cl::desc("Just print the max value of nodes fields"), llvm::cl::init(false));

static void LonestarPrintVersion() {
  std::cout << "Galois Benchmark Suite v" << Galois::getVersion() << " (" << Galois::getRevision() << ")\n";
}


//! initialize lonestar benchmark
void LonestarStart(int argc, char** argv, 
                   const char* app, const char* desc, const char* url) {
  
  llvm::cl::SetVersionPrinter(LonestarPrintVersion);
  llvm::cl::ParseCommandLineOptions(argc, argv);
  numThreads = Galois::setActiveThreads(numThreads); 
  
  LonestarPrintVersion();
  std::cout << "Copyright (C) " << Galois::getCopyrightYear() << " The University of Texas at Austin\n";
  std::cout << "http://iss.ices.utexas.edu/galois/\n\n";
  std::cout << "application: " <<  (app ? app : "unspecified") << "\n";
  if (desc)
    std::cout << desc << "\n";
  if (url)
    std::cout << "http://iss.ices.utexas.edu/?p=projects/galois/benchmarks/" << url << "\n";
  std::cout << "\n";

  std::ostringstream cmdout;
  for (int i = 0; i < argc; ++i) {
    cmdout << argv[i];
    if (i != argc - 1)
      cmdout << " ";
  }
  Galois::Runtime::reportStatGlobal("CommandLine", cmdout.str());
  Galois::Runtime::reportStat("(NULL)", "CommandLine", cmdout.str(), 0);
  Galois::Runtime::reportStat("(NULL)", "Threads", (unsigned long)numThreads, 0);
  Galois::Runtime::reportStat("(NULL)", "Hosts", (unsigned long)Galois::Runtime::getSystemNetworkInterface().Num, 0);
  Galois::Runtime::reportStat("(NULL)", "Runs", (unsigned long)numRuns, 0);

  char name[256];
  gethostname(name, 256);
  Galois::Runtime::reportStatGlobal("Hostname", name);

  Galois::Runtime::reportStatGlobal("Threads", numThreads);

  Galois::Runtime::reportStatGlobal("Hosts", Galois::Runtime::getSystemNetworkInterface().Num);

  Galois::Runtime::reportStatGlobal("Runs", numRuns);

  if(savegraph)
    Galois::Runtime::reportStat("(NULL)", "Saving Graph structure to:", (outputfolder + "/" + outputfile).c_str(), 0);
}

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

#include "galois/gIO.h"
#include "galois/substrate/Termination.h"

// vtable anchoring
galois::substrate::TerminationDetection::~TerminationDetection(void) {}

static galois::substrate::TerminationDetection* TERM = nullptr;
//Added by Chinmay
static galois::substrate::TerminationDetection* TERM1 = nullptr;
static galois::substrate::TerminationDetection* TERM2 = nullptr;

void galois::substrate::internal::setTermDetect(
    galois::substrate::TerminationDetection* t) {
  GALOIS_ASSERT(!(TERM && t), "Double initialization of TerminationDetection");
  TERM = t;
}

galois::substrate::TerminationDetection&
galois::substrate::getSystemTermination(unsigned activeThreads) {
  TERM->init(activeThreads);
  printf("Termination.cpp: TERM->init complete\n");
  return *TERM;
}

//Added by Chinmay
galois::substrate::TerminationDetection&
galois::substrate::getSystemTerminationWorkload1(unsigned activeThreads) {
  TERM1->init(activeThreads);
  printf("Termination.cpp: TERM1->init complete\n");
  return *TERM1;
}

galois::substrate::TerminationDetection&
galois::substrate::getSystemTerminationWorkload2(unsigned activeThreads) {
  TERM2->init(activeThreads);
  printf("Termination.cpp: TERM2->init complete\n");
  return *TERM2;
}

void galois::substrate::internal::setTermDetectWorkload1(
    galois::substrate::TerminationDetection* t) {
  GALOIS_ASSERT(!(TERM1 && t), "Double initialization of TerminationDetection1");
  TERM1 = t;
}

void galois::substrate::internal::setTermDetectWorkload2(
    galois::substrate::TerminationDetection* t) {
  GALOIS_ASSERT(!(TERM2 && t), "Double initialization of TerminationDetection2");
  TERM2 = t;
}

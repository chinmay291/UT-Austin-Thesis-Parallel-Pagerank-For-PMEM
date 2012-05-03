/** An element (i.e., a triangle or a boundary line) -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
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
 *
 * @author Xin Sui <xinsui@cs.utexas.edu>
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#ifndef ELEMENT_H
#define ELEMENT_H

#include "Tuple.h"

#include <ostream>
#include <stdlib.h>

class Point;

class ElementMark {
  std::pair<long,unsigned> val;
public:
  ElementMark(): val(0, 0) { }
  ElementMark(const ElementMark& x): val(x.val) { }
  ElementMark(unsigned hi, unsigned lo): val(hi, lo) { }

  void update(const ElementMark& m) {
    val = m.val;
  }

  bool operator==(const ElementMark& m) const {
    return val == m.val;
  }

  bool operator!=(const ElementMark& m) const {
    return val != m.val;
  }
};


class Element {
  Point* points[3];
  ElementMark mark;
  bool m_boundary;
  
public:
  Element(const Element& e): mark(e.mark), m_boundary(e.m_boundary) {
    points[0] = e.points[0];
    points[1] = e.points[1];
    points[2] = e.points[2];
  }

  Element(Point* a, Point* b, Point* c):
    m_boundary(false)
  {
    points[0] = a;
    points[1] = b;
    points[2] = c;
  }

  Element(Point* a, Point* b):
    m_boundary(true)
  {
    points[0] = a;
    points[1] = b;
  }
  
  ElementMark& getMark() { return mark; }
  const ElementMark& getMark() const { return mark; }

  Point* getPoint(int i) { return points[i]; }
  const Point* getPoint(int i) const { return points[i]; }

  bool boundary() const { return m_boundary; }
  int dim() const { return m_boundary ? 2 : 3; }

  bool clockwise() const;
  
  //! determine if a tuple is inside the triangle
  bool inTriangle(const Tuple& p) const;

  //! determine if the circumcircle of the triangle contains the tuple
  bool inCircle(const Tuple& p) const;

  std::ostream& print(std::ostream& out) const;
};

std::ostream& operator<<(std::ostream& out, const Element& e);

#endif

///|/ Copyright (c) Prusa Research 2019 - 2023 Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <arrange/Compat.hpp>

#if defined(_MSC_VER) && defined(__clang__)
#define BOOST_NO_CXX17_HDR_STRING_VIEW
#endif

#include <boost/rational.hpp>

#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/Int128.hpp>

#if !defined(HAS_INTRINSIC_128_TYPE) || defined(__APPLE__)
#include <boost/multiprecision/integer.hpp>
#endif

#include <libnest2d/backends/libslic3r/geometries.hpp>
#include <libnest2d/utils/rotcalipers.hpp>

namespace Slic3r {

// Orio: same as fit_into_box_rotation() of PrusaSlicer 2.9 (MinAreaBoundingBox.cpp).

// Used as compute type.
using Unit = int64_t;

#if !defined(HAS_INTRINSIC_128_TYPE) || defined(__APPLE__)
using Rational = boost::rational<boost::multiprecision::int128_t>;
#else
using Rational = boost::rational<__int128>;
#endif

double fit_into_box_rotation(const Polygon &shape, const BoundingBox &bb)
{
    using namespace libnest2d;

    _Box<Point> box{{bb.min.x(), bb.min.y()}, {bb.max.x(), bb.max.y()}};

    // The libnest2d backend of this libslic3r only implements shapelike::rotate() for ExPolygon, which
    // fitIntoBoxRotation() needs (PrusaSlicer calls it with the Polygon).
    return fitIntoBoxRotation<ExPolygon, TCompute<ExPolygon>, Rational>(ExPolygon{shape},
                                                                        box,
                                                                        EPSILON);
}

} // namespace Slic3r

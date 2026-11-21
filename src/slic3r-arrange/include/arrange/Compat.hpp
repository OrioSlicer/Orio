///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef ARRANGE_COMPAT_HPP
#define ARRANGE_COMPAT_HPP

// Orio: helpers that the arrange library of PrusaSlicer 2.9 takes from its libslic3r (libslic3r.h,
// Polygon.hpp, MinAreaBoundingBox.hpp) and that the libslic3r of Orio (Bambu Studio 02.07) lacks.
// They live here so that porting the arrange library does not touch headers included by the whole project.

#include <functional>
#include <iterator>
#include <type_traits>

#include <libslic3r/libslic3r.h>
#include <libslic3r/Polygon.hpp>
#include <libslic3r/BoundingBox.hpp>

namespace Slic3r {

namespace detail_strip_ref_wrappers {
template<class T> struct StripCVRef_ { using type = remove_cvref_t<T>; };
template<class T> struct StripCVRef_<std::reference_wrapper<T>>
{
    using type = std::remove_cv_t<T>;
};
} // namespace detail_strip_ref_wrappers

// Removes reference wrappers as well
template<class T> using StripCVRef =
    typename detail_strip_ref_wrappers::StripCVRef_<remove_cvref_t<T>>::type;

template<class Cont> auto crange(Cont &&cont)
{
    return Range{std::cbegin(cont), std::cend(cont)};
}

// Helper to be used in static_assert.
template<class T> struct always_false { enum { value = false }; };

template<class I> IntegerOnly<I, Polygons> reserve_polygons(I cap)
{
    return reserve_vector<Polygon, I, typename Polygons::allocator_type>(cap);
}

// Rotation that makes the shape fit into the box, if there is one.
double fit_into_box_rotation(const Polygon &shape, const BoundingBox &box);

// BoundingBox::translate() of PrusaSlicer takes a Point; here it takes a Vec2d or two coordinates.
inline void translate_bb(BoundingBox &bb, const Vec2crd &v)
{
    bb.min += v;
    bb.max += v;
}

} // namespace Slic3r

#endif // ARRANGE_COMPAT_HPP

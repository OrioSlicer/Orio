///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef ARRANGEKERNELUTILS_HPP
#define ARRANGEKERNELUTILS_HPP

#include <type_traits>

#include <arrange/NFP/NFPArrangeItemTraits.hpp>
#include <arrange/Beds.hpp>
#include <arrange/DataStoreTraits.hpp>

namespace Slic3r { namespace arr2 {

template<class Itm, class Bed, class Context>
bool find_initial_position(Itm &itm,
                           const Vec2crd &sink,
                           const Bed &bed,
                           const Context &packing_context)
{
    bool ret = false;

    //Orio: as in PrusaSlicer, not for the plates with excluded regions (IrregularBed) nor for the plates
    // with a fixed wipe tower. A big first item put at the sink leaves no room for the next ones there (the two
    // halves of a cut sphere did not fit together on an X1 plate): the pile grows from the border of the plate,
    // and align_piles() (ArrangePrusa.cpp) moves it to the alignment afterwards.
    if constexpr (std::is_convertible_v<Bed, RectangleBed> ||
                  std::is_convertible_v<Bed, InfiniteBed> ||
                  std::is_convertible_v<Bed, CircleBed>)
    {
        if (all_items_range(packing_context).empty()) {
            auto rotations = allowed_rotations(itm);

            //Orio: the rotations are relative to the current one, as in pack() of PackStrategyNFP, and
            // the hull is taken from the item at each rotation. PrusaSlicer turned the hull around the origin
            // while the item turns around its translation: the item could land away from the sink.
            const double rot0 = get_rotation(itm);

            for (double rot : rotations) {
                set_rotation(itm, rot0 + rot);
                auto bbitm = get_extents(envelope_convex_hull(itm));

                Vec2crd d = sink - bbitm.center();
                translate_bb(bbitm, d);

                if (bounding_box(bed).contains(bbitm)) {
                    translate(itm, d);
                    ret = true;
                    break;
                }
            }

            if (!ret)
                set_rotation(itm, rot0);
        }
    }

    return ret;
}

template<class ArrItem> std::optional<Vec2crd> get_gravity_sink(const ArrItem &itm)
{
    constexpr const char * SinkKey = "sink";

    std::optional<Vec2crd> ret;

    auto ptr = get_data<Vec2crd>(itm, SinkKey);

    if (ptr)
        ret = *ptr;

    return ret;
}

template<class ArrItem> bool is_wipe_tower(const ArrItem &itm)
{
    constexpr const char * Key = "is_wipe_tower";

    return has_key(itm, Key);
}

}} // namespace Slic3r::arr2

#endif // ARRANGEKERNELUTILS_HPP

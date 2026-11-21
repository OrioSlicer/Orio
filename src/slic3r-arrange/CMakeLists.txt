project(slic3r-arrange)
cmake_minimum_required(VERSION 3.13)

# Orio: arrange library of PrusaSlicer 2.9.6 (src/slic3r-arrange), plus the arrange item of its
# src/slic3r-arrange-wrapper (include/arrange-wrapper/Items). The scene and the tasks of PrusaSlicer are not
# ported: the "Arrange" tool builds the items itself (slic3r/GUI/Jobs/ArrangePrusa.cpp).
add_library(slic3r-arrange STATIC
    include/arrange/Compat.hpp
    include/arrange/Beds.hpp
    include/arrange/ArrangeItemTraits.hpp
    include/arrange/PackingContext.hpp
    include/arrange/NFP/NFPArrangeItemTraits.hpp
    include/arrange/NFP/NFP.hpp
    include/arrange/ArrangeBase.hpp
    include/arrange/DataStoreTraits.hpp
    include/arrange/ArrangeFirstFit.hpp
    include/arrange/NFP/PackStrategyNFP.hpp
    include/arrange/NFP/Kernels/TMArrangeKernel.hpp
    include/arrange/NFP/Kernels/GravityKernel.hpp
    include/arrange/NFP/RectangleOverfitPackingStrategy.hpp
    include/arrange/NFP/EdgeCache.hpp
    include/arrange/NFP/Kernels/KernelTraits.hpp
    include/arrange/NFP/NFPConcave_Tesselate.hpp
    include/arrange/NFP/Kernels/KernelUtils.hpp
    include/arrange/NFP/Kernels/RectangleOverfitKernelWrapper.hpp
    include/arrange-wrapper/Items/ArbitraryDataStore.hpp
    include/arrange-wrapper/Items/MutableItemTraits.hpp
    include/arrange-wrapper/Items/ArrangeItem.hpp

    src/Compat.cpp
    src/Beds.cpp
    src/NFP/NFP.cpp
    src/NFP/NFPConcave_Tesselate.cpp
    src/NFP/EdgeCache.cpp
    src/NFP/CircularEdgeIterator.hpp
    src/Items/ArrangeItem.cpp
)

target_include_directories(slic3r-arrange PRIVATE src)
target_include_directories(slic3r-arrange PUBLIC include)
target_link_libraries(slic3r-arrange PUBLIC libslic3r libnest2d)

#ifndef slic3r_MeshToStep_hpp_
#define slic3r_MeshToStep_hpp_

// Orio: a closed mesh made into a STEP solid, for the "G-code to Mesh" tab. Level 1: a face per triangle;
// level 2: coplanar triangles merged into planar faces; level 3: the flat pieces gathered into the cylinders, cones,
// spheres and tori they lie on, each made a face on its exact surface (the edges are still those of the mesh); level 4:
// no facets, what the simple surfaces leave becomes extrusions, revolutions and helices of a profile, and free B-spline
// surfaces, and the edges are fitted curves (lines, arcs, B-splines) lying on both their faces. The solid is built from
// the topology of the mesh, without sewing: a vertex per corner, an edge per piece of boundary between two regions,
// shared by both faces. It is then healed and checked by OpenCASCADE, and written in millimetres.

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

class MeshToStep
{
public:
    struct Settings
    {
        // 1 a face per triangle, 2 coplanar triangles merged, 3 surfaces recognized, 4 no facets.
        int    level{ 4 };
        // Largest distance between the mesh and a merged plane or a recognized surface, mm.
        double tolerance{ 0.01 };
        // Triangles of a recognized curved surface, at least.
        int    min_faces{ 6 };
    };

    // Facet: a small plane of a surface which was not recognized. Extrusion, Revolution, Helix: a profile moved along
    // a direction, around an axis, or both (a thread). Free: any other smooth surface (a fillet whose radius varies).
    enum class Kind { Plane, Cylinder, Cone, Sphere, Torus, Facet, Extrusion, Revolution, Helix, Free };

    // Surface of a region. Plane, Facet: normal axis, axis.p = offset. Cylinder: axis through center, radius.
    // Cone: apex center, axis toward the wide side, half-angle angle. Sphere: center, radius. Torus: center, axis,
    // radius of the circle of the centers of the tube, minor radius of the tube.
    // With poles: a B-spline surface of degree 3, nu x nv poles (pole (k, l) at k * nv + l) on clamped knots (any kind
    // of level 4: a helix, a free surface, an extrusion or a revolution which its profile does not explain within the
    // tolerance). Else Extrusion, Revolution: a profile, a B-spline curve of degree 3 (clamped knots, poles), in the
    // plane through the origin square to the axis, (x, y) along (e1, e2), moved along the axis (extrusion); or in the
    // half plane of a meridian, (radius, height) along (e1, axis) from center, turned around the axis (revolution).
    struct Surface
    {
        Kind                kind{ Kind::Plane };
        Vec3d               axis{ Vec3d::UnitZ() };
        double              offset{ 0. };
        Vec3d               center{ Vec3d::Zero() };
        double              radius{ 0. };
        double              minor{ 0. };
        double              angle{ 0. };
        Vec3d               e1{ Vec3d::UnitX() };
        Vec3d               e2{ Vec3d::UnitY() };
        std::vector<double> knots;
        std::vector<Vec2d>  profile;
        std::vector<double> uknots;
        std::vector<double> vknots;
        int                 nu{ 0 };
        int                 nv{ 0 };
        std::vector<Vec3d>  poles;
    };

    struct Result
    {
        bool   valid{ false };
        size_t solids{ 0 };
        size_t faces{ 0 };
        // mm3.
        double volume{ 0. };
        // Level 4: the regions whose face OpenCASCADE does not find valid once healed (see with_facets()).
        std::vector<int> invalid;
    };

    // The mesh is not closed: edges of a single face, edges of more than two faces (or two faces facing the same way).
    struct NotClosed : std::runtime_error
    {
        NotClosed(size_t open, size_t shared) : std::runtime_error("The mesh is not closed"), open_edges(open), shared_edges(shared) {}
        size_t open_edges;
        size_t shared_edges;
    };
    // Thrown when canceled() turns true.
    struct Canceled
    {};
    // Thrown by write() when the file cannot be written (a folder without the right to write, a file open elsewhere).
    struct NotWritten : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // The mesh split into regions, each with its surface. Throws NotClosed, or Canceled.
    MeshToStep(const indexed_triangle_set &its, const Settings &settings, const std::function<bool()> &canceled = [] { return false; });

    const indexed_triangle_set &mesh() const { return m_its; }
    // A copy whose given regions are made of facets, the planes of their flat pieces as at level 2, the others kept:
    // for the regions of level 4 whose face OpenCASCADE does not find valid (Result::invalid).
    MeshToStep with_facets(const std::vector<int> &regions) const;
    // Region of each face.
    const std::vector<int>     &regions() const { return m_region; }
    const std::vector<Surface> &surfaces() const { return m_surfaces; }

    // The solids (one per piece of the mesh) written into a STEP file, under that name. progress gets a fraction of the
    // work done, from the thread which calls. Level 4: fitted_edges false, straight edges between the mesh vertices as
    // below; its validity is that of the file read back. Throws NotWritten if the file cannot be written,
    // Slic3r::RuntimeError if OpenCASCADE fails on the solid, or Canceled.
    Result write(const std::string &path, const std::string &name, const std::function<void(double)> &progress,
                 const std::function<bool()> &canceled, bool fitted_edges = true) const;

private:
    void face_regions(const std::function<bool()> &canceled);
    void planar_regions(double tolerance, double angle, const std::function<bool()> &canceled);
    void recognize(bool strict, const std::function<bool()> &canceled);
    void recognize_free(const std::function<bool()> &canceled);
    void name_facets();

    indexed_triangle_set m_its;
    Settings             m_settings;
    // Face across each edge of a face (edge k goes from vertex k to vertex k + 1).
    std::vector<Vec3i>   m_twin;
    std::vector<Vec3d>   m_normal;
    std::vector<double>  m_area;
    // How far the inside of each face, and the middle of each of its edges (3 f + k), may be from a smooth surface
    // through the vertices: a face of a curved part is a chord of it.
    std::vector<double>  m_sagitta;
    std::vector<double>  m_edge_sagitta;
    std::vector<int>     m_region;
    std::vector<Surface> m_surfaces;
};

} // namespace Slic3r

#endif // slic3r_MeshToStep_hpp_

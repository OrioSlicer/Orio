#include "MeshToStep.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/libslic3r.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <ElCLib.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeSegment.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomAbs_Shape.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_Circle.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <Geom_SurfaceOfLinearExtrusion.hxx>
#include <Geom_SurfaceOfRevolution.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Precision.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Reader.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Slic3r {

namespace {

using Kind    = MeshToStep::Kind;
using Surface = MeshToStep::Surface;

uint64_t edge_key(int u, int v) { return (uint64_t(uint32_t(u)) << 32) | uint64_t(uint32_t(v)); }

void check(const std::function<bool()> &canceled)
{
    if (canceled())
        throw MeshToStep::Canceled();
}

Vec3d vertex(const indexed_triangle_set &its, int v) { return its.vertices[v].cast<double>(); }

// Least squares plane: unit normal, offset (normal.p = offset), largest distance of the points to it.
struct PlaneFit
{
    Vec3d  normal;
    double offset;
    double error;
};

PlaneFit fit_plane(const std::vector<Vec3d> &points)
{
    Vec3d c = Vec3d::Zero();
    for (const Vec3d &p : points)
        c += p;
    c /= double(points.size());
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (const Vec3d &p : points)
        cov += (p - c) * (p - c).transpose();
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(cov);
    const Vec3d                                          n = solver.eigenvectors().col(0);
    const double                                         d = n.dot(c);
    double                                               error = 0.;
    for (const Vec3d &p : points)
        error = std::max(error, std::abs(n.dot(p) - d));
    return { n, d, error };
}

// Circle through 2D points: algebraic fit, then Gauss-Newton. False if it went wrong.
bool fit_circle(const std::vector<Vec2d> &q, Vec2d &center, double &radius)
{
    const Eigen::Index n = Eigen::Index(q.size());
    if (n < 3)
        return false;
    Eigen::MatrixXd A(n, 3);
    Eigen::VectorXd b(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        A(i, 0) = 2. * q[i].x();
        A(i, 1) = 2. * q[i].y();
        A(i, 2) = 1.;
        b(i)    = q[i].squaredNorm();
    }
    const Eigen::Vector3d sol = A.colPivHouseholderQr().solve(b);
    center                    = Vec2d(sol(0), sol(1));
    radius                    = std::sqrt(std::max(sol(2) + center.squaredNorm(), 0.));
    for (int iteration = 0; iteration < 10; ++iteration) {
        Eigen::MatrixXd J(n, 3);
        Eigen::VectorXd residual(n);
        for (Eigen::Index i = 0; i < n; ++i) {
            const Vec2d  d    = q[i] - center;
            const double dist = std::max(d.norm(), 1e-300);
            residual(i)       = dist - radius;
            J(i, 0)           = -d.x() / dist;
            J(i, 1)           = -d.y() / dist;
            J(i, 2)           = -1.;
        }
        const Eigen::Vector3d step = J.colPivHouseholderQr().solve(-residual);
        center += Vec2d(step(0), step(1));
        radius += step(2);
        if (step.cwiseAbs().maxCoeff() < 1e-12)
            break;
    }
    radius = std::abs(radius);
    return std::isfinite(radius) && std::isfinite(center.x()) && std::isfinite(center.y());
}

// Two unit vectors making a direct orthonormal frame with a.
void basis(const Vec3d &a, Vec3d &u, Vec3d &v)
{
    const Vec3d t = std::abs(a.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
    u             = a.cross(t).normalized();
    v             = a.cross(u);
}

double distance(const Surface &s, const Vec3d &p)
{
    switch (s.kind) {
    case Kind::Cylinder: {
        const Vec3d q = p - s.center;
        return std::abs((q - q.dot(s.axis) * s.axis).norm() - s.radius);
    }
    case Kind::Cone: {
        const Vec3d  q   = p - s.center;
        const double h   = q.dot(s.axis);
        const double rho = (q - h * s.axis).norm();
        return std::abs(rho * std::cos(s.angle) - h * std::sin(s.angle));
    }
    case Kind::Sphere: return std::abs((p - s.center).norm() - s.radius);
    case Kind::Torus: {
        const Vec3d  q   = p - s.center;
        const double h   = q.dot(s.axis);
        const double rho = (q - h * s.axis).norm();
        return std::abs(std::hypot(rho - s.radius, h) - s.minor);
    }
    default: return std::abs(s.axis.dot(p) - s.offset);
    }
}

// Normal of the surface at a point near it, away from its axis (or center, or tube).
Vec3d normal(const Surface &s, const Vec3d &p)
{
    switch (s.kind) {
    case Kind::Cylinder: {
        const Vec3d q = p - s.center;
        return (q - q.dot(s.axis) * s.axis).normalized();
    }
    case Kind::Cone: {
        const Vec3d q      = p - s.center;
        const Vec3d radial = (q - q.dot(s.axis) * s.axis).normalized();
        return radial * std::cos(s.angle) - s.axis * std::sin(s.angle);
    }
    case Kind::Sphere: return (p - s.center).normalized();
    case Kind::Torus: {
        const Vec3d q      = p - s.center;
        const Vec3d radial = (q - q.dot(s.axis) * s.axis).normalized();
        return (q - s.radius * radial).normalized();
    }
    default: return s.axis;
    }
}

// Points P of a group of flat pieces, and the centers C, normals N and areas W of the pieces.
struct Samples
{
    std::vector<Vec3d>  P;
    std::vector<Vec3d>  C;
    std::vector<Vec3d>  N;
    std::vector<double> W;
};

// strict (level 4): the normals must turn clearly more around the axis than off it, which finds a cylinder even on a
// narrow strip.
std::optional<Surface> fit_cylinder(const Samples &s, bool strict)
{
    // The axis goes the way the normals do not.
    Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
    for (size_t i = 0; i < s.N.size(); ++i)
        M += s.W[i] * s.N[i] * s.N[i].transpose();
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(M);
    const Eigen::Vector3d                                vals = solver.eigenvalues();
    if (strict ? vals(1) < 1e-10 * std::max(vals(2), 1e-300) || vals(0) > 0.02 * vals(1) : vals(1) < 1e-3 * vals(2))
        return {};
    const Vec3d a = solver.eigenvectors().col(0);
    Vec3d       u, v;
    basis(a, u, v);
    std::vector<Vec2d> q;
    q.reserve(s.P.size());
    for (const Vec3d &p : s.P)
        q.emplace_back(p.dot(u), p.dot(v));
    Vec2d  c;
    double r;
    if (!fit_circle(q, c, r))
        return {};
    Surface out;
    out.kind   = Kind::Cylinder;
    out.axis   = a;
    out.center = c.x() * u + c.y() * v;
    out.radius = r;
    return out;
}

// Point of the axis of direction a which the normal lines (C, N) meet best: every normal of a surface of revolution
// meets its axis.
std::optional<Vec3d> axis_point(const Samples &s, const Vec3d &a)
{
    Vec3d u, v;
    basis(a, u, v);
    std::vector<Vec2d>  rows;
    std::vector<double> rhs;
    for (size_t i = 0; i < s.N.size(); ++i) {
        Vec2d        n2(s.N[i].dot(u), s.N[i].dot(v));
        const double L = n2.norm();
        if (L <= 1e-6)
            continue;
        n2 /= L;
        const Vec2d  p2(s.C[i].dot(u), s.C[i].dot(v));
        const Vec2d  perp(-n2.y(), n2.x());
        const double w = std::sqrt(s.W[i]);
        rows.emplace_back(perp.x() * w, perp.y() * w);
        rhs.push_back(perp.dot(p2) * w);
    }
    if (rows.size() < 2)
        return {};
    Eigen::MatrixXd A(Eigen::Index(rows.size()), 2);
    Eigen::VectorXd b(Eigen::Index(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i) {
        A.row(Eigen::Index(i)) = rows[i].transpose();
        b(Eigen::Index(i))     = rhs[i];
    }
    const Eigen::Vector2d x = A.colPivHouseholderQr().solve(b);
    return Vec3d(x(0) * u + x(1) * v);
}

std::optional<Surface> fit_cone(const Samples &s)
{
    // The normals of a cone make a constant angle with its axis: they lie on a plane (as points of the unit sphere)
    // whose normal is the axis.
    double W = 0.;
    Vec3d  m = Vec3d::Zero();
    for (size_t i = 0; i < s.N.size(); ++i) {
        m += s.W[i] * s.N[i];
        W += s.W[i];
    }
    m /= W;
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (size_t i = 0; i < s.N.size(); ++i)
        cov += s.W[i] * (s.N[i] - m) * (s.N[i] - m).transpose();
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(cov);
    Vec3d                                                a = solver.eigenvectors().col(0);
    if (std::abs(m.dot(a)) < 1e-3 || solver.eigenvalues()(1) < 1e-6)
        return {};
    const std::optional<Vec3d> x = axis_point(s, a);
    if (!x)
        return {};
    // Radius against height along the axis: a line, rho = rho0 + h tan(angle), whose zero is the apex.
    double sh = 0., shh = 0., sr = 0., shr = 0.;
    for (const Vec3d &p : s.P) {
        const Vec3d  q   = p - *x;
        const double h   = q.dot(a);
        const double rho = (q - h * a).norm();
        sh += h;
        shh += h * h;
        sr += rho;
        shr += h * rho;
    }
    const double n   = double(s.P.size());
    const double det = n * shh - sh * sh;
    if (std::abs(det) < 1e-300)
        return {};
    double slope = (n * shr - sh * sr) / det;
    double rho0  = (sr - slope * sh) / n;
    if (std::abs(slope) < 1e-6)
        return {};
    double apex = -rho0 / slope;
    if (slope < 0.) {
        a     = -a;
        slope = -slope;
        apex  = -apex;
    }
    Surface out;
    out.kind   = Kind::Cone;
    out.axis   = a;
    out.center = *x + apex * a;
    out.angle  = std::atan(slope);
    return out;
}

std::optional<Surface> fit_sphere(const Samples &s)
{
    const Eigen::Index n = Eigen::Index(s.P.size());
    if (n < 4)
        return {};
    Eigen::MatrixXd A(n, 4);
    Eigen::VectorXd b(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        const Vec3d &p = s.P[size_t(i)];
        A.row(i) << 2. * p.x(), 2. * p.y(), 2. * p.z(), 1.;
        b(i) = p.squaredNorm();
    }
    const Eigen::Vector4d sol = A.colPivHouseholderQr().solve(b);
    const Vec3d           c(sol(0), sol(1), sol(2));
    const double          r2 = sol(3) + c.squaredNorm();
    if (!(r2 > 0.))
        return {};
    Surface out;
    out.kind   = Kind::Sphere;
    out.center = c;
    out.radius = std::sqrt(r2);
    return out;
}

std::optional<Surface> fit_torus(const Samples &s)
{
    // The axis of a surface of revolution is the line which meets every normal line (Pottmann): for the lines
    // (C x N, N), the axis (d, m) minimizes the sum of (d.(C x N) + m.N)^2 with |d| = 1.
    Eigen::Matrix<double, 6, 6> M = Eigen::Matrix<double, 6, 6>::Zero();
    for (size_t i = 0; i < s.N.size(); ++i) {
        Eigen::Matrix<double, 6, 1> line;
        line << s.C[i].cross(s.N[i]), s.N[i];
        M += s.W[i] * line * line.transpose();
    }
    const Eigen::Matrix3d A  = M.topLeftCorner<3, 3>();
    const Eigen::Matrix3d B  = M.topRightCorner<3, 3>();
    const Eigen::Matrix3d Cm = M.bottomRightCorner<3, 3>();
    const Eigen::FullPivLU<Eigen::Matrix3d> lu(Cm);
    if (!lu.isInvertible())
        return {};
    const Eigen::Matrix3d                                Ci = lu.inverse();
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(A - B * Ci * B.transpose());
    const Eigen::Vector3d                                vals = solver.eigenvalues();
    // All normals through one point (a sphere), or parallel (a plane): no axis.
    if (vals(1) < 1e-6 * std::max(vals(2), 1e-300))
        return {};
    const Eigen::Vector3d d  = solver.eigenvectors().col(0);
    const Eigen::Vector3d mm = -Ci * B.transpose() * d;
    const Vec3d           a  = Vec3d(d).normalized();
    const Vec3d           x  = Vec3d(d.cross(mm));
    // In the half-plane of a meridian, the points lie on the circle of the tube.
    std::vector<Vec2d> q;
    q.reserve(s.P.size());
    for (const Vec3d &p : s.P) {
        const Vec3d  r = p - x;
        const double h = r.dot(a);
        q.emplace_back((r - h * a).norm(), h);
    }
    Vec2d  c;
    double r;
    if (!fit_circle(q, c, r) || c.x() <= r)
        return {};
    Surface out;
    out.kind   = Kind::Torus;
    out.axis   = a;
    out.center = x + c.y() * a;
    out.radius = c.x();
    out.minor  = r;
    return out;
}

// The curved surfaces which pass within tolerance of every point, the simplest first (cylinder, cone, sphere,
// torus), with their largest distance. size: size of the part, to turn away nearly flat fits (huge radii).
std::vector<std::pair<Surface, double>> all_fits(const Samples &s, double tolerance, double size, bool strict)
{
    std::vector<std::pair<Surface, double>> found;
    for (int kind = 0; kind < 4; ++kind) {
        const std::optional<Surface> shape = kind == 0 ? fit_cylinder(s, strict) : kind == 1 ? fit_cone(s) : kind == 2 ? fit_sphere(s) : fit_torus(s);
        if (!shape)
            continue;
        if (shape->kind != Kind::Cone && (!std::isfinite(shape->radius) || shape->radius > 20. * size))
            continue;
        if (shape->kind == Kind::Cone && (shape->angle < 0.5 * PI / 180. || shape->angle > 89.5 * PI / 180.))
            continue;
        double error = 0.;
        for (const Vec3d &p : s.P)
            error = std::max(error, distance(*shape, p));
        if (error <= tolerance)
            found.emplace_back(*shape, error);
    }
    return found;
}

// The surface which passes closest to the points; a simpler one when it does nearly as well: with the exact vertices
// of a CAD model, the true one fits much better than the others.
std::optional<Surface> best_fit(const Samples &s, double tolerance, double size, bool strict)
{
    const std::vector<std::pair<Surface, double>> found = all_fits(s, tolerance, size, strict);
    if (found.empty())
        return {};
    double best = found.front().second;
    for (const auto &f : found)
        best = std::min(best, f.second);
    for (const auto &f : found)
        if (f.second <= std::max(3. * best, 0.02 * tolerance))
            return f.first;
    return {};
}

// Simplifies a polyline (Douglas-Peucker): the indices kept, so that it stays within tolerance; at least `at_least`
// points kept between the ends when there are.
std::vector<int> douglas_peucker(const std::vector<Vec3d> &points, double tolerance, int at_least)
{
    std::vector<int>                 keep{ 0, int(points.size()) - 1 };
    std::vector<std::pair<int, int>> stack{ { 0, int(points.size()) - 1 } };
    while (!stack.empty()) {
        const auto [i, j] = stack.back();
        stack.pop_back();
        if (j <= i + 1)
            continue;
        const Vec3d  a  = points[i];
        const Vec3d  ab = points[j] - a;
        const double L  = ab.norm();
        int          m  = -1;
        double       dm = -1.;
        for (int k = i + 1; k < j; ++k) {
            const double d = L < 1e-12 ? (points[k] - a).norm() : (points[k] - a).cross(ab / L).norm();
            if (d > dm) {
                dm = d;
                m  = k;
            }
        }
        if (dm > tolerance || int(keep.size()) - 2 < at_least) {
            keep.push_back(m);
            stack.emplace_back(i, m);
            stack.emplace_back(m, j);
        }
    }
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    return keep;
}

// OCCT progress into a part [from, to] of ours, stopped by canceled().
class Indicator : public Message_ProgressIndicator
{
public:
    Indicator(const std::function<void(double)> &report, const std::function<bool()> &canceled, double from, double to)
        : m_report(report), m_canceled(canceled), m_from(from), m_to(to)
    {}
    Standard_Boolean UserBreak() override { return m_canceled(); }
    void             Show(const Message_ProgressScope &, const Standard_Boolean) override
    {
        const double at = m_from + (m_to - m_from) * GetPosition();
        if (at >= m_last + 0.01) {
            m_last = at;
            m_report(at);
        }
    }

private:
    const std::function<void(double)> &m_report;
    const std::function<bool()>       &m_canceled;
    double                             m_from;
    double                             m_to;
    double                             m_last{ -1. };
};

gp_Pnt pnt(const Vec3d &p) { return gp_Pnt(p.x(), p.y(), p.z()); }
Vec3d  vec(const gp_Pnt &p) { return Vec3d(p.X(), p.Y(), p.Z()); }

// Circle through 3D points, in their mean plane: center, radius, and the largest distance of the points to it.
bool fit_circle_3d(const std::vector<Vec3d> &P, Vec3d &center, double &radius, double &error)
{
    Vec3d c = Vec3d::Zero();
    for (const Vec3d &p : P)
        c += p;
    c /= double(P.size());
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (const Vec3d &p : P)
        cov += (p - c) * (p - c).transpose();
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(cov);
    const Vec3d n = solver.eigenvectors().col(0), e1 = solver.eigenvectors().col(2), e2 = solver.eigenvectors().col(1);
    std::vector<Vec2d> q;
    for (const Vec3d &p : P)
        q.emplace_back((p - c).dot(e1), (p - c).dot(e2));
    Vec2d c2;
    if (!fit_circle(q, c2, radius))
        return false;
    center = c + c2.x() * e1 + c2.y() * e2;
    error  = 0.;
    for (const Vec3d &p : P) {
        const Vec3d  rel = p - center;
        const double h   = rel.dot(n);
        error            = std::max(error, std::hypot(h, (rel - h * n).norm() - radius));
    }
    return true;
}

// The distinct values of a clamped knot vector and their multiplicities, as OpenCASCADE takes them.
struct Knots
{
    TColStd_Array1OfReal    values;
    TColStd_Array1OfInteger mults;

    explicit Knots(const std::vector<double> &kv) : values(1, distinct(kv)), mults(1, distinct(kv))
    {
        int at = 0;
        for (size_t i = 0; i < kv.size(); ++i)
            if (i > 0 && std::abs(kv[i] - kv[i - 1]) < 1e-12)
                mults.ChangeValue(at) += 1;
            else {
                ++at;
                values.SetValue(at, kv[i]);
                mults.SetValue(at, 1);
            }
    }
    static int distinct(const std::vector<double> &kv)
    {
        int count = 0;
        for (size_t i = 0; i < kv.size(); ++i)
            count += i == 0 || std::abs(kv[i] - kv[i - 1]) >= 1e-12;
        return count;
    }
};

Handle(Geom_Surface) occt_surface(const Surface &s, const std::vector<Vec3d> &points)
{
    auto ax3 = [](const Vec3d &o, const Vec3d &z) { return gp_Ax3(gp_Pnt(o.x(), o.y(), o.z()), gp_Dir(z.x(), z.y(), z.z())); };
    if (!s.poles.empty()) {
        TColgp_Array2OfPnt poles(1, s.nu, 1, s.nv);
        for (int k = 0; k < s.nu; ++k)
            for (int l = 0; l < s.nv; ++l)
                poles.SetValue(k + 1, l + 1, pnt(s.poles[size_t(k) * size_t(s.nv) + size_t(l)]));
        const Knots ku(s.uknots), kv(s.vknots);
        return new Geom_BSplineSurface(poles, ku.values, kv.values, ku.mults, kv.mults, 3, 3);
    }
    if (s.kind == Kind::Extrusion || s.kind == Kind::Revolution) {
        // The profile in space: in the plane through the origin square to the axis (extrusion), or in the half plane of
        // a meridian (revolution).
        const bool         extrusion = s.kind == Kind::Extrusion;
        TColgp_Array1OfPnt poles(1, int(s.profile.size()));
        for (size_t i = 0; i < s.profile.size(); ++i) {
            const Vec2d &q = s.profile[i];
            poles.SetValue(int(i) + 1, pnt(extrusion ? Vec3d(q.x() * s.e1 + q.y() * s.e2) : Vec3d(s.center + q.x() * s.e1 + q.y() * s.axis)));
        }
        const Knots               k(s.knots);
        Handle(Geom_BSplineCurve) curve = new Geom_BSplineCurve(poles, k.values, k.mults, 3);
        const gp_Dir              d(s.axis.x(), s.axis.y(), s.axis.z());
        if (extrusion)
            return new Geom_SurfaceOfLinearExtrusion(curve, d);
        return new Geom_SurfaceOfRevolution(curve, gp_Ax1(pnt(s.center), d));
    }
    switch (s.kind) {
    case Kind::Cylinder: return new Geom_CylindricalSurface(ax3(s.center, s.axis), s.radius);
    case Kind::Cone: {
        // The reference circle at the mean height of the region: the apex is a singular point of the parameters.
        double h = 0.;
        for (const Vec3d &p : points)
            h += (p - s.center).dot(s.axis);
        h = std::max(h / double(points.size()), 0.);
        return new Geom_ConicalSurface(ax3(s.center + h * s.axis, s.axis), s.angle, h * std::tan(s.angle));
    }
    case Kind::Sphere: {
        // Its poles, where the parameters are singular, away from the region: along the direction its normals cover
        // least (square to the middle of a cap, along the axis of a band). A face around a pole is not valid.
        Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
        for (const Vec3d &p : points) {
            const Vec3d n = (p - s.center).normalized();
            M += n * n.transpose();
        }
        const Vec3d axis = points.empty() ? Vec3d(Vec3d::UnitZ()) : Vec3d(Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d>(M).eigenvectors().col(0));
        return new Geom_SphericalSurface(ax3(s.center, axis), s.radius);
    }
    case Kind::Torus: return new Geom_ToroidalSurface(ax3(s.center, s.axis), s.radius, s.minor);
    default: return new Geom_Plane(ax3(s.offset * s.axis, s.axis));
    }
}

// Does the normal of the OCCT surface (Du x Dv) point the way of the faces of the mesh near the point?
bool same_side(const Handle(Geom_Surface) &surface, const Vec3d &point, const Vec3d &mesh_normal)
{
    GeomAPI_ProjectPointOnSurf projection(gp_Pnt(point.x(), point.y(), point.z()), surface);
    if (projection.NbPoints() == 0)
        return true;
    Standard_Real u, v;
    projection.LowerDistanceParameters(u, v);
    GeomLProp_SLProps props(surface, u, v, 1, 1e-9);
    if (!props.IsNormalDefined())
        return true;
    const gp_Dir n = props.Normal();
    return n.X() * mesh_normal.x() + n.Y() * mesh_normal.y() + n.Z() * mesh_normal.z() > 0.;
}

// ---- Level 4: least squares B-splines of degree 3 on clamped uniform knots ----

std::vector<double> clamped_knots(int spans, int degree, double lo, double hi)
{
    std::vector<double> kv;
    for (int i = 0; i < degree; ++i)
        kv.push_back(lo);
    // The last one exactly hi: lo + (hi - lo) may miss it by a rounding, a knot of its own for OpenCASCADE.
    for (int i = 0; i <= spans; ++i)
        kv.push_back(i == spans ? hi : lo + (hi - lo) * double(i) / double(spans));
    for (int i = 0; i < degree; ++i)
        kv.push_back(hi);
    return kv;
}

// Span of t in a clamped knot vector (t is brought into its range): degree <= span < number of functions.
int find_span(const std::vector<double> &kv, int degree, double t)
{
    const int n = int(kv.size()) - degree - 1;
    if (t >= kv[n])
        return n - 1;
    if (t <= kv[degree])
        return degree;
    int lo = degree, hi = n;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (t < kv[mid])
            hi = mid;
        else
            lo = mid;
    }
    return lo;
}

// The degree + 1 basis functions which are not zero at t (Cox - de Boor): N[j] is that of index span - degree + j.
void basis_values(const std::vector<double> &kv, int degree, int span, double t, double *N)
{
    const int n = int(kv.size()) - degree - 1;
    t           = std::clamp(t, kv[degree], kv[n]);
    double left[8], right[8];
    N[0] = 1.;
    for (int j = 1; j <= degree; ++j) {
        left[j]      = t - kv[span + 1 - j];
        right[j]     = kv[span + j] - t;
        double saved = 0.;
        for (int r = 0; r < j; ++r) {
            const double den = right[r + 1] + left[j - r];
            const double tmp = den != 0. ? N[r] / den : 0.;
            N[r]             = saved + right[r + 1] * tmp;
            saved            = left[j - r] * tmp;
        }
        N[j] = saved;
    }
}

// A B-spline curve of degree 3 in a plane.
struct Curve2
{
    std::vector<double> kv;
    std::vector<Vec2d>  poles;

    double first() const { return kv[3]; }
    double last() const { return kv[kv.size() - 4]; }
    Vec2d  at(double t) const
    {
        const int span = find_span(kv, 3, t);
        double    N[4];
        basis_values(kv, 3, span, t, N);
        Vec2d p = Vec2d::Zero();
        for (int j = 0; j < 4; ++j)
            p += N[j] * poles[span - 3 + j];
        return p;
    }
    // Points at count parameters evenly spread.
    void polyline(int count, std::vector<double> &ts, std::vector<Vec2d> &pts) const
    {
        ts.resize(size_t(count));
        pts.resize(size_t(count));
        for (int i = 0; i < count; ++i) {
            ts[i]  = first() + (last() - first()) * double(i) / double(count - 1);
            pts[i] = at(ts[i]);
        }
    }
};

// Closest point of each Q on the polyline (whose vertices have the parameters ts): distance and parameter.
void project_on_polyline(const std::vector<Vec2d> &Q, const std::vector<double> &ts, const std::vector<Vec2d> &poly, std::vector<double> &dist,
                         std::vector<double> &param)
{
    dist.assign(Q.size(), std::numeric_limits<double>::max());
    param.assign(Q.size(), 0.);
    for (size_t i = 0; i < Q.size(); ++i)
        for (size_t s = 0; s + 1 < poly.size(); ++s) {
            const Vec2d  ab = poly[s + 1] - poly[s];
            const double L2 = std::max(ab.squaredNorm(), 1e-300);
            const double w  = std::clamp((Q[i] - poly[s]).dot(ab) / L2, 0., 1.);
            const double d  = (Q[i] - (poly[s] + w * ab)).norm();
            if (d < dist[i]) {
                dist[i]  = d;
                param[i] = ts[s] + w * (ts[s + 1] - ts[s]);
            }
        }
}

// Weighted least squares curve of `spans` spans through the points Q of parameters t in [0, 1] (the knots go a little
// past both ends), a light smoothing of the second differences of the poles; the parameters corrected by projection
// after each round. dist: distances of the points to the curve at the end.
Curve2 fit_curve_weighted(const std::vector<Vec2d> &Q, std::vector<double> &t, int spans, const std::vector<double> &w, std::vector<double> &dist,
                          int rounds = 3)
{
    const double margin = 0.04, smooth = 1e-6;
    Curve2       c;
    c.kv        = clamped_knots(spans, 3, -margin, 1. + margin);
    const int n = spans + 3;
    for (int round = 0; round < rounds; ++round) {
        Eigen::MatrixXd BtB = Eigen::MatrixXd::Zero(n, n);
        Eigen::MatrixXd BtQ = Eigen::MatrixXd::Zero(n, 2);
        for (size_t i = 0; i < Q.size(); ++i) {
            const int span = find_span(c.kv, 3, t[i]);
            double    N[4];
            basis_values(c.kv, 3, span, t[i], N);
            for (int a = 0; a < 4; ++a) {
                for (int b = 0; b < 4; ++b)
                    BtB(span - 3 + a, span - 3 + b) += w[i] * N[a] * N[b];
                BtQ(span - 3 + a, 0) += w[i] * N[a] * Q[i].x();
                BtQ(span - 3 + a, 1) += w[i] * N[a] * Q[i].y();
            }
        }
        Eigen::MatrixXd DtD = Eigen::MatrixXd::Zero(n, n);
        for (int r = 0; r + 2 < n; ++r) {
            const double coef[3] = { 1., -2., 1. };
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    DtD(r + a, r + b) += coef[a] * coef[b];
        }
        const double    lambda = smooth * BtB.trace() / std::max(DtD.trace(), 1e-300);
        Eigen::MatrixXd A      = BtB + lambda * DtD;
        A.diagonal().array() += 1e-12;
        const Eigen::MatrixXd P = A.ldlt().solve(BtQ);
        c.poles.resize(size_t(n));
        for (int i = 0; i < n; ++i)
            c.poles[i] = Vec2d(P(i, 0), P(i, 1));
        std::vector<double> ts;
        std::vector<Vec2d>  poly;
        c.polyline(std::max(200, 20 * spans), ts, poly);
        project_on_polyline(Q, ts, poly, dist, t);
    }
    return c;
}

// Curve within tolerance of the points Q (in their order, parameters t in [0, 1]): spans doubled until it fits.
std::optional<std::pair<Curve2, double>> fit_curve(const std::vector<Vec2d> &Q, std::vector<double> t, double tolerance, int max_spans)
{
    const std::vector<double> w(Q.size(), 1.);
    for (int spans = 1;; spans *= 2) {
        std::vector<double> dist;
        Curve2              c   = fit_curve_weighted(Q, t, spans, w, dist, 4);
        const double        err = dist.empty() ? 0. : *std::max_element(dist.begin(), dist.end());
        if (err <= tolerance)
            return std::make_pair(std::move(c), err);
        if (spans >= max_spans || spans + 3 >= int(Q.size()))
            return {};
    }
}

// A B-spline surface of degree 3 in u and v: nu x nv poles, u major (pole (k, l) at k * nv + l).
struct BSurface
{
    std::vector<double> ku, kv;
    int                 nu{ 0 }, nv{ 0 };
    std::vector<Vec3d>  poles;
};

// Surface S(u, v) on the box (u0, u1, v0, v1) with every S(U_i, V_i) within tolerance of X_i (limits: a distance of
// its own for each point instead): spans doubled along u or v (the one which lowers the error most) from (su, sv) until
// it fits; a thin plate term keeps the parts without points smooth. Sparse normal equations. The poles must stay near
// the points.
std::optional<std::pair<BSurface, double>> fit_surface(const std::vector<double> &U, const std::vector<double> &V, const std::vector<Vec3d> &X,
                                                       const double box[4], double tolerance, int max_poles, int su, int sv,
                                                       const std::vector<double> *limits = nullptr)
{
    const double smooth = 1e-4;
    const size_t m      = X.size();
    auto         solve  = [&](int spans_u, int spans_v, BSurface &out) {
        out.ku          = clamped_knots(spans_u, 3, box[0], box[1]);
        out.kv          = clamped_knots(spans_v, 3, box[2], box[3]);
        out.nu          = spans_u + 3;
        out.nv          = spans_v + 3;
        const int    n  = out.nu * out.nv;
        const double hu = (box[1] - box[0]) / double(spans_u), hv = (box[3] - box[2]) / double(spans_v);
        // The design matrix, a row of 16 products of basis functions per point (its normal equations by a sparse
        // product: 16 entries a point, not 256).
        std::vector<Eigen::Triplet<double>> entries, dtd;
        entries.reserve(m * 16);
        Eigen::MatrixXd Xm(static_cast<Eigen::Index>(m), 3);
        double          trace_a = 0., trace_d = 0.;
        for (size_t i = 0; i < m; ++i) {
            const int span_u = find_span(out.ku, 3, U[i]);
            const int span_v = find_span(out.kv, 3, V[i]);
            double    Nu[4], Nv[4];
            basis_values(out.ku, 3, span_u, U[i], Nu);
            basis_values(out.kv, 3, span_v, V[i], Nv);
            for (int a = 0; a < 4; ++a)
                for (int b = 0; b < 4; ++b) {
                    const double value = Nu[a] * Nv[b];
                    entries.emplace_back(int(i), (span_u - 3 + a) * out.nv + (span_v - 3 + b), value);
                    trace_a += value * value;
                }
            Xm.row(static_cast<Eigen::Index>(i)) = X[i].transpose();
        }
        Eigen::SparseMatrix<double> design(static_cast<Eigen::Index>(m), n);
        design.setFromTriplets(entries.begin(), entries.end());
        // Thin plate: second differences along u, along v and mixed, scaled to the size of the spans.
        auto add_row = [&](const std::vector<std::pair<int, double>> &row) {
            for (const auto &[i, a] : row) {
                trace_d += a * a;
                for (const auto &[j, b] : row)
                    dtd.emplace_back(i, j, a * b);
            }
        };
        for (int k = 0; k < out.nu; ++k)
            for (int l = 0; l < out.nv; ++l) {
                const int at = k * out.nv + l;
                if (k + 2 < out.nu)
                    add_row({ { at, 1. / (hu * hu) }, { at + out.nv, -2. / (hu * hu) }, { at + 2 * out.nv, 1. / (hu * hu) } });
                if (l + 2 < out.nv)
                    add_row({ { at, 1. / (hv * hv) }, { at + 1, -2. / (hv * hv) }, { at + 2, 1. / (hv * hv) } });
                if (k + 1 < out.nu && l + 1 < out.nv) {
                    const double s = std::sqrt(2.) / (hu * hv);
                    add_row({ { at, s }, { at + 1, -s }, { at + out.nv, -s }, { at + out.nv + 1, s } });
                }
            }
        const double lambda = smooth * trace_a / std::max(trace_d, 1e-300);
        for (Eigen::Triplet<double> &t : dtd)
            t = Eigen::Triplet<double>(t.row(), t.col(), lambda * t.value());
        for (int i = 0; i < n; ++i)
            dtd.emplace_back(i, i, 1e-12);
        Eigen::SparseMatrix<double> plate(n, n);
        plate.setFromTriplets(dtd.begin(), dtd.end());
        const Eigen::SparseMatrix<double>                  equations = Eigen::SparseMatrix<double>(design.transpose() * design) + plate;
        Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(equations);
        if (solver.info() != Eigen::Success)
            return std::numeric_limits<double>::max();
        const Eigen::MatrixXd P = solver.solve(Eigen::MatrixXd(design.transpose() * Xm));
        if (solver.info() != Eigen::Success)
            return std::numeric_limits<double>::max();
        out.poles.resize(size_t(n));
        for (int i = 0; i < n; ++i)
            out.poles[i] = Vec3d(P(i, 0), P(i, 1), P(i, 2));
        const Eigen::MatrixXd residual = Eigen::MatrixXd(design * P) - Xm;
        if (m == 0)
            return 0.;
        if (limits == nullptr)
            return residual.rowwise().norm().maxCoeff();
        // Each point within its own limit: the largest distance over its limit, at the scale of the tolerance.
        double worst = 0.;
        for (size_t i = 0; i < m; ++i)
            worst = std::max(worst, residual.row(static_cast<Eigen::Index>(i)).norm() / (*limits)[i]);
        return worst * tolerance;
    };
    if ((su + 3) * (sv + 3) > max_poles)
        return {};
    BSurface surface;
    double   err = solve(su, sv, surface);
    while (err > tolerance) {
        BSurface best;
        double   best_err = std::numeric_limits<double>::max();
        int      best_u = 0, best_v = 0;
        for (const auto &[tu, tv] : { std::make_pair(2 * su, sv), std::make_pair(su, 2 * sv) }) {
            if ((tu + 3) * (tv + 3) > std::min(max_poles, int(m)))
                continue;
            BSurface     s;
            const double e = solve(tu, tv, s);
            if (e < best_err) {
                best_err = e;
                best     = std::move(s);
                best_u   = tu;
                best_v   = tv;
            }
        }
        if (best_u == 0)
            return {};
        surface = std::move(best);
        err     = best_err;
        su      = best_u;
        sv      = best_v;
    }
    BoundingBoxf3 bb;
    for (const Vec3d &x : X)
        bb.merge(x);
    const double reach = 2. * bb.size().norm() + 10. * tolerance;
    for (const Vec3d &p : surface.poles)
        for (int k = 0; k < 3; ++k)
            if (p(k) < bb.min(k) - reach || p(k) > bb.max(k) + reach)
                return {};
    return std::make_pair(std::move(surface), err);
}

// ---- Level 4: the mesh as the functions below see it ----

struct MeshView
{
    const indexed_triangle_set &its;
    const std::vector<Vec3i>   &twin;
    const std::vector<Vec3d>   &normal;
    const std::vector<double>  &area;
    // See chord_sagitta().
    const std::vector<double>  &sagitta;
    const std::vector<double>  &edge_sagitta;
    double                      size{ 0. };

    int   faces() const { return int(its.indices.size()); }
    Vec3d point(int v) const { return its.vertices[v].cast<double>(); }
    Vec3d centroid(int f) const { return (point(its.indices[f][0]) + point(its.indices[f][1]) + point(its.indices[f][2])) / 3.; }
};

// How far the inside of each face (face[f]), and the middle of each of its edges (edge[3 f + k]), may be from a smooth
// surface through the vertices, a face of a curved part being a chord of it. Across each smooth edge, the angle between
// the normals over the distance between the centres (the curvature), times the square of the width of the narrower of
// the two faces over 8; a face takes the sum over its edges (a sphere bends both ways under it), the middle of an edge
// the larger of its two faces (the diagonal of a square of a sphere is nearly flat across, not along). Zero where the
// mesh is flat; a sharp edge (over 35 degrees) does not count; a large flat face next to a narrow strip of a fillet stays
// at zero: the bend is in the strip.
void chord_sagitta(const indexed_triangle_set &its, const std::vector<Vec3i> &twin, const std::vector<Vec3d> &normal, const std::vector<double> &area,
                   std::vector<double> &face, std::vector<double> &edge)
{
    const int    nf        = int(its.indices.size());
    const double cos_sharp = std::cos(35. * PI / 180.);
    face.assign(size_t(nf), 0.);
    edge.assign(size_t(nf) * 3, 0.);
    // A Vec3d, not an expression of Eigen over the temporaries of vertex().
    auto centre = [&](int f) -> Vec3d { return (vertex(its, its.indices[f][0]) + vertex(its, its.indices[f][1]) + vertex(its, its.indices[f][2])) / 3.; };
    for (int f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            const int g = twin[f][k];
            if (g < 0)
                continue;
            const double c = std::clamp(normal[f].dot(normal[g]), -1., 1.);
            if (c < cos_sharp)
                continue;
            const Vec3d  along  = vertex(its, its.indices[f][(k + 1) % 3]) - vertex(its, its.indices[f][k]);
            const double length = std::max(along.norm(), 1e-12);
            // Across the edge only: a long face does not bend along it (the tall triangles of a cylinder).
            const Vec3d  d      = centre(g) - centre(f);
            const double apart  = std::max((d - d.dot(along) / (length * length) * along).norm(), 1e-12);
            const double width  = 2. * std::min(area[f], area[g]) / length;
            face[f] += width * width * std::acos(c) / (8. * apart);
        }
    for (int f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k)
            edge[size_t(f) * 3 + size_t(k)] = std::max(face[f], twin[f][k] >= 0 ? face[twin[f][k]] : 0.);
}

// Points inside faces (their centres and the middles of their edges, each edge once), each a mean of vertices (corners
// and weights: what is known at the vertices is interpolated the same way), with how far each may be from the surface
// besides the tolerance (its sagitta): a surface fitted through the vertices alone may swing over a large face (the
// walls of a slot, triangles from its top to its bottom).
struct Interior
{
    std::vector<Vec3d>              points;
    std::vector<std::array<int, 3>> corners;
    std::vector<Vec3d>              weights;
    std::vector<double>             sagitta;
};

Interior interior_points(const MeshView &mesh, const std::vector<int> &faces)
{
    Interior out;
    for (int f : faces) {
        const Vec3i &t = mesh.its.indices[f];
        out.points.push_back(mesh.centroid(f));
        out.corners.push_back({ t[0], t[1], t[2] });
        out.weights.emplace_back(1. / 3., 1. / 3., 1. / 3.);
        out.sagitta.push_back(mesh.sagitta[f]);
    }
    std::unordered_set<uint64_t> seen;
    for (int f : faces)
        for (int k = 0; k < 3; ++k) {
            const int a = mesh.its.indices[f][k], b = mesh.its.indices[f][(k + 1) % 3];
            if (!seen.insert(edge_key(std::min(a, b), std::max(a, b))).second)
                continue;
            out.points.push_back(0.5 * (mesh.point(a) + mesh.point(b)));
            out.corners.push_back({ a, b, b });
            out.weights.emplace_back(0.5, 0.5, 0.);
            out.sagitta.push_back(mesh.edge_sagitta[size_t(f) * 3 + size_t(k)]);
        }
    return out;
}

// The point of a B-spline surface at (u, v).
Vec3d surface_point(const BSurface &s, double u, double v)
{
    const int span_u = find_span(s.ku, 3, u), span_v = find_span(s.kv, 3, v);
    double    Nu[4], Nv[4];
    basis_values(s.ku, 3, span_u, u, Nu);
    basis_values(s.kv, 3, span_v, v, Nv);
    Vec3d p = Vec3d::Zero();
    for (int a = 0; a < 4; ++a)
        for (int b = 0; b < 4; ++b)
            p += Nu[a] * Nv[b] * s.poles[size_t((span_u - 3 + a) * s.nv + (span_v - 3 + b))];
    return p;
}

// Whether a surface leaves the inside of faces (their points of interior_points(), at the parameters U, V) by more than
// the tolerance and their sagitta.
bool swings(const BSurface &s, const std::vector<double> &U, const std::vector<double> &V, const Interior &inside, double tolerance)
{
    for (size_t i = 0; i < inside.points.size(); ++i)
        if ((surface_point(s, U[i], V[i]) - inside.points[i]).norm() > tolerance + inside.sagitta[i])
            return true;
    return false;
}

// Connected parts of a set of faces (through their edges).
std::vector<std::vector<int>> components(const MeshView &mesh, const std::vector<int> &faces)
{
    std::vector<char> in(size_t(mesh.faces()), 0), seen(size_t(mesh.faces()), 0);
    for (int f : faces)
        in[f] = 1;
    std::vector<std::vector<int>> out;
    for (int s : faces) {
        if (seen[s])
            continue;
        seen[s] = 1;
        std::vector<int> part{ s };
        for (size_t i = 0; i < part.size(); ++i)
            for (int k = 0; k < 3; ++k) {
                const int g = mesh.twin[part[i]][k];
                if (g >= 0 && in[g] && !seen[g]) {
                    seen[g] = 1;
                    part.push_back(g);
                }
            }
        out.push_back(std::move(part));
    }
    return out;
}

// Vertices of a set of faces (sorted) and its edges, as pairs of indices into them.
void vertex_graph(const MeshView &mesh, const std::vector<int> &faces, std::vector<int> &verts, std::vector<std::pair<int, int>> &edges)
{
    verts.clear();
    for (int f : faces)
        for (int j = 0; j < 3; ++j)
            verts.push_back(mesh.its.indices[f][j]);
    sort_remove_duplicates(verts);
    auto local = [&](int v) { return int(std::lower_bound(verts.begin(), verts.end(), v) - verts.begin()); };
    edges.clear();
    for (int f : faces)
        for (int j = 0; j < 3; ++j) {
            const int a = local(mesh.its.indices[f][j]), b = local(mesh.its.indices[f][(j + 1) % 3]);
            edges.emplace_back(std::min(a, b), std::max(a, b));
        }
    sort_remove_duplicates(edges);
}

// How much the normals of the faces turn: largest angle to their mean, degrees.
double turning(const MeshView &mesh, const std::vector<int> &faces)
{
    Vec3d m = Vec3d::Zero();
    for (int f : faces)
        m += mesh.area[f] * mesh.normal[f];
    if (m.norm() < 1e-9)
        return 180.;
    m.normalize();
    double lowest = 1.;
    for (int f : faces)
        lowest = std::min(lowest, mesh.normal[f].dot(m));
    return std::acos(std::clamp(lowest, -1., 1.)) * 180. / PI;
}

// ---- Level 4: surfaces made by moving a profile ----

enum class Move { Extrusion, Revolution, Helix };

// A motion which slides a surface along itself: along d (extrusion), around the axis through c of direction d
// (revolution), or both (helix, p the displacement along d per radian). e1, e2 complete d into a direct frame.
struct Motion
{
    Move   kind{ Move::Extrusion };
    Vec3d  d{ Vec3d::UnitZ() };
    Vec3d  c{ Vec3d::Zero() };
    Vec3d  e1{ Vec3d::UnitX() };
    Vec3d  e2{ Vec3d::UnitY() };
    double p{ 0. };

    Vec3d  velocity(const Vec3d &x) const { return kind == Move::Extrusion ? d : Vec3d(d.cross(x - c) + p * d); }
    // Sine of the angle between a normal and the surfaces the motion slides along themselves (0: compatible).
    double off_angle(const Vec3d &x, const Vec3d &n) const
    {
        const Vec3d v = velocity(x);
        return std::abs(v.dot(n)) / std::max(v.norm(), 1e-300);
    }
    double angle(const Vec3d &x) const
    {
        const Vec3d r = x - c;
        return std::atan2(r.dot(e2), r.dot(e1));
    }
    // Coordinates in the plane of the profile: (x, y) square to an extrusion, (radius, height) for a revolution,
    // (radius, height - p theta) for a helix, theta its angle unwrapped along the region.
    Vec2d profile(const Vec3d &x, double theta) const
    {
        if (kind == Move::Extrusion)
            return Vec2d(x.dot(e1), x.dot(e2));
        const Vec3d  r   = x - c;
        double       h   = r.dot(d);
        const double rho = (r - h * d).norm();
        if (kind == Move::Helix)
            h -= p * theta;
        return Vec2d(rho, h);
    }
};

Motion make_motion(Move kind, const Vec3d &direction, const Vec3d &point = Vec3d::Zero(), double p = 0.)
{
    Motion m;
    m.kind = kind;
    m.d    = direction.normalized();
    basis(m.d, m.e1, m.e2);
    m.c = point - point.dot(m.d) * m.d;
    m.p = kind == Move::Helix ? p : 0.;
    return m;
}

// A kinematic surface: its motion and its profile, a curve of the plane of the profile; exact when every vertex lies
// within the tolerance of the surface the motion makes of the curve (otherwise the motion only gives the parameters
// of a free surface).
struct Profile
{
    Motion motion;
    Curve2 curve;
    double error{ 0. };
    bool   exact{ false };
};

// The motion which slides the surface of points X and normals N (weights W) along itself best (Pottmann): smallest
// eigenvector of sum w l l^T with l = (x x n, n), the points centered and scaled first. With the ratio of the two
// smallest eigenvalues (small: a kinematic surface).
std::pair<Motion, double> pottmann(const std::vector<Vec3d> &X, const std::vector<Vec3d> &N, const std::vector<double> &W)
{
    Vec3d  center = Vec3d::Zero();
    double total  = 0.;
    for (size_t i = 0; i < X.size(); ++i) {
        center += W[i] * X[i];
        total += W[i];
    }
    center /= std::max(total, 1e-300);
    double scale = 1e-12;
    for (const Vec3d &x : X)
        scale = std::max(scale, (x - center).cwiseAbs().maxCoeff());
    Eigen::Matrix<double, 6, 6> M = Eigen::Matrix<double, 6, 6>::Zero();
    for (size_t i = 0; i < X.size(); ++i) {
        const Vec3d                 y = (X[i] - center) / scale;
        Eigen::Matrix<double, 6, 1> l;
        l << y.cross(N[i]), N[i];
        M += W[i] * l * l.transpose();
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix<double, 6, 6>> solver(M);
    const Eigen::Matrix<double, 6, 1>                                vec   = solver.eigenvectors().col(0);
    const double                                                     ratio = solver.eigenvalues()(0) / std::max(solver.eigenvalues()(1), 1e-300);
    const Vec3d                                                      c(vec(0), vec(1), vec(2));
    const Vec3d                                                      cb(vec(3), vec(4), vec(5));
    if (c.norm() < 1e-3 * cb.norm())
        return { make_motion(Move::Extrusion, cb), ratio };
    const double cc = c.squaredNorm();
    const Vec3d  x0 = c.cross(cb) / cc;
    return { make_motion(Move::Helix, c, center + scale * x0, c.dot(cb) / cc * scale), ratio };
}

// Points and normals of a set of faces for its motion: its vertices whose faces all belong to the set, with the area
// weighted mean of their normals (closer to the surface than the normal of a flat face of a curve).
void vertex_normals(const MeshView &mesh, const std::vector<int> &faces, std::vector<Vec3d> &X, std::vector<Vec3d> &N, std::vector<double> &W)
{
    const size_t        nv = mesh.its.vertices.size();
    std::vector<Vec3d>  acc(nv, Vec3d::Zero());
    std::vector<double> wsum(nv, 0.);
    std::vector<char>   inside(size_t(mesh.faces()), 0), outside(nv, 0);
    for (int f : faces) {
        inside[f] = 1;
        for (int j = 0; j < 3; ++j) {
            acc[mesh.its.indices[f][j]] += mesh.area[f] * mesh.normal[f];
            wsum[mesh.its.indices[f][j]] += mesh.area[f];
        }
    }
    for (int f = 0; f < mesh.faces(); ++f)
        if (!inside[f])
            for (int j = 0; j < 3; ++j)
                outside[mesh.its.indices[f][j]] = 1;
    size_t interior = 0;
    for (size_t v = 0; v < nv; ++v)
        if (wsum[v] > 0. && !outside[v])
            ++interior;
    X.clear();
    N.clear();
    W.clear();
    for (size_t v = 0; v < nv; ++v)
        if (wsum[v] > 0. && (!outside[v] || interior < 12) && acc[v].norm() > 0.) {
            X.push_back(mesh.point(int(v)));
            N.push_back(acc[v].normalized());
            W.push_back(wsum[v]);
        }
}

// The axis of a simple surface (point, direction) close to the axis of the motion, which is then kept.
bool snap(Motion &motion, const std::vector<std::pair<Vec3d, Vec3d>> &axes, double size)
{
    double best = std::numeric_limits<double>::max();
    Vec3d  point, direction;
    for (const auto &[c, a0] : axes) {
        const Vec3d a = a0.normalized();
        if (std::abs(a.dot(motion.d)) < std::cos(2. * PI / 180.))
            continue;
        const Vec3d  rel  = motion.c - c;
        const double dist = (rel - rel.dot(a) * a).norm();
        if (dist < 0.02 * size && dist < best) {
            best      = dist;
            point     = c;
            direction = a.dot(motion.d) > 0. ? a : Vec3d(-a);
        }
    }
    if (best == std::numeric_limits<double>::max())
        return false;
    motion = make_motion(motion.kind, direction, point, motion.p);
    return true;
}

// Displacement per radian of a helix of fixed axis which slides the points (normals N) along themselves best.
void refit_pitch(Motion &motion, const std::vector<Vec3d> &X, const std::vector<Vec3d> &N, const std::vector<double> &W)
{
    double num = 0., den = 0.;
    for (size_t i = 0; i < X.size(); ++i) {
        const double rot = motion.d.cross(X[i] - motion.c).dot(N[i]);
        const double ax  = N[i].dot(motion.d);
        num += W[i] * rot * ax;
        den += W[i] * ax * ax;
    }
    if (den > 1e-300)
        motion = make_motion(Move::Helix, motion.d, motion.c, -num / den);
}

// Directions of extrusion voted by the edges between two of the faces (n_f x n_g, weight length x angle), the strongest
// few after a mean shift on the sphere, with their share of the votes.
std::vector<std::pair<Vec3d, double>> vote_directions(const MeshView &mesh, const std::vector<int> &faces, int count = 6)
{
    std::vector<char> in(size_t(mesh.faces()), 0);
    for (int f : faces)
        in[f] = 1;
    std::vector<Vec3d>  dirs;
    std::vector<double> weights;
    const double        sin_min = std::sin(0.2 * PI / 180.);
    for (int f : faces)
        for (int k = 0; k < 3; ++k) {
            const int g = mesh.twin[f][k];
            if (g < 0 || g < f || !in[g])
                continue;
            Vec3d        c = mesh.normal[f].cross(mesh.normal[g]);
            const double s = c.norm();
            if (s < sin_min)
                continue;
            c /= s;
            int big = 0;
            for (int j = 1; j < 3; ++j)
                if (std::abs(c(j)) > std::abs(c(big)))
                    big = j;
            if (c(big) < 0.)
                c = -c;
            const double L = (mesh.point(mesh.its.indices[f][(k + 1) % 3]) - mesh.point(mesh.its.indices[f][k])).norm();
            dirs.push_back(c);
            weights.push_back(L * std::asin(std::min(s, 1.)));
        }
    std::vector<std::pair<Vec3d, double>> found;
    if (dirs.empty())
        return found;
    double total = 0.;
    for (double w : weights)
        total += w;
    std::vector<char> remaining(dirs.size(), 1);
    const double      cos2 = std::cos(2. * PI / 180.), cos1 = std::cos(1. * PI / 180.);
    for (int round = 0; round < count; ++round) {
        int heaviest = -1;
        for (size_t i = 0; i < dirs.size(); ++i)
            if (remaining[i] && (heaviest < 0 || weights[i] > weights[heaviest]))
                heaviest = int(i);
        if (heaviest < 0)
            break;
        Vec3d d = dirs[heaviest];
        for (int iteration = 0; iteration < 10; ++iteration) {
            Vec3d m = Vec3d::Zero();
            for (size_t i = 0; i < dirs.size(); ++i)
                if (remaining[i] && std::abs(dirs[i].dot(d)) > cos2)
                    m += (dirs[i].dot(d) >= 0. ? 1. : -1.) * weights[i] * dirs[i];
            if (m.norm() < 1e-300)
                break;
            d = m.normalized();
        }
        double share = 0.;
        for (size_t i = 0; i < dirs.size(); ++i)
            if (remaining[i] && std::abs(dirs[i].dot(d)) > cos1) {
                share += weights[i];
                remaining[i] = 0;
            }
        found.emplace_back(d, share / total);
    }
    return found;
}

// Angle of each point around the axis, made continuous along the edges (a helix turns several times).
std::vector<double> unwrap(const Motion &motion, const std::vector<Vec3d> &X, const std::vector<std::pair<int, int>> &edges)
{
    std::vector<double>           theta(X.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<std::vector<int>> adj(X.size());
    for (const auto &[a, b] : edges) {
        adj[a].push_back(b);
        adj[b].push_back(a);
    }
    for (size_t s = 0; s < X.size(); ++s) {
        if (!std::isnan(theta[s]))
            continue;
        theta[s] = motion.angle(X[s]);
        std::deque<int> queue{ int(s) };
        while (!queue.empty()) {
            const int u = queue.front();
            queue.pop_front();
            for (int w : adj[u])
                if (std::isnan(theta[w])) {
                    const double raw = motion.angle(X[w]);
                    theta[w]         = raw + 2. * PI * std::round((theta[u] - raw) / (2. * PI));
                    queue.push_back(w);
                }
        }
    }
    return theta;
}

// Distances from a source through the edges, weighted by the lengths of the profile coordinates (Dijkstra).
std::vector<double> profile_distances(const std::vector<Vec2d> &Q, const std::vector<std::vector<std::pair<int, double>>> &adj, int source)
{
    std::vector<double> dist(Q.size(), std::numeric_limits<double>::infinity());
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    dist[source] = 0.;
    heap.emplace(0., source);
    while (!heap.empty()) {
        const auto [d, u] = heap.top();
        heap.pop();
        if (d > dist[u])
            continue;
        for (const auto &[w, length] : adj[u])
            if (d + length < dist[w]) {
                dist[w] = d + length;
                heap.emplace(dist[w], w);
            }
    }
    return dist;
}

// Angles (unwrapped; empty for an extrusion), profile points and their places along the profile in [0, 1]: the
// distance through the edges (measured in the profile plane) from one end. False when the faces are not connected.
bool profile_order(const Motion &motion, const std::vector<Vec3d> &X, const std::vector<std::pair<int, int>> &edges, std::vector<double> &theta,
                   std::vector<Vec2d> &Q, std::vector<double> &t)
{
    theta.clear();
    if (motion.kind != Move::Extrusion)
        theta = unwrap(motion, X, edges);
    Q.resize(X.size());
    for (size_t i = 0; i < X.size(); ++i)
        Q[i] = motion.profile(X[i], theta.empty() ? 0. : theta[i]);
    std::vector<std::vector<std::pair<int, double>>> adj(X.size());
    for (const auto &[a, b] : edges) {
        const double w = (Q[a] - Q[b]).norm() + 1e-12;
        adj[a].emplace_back(b, w);
        adj[b].emplace_back(a, w);
    }
    const std::vector<double> d0 = profile_distances(Q, adj, 0);
    int                       end = 0;
    for (size_t i = 0; i < d0.size(); ++i) {
        if (!std::isfinite(d0[i]))
            return false;
        if (d0[i] > d0[end])
            end = int(i);
    }
    t                 = profile_distances(Q, adj, end);
    const double span = *std::max_element(t.begin(), t.end());
    if (!(span > 1e-9))
        return false;
    for (double &x : t)
        x /= span;
    return true;
}

// The faces (the largest connected part of them) whose vertices lie within `loose` of one profile curve under the
// motion, and that profile (exact when within tolerance). A robust fit (weights falling with the distance) first
// finds the curve most of the points follow; the faces with a vertex off it go back (another surface).
std::optional<std::pair<std::vector<int>, Profile>> fit_profile(const MeshView &mesh, const Motion &motion, std::vector<int> faces, double tolerance,
                                                                double loose, size_t min_faces)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<int>                 verts;
        std::vector<std::pair<int, int>> edges;
        vertex_graph(mesh, faces, verts, edges);
        std::vector<Vec3d> X(verts.size());
        for (size_t i = 0; i < verts.size(); ++i)
            X[i] = mesh.point(verts[i]);
        std::vector<double> theta, t;
        std::vector<Vec2d>  Q;
        if (!profile_order(motion, X, edges, theta, Q, t))
            return {};
        // Points which share a place in the profile plane (the two ends of a straight line of an extrusion) once, in
        // their order along the profile.
        const double                 grid = std::max(tolerance * 0.01, 1e-7);
        std::map<std::pair<long long, long long>, int> first;
        for (size_t i = 0; i < Q.size(); ++i)
            first.emplace(std::make_pair((long long) std::llround(Q[i].x() / grid), (long long) std::llround(Q[i].y() / grid)), int(i));
        std::vector<int> order;
        for (const auto &kv : first)
            order.push_back(kv.second);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return t[a] < t[b]; });
        if (order.size() < 6)
            return {};
        std::vector<Vec2d>  Qo(order.size());
        std::vector<double> to(order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            Qo[i] = Q[order[i]];
            to[i] = t[order[i]];
        }
        // Few spans for few points: a curve weaving through scattered points is no profile.
        const int           max_spans = std::max(1, std::min(32, int(order.size()) / 4));
        int                 spans     = 1;
        Curve2              curve;
        std::vector<double> dist;
        for (;;) {
            std::vector<double> w(Qo.size(), 1.), tt = to;
            for (int round = 0; round < 4; ++round) {
                curve = fit_curve_weighted(Qo, tt, spans, w, dist);
                for (size_t i = 0; i < w.size(); ++i)
                    w[i] = 1. / (1. + (dist[i] / loose) * (dist[i] / loose));
            }
            size_t inside = 0;
            for (double d : dist)
                inside += d <= loose;
            if (double(inside) >= 0.98 * double(dist.size()) || spans >= max_spans)
                break;
            spans = std::min(2 * spans, max_spans);
        }
        std::vector<double> ts, all_dist, all_param;
        std::vector<Vec2d>  poly;
        curve.polyline(std::max(400, 40 * spans), ts, poly);
        project_on_polyline(Q, ts, poly, all_dist, all_param);
        std::vector<char> off(mesh.its.vertices.size(), 0);
        bool              any_off = false;
        for (size_t i = 0; i < Q.size(); ++i)
            if (all_dist[i] > loose) {
                off[verts[i]] = 1;
                any_off       = true;
            }
        if (!any_off) {
            Profile profile;
            profile.motion = motion;
            if (auto exact = fit_curve(Qo, to, tolerance, max_spans)) {
                profile.curve = std::move(exact->first);
                profile.error = exact->second;
                profile.exact = true;
            } else if (auto rough = fit_curve(Qo, to, loose, max_spans)) {
                profile.curve = std::move(rough->first);
                profile.error = rough->second;
            } else
                return {};
            return std::make_pair(std::move(faces), std::move(profile));
        }
        // The faces with a vertex off the curve go back; the largest connected part of the rest is tried again.
        std::vector<int> kept;
        for (int f : faces)
            if (!off[mesh.its.indices[f][0]] && !off[mesh.its.indices[f][1]] && !off[mesh.its.indices[f][2]])
                kept.push_back(f);
        if (kept.size() < min_faces)
            return {};
        std::vector<std::vector<int>> parts = components(mesh, kept);
        faces = *std::max_element(parts.begin(), parts.end(), [](const std::vector<int> &a, const std::vector<int> &b) { return a.size() < b.size(); });
        if (faces.size() < min_faces)
            return {};
    }
    return {};
}

// Minimum of f near x0 by the simplex of Nelder and Mead.
std::vector<double> nelder_mead(const std::function<double(const std::vector<double> &)> &f, const std::vector<double> &x0, const std::vector<double> &step,
                                int iterations)
{
    const size_t                     n = x0.size();
    std::vector<std::vector<double>> pts{ x0 };
    for (size_t i = 0; i < n; ++i) {
        std::vector<double> x = x0;
        x[i] += step[i];
        pts.push_back(x);
    }
    std::vector<double> vals;
    for (const std::vector<double> &x : pts)
        vals.push_back(f(x));
    auto combine = [&](const std::vector<double> &a, const std::vector<double> &b, double k) {
        std::vector<double> out(n);
        for (size_t i = 0; i < n; ++i)
            out[i] = a[i] + k * (a[i] - b[i]);
        return out;
    };
    for (int it = 0; it < iterations; ++it) {
        std::vector<size_t> order(n + 1);
        for (size_t i = 0; i <= n; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return vals[a] < vals[b]; });
        std::vector<std::vector<double>> p2;
        std::vector<double>              v2;
        for (size_t i : order) {
            p2.push_back(pts[i]);
            v2.push_back(vals[i]);
        }
        pts  = std::move(p2);
        vals = std::move(v2);
        if (std::abs(vals[n] - vals[0]) <= 1e-12 * std::max(std::abs(vals[0]), 1e-300))
            break;
        std::vector<double> centroid(n, 0.);
        for (size_t i = 0; i < n; ++i)
            for (size_t k = 0; k < n; ++k)
                centroid[k] += pts[i][k] / double(n);
        const std::vector<double> xr = combine(centroid, pts[n], 1.);
        const double              fr = f(xr);
        if (fr < vals[0]) {
            const std::vector<double> xe = combine(centroid, pts[n], 2.);
            const double              fe = f(xe);
            if (fe < fr) {
                pts[n]  = xe;
                vals[n] = fe;
            } else {
                pts[n]  = xr;
                vals[n] = fr;
            }
        } else if (fr < vals[n - 1]) {
            pts[n]  = xr;
            vals[n] = fr;
        } else {
            const std::vector<double> xc = combine(centroid, pts[n], -0.5);
            const double              fc = f(xc);
            if (fc < vals[n]) {
                pts[n]  = xc;
                vals[n] = fc;
            } else
                for (size_t i = 1; i <= n; ++i) {
                    for (size_t k = 0; k < n; ++k)
                        pts[i][k] = pts[0][k] + 0.5 * (pts[i][k] - pts[0][k]);
                    vals[i] = f(pts[i]);
                }
        }
    }
    return pts[std::min_element(vals.begin(), vals.end()) - vals.begin()];
}

// The motion adjusted so that the profile points of the faces lie closest to one curve: the thickness of the cloud
// across the profile, in slices along it (their order kept from the first motion). fixed_axis: only the
// displacement of a helix (golden section), or nothing.
Motion refine_motion(const MeshView &mesh, const Motion &motion, const std::vector<int> &faces, bool fixed_axis)
{
    if (fixed_axis && motion.kind != Move::Helix)
        return motion;
    std::vector<int>                 verts;
    std::vector<std::pair<int, int>> edges;
    vertex_graph(mesh, faces, verts, edges);
    std::vector<Vec3d> X(verts.size());
    for (size_t i = 0; i < verts.size(); ++i)
        X[i] = mesh.point(verts[i]);
    std::vector<double> theta, t;
    std::vector<Vec2d>  Q;
    if (!profile_order(motion, X, edges, theta, Q, t))
        return motion;
    const int        bins = std::max(10, std::min(400, int(X.size()) / 12));
    std::vector<int> slot(X.size());
    for (size_t i = 0; i < X.size(); ++i)
        slot[i] = std::min(int(t[i] * bins), bins - 1);
    Vec3d mean = Vec3d::Zero();
    for (const Vec3d &x : X)
        mean += x;
    mean /= double(X.size());
    double size = 1e-9;
    for (const Vec3d &x : X)
        size = std::max(size, (x - mean).cwiseAbs().maxCoeff());

    auto make = [&](const std::vector<double> &x) {
        if (motion.kind == Move::Extrusion)
            return make_motion(Move::Extrusion, motion.d + x[0] * motion.e1 + x[1] * motion.e2);
        const Vec3d a = motion.d + x[0] * motion.e1 + x[1] * motion.e2;
        const Vec3d c = motion.c + x[2] * motion.e1 + x[3] * motion.e2;
        return make_motion(motion.kind, a, c, motion.kind == Move::Helix ? motion.p + x[4] : 0.);
    };
    auto thickness = [&](const std::vector<double> &x) {
        const Motion        m = make(x);
        std::vector<double> cnt(size_t(bins), 0.), sx(size_t(bins), 0.), sy(size_t(bins), 0.), sxx(size_t(bins), 0.), syy(size_t(bins), 0.),
            sxy(size_t(bins), 0.);
        for (size_t i = 0; i < X.size(); ++i) {
            double th = 0.;
            if (!theta.empty()) {
                // The same turn as before for each point: the branch of its new angle closest to the old one.
                const double raw = m.angle(X[i]);
                th               = raw + 2. * PI * std::round((theta[i] - raw) / (2. * PI));
            }
            const Vec2d q = m.profile(X[i], th);
            const int   s = slot[i];
            cnt[s] += 1.;
            sx[s] += q.x();
            sy[s] += q.y();
            sxx[s] += q.x() * q.x();
            syy[s] += q.y() * q.y();
            sxy[s] += q.x() * q.y();
        }
        double total = 0.;
        for (int s = 0; s < bins; ++s) {
            if (cnt[s] < 1.)
                continue;
            const double k = cnt[s];
            const double cxx = sxx[s] / k - (sx[s] / k) * (sx[s] / k), cyy = syy[s] / k - (sy[s] / k) * (sy[s] / k);
            const double cxy = sxy[s] / k - (sx[s] / k) * (sy[s] / k);
            const double tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
            const double across = 0.5 * (tr - std::sqrt(std::max(tr * tr - 4. * det, 0.)));
            total += std::max(across, 0.) * k;
        }
        return total;
    };
    if (fixed_axis) {
        double       lo = -0.02 * std::abs(motion.p), hi = 0.02 * std::abs(motion.p);
        const double g  = (std::sqrt(5.) - 1.) / 2.;
        double       x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
        auto         at = [&](double dp) { return thickness({ 0., 0., 0., 0., dp }); };
        double       f1 = at(x1), f2 = at(x2);
        for (int it = 0; it < 60; ++it) {
            if (f1 < f2) {
                hi = x2;
                x2 = x1;
                f2 = f1;
                x1 = hi - g * (hi - lo);
                f1 = at(x1);
            } else {
                lo = x1;
                x1 = x2;
                f1 = f2;
                x2 = lo + g * (hi - lo);
                f2 = at(x2);
            }
        }
        return make({ 0., 0., 0., 0., 0.5 * (lo + hi) });
    }
    const size_t        n = motion.kind == Move::Extrusion ? 2 : motion.kind == Move::Revolution ? 4 : 5;
    std::vector<double> step{ 1e-3, 1e-3, 1e-3 * size, 1e-3 * size, 1e-3 * std::max(std::abs(motion.p), 1e-3) };
    step.resize(n);
    return make(nelder_mead(thickness, std::vector<double>(n, 0.), step, 600));
}

// Length of the region along the motion over the length of its profile: a real extrusion, revolution or helix is long
// along its motion; a small patch of any surface fits some motion as well.
double stretch(const MeshView &mesh, const std::vector<int> &faces, const Profile &profile)
{
    std::vector<int>                 verts;
    std::vector<std::pair<int, int>> edges;
    vertex_graph(mesh, faces, verts, edges);
    std::vector<Vec3d> X(verts.size());
    for (size_t i = 0; i < verts.size(); ++i)
        X[i] = mesh.point(verts[i]);
    const Motion       &m = profile.motion;
    std::vector<double> theta;
    if (m.kind != Move::Extrusion)
        theta = unwrap(m, X, edges);
    std::vector<Vec2d> Q(X.size());
    double             lo = std::numeric_limits<double>::max(), hi = -lo, rho = 0.;
    for (size_t i = 0; i < X.size(); ++i) {
        Q[i]             = m.profile(X[i], theta.empty() ? 0. : theta[i]);
        const double pos = m.kind == Move::Extrusion ? X[i].dot(m.d) : theta[i];
        lo               = std::min(lo, pos);
        hi               = std::max(hi, pos);
        rho += Q[i].x();
    }
    double along = hi - lo;
    if (m.kind != Move::Extrusion)
        along *= std::hypot(rho / double(X.size()), m.p);
    std::vector<double> ts, dist, param;
    std::vector<Vec2d>  poly;
    profile.curve.polyline(400, ts, poly);
    project_on_polyline(Q, ts, poly, dist, param);
    const double t0     = *std::min_element(param.begin(), param.end());
    const double t1     = *std::max_element(param.begin(), param.end());
    double       length = 0.;
    Vec2d        prev   = profile.curve.at(t0);
    for (int i = 1; i <= 100; ++i) {
        const Vec2d q = profile.curve.at(t0 + (t1 - t0) * double(i) / 100.);
        length += (q - prev).norm();
        prev = q;
    }
    return along / std::max(length, 1e-12);
}

bool same_motion(const Motion &m1, const Motion &m2, double size)
{
    if (m1.kind != m2.kind)
        return false;
    const double cos_half = std::cos(0.5 * PI / 180.);
    if (std::abs(m1.d.dot(m2.d)) < cos_half)
        return false;
    if (m1.kind == Move::Extrusion)
        return true;
    const Vec3d rel = m2.c - m1.c;
    if ((rel - rel.dot(m1.d) * m1.d).norm() > 1e-3 * size)
        return false;
    if (m1.kind == Move::Helix) {
        const double p2 = m1.d.dot(m2.d) > 0. ? m2.p : -m2.p;
        return std::abs(m1.p - p2) <= 2e-3 * std::abs(m1.p);
    }
    return true;
}

using Kinematic = std::pair<std::vector<int>, Profile>;

// Neighbouring regions of the same motion merged (pieces of one flank of a thread cut apart), when one profile explains
// them together.
void merge_same(const MeshView &mesh, std::vector<Kinematic> &found, double tolerance, double loose)
{
    for (bool merged = true; merged;) {
        merged = false;
        std::vector<int> owner(size_t(mesh.faces()), -1);
        for (size_t i = 0; i < found.size(); ++i)
            for (int f : found[i].first)
                owner[f] = int(i);
        for (size_t i = 0; i < found.size() && !merged; ++i) {
            std::vector<int> touching;
            for (int f : found[i].first)
                for (int k = 0; k < 3; ++k) {
                    const int g = mesh.twin[f][k];
                    if (g >= 0 && owner[g] >= 0 && owner[g] != int(i))
                        touching.push_back(owner[g]);
                }
            sort_remove_duplicates(touching);
            for (int j : touching) {
                if (!same_motion(found[i].second.motion, found[j].second.motion, mesh.size))
                    continue;
                std::vector<int> both = found[i].first;
                both.insert(both.end(), found[j].first.begin(), found[j].first.end());
                auto res = fit_profile(mesh, found[i].second.motion, both, tolerance, loose, 8);
                if (!res || res->first.size() != both.size())
                    continue;
                found[i] = std::move(*res);
                found.erase(found.begin() + j);
                merged = true;
                break;
            }
        }
    }
}

// Regions of the free faces (free: a mask changed in place) explained by an extrusion, a revolution or a helix. axes:
// (point, direction) of the simple surfaces, to which an axis close to them is snapped.
std::vector<Kinematic> extract_kinematic(const MeshView &mesh, std::vector<char> &free, double tolerance, const std::vector<std::pair<Vec3d, Vec3d>> &axes,
                                         const std::function<bool()> &canceled)
{
    const double           loose = 5. * tolerance, sin_max = std::sin(3. * PI / 180.);
    const size_t           min_faces = 8;
    std::vector<Kinematic> found;
    auto                   free_faces = [&]() {
        std::vector<int> out;
        for (int f = 0; f < mesh.faces(); ++f)
            if (free[f])
                out.push_back(f);
        return out;
    };
    auto take = [&](const Motion &motion, bool fixed_axis) {
        std::vector<int> ok;
        for (int f : free_faces())
            if (motion.off_angle(mesh.centroid(f), mesh.normal[f]) < sin_max)
                ok.push_back(f);
        std::vector<std::vector<int>> queue;
        for (std::vector<int> &part : components(mesh, ok))
            if (part.size() >= min_faces)
                queue.push_back(std::move(part));
        while (!queue.empty()) {
            check(canceled);
            std::vector<int> piece = std::move(queue.back());
            queue.pop_back();
            const Motion refined = refine_motion(mesh, motion, piece, fixed_axis);
            auto         res     = fit_profile(mesh, refined, piece, tolerance, loose, min_faces);
            // An extrusion needs only be as long as its profile (the rounded corner of a short wall); a revolution or a
            // helix, found by the normals alone, must be longer to tell it from a patch of any surface.
            if (!res || turning(mesh, res->first) < 2. || stretch(mesh, res->first, res->second) < (motion.kind == Move::Extrusion ? 1. : 3.))
                continue;
            for (int f : res->first)
                free[f] = 0;
            found.push_back(std::move(*res));
            // What the profile left of the piece may be another region under the same motion.
            std::vector<int> rest;
            for (int f : piece)
                if (free[f])
                    rest.push_back(f);
            for (std::vector<int> &part : components(mesh, rest))
                if (part.size() >= min_faces)
                    queue.push_back(std::move(part));
        }
    };
    // Extrusions first, the directions with the largest share of the votes first (a narrow strip at the top of a round
    // fits any direction square to its normals, and goes to the first motion tried), then a revolution or a helix per
    // piece left.
    std::vector<std::pair<Vec3d, double>> votes = vote_directions(mesh, free_faces());
    std::stable_sort(votes.begin(), votes.end(), [](const std::pair<Vec3d, double> &a, const std::pair<Vec3d, double> &b) { return a.second > b.second; });
    for (const auto &[d, share] : votes)
        if (share >= 0.02)
            take(make_motion(Move::Extrusion, d), true);
    for (const std::vector<int> &piece : components(mesh, free_faces())) {
        if (piece.size() < min_faces || !std::all_of(piece.begin(), piece.end(), [&](int f) { return free[f] != 0; }))
            continue;
        check(canceled);
        std::vector<Vec3d>  X, N;
        std::vector<double> W;
        vertex_normals(mesh, piece, X, N, W);
        auto [motion, ratio] = pottmann(X, N, W);
        if (ratio > 0.02 || motion.kind == Move::Extrusion)
            continue;
        const bool snapped = snap(motion, axes, mesh.size);
        if (std::abs(motion.p) * 2. * PI < 10. * tolerance)
            motion = make_motion(Move::Revolution, motion.d, motion.c);
        else
            refit_pitch(motion, X, N, W);
        take(motion, snapped);
    }
    merge_same(mesh, found, tolerance, loose);
    return found;
}

// ---- Level 4: free surfaces, over the plane of a patch or the parameters of a motion ----

std::vector<int> vertices_of(const MeshView &mesh, const std::vector<int> &faces)
{
    std::vector<int> verts;
    verts.reserve(faces.size() * 3);
    for (int f : faces)
        for (int j = 0; j < 3; ++j)
            verts.push_back(mesh.its.indices[f][j]);
    sort_remove_duplicates(verts);
    return verts;
}

// Euler characteristic and number of boundary loops of a set of faces (a disk: 1 and 1).
std::pair<int, int> topology(const MeshView &mesh, const std::vector<int> &faces)
{
    std::vector<char> in(size_t(mesh.faces()), 0);
    for (int f : faces)
        in[f] = 1;
    std::vector<std::pair<int, int>> border;
    for (int f : faces)
        for (int k = 0; k < 3; ++k) {
            const int g = mesh.twin[f][k];
            if (g < 0 || !in[g])
                border.emplace_back(mesh.its.indices[f][k], mesh.its.indices[f][(k + 1) % 3]);
        }
    const std::vector<int> verts = vertices_of(mesh, faces);
    // Each inner edge is a side of two of the faces, each border edge of one.
    const int edges = (3 * int(faces.size()) + int(border.size())) / 2;
    const int chi   = int(verts.size()) - edges + int(faces.size());
    std::vector<int> parent(verts.size());
    for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = int(i);
    auto local = [&](int v) { return int(std::lower_bound(verts.begin(), verts.end(), v) - verts.begin()); };
    auto find  = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x         = parent[x];
        }
        return x;
    };
    for (const auto &[a, b] : border) {
        const int ra = find(local(a)), rb = find(local(b));
        if (ra != rb)
            parent[ra] = rb;
    }
    std::vector<int> roots;
    for (const auto &[a, b] : border)
        roots.push_back(find(local(a)));
    sort_remove_duplicates(roots);
    return { chi, int(roots.size()) };
}

// Whether two of the segments [A_i, B_i] cross (shared ends do not count), through the cells of a grid.
bool segments_cross(const std::vector<Vec2d> &A, const std::vector<Vec2d> &B)
{
    const size_t n = A.size();
    if (n < 2)
        return false;
    Vec2d lo = A[0], hi = A[0];
    for (size_t i = 0; i < n; ++i) {
        lo = lo.cwiseMin(A[i]).cwiseMin(B[i]);
        hi = hi.cwiseMax(A[i]).cwiseMax(B[i]);
    }
    const double h = std::max((hi - lo).maxCoeff(), 1e-12) / double(std::max(1, int(std::sqrt(double(n)))));
    std::unordered_map<uint64_t, std::vector<int>> grid;
    for (size_t i = 0; i < n; ++i) {
        const Vec2d a = A[i].cwiseMin(B[i]) - lo, b = A[i].cwiseMax(B[i]) - lo;
        for (int x = int(a.x() / h); x <= int(b.x() / h); ++x)
            for (int y = int(a.y() / h); y <= int(b.y() / h); ++y)
                grid[edge_key(x, y)].push_back(int(i));
    }
    auto cross = [&](int i, int j) {
        const Vec2d  p = A[i], r = B[i] - A[i], q = A[j], s = B[j] - A[j];
        const double den = r.x() * s.y() - r.y() * s.x();
        if (std::abs(den) < 1e-300)
            return false;
        const double t = ((q.x() - p.x()) * s.y() - (q.y() - p.y()) * s.x()) / den;
        const double u = ((q.x() - p.x()) * r.y() - (q.y() - p.y()) * r.x()) / den;
        return 1e-9 < t && t < 1. - 1e-9 && 1e-9 < u && u < 1. - 1e-9;
    };
    for (const auto &[key, ids] : grid)
        for (size_t a = 0; a < ids.size(); ++a)
            for (size_t b = a + 1; b < ids.size(); ++b)
                if (cross(ids[a], ids[b]))
                    return true;
    return false;
}

// Plane of projection of a free patch: origin o, normal n, axes e1, e2 along its length and its width (the box of the
// parameters then hugs it: a long diagonal patch in a square box leaves empty corners where the surface may wander).
struct Frame
{
    Vec3d o, n, e1, e2;
    Vec2d uv(const Vec3d &x) const { return Vec2d((x - o).dot(e1), (x - o).dot(e2)); }
};

// The frame of the mean normal of the faces when every face faces it (within 80 degrees) and the faces make a disk
// whose border, seen on the plane, does not cross itself: the plane then sees the patch once.
std::optional<Frame> plane_frame(const MeshView &mesh, const std::vector<int> &faces)
{
    Vec3d m = Vec3d::Zero();
    for (int f : faces)
        m += mesh.area[f] * mesh.normal[f];
    if (m.norm() < 1e-9)
        return {};
    m.normalize();
    const double cos_max = std::cos(80. * PI / 180.);
    for (int f : faces)
        if (mesh.normal[f].dot(m) < cos_max)
            return {};
    const auto [chi, loops] = topology(mesh, faces);
    if (chi != 1 || loops != 1)
        return {};
    const std::vector<int> verts = vertices_of(mesh, faces);
    Frame                  frame;
    frame.o = Vec3d::Zero();
    for (int v : verts)
        frame.o += mesh.point(v);
    frame.o /= double(verts.size());
    frame.n = m;
    basis(m, frame.e1, frame.e2);
    Vec2d              mean = Vec2d::Zero();
    std::vector<Vec2d> uv;
    for (int v : verts) {
        uv.push_back(frame.uv(mesh.point(v)));
        mean += uv.back();
    }
    mean /= double(uv.size());
    Eigen::Matrix2d cov = Eigen::Matrix2d::Zero();
    for (const Vec2d &q : uv)
        cov += (q - mean) * (q - mean).transpose();
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> solver(cov);
    const Vec2d                                          d = solver.eigenvectors().col(1);
    frame.e1                                               = (d.x() * frame.e1 + d.y() * frame.e2).normalized();
    frame.e2                                               = frame.n.cross(frame.e1);
    std::vector<char> in(size_t(mesh.faces()), 0);
    for (int f : faces)
        in[f] = 1;
    std::vector<Vec2d> A, B;
    for (int f : faces)
        for (int k = 0; k < 3; ++k) {
            const int g = mesh.twin[f][k];
            if (g < 0 || !in[g]) {
                A.push_back(frame.uv(mesh.point(mesh.its.indices[f][k])));
                B.push_back(frame.uv(mesh.point(mesh.its.indices[f][(k + 1) % 3])));
            }
        }
    if (segments_cross(A, B))
        return {};
    return frame;
}

// Two halves of a piece, grown from its two faces farthest apart (distances between centers through the edges): each
// half is connected and the cut between them follows the distances, where a plane through the piece may cut a strip
// lengthwise into slivers. Each half in its connected parts.
std::vector<std::vector<int>> split(const MeshView &mesh, const std::vector<int> &faces)
{
    std::unordered_map<int, int> index;
    std::vector<Vec3d>           C(faces.size());
    for (size_t i = 0; i < faces.size(); ++i) {
        index.emplace(faces[i], int(i));
        C[i] = mesh.centroid(faces[i]);
    }
    auto distances = [&](int source) {
        std::vector<double> dist(faces.size(), std::numeric_limits<double>::infinity());
        using Item = std::pair<double, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
        dist[source] = 0.;
        heap.emplace(0., source);
        while (!heap.empty()) {
            const auto [d, i] = heap.top();
            heap.pop();
            if (d > dist[i])
                continue;
            for (int k = 0; k < 3; ++k) {
                const auto it = index.find(mesh.twin[faces[i]][k]);
                if (it == index.end())
                    continue;
                const int    j  = it->second;
                const double nd = d + (C[j] - C[i]).norm();
                if (nd < dist[j]) {
                    dist[j] = nd;
                    heap.emplace(nd, j);
                }
            }
        }
        return dist;
    };
    auto farthest = [](const std::vector<double> &dist) {
        int best = 0;
        for (int i = 0; i < int(dist.size()); ++i)
            if (std::isfinite(dist[i]) && dist[i] > dist[best])
                best = i;
        return best;
    };
    std::vector<int> side[2];
    if (!faces.empty()) {
        const int                 a  = farthest(distances(0));
        const std::vector<double> da = distances(a);
        const std::vector<double> db = distances(farthest(da));
        for (size_t i = 0; i < faces.size(); ++i)
            side[da[i] <= db[i] ? 0 : 1].push_back(faces[i]);
    }
    if (side[0].empty() || side[1].empty()) {
        // Nothing on one side (faces without area): the list cut in two.
        side[0].assign(faces.begin(), faces.begin() + faces.size() / 2);
        side[1].assign(faces.begin() + faces.size() / 2, faces.end());
    }
    std::vector<std::vector<int>> out;
    for (const std::vector<int> &half : side)
        for (std::vector<int> &part : components(mesh, half))
            out.push_back(std::move(part));
    return out;
}

// Points on the simple surfaces along the border of a region, with the vertex next to each: its free surface is fitted
// through them too, so that it meets the simple surfaces between the vertices as well (a mesh edge is a chord).
struct Pins
{
    std::vector<Vec3d> points;
    std::vector<int>   vertex;
};

// Closest point of a simple surface to p.
Vec3d project(const Surface &s, const Vec3d &p)
{
    if (s.kind == Kind::Plane || s.kind == Kind::Facet)
        return p - (s.axis.dot(p) - s.offset) * s.axis;
    const Vec3d q = p - s.center;
    if (s.kind == Kind::Sphere)
        return s.center + s.radius * q / std::max(q.norm(), 1e-300);
    const double h      = q.dot(s.axis);
    const Vec3d  radial = q - h * s.axis;
    const double rho    = radial.norm();
    const Vec3d  e      = radial / std::max(rho, 1e-300);
    switch (s.kind) {
    case Kind::Cylinder: return s.center + h * s.axis + s.radius * e;
    case Kind::Cone: {
        // In the half plane (rho, h) of the apex, onto the line rho = h tan(angle).
        const double t = std::max(rho * std::sin(s.angle) + h * std::cos(s.angle), 0.);
        return s.center + t * std::cos(s.angle) * s.axis + t * std::sin(s.angle) * e;
    }
    case Kind::Torus: {
        Vec2d d(rho - s.radius, h);
        d /= std::max(d.norm(), 1e-300);
        return s.center + (s.radius + s.minor * d.x()) * e + s.minor * d.y() * s.axis;
    }
    default: return p;
    }
}

// The pins of the faces of a region (label: region of each face; the first ones those of the simple surfaces).
Pins pins_for(const MeshView &mesh, const std::vector<int> &faces, const std::vector<int> &label, const std::vector<Surface> &simple)
{
    Pins pins;
    for (int f : faces)
        for (int k = 0; k < 3; ++k) {
            const int g = mesh.twin[f][k];
            if (g < 0)
                continue;
            const int s = label[g];
            if (s == label[f] || s < 0 || s >= int(simple.size()))
                continue;
            const int a = mesh.its.indices[f][k], b = mesh.its.indices[f][(k + 1) % 3];
            for (const double w : { 0.25, 0.5, 0.75 }) {
                pins.points.push_back(project(simple[s], (1. - w) * mesh.point(a) + w * mesh.point(b)));
                pins.vertex.push_back(w < 0.5 ? a : b);
            }
        }
    return pins;
}

// Free surface over the plane of a patch, through its vertices and its pins.
std::optional<BSurface> fit_plane_patch(const MeshView &mesh, const std::vector<int> &faces, double tolerance, const Pins &pins)
{
    const std::optional<Frame> frame = plane_frame(mesh, faces);
    if (!frame)
        return {};
    std::vector<double> U, V;
    std::vector<Vec3d>  X;
    double              box[4] = { std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                   -std::numeric_limits<double>::max() };
    for (int v : vertices_of(mesh, faces)) {
        X.push_back(mesh.point(v));
        const Vec2d q = frame->uv(X.back());
        U.push_back(q.x());
        V.push_back(q.y());
        box[0] = std::min(box[0], q.x());
        box[1] = std::max(box[1], q.x());
        box[2] = std::min(box[2], q.y());
        box[3] = std::max(box[3], q.y());
    }
    // Room around the points, for the edges fitted near the border.
    const double pad = 0.1 * std::max(box[1] - box[0], box[3] - box[2]) + 1e-6;
    box[0] -= pad;
    box[1] += pad;
    box[2] -= pad;
    box[3] += pad;
    for (const Vec3d &p : pins.points) {
        const Vec2d q = frame->uv(p);
        U.push_back(q.x());
        V.push_back(q.y());
        X.push_back(p);
    }
    std::optional<std::pair<BSurface, double>> fit = fit_surface(U, V, X, box, tolerance, 2000, 1, 1);
    if (!fit)
        return {};
    // The inside of the faces within the tolerance and their sagitta; else fitted again through it too.
    const Interior inside = interior_points(mesh, faces);
    std::vector<double> IU, IV;
    for (const Vec3d &p : inside.points) {
        const Vec2d q = frame->uv(p);
        IU.push_back(q.x());
        IV.push_back(q.y());
    }
    if (!swings(fit->first, IU, IV, inside, tolerance))
        return std::move(fit->first);
    std::vector<double> limits(X.size(), tolerance);
    for (size_t i = 0; i < inside.points.size(); ++i) {
        U.push_back(IU[i]);
        V.push_back(IV[i]);
        X.push_back(inside.points[i]);
        limits.push_back(tolerance + inside.sagitta[i]);
    }
    fit = fit_surface(U, V, X, box, tolerance, 2000, 1, 1, &limits);
    if (!fit)
        return {};
    return std::move(fit->first);
}

// Free surface over the parameters of a kinematic region: along the motion (position, or angle unwrapped along the
// region) and the place along the profile; the angle of a pin is taken on the turn of its vertex.
std::optional<BSurface> fit_kinematic_patch(const MeshView &mesh, const std::vector<int> &faces, const Profile &profile, double tolerance, const Pins &pins)
{
    std::vector<int>                 verts;
    std::vector<std::pair<int, int>> edges;
    vertex_graph(mesh, faces, verts, edges);
    const Motion       &m = profile.motion;
    std::vector<Vec3d>  X(verts.size());
    for (size_t i = 0; i < verts.size(); ++i)
        X[i] = mesh.point(verts[i]);
    std::vector<double> theta;
    if (m.kind != Move::Extrusion)
        theta = unwrap(m, X, edges);
    std::vector<double> U(X.size());
    std::vector<Vec2d>  Q(X.size());
    for (size_t i = 0; i < X.size(); ++i) {
        U[i] = m.kind == Move::Extrusion ? X[i].dot(m.d) : theta[i];
        Q[i] = m.profile(X[i], theta.empty() ? 0. : theta[i]);
    }
    const size_t vertices = X.size();
    for (size_t i = 0; i < pins.points.size(); ++i) {
        const auto at = std::lower_bound(verts.begin(), verts.end(), pins.vertex[i]);
        if (at == verts.end() || *at != pins.vertex[i])
            continue;
        const Vec3d &p  = pins.points[i];
        double       pu = 0.;
        if (m.kind == Move::Extrusion)
            pu = p.dot(m.d);
        else {
            const double raw = m.angle(p);
            pu               = raw + 2. * PI * std::round((U[at - verts.begin()] - raw) / (2. * PI));
        }
        X.push_back(p);
        U.push_back(pu);
        Q.push_back(m.profile(p, pu));
    }
    std::vector<double> ts, dist, V;
    std::vector<Vec2d>  poly;
    profile.curve.polyline(1000, ts, poly);
    project_on_polyline(Q, ts, poly, dist, V);
    const auto   [u0, u1] = std::minmax_element(U.begin(), U.begin() + vertices);
    const auto   [v0, v1] = std::minmax_element(V.begin(), V.begin() + vertices);
    const double du = *u1 - *u0, dv = *v1 - *v0;
    // Room around the parameters of the vertices: the edges fitted along the border, and their curves on the face which
    // OpenCASCADE computes, must stay on the surface (where a thread starts out of its profile, 0.17 of it below): a
    // quarter of the profile along a helix, a tenth otherwise (a fillet fitted closely curls beyond its end, and a
    // corner of the mesh falls on that part: the wire of the face crosses itself there).
    const double pad    = m.kind == Move::Helix ? 0.25 : 0.1;
    const double box[4] = { *u0 - 0.05 * du - 1e-6, *u1 + 0.05 * du + 1e-6, *v0 - pad * dv - 1e-6, *v1 + pad * dv + 1e-6 };
    // Spans to start with: along a helix or a revolution, 16 per turn (a cubic follows a circle well then).
    const int su = m.kind == Move::Extrusion ? 1 : std::max(1, int(std::ceil(16. * du / (2. * PI))));
    std::optional<std::pair<BSurface, double>> fit = fit_surface(U, V, X, box, tolerance, 3000, su, 1);
    if (!fit)
        return {};
    // The inside of the faces, its parameters interpolated from those of the vertices (a projection on the profile may
    // jump where the region bends away from the motion): within the tolerance and its sagitta; else fitted again
    // through it too.
    const Interior inside = interior_points(mesh, faces);
    std::vector<double> IU, IV;
    for (size_t i = 0; i < inside.points.size(); ++i) {
        double iu = 0., iv = 0.;
        for (int j = 0; j < 3; ++j) {
            const size_t at = size_t(std::lower_bound(verts.begin(), verts.end(), inside.corners[i][j]) - verts.begin());
            iu += inside.weights[i](j) * U[at];
            iv += inside.weights[i](j) * V[at];
        }
        IU.push_back(iu);
        IV.push_back(iv);
    }
    if (!swings(fit->first, IU, IV, inside, tolerance))
        return std::move(fit->first);
    std::vector<double> limits(X.size(), tolerance);
    for (size_t i = 0; i < inside.points.size(); ++i) {
        U.push_back(IU[i]);
        V.push_back(IV[i]);
        X.push_back(inside.points[i]);
        limits.push_back(tolerance + inside.sagitta[i]);
    }
    fit = fit_surface(U, V, X, box, tolerance, 3000, su, 1, &limits);
    if (!fit)
        return {};
    return std::move(fit->first);
}

// Whether every vertex of the faces lies within tolerance of the exact extrusion or revolution of the profile, and the
// inside of the faces within the tolerance and its sagitta (the profile may swing between the places of the vertices).
bool exact_profile(const MeshView &mesh, const std::vector<int> &faces, const Profile &profile, double tolerance)
{
    std::vector<Vec2d> Q;
    for (int v : vertices_of(mesh, faces))
        Q.push_back(profile.motion.profile(mesh.point(v), 0.));
    std::vector<double> ts, dist, param;
    std::vector<Vec2d>  poly;
    profile.curve.polyline(2000, ts, poly);
    project_on_polyline(Q, ts, poly, dist, param);
    if (dist.empty() || *std::max_element(dist.begin(), dist.end()) > tolerance)
        return false;
    const Interior inside = interior_points(mesh, faces);
    Q.clear();
    for (const Vec3d &p : inside.points)
        Q.push_back(profile.motion.profile(p, 0.));
    project_on_polyline(Q, ts, poly, dist, param);
    for (size_t i = 0; i < dist.size(); ++i)
        if (dist[i] > tolerance + inside.sagitta[i])
            return false;
    return true;
}

// Free surfaces covering the faces: each connected piece cut in two until its plane sees it once and a surface fits it
// (a face alone always fits its plane).
std::vector<std::vector<int>> free_patches(const MeshView &mesh, const std::vector<int> &faces, double tolerance, const std::function<bool()> &canceled)
{
    std::vector<std::vector<int>> out, queue = components(mesh, faces);
    while (!queue.empty()) {
        check(canceled);
        std::vector<int> piece = std::move(queue.back());
        queue.pop_back();
        if (piece.size() == 1 || fit_plane_patch(mesh, piece, tolerance, Pins())) {
            out.push_back(std::move(piece));
            continue;
        }
        for (std::vector<int> &part : split(mesh, piece))
            queue.push_back(std::move(part));
    }
    return out;
}

// ---- Level 4: borders between the regions fitted after the simple surfaces ----

// A face of a movable region with two of its three neighbours in another movable region goes to it (an ear sticking
// out, a notch filled), until nothing moves.
void smooth_borders(const MeshView &mesh, std::vector<int> &label, const std::function<bool(int)> &movable)
{
    for (int pass = 0; pass < 8; ++pass) {
        int changed = 0;
        for (int f = 0; f < mesh.faces(); ++f) {
            if (!movable(label[f]))
                continue;
            for (int k = 0; k < 3; ++k) {
                const int g = mesh.twin[f][k];
                if (g < 0 || label[g] == label[f] || !movable(label[g]))
                    continue;
                int count = 0;
                for (int j = 0; j < 3; ++j)
                    count += mesh.twin[f][j] >= 0 && label[mesh.twin[f][j]] == label[g];
                if (count >= 2) {
                    label[f] = label[g];
                    ++changed;
                    break;
                }
            }
        }
        if (changed == 0)
            break;
    }
}

// Pieces (connected parts of a movable region) of fewer than min_faces faces join the movable region they share most
// edges with.
void absorb_small(const MeshView &mesh, std::vector<int> &label, const std::function<bool(int)> &movable, size_t min_faces)
{
    std::map<int, std::vector<int>> members;
    for (int f = 0; f < mesh.faces(); ++f)
        members[label[f]].push_back(f);
    for (auto &[r, faces] : members) {
        if (!movable(r))
            continue;
        // The faces which joined the region since: in its list; those which left it: no longer of it.
        std::vector<int> now;
        for (int f : faces)
            if (label[f] == r)
                now.push_back(f);
        for (const std::vector<int> &piece : components(mesh, now)) {
            if (piece.size() >= min_faces)
                continue;
            std::vector<std::pair<int, int>> count;
            for (int f : piece)
                for (int k = 0; k < 3; ++k) {
                    const int g = mesh.twin[f][k];
                    if (g < 0 || label[g] == r || !movable(label[g]))
                        continue;
                    auto it = std::find_if(count.begin(), count.end(), [&](const std::pair<int, int> &c) { return c.first == label[g]; });
                    if (it == count.end())
                        count.emplace_back(label[g], 1);
                    else
                        ++it->second;
                }
            if (count.empty())
                continue;
            const int to = std::max_element(count.begin(), count.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
                               return a.second < b.second;
                           })->first;
            for (int f : piece)
                label[f] = to;
            auto joined = members.find(to);
            if (joined != members.end())
                joined->second.insert(joined->second.end(), piece.begin(), piece.end());
        }
    }
}

// The simple surfaces of level 4 are fitted within a tenth of the tolerance (no arc on a fillet whose radius varies).
// Within the whole tolerance: a connected piece of free faces (label -1) which one simple surface explains entirely
// becomes that surface; then a surface and a neighbour (a surface of the same kind, or a piece of free faces) which one
// surface of that kind explains together are merged, the largest surfaces first. A sphere subdivided by Loop is 0.04 mm
// from any sphere: in pieces of strict spheres and free faces otherwise.
void widen(const MeshView &mesh, std::vector<int> &label, std::vector<Surface> &surfaces, double tolerance, const std::function<bool()> &canceled)
{
    auto samples = [&](const std::vector<int> &faces) {
        Samples s;
        for (int v : vertices_of(mesh, faces))
            s.P.push_back(mesh.point(v));
        for (int f : faces) {
            s.C.push_back(mesh.centroid(f));
            s.N.push_back(mesh.normal[f]);
            s.W.push_back(mesh.area[f]);
        }
        return s;
    };
    auto mean_normal = [](const Samples &s) {
        Vec3d m = Vec3d::Zero();
        for (size_t i = 0; i < s.N.size(); ++i)
            m += s.W[i] * s.N[i];
        return m;
    };
    // A curved surface must turn: nearly flat pieces fit a large sphere or an open cone as well as a plane.
    auto turns = [&](const Samples &s) {
        Vec3d m = mean_normal(s);
        if (m.norm() < 1e-9)
            return true;
        m.normalize();
        double lowest = 1.;
        for (const Vec3d &n : s.N)
            lowest = std::min(lowest, n.dot(m));
        return lowest <= std::cos(15. * PI / 180.);
    };
    // The inside of the faces within the tolerance and their sagitta: a surface through the corners of a large flat face
    // may bulge between them.
    auto inside = [&](const Surface &shape, const std::vector<int> &faces) {
        for (int f : faces)
            if (distance(shape, mesh.centroid(f)) > tolerance + mesh.sagitta[f])
                return false;
        return true;
    };
    auto fit_kind = [&](const std::vector<int> &faces, Kind kind) {
        const Samples          s = samples(faces);
        std::optional<Surface> shape;
        if (kind == Kind::Plane) {
            PlaneFit fit = fit_plane(s.P);
            if (fit.normal.dot(mean_normal(s)) < 0.) {
                fit.normal = -fit.normal;
                fit.offset = -fit.offset;
            }
            Surface plane;
            plane.axis   = fit.normal;
            plane.offset = fit.offset;
            shape        = plane;
        } else if (kind == Kind::Cylinder)
            shape = fit_cylinder(s, true);
        else if (kind == Kind::Cone)
            shape = fit_cone(s);
        else if (kind == Kind::Sphere)
            shape = fit_sphere(s);
        else if (kind == Kind::Torus)
            shape = fit_torus(s);
        if (!shape)
            return shape;
        if (shape->kind != Kind::Plane && shape->kind != Kind::Cone && (!std::isfinite(shape->radius) || shape->radius > 20. * mesh.size))
            return std::optional<Surface>();
        if (shape->kind == Kind::Cone && (shape->angle < 0.5 * PI / 180. || shape->angle > 89.5 * PI / 180.))
            return std::optional<Surface>();
        for (const Vec3d &p : s.P)
            if (distance(*shape, p) > tolerance)
                return std::optional<Surface>();
        if (!inside(*shape, faces))
            return std::optional<Surface>();
        return shape;
    };
    auto free_faces = [&]() {
        std::vector<int> out;
        for (int f = 0; f < mesh.faces(); ++f)
            if (label[f] < 0)
                out.push_back(f);
        return out;
    };

    for (const std::vector<int> &part : components(mesh, free_faces())) {
        if (part.size() < 20)
            continue;
        check(canceled);
        const Samples s = samples(part);
        if (!turns(s))
            continue;
        if (const std::optional<Surface> shape = best_fit(s, tolerance, mesh.size, true); shape && inside(*shape, part)) {
            for (int f : part)
                label[f] = int(surfaces.size());
            surfaces.push_back(*shape);
        }
    }
    for (bool merged = true; merged;) {
        merged = false;
        check(canceled);
        const std::vector<std::vector<int>> parts = components(mesh, free_faces());
        std::vector<int>                    part_of(size_t(mesh.faces()), -1);
        for (size_t i = 0; i < parts.size(); ++i)
            for (int f : parts[i])
                part_of[f] = int(i);
        std::vector<std::vector<int>> faces_of(surfaces.size());
        std::vector<double>           area(surfaces.size(), 0.);
        for (int f = 0; f < mesh.faces(); ++f)
            if (label[f] >= 0) {
                faces_of[label[f]].push_back(f);
                area[label[f]] += mesh.area[f];
            }
        std::vector<int> order;
        for (int r = 0; r < int(surfaces.size()); ++r)
            if (!faces_of[r].empty())
                order.push_back(r);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return area[a] > area[b]; });
        for (int r : order) {
            // Its neighbours: the pieces of free faces (0, piece), then the surfaces (1, surface).
            std::vector<std::pair<int, int>> around;
            for (int f : faces_of[r])
                for (int k = 0; k < 3; ++k) {
                    const int g = mesh.twin[f][k];
                    if (g >= 0 && label[g] != r)
                        around.emplace_back(label[g] >= 0 ? 1 : 0, label[g] >= 0 ? label[g] : part_of[g]);
                }
            sort_remove_duplicates(around);
            for (const auto &[what, q] : around) {
                if (what == 1 && surfaces[q].kind != surfaces[r].kind)
                    continue;
                const std::vector<int> &other = what == 1 ? faces_of[q] : parts[q];
                std::vector<int>        both  = faces_of[r];
                both.insert(both.end(), other.begin(), other.end());
                const std::optional<Surface> shape = fit_kind(both, surfaces[r].kind);
                if (!shape)
                    continue;
                for (int f : other)
                    label[f] = r;
                surfaces[r] = *shape;
                merged      = true;
                break;
            }
            if (merged)
                break;
        }
    }
}

// Regions thinner than the tolerance (no more area than a strip as wide as the tolerance along them: a sliver, a
// needle) joined to the neighbour they share the longest border with: the repair of OpenCASCADE takes the edges of such
// a face away and leaves a face without border (written as an infinite plane, a second solid). Thinner than the
// tolerance, they lie within it of the surface of that neighbour.
void join_thin(const MeshView &mesh, std::vector<int> &region, double tolerance)
{
    auto thin = [&](const std::vector<int> &faces) {
        BoundingBoxf3 box;
        double        area = 0.;
        for (int f : faces) {
            area += mesh.area[f];
            for (int j = 0; j < 3; ++j)
                box.merge(mesh.point(mesh.its.indices[f][j]));
        }
        return 2. * area <= tolerance * box.size().norm();
    };
    // Again while some move: one joined to another which then joins a third is left alone otherwise.
    for (bool moved = true; moved;) {
        moved = false;
        std::map<int, std::vector<int>> faces_of;
        for (int f = 0; f < mesh.faces(); ++f)
            faces_of[region[f]].push_back(f);
        for (const auto &[r, faces] : faces_of) {
            if (!thin(faces))
                continue;
            std::map<int, double> border;
            for (int f : faces)
                for (int k = 0; k < 3; ++k) {
                    const int g = mesh.twin[f][k];
                    if (g >= 0 && region[g] != r)
                        border[region[g]] += (mesh.point(mesh.its.indices[f][(k + 1) % 3]) - mesh.point(mesh.its.indices[f][k])).norm();
                }
            if (border.empty())
                continue;
            const int to = std::max_element(border.begin(), border.end(), [](const std::pair<const int, double> &a, const std::pair<const int, double> &b) {
                               return a.second < b.second;
                           })->first;
            for (int f : faces)
                region[f] = to;
            moved = true;
        }
    }
}

// The regions left without faces (joined to another) taken out, the others numbered again in their order: the legend and
// the count of surfaces see only those of the solid.
void drop_empty_regions(std::vector<int> &region, std::vector<Surface> &surfaces)
{
    std::vector<int> index(surfaces.size(), -1);
    for (int r : region)
        index[r] = 0;
    std::vector<Surface> kept;
    for (size_t r = 0; r < surfaces.size(); ++r)
        if (index[r] == 0) {
            index[r] = int(kept.size());
            kept.push_back(std::move(surfaces[r]));
        }
    for (int &r : region)
        r = index[r];
    surfaces = std::move(kept);
}

// The faces grown into planar pieces, the largest faces first, as level 2 does: a face joins when its normal is within
// angle degrees of the plane of the piece and its corners within tolerance of it; the plane is fitted again on the whole
// piece when it stops growing. Each piece with its plane (a facet), its normal outward.
std::vector<std::pair<std::vector<int>, Surface>> planar_pieces(const MeshView &mesh, const std::vector<int> &faces, double tolerance, double angle)
{
    std::unordered_map<int, int> piece_of;
    for (int f : faces)
        piece_of[f] = -1;
    std::vector<int> order = faces;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return mesh.area[a] > mesh.area[b]; });
    const double                                      cos_max = std::cos(angle * PI / 180.);
    std::vector<std::pair<std::vector<int>, Surface>> out;
    // A face thinner than the tolerance (a sliver, a needle) has no reliable normal: its corners alone decide. Left alone,
    // the repair takes the edges of such a face away and leaves a face without border, written as an infinite plane.
    auto thin = [&](int g) {
        double longest = 0.;
        for (int j = 0; j < 3; ++j)
            longest = std::max(longest, (mesh.point(mesh.its.indices[g][(j + 1) % 3]) - mesh.point(mesh.its.indices[g][j])).norm());
        return 2. * mesh.area[g] <= tolerance * longest;
    };
    for (int seed : order) {
        if (piece_of[seed] >= 0)
            continue;
        const int        id = int(out.size());
        Vec3d            n  = mesh.normal[seed];
        double           d  = n.dot(mesh.point(mesh.its.indices[seed][0]));
        std::vector<int> members{ seed };
        piece_of[seed] = id;
        for (int round = 0; round < 3; ++round) {
            std::deque<int> queue(members.begin(), members.end());
            bool            grew = false;
            while (!queue.empty()) {
                const int f = queue.front();
                queue.pop_front();
                for (int k = 0; k < 3; ++k) {
                    const int  g  = mesh.twin[f][k];
                    const auto it = piece_of.find(g);
                    if (it == piece_of.end() || it->second >= 0 || (!thin(g) && mesh.normal[g].dot(n) < cos_max))
                        continue;
                    bool close = true;
                    for (int j = 0; j < 3 && close; ++j)
                        close = std::abs(n.dot(mesh.point(mesh.its.indices[g][j])) - d) <= tolerance;
                    if (!close)
                        continue;
                    it->second = id;
                    members.push_back(g);
                    queue.push_back(g);
                    grew = true;
                }
            }
            if (members.size() == 1 || !grew)
                break;
            std::vector<Vec3d> points;
            for (int v : vertices_of(mesh, members))
                points.push_back(mesh.point(v));
            PlaneFit fit = fit_plane(points);
            if (fit.normal.dot(n) < 0.) {
                fit.normal = -fit.normal;
                fit.offset = -fit.offset;
            }
            if (fit.error > tolerance)
                break;
            n = fit.normal;
            d = fit.offset;
        }
        Surface plane;
        plane.kind   = Kind::Facet;
        plane.axis   = n;
        plane.offset = d;
        out.emplace_back(std::move(members), plane);
    }
    return out;
}

// ---- Level 4: the surfaces of the regions as MeshToStep keeps them ----

Kind kind_of(Move move) { return move == Move::Extrusion ? Kind::Extrusion : move == Move::Revolution ? Kind::Revolution : Kind::Helix; }

Surface profile_surface(const Profile &profile)
{
    Surface s;
    s.kind    = kind_of(profile.motion.kind);
    s.axis    = profile.motion.d;
    s.center  = profile.motion.c;
    s.e1      = profile.motion.e1;
    s.e2      = profile.motion.e2;
    s.knots   = profile.curve.kv;
    s.profile = profile.curve.poles;
    return s;
}

Surface bspline_surface(Kind kind, BSurface &&b)
{
    Surface s;
    s.kind   = kind;
    s.uknots = std::move(b.ku);
    s.vknots = std::move(b.kv);
    s.nu     = b.nu;
    s.nv     = b.nv;
    s.poles  = std::move(b.poles);
    return s;
}

} // namespace

MeshToStep::MeshToStep(const indexed_triangle_set &its, const Settings &settings, const std::function<bool()> &canceled)
    : m_its(its), m_settings(settings)
{
    its_merge_vertices(m_its);
    its_remove_degenerate_faces(m_its);
    its_compactify_vertices(m_its);
    const int nf = int(m_its.indices.size());
    // Each edge once in each direction, in two faces: a closed mesh, its faces turned the same way.
    std::unordered_map<uint64_t, int> heads;
    heads.reserve(size_t(nf) * 3);
    size_t shared = 0;
    for (int f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k)
            if (!heads.emplace(edge_key(m_its.indices[f][k], m_its.indices[f][(k + 1) % 3]), f).second)
                ++shared;
    size_t open = 0;
    m_twin.assign(size_t(nf), Vec3i(-1, -1, -1));
    for (int f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            const auto it = heads.find(edge_key(m_its.indices[f][(k + 1) % 3], m_its.indices[f][k]));
            if (it == heads.end())
                ++open;
            else
                m_twin[f][k] = it->second;
        }
    if (open > 0 || shared > 0 || nf == 0)
        throw NotClosed(open, shared);
    m_normal.resize(size_t(nf));
    m_area.resize(size_t(nf));
    for (int f = 0; f < nf; ++f) {
        const Vec3d a = vertex(m_its, m_its.indices[f][0]);
        const Vec3d n = (vertex(m_its, m_its.indices[f][1]) - a).cross(vertex(m_its, m_its.indices[f][2]) - a);
        m_area[f]     = 0.5 * n.norm();
        m_normal[f]   = n.norm() > 0. ? Vec3d(n / n.norm()) : Vec3d::UnitZ();
    }
    chord_sagitta(m_its, m_twin, m_normal, m_area, m_sagitta, m_edge_sagitta);
    check(canceled);
    if (m_settings.level <= 1)
        face_regions(canceled);
    else if (m_settings.level == 2)
        planar_regions(m_settings.tolerance, std::min(5., 0.5 * m_settings.tolerance / 0.01), canceled);
    else if (m_settings.level == 3)
        recognize(false, canceled);
    else {
        recognize(true, canceled);
        recognize_free(canceled);
    }
    // Level 4 leaves no facets: its small planes are true planes.
    if (m_settings.level <= 3)
        name_facets();
}

void MeshToStep::face_regions(const std::function<bool()> &)
{
    m_region.resize(m_its.indices.size());
    m_surfaces.resize(m_its.indices.size());
    for (size_t f = 0; f < m_its.indices.size(); ++f) {
        m_region[f]         = int(f);
        m_surfaces[f].axis  = m_normal[f];
        m_surfaces[f].offset = m_normal[f].dot(vertex(m_its, m_its.indices[f][0]));
    }
}

// Faces grown into planar regions, the largest faces first. A face joins when its normal is within angle degrees of
// the plane of the region and its corners within tolerance of it; the plane is fitted again on the whole region when
// it stops growing, and the growth goes on with it.
void MeshToStep::planar_regions(double tolerance, double angle, const std::function<bool()> &canceled)
{
    const int nf = int(m_its.indices.size());
    m_region.assign(size_t(nf), -1);
    m_surfaces.clear();
    const double     cos_max = std::cos(angle * PI / 180.);
    std::vector<int> order(static_cast<size_t>(nf));
    for (int f = 0; f < nf; ++f)
        order[f] = f;
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return m_area[a] > m_area[b]; });
    for (int seed : order) {
        if (m_region[seed] >= 0)
            continue;
        if ((m_surfaces.size() & 1023) == 0)
            check(canceled);
        const int        rid = int(m_surfaces.size());
        Vec3d            n   = m_normal[seed];
        double           d   = n.dot(vertex(m_its, m_its.indices[seed][0]));
        std::vector<int> members{ seed };
        m_region[seed] = rid;
        for (int round = 0; round < 3; ++round) {
            std::deque<int> queue(members.begin(), members.end());
            bool            grew = false;
            while (!queue.empty()) {
                const int f = queue.front();
                queue.pop_front();
                for (int k = 0; k < 3; ++k) {
                    const int g = m_twin[f][k];
                    // A sliver has no reliable normal: its corners alone decide.
                    if (g < 0 || m_region[g] >= 0 || (m_area[g] > 1e-9 && m_normal[g].dot(n) < cos_max))
                        continue;
                    bool close = true;
                    for (int j = 0; j < 3 && close; ++j)
                        close = std::abs(n.dot(vertex(m_its, m_its.indices[g][j])) - d) <= tolerance;
                    if (!close)
                        continue;
                    m_region[g] = rid;
                    members.push_back(g);
                    queue.push_back(g);
                    grew = true;
                }
            }
            if (members.size() == 1 || !grew)
                break;
            // The plane of the whole region, then grown again with it.
            std::vector<int> verts;
            for (int f : members)
                for (int j = 0; j < 3; ++j)
                    verts.push_back(m_its.indices[f][j]);
            sort_remove_duplicates(verts);
            std::vector<Vec3d> points;
            points.reserve(verts.size());
            for (int v : verts)
                points.push_back(vertex(m_its, v));
            PlaneFit fit = fit_plane(points);
            if (fit.normal.dot(n) < 0.) {
                fit.normal = -fit.normal;
                fit.offset = -fit.offset;
            }
            if (fit.error > tolerance)
                break;
            n = fit.normal;
            d = fit.offset;
        }
        Surface plane;
        plane.axis   = n;
        plane.offset = d;
        m_surfaces.push_back(plane);
    }
}

// The flat pieces of level 2 gathered into the curved surfaces they lie on. A curved surface is kept when it covers
// at least min_faces faces and 0.1 % of the area of the part, and its normals turn by 15 degrees or more: a nearly flat
// piece is ambiguous (a large sphere, a very open cone or a torus fit it as well) and stays in flat facets.
// strict (level 4): within a tenth of the tolerance (no arc found on a fillet whose radius varies), from seeds which may
// leave out a neighbour belonging to another surface (the piece with each pair of its neighbours, the largest result
// kept), without the rule of the area; only a small curved surface (under 1 % of the area) goes back to the facets
// around it; small flat pieces next to another one are left free (region -1) for recognize_free().
void MeshToStep::recognize(bool strict, const std::function<bool()> &canceled)
{
    const double tolerance = strict ? 0.1 * m_settings.tolerance : m_settings.tolerance;
    planar_regions(tolerance, std::min(5., 0.5 * m_settings.tolerance / 0.01), canceled);
    const int count = int(m_surfaces.size());
    const int nf    = int(m_its.indices.size());

    // The flat pieces: faces, vertices, area, normal, center, neighbours.
    // static_cast, not size_t(count): "T x(size_t(count));" would declare a function.
    const size_t                  units = static_cast<size_t>(count);
    std::vector<std::vector<int>> faces_of(units), verts_of(units), neighbors(units);
    std::vector<double>           area(units, 0.);
    std::vector<Vec3d>            centroid(units), unit_normal(units);
    for (int f = 0; f < nf; ++f)
        faces_of[m_region[f]].push_back(f);
    double total_area = 0.;
    for (int u = 0; u < count; ++u) {
        for (int f : faces_of[u]) {
            for (int j = 0; j < 3; ++j)
                verts_of[u].push_back(m_its.indices[f][j]);
            area[u] += m_area[f];
        }
        sort_remove_duplicates(verts_of[u]);
        Vec3d c = Vec3d::Zero();
        for (int v : verts_of[u])
            c += vertex(m_its, v);
        centroid[u]    = c / double(verts_of[u].size());
        unit_normal[u] = m_surfaces[u].axis;
        total_area += area[u];
    }
    for (int f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            const int g = m_twin[f][k];
            if (g >= 0 && m_region[g] != m_region[f])
                neighbors[m_region[f]].push_back(m_region[g]);
        }
    for (std::vector<int> &n : neighbors)
        sort_remove_duplicates(n);

    BoundingBoxf3 box;
    for (const stl_vertex &v : m_its.vertices)
        box.merge(v.cast<double>());
    const double size       = box.size().norm();
    const double cos_smooth = std::cos(35. * PI / 180.);
    const double cos_normal = std::cos(25. * PI / 180.);

    std::vector<int>              owner(size_t(count), -1);
    std::vector<char>             tried(size_t(count), 0);
    std::vector<Surface>          curved;
    std::vector<std::vector<int>> members_of;

    auto smooth_neighbors = [&](int u) {
        std::vector<int> out;
        for (int w : neighbors[u])
            if (owner[w] < 0 && unit_normal[w].dot(unit_normal[u]) >= cos_smooth)
                out.push_back(w);
        return out;
    };
    auto samples = [&](const std::vector<int> &group) {
        Samples          s;
        std::vector<int> verts;
        for (int u : group) {
            verts.insert(verts.end(), verts_of[u].begin(), verts_of[u].end());
            s.C.push_back(centroid[u]);
            s.N.push_back(unit_normal[u]);
            s.W.push_back(area[u]);
        }
        sort_remove_duplicates(verts);
        s.P.reserve(verts.size());
        for (int v : verts)
            s.P.push_back(vertex(m_its, v));
        return s;
    };
    auto fits = [&](const Surface &shape, int w) {
        for (int v : verts_of[w])
            if (distance(shape, vertex(m_its, v)) > tolerance)
                return false;
        // The inside of its faces within the tolerance and their sagitta: a large flat face fits a cylinder through its
        // corners which bulges between them (two triangles of 53 x 19 mm, a cylinder of radius 39 out by 10 mm).
        for (int f : faces_of[w]) {
            const Vec3d c = (vertex(m_its, m_its.indices[f][0]) + vertex(m_its, m_its.indices[f][1]) + vertex(m_its, m_its.indices[f][2])) / 3.;
            if (distance(shape, c) > m_settings.tolerance + m_sagitta[f])
                return false;
        }
        return std::abs(normal(shape, centroid[w]).dot(unit_normal[w])) >= cos_normal;
    };
    auto grow = [&](const Surface &shape, const std::vector<int> &group) {
        std::vector<int>        members = group;
        std::unordered_set<int> in(members.begin(), members.end());
        std::deque<int>         queue(group.begin(), group.end());
        while (!queue.empty()) {
            const int u = queue.front();
            queue.pop_front();
            for (int w : neighbors[u]) {
                if (owner[w] >= 0 || in.count(w) > 0 || !fits(shape, w))
                    continue;
                in.insert(w);
                members.push_back(w);
                queue.push_back(w);
            }
        }
        std::sort(members.begin(), members.end());
        return members;
    };
    auto area_of = [&](const std::vector<int> &group) {
        double a = 0.;
        for (int u : group)
            a += area[u];
        return a;
    };
    auto substantial = [&](const std::vector<int> &group) {
        size_t faces = 0;
        for (int u : group)
            faces += faces_of[u].size();
        if (faces < size_t(strict ? 4 : std::max(m_settings.min_faces, 1)))
            return false;
        if (!strict && area_of(group) < 0.001 * total_area)
            return false;
        Vec3d mean = Vec3d::Zero();
        for (int u : group)
            mean += area[u] * unit_normal[u];
        if (mean.norm() <= 0.)
            return true;
        mean.normalize();
        double lowest = 1.;
        for (int u : group)
            lowest = std::min(lowest, unit_normal[u].dot(mean));
        return lowest <= std::cos(15. * PI / 180.);
    };

    std::vector<int> candidates;
    for (int u = 0; u < count; ++u)
        if (smooth_neighbors(u).size() >= 2)
            candidates.push_back(u);
    std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) { return area[a] > area[b]; });
    for (size_t c = 0; c < candidates.size(); ++c) {
        const int seed = candidates[c];
        if (owner[seed] >= 0 || tried[seed])
            continue;
        if ((c & 63) == 0)
            check(canceled);
        const std::vector<int> around = smooth_neighbors(seed);
        if (around.size() < 2)
            continue;
        // Seeds: the piece with its smooth neighbours; strict, also with each pair of them (a piece of a curved surface
        // has a few; a large plane bordered by fillets may have hundreds, and no curved surface to seed). Each one alone,
        // then with the next ring; the surface which grows the largest is kept.
        std::vector<std::vector<int>> seeds{ { seed } };
        seeds[0].insert(seeds[0].end(), around.begin(), around.end());
        if (strict && around.size() > 2 && around.size() <= 16)
            for (size_t a = 0; a < around.size(); ++a)
                for (size_t b = a + 1; b < around.size(); ++b)
                    seeds.push_back({ seed, around[a], around[b] });
        std::optional<Surface> shape;
        std::vector<int>       members;
        for (const std::vector<int> &group : seeds) {
            check(canceled);
            std::vector<int> ring2;
            for (size_t i = 1; i < group.size(); ++i)
                for (int w : smooth_neighbors(group[i]))
                    if (std::find(group.begin(), group.end(), w) == group.end())
                        ring2.push_back(w);
            sort_remove_duplicates(ring2);
            std::vector<int> wider = group;
            wider.insert(wider.end(), ring2.begin(), ring2.end());
            const std::vector<int> *trials[2] = { &group, &wider };
            for (const std::vector<int> *trial : trials) {
                std::optional<Surface> found = best_fit(samples(*trial), tolerance, size, strict);
                if (!found || !std::all_of(trial->begin(), trial->end(), [&](int w) { return fits(*found, w); }))
                    continue;
                // Fitted again on what it grew to, which may grow further.
                std::vector<int> grown = grow(*found, *trial);
                for (int round = 0; round < 4; ++round) {
                    const std::optional<Surface> refit = best_fit(samples(grown), tolerance, size, strict);
                    if (!refit)
                        break;
                    std::vector<int> more = grow(*refit, grown);
                    if (more.size() < grown.size())
                        break;
                    const bool changed = more.size() > grown.size();
                    found              = refit;
                    grown              = std::move(more);
                    if (!changed)
                        break;
                }
                if (!shape || area_of(grown) > area_of(members)) {
                    shape   = found;
                    members = std::move(grown);
                }
                break;
            }
        }
        if (!shape)
            continue;
        if (!substantial(members)) {
            // Not a seed again: the same pieces would give the same surface.
            for (int u : members)
                tried[u] = 1;
            continue;
        }
        for (int u : members)
            owner[u] = int(curved.size());
        curved.push_back(*shape);
        members_of.push_back(members);
    }

    // Neighbouring surfaces which one surface fits as well are merged, the largest first.
    for (bool merged = true; merged;) {
        merged = false;
        check(canceled);
        std::vector<int> order;
        for (int i = 0; i < int(curved.size()); ++i)
            if (!members_of[i].empty())
                order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return area_of(members_of[a]) > area_of(members_of[b]); });
        for (int i : order) {
            std::vector<int> touching;
            for (int u : members_of[i])
                for (int w : neighbors[u])
                    if (owner[w] >= 0 && owner[w] != i)
                        touching.push_back(owner[w]);
            sort_remove_duplicates(touching);
            std::stable_sort(touching.begin(), touching.end(), [&](int a, int b) { return area_of(members_of[a]) > area_of(members_of[b]); });
            for (int j : touching) {
                if (members_of[j].empty())
                    continue;
                std::vector<int> both = members_of[i];
                both.insert(both.end(), members_of[j].begin(), members_of[j].end());
                std::sort(both.begin(), both.end());
                const std::optional<Surface> refit = best_fit(samples(both), tolerance, size, strict);
                if (!refit || !std::all_of(both.begin(), both.end(), [&](int w) { return fits(*refit, w); }))
                    continue;
                curved[i]     = *refit;
                members_of[i] = both;
                for (int u : members_of[j])
                    owner[u] = i;
                members_of[j].clear();
                merged = true;
            }
            if (merged)
                break;
        }
    }

    // A curved surface bordered mostly by facets is a piece of a surface which is not recognized (the helix of a
    // thread is close to a cone here and there): back to facets, until none is left. A facet is a flat piece left
    // alone which is small (under 0.2 % of the area of the part); strict, also a piece of a surface sent back.
    std::vector<char> facet(units, 0);
    for (int u = 0; u < count; ++u)
        facet[u] = owner[u] < 0 && area[u] < 0.002 * total_area;
    std::unordered_map<uint64_t, double> border_length;
    for (int f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            const int g = m_twin[f][k];
            if (g >= 0 && m_region[g] != m_region[f])
                border_length[edge_key(m_region[f], m_region[g])] +=
                    (vertex(m_its, m_its.indices[f][(k + 1) % 3]) - vertex(m_its, m_its.indices[f][k])).norm();
        }
    for (bool demoted = true; demoted;) {
        demoted = false;
        for (size_t i = 0; i < curved.size(); ++i) {
            // Strict: a large one stays (a shank whose border is a thread).
            if (members_of[i].empty() || (strict && area_of(members_of[i]) >= 0.01 * total_area))
                continue;
            double border = 0., alone = 0.;
            for (int u : members_of[i])
                for (int w : neighbors[u]) {
                    if (owner[w] == int(i))
                        continue;
                    const auto   it     = border_length.find(edge_key(u, w));
                    const double length = it == border_length.end() ? 0. : it->second;
                    border += length;
                    if (owner[w] < 0 && (strict ? facet[w] != 0 : area[w] < 0.002 * total_area))
                        alone += length;
                }
            if (border > 0. && alone > 0.6 * border) {
                for (int u : members_of[i]) {
                    owner[u] = -1;
                    facet[u] = 1;
                }
                members_of[i].clear();
                demoted = true;
            }
        }
    }

    // The curved surfaces kept, then the flat pieces left, each a plane.
    std::vector<Surface> planes = std::move(m_surfaces);
    std::vector<int>     index(curved.size(), -1);
    m_surfaces.clear();
    for (size_t i = 0; i < curved.size(); ++i)
        if (!members_of[i].empty()) {
            index[i] = int(m_surfaces.size());
            m_surfaces.push_back(curved[i]);
        }
    std::vector<int> region_of_unit(units);
    for (int u = 0; u < count; ++u) {
        if (owner[u] >= 0)
            region_of_unit[u] = index[owner[u]];
        else if (strict && facet[u] &&
                 std::any_of(neighbors[u].begin(), neighbors[u].end(), [&](int w) { return facet[w] && unit_normal[w].dot(unit_normal[u]) >= cos_smooth; }))
            region_of_unit[u] = -1;
        else {
            region_of_unit[u] = int(m_surfaces.size());
            m_surfaces.push_back(planes[u]);
        }
    }
    for (int f = 0; f < nf; ++f)
        m_region[f] = region_of_unit[m_region[f]];
}

// Level 4: the faces which no simple surface explains (region -1) made of surfaces without facets. Extrusions,
// revolutions and helices first, then free patches; the borders between them cleaned, the small pieces joined to a
// neighbour; then a surface for each region within the tolerance: exact (the extrusion or the revolution of a profile)
// or a B-spline surface through the vertices and the pins of the region, a patch which none fits cut in two.
void MeshToStep::recognize_free(const std::function<bool()> &canceled)
{
    const double  tolerance = m_settings.tolerance;
    const int     nf        = int(m_its.indices.size());
    BoundingBoxf3 box;
    for (const stl_vertex &v : m_its.vertices)
        box.merge(v.cast<double>());
    const MeshView mesh{ m_its, m_twin, m_normal, m_area, m_sagitta, m_edge_sagitta, box.size().norm() };
    widen(mesh, m_region, m_surfaces, tolerance, canceled);

    // Regions: the simple surfaces, then the kinematic ones, then the free patches.
    const int         simple = int(m_surfaces.size());
    std::vector<int>  label  = m_region;
    std::vector<char> free(size_t(nf), 0);
    for (int f = 0; f < nf; ++f)
        free[f] = label[f] < 0;
    std::vector<std::pair<Vec3d, Vec3d>> axes;
    for (const Surface &s : m_surfaces)
        if (s.kind == Kind::Cylinder || s.kind == Kind::Cone || s.kind == Kind::Torus)
            axes.emplace_back(s.center, s.axis);
    const std::vector<Kinematic> found = extract_kinematic(mesh, free, tolerance, axes, canceled);
    for (size_t i = 0; i < found.size(); ++i)
        for (int f : found[i].first)
            label[f] = simple + int(i);
    const std::function<bool(int)> movable = [simple](int r) { return r < 0 || r >= simple; };
    smooth_borders(mesh, label, movable);
    int next = simple + int(found.size());
    {
        std::vector<int> left;
        for (int f = 0; f < nf; ++f)
            if (label[f] < 0)
                left.push_back(f);
        for (const std::vector<int> &patch : free_patches(mesh, left, tolerance, canceled)) {
            for (int f : patch)
                label[f] = next;
            ++next;
        }
    }
    absorb_small(mesh, label, movable, 40);
    check(canceled);

    std::vector<std::vector<int>> faces_of(static_cast<size_t>(next));
    for (int f = 0; f < nf; ++f)
        faces_of[label[f]].push_back(f);
    std::vector<std::pair<int, std::vector<int>>> pending;
    for (int r = 0; r < next; ++r)
        pending.emplace_back(r, std::move(faces_of[r]));
    std::vector<Surface> surfaces;
    std::vector<int>     region(static_cast<size_t>(nf), -1);
    while (!pending.empty()) {
        check(canceled);
        const int        r     = pending.back().first;
        std::vector<int> faces = std::move(pending.back().second);
        pending.pop_back();
        if (faces.empty())
            continue;
        std::optional<Surface> surface;
        if (r < simple)
            surface = m_surfaces[r];
        else if (r < simple + int(found.size())) {
            const Profile &profile = found[r - simple].second;
            if (profile.exact && profile.motion.kind != Move::Helix && exact_profile(mesh, faces, profile, tolerance))
                surface = profile_surface(profile);
            else if (std::optional<BSurface> patch = fit_kinematic_patch(mesh, faces, profile, tolerance, pins_for(mesh, faces, label, m_surfaces)))
                surface = bspline_surface(kind_of(profile.motion.kind), std::move(*patch));
        }
        if (!surface) {
            if (std::optional<BSurface> patch = fit_plane_patch(mesh, faces, tolerance, pins_for(mesh, faces, label, m_surfaces)))
                surface = bspline_surface(Kind::Free, std::move(*patch));
            else if (faces.size() > 1) {
                for (std::vector<int> &part : split(mesh, faces))
                    pending.emplace_back(next++, std::move(part));
                continue;
            } else if (std::optional<BSurface> alone = fit_plane_patch(mesh, faces, tolerance, Pins()))
                surface = bspline_surface(Kind::Free, std::move(*alone));
            else {
                // The plane of the face: the one facet left.
                Surface plane;
                plane.kind   = Kind::Facet;
                plane.axis   = m_normal[faces.front()];
                plane.offset = plane.axis.dot(mesh.point(m_its.indices[faces.front()][0]));
                surface      = plane;
            }
        }
        for (int f : faces)
            region[f] = int(surfaces.size());
        surfaces.push_back(std::move(*surface));
    }
    join_thin(mesh, region, tolerance);
    drop_empty_regions(region, surfaces);
    m_region   = std::move(region);
    m_surfaces = std::move(surfaces);
}

MeshToStep MeshToStep::with_facets(const std::vector<int> &regions) const
{
    MeshToStep     out(*this);
    const MeshView mesh{ out.m_its, out.m_twin, out.m_normal, out.m_area, out.m_sagitta, out.m_edge_sagitta, 0. };
    const double   angle = std::min(5., 0.5 * m_settings.tolerance / 0.01);
    // A piece relabels faces of its own region only: the lists made once stay right.
    std::vector<std::vector<int>> faces_of(out.m_surfaces.size());
    for (int f = 0; f < int(out.m_region.size()); ++f)
        faces_of[out.m_region[f]].push_back(f);
    for (int r : regions) {
        if (r < 0 || r >= int(faces_of.size()))
            continue;
        const std::vector<int> &faces = faces_of[r];
        // The first piece keeps the number of the region, the others are new regions.
        bool first = true;
        for (auto &[piece, plane] : planar_pieces(mesh, faces, m_settings.tolerance, angle)) {
            const int id = first ? r : int(out.m_surfaces.size());
            if (first)
                out.m_surfaces[r] = plane;
            else
                out.m_surfaces.push_back(plane);
            for (int f : piece)
                out.m_region[f] = id;
            first = false;
        }
    }
    join_thin(mesh, out.m_region, m_settings.tolerance);
    drop_empty_regions(out.m_region, out.m_surfaces);
    return out;
}

// Planes under 0.2 % of the area of the part are facets of a surface which is not recognized.
void MeshToStep::name_facets()
{
    std::vector<double> area(m_surfaces.size(), 0.);
    double              total = 0.;
    for (size_t f = 0; f < m_region.size(); ++f) {
        area[m_region[f]] += m_area[f];
        total += m_area[f];
    }
    for (size_t r = 0; r < m_surfaces.size(); ++r)
        if (m_surfaces[r].kind == Kind::Plane && area[r] < 0.002 * total)
            m_surfaces[r].kind = Kind::Facet;
}

MeshToStep::Result MeshToStep::write(const std::string &path, const std::string &name, const std::function<void(double)> &progress,
                                     const std::function<bool()> &canceled, bool fitted_edges) const
try {
    const int nf = int(m_its.indices.size());
    // Where each directed edge of the mesh is: face * 3 + k.
    std::unordered_map<uint64_t, int> edge_at;
    edge_at.reserve(size_t(nf) * 3);
    for (int f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k)
            edge_at.emplace(edge_key(m_its.indices[f][k], m_its.indices[f][(k + 1) % 3]), f * 3 + k);

    // Boundary loops of each region, in the direction of its faces: outer loops counterclockwise seen from outside,
    // holes clockwise. From a border half-edge, the next one turns around its head through the faces of the region.
    auto border = [&](int f, int k) { return m_region[m_twin[f][k]] != m_region[f]; };
    std::map<int, std::vector<std::vector<int>>> loops;
    std::vector<char>                            used(size_t(nf) * 3, 0);
    for (int f0 = 0; f0 < nf; ++f0)
        for (int k0 = 0; k0 < 3; ++k0) {
            if (!border(f0, k0) || used[f0 * 3 + k0])
                continue;
            std::vector<int> loop;
            int              f = f0, k = k0;
            while (!used[f * 3 + k]) {
                used[f * 3 + k] = 1;
                loop.push_back(m_its.indices[f][k]);
                k = (k + 1) % 3;
                while (!border(f, k)) {
                    const int g  = m_twin[f][k];
                    const int at = edge_at.at(edge_key(m_its.indices[f][(k + 1) % 3], m_its.indices[f][k]));
                    f            = g;
                    k            = (at % 3 + 1) % 3;
                }
            }
            loops[m_region[f0]].push_back(std::move(loop));
        }
    check(canceled);

    // Corners: vertices where three regions or more meet, or where a region passes more than once.
    std::vector<std::vector<int>> regions_at(m_its.vertices.size());
    for (int f = 0; f < nf; ++f)
        for (int j = 0; j < 3; ++j)
            regions_at[m_its.indices[f][j]].push_back(m_region[f]);
    std::vector<int> passes(m_its.vertices.size(), 0);
    for (const auto &[r, list] : loops)
        for (const std::vector<int> &loop : list)
            for (int v : loop)
                ++passes[v];
    std::vector<char> corner(m_its.vertices.size(), 0);
    for (size_t v = 0; v < m_its.vertices.size(); ++v) {
        sort_remove_duplicates(regions_at[v]);
        corner[v] = regions_at[v].size() >= 3 || passes[v] > 2;
    }
    // The loop cut at its corners: chains of vertices from a corner to the next one. A loop without corner is cut at
    // its smallest vertex, so that both regions along it cut it at the same place.
    auto chains_of = [&](const std::vector<int> &loop) {
        std::vector<int> at;
        for (int i = 0; i < int(loop.size()); ++i)
            if (corner[loop[i]])
                at.push_back(i);
        if (at.empty())
            at.push_back(int(std::min_element(loop.begin(), loop.end()) - loop.begin()));
        std::vector<std::vector<int>> chains;
        for (size_t c = 0; c < at.size(); ++c) {
            const int        a = at[c], b = c + 1 < at.size() ? at[c + 1] : at[0] + int(loop.size());
            std::vector<int> chain;
            for (int i = a; i <= b; ++i)
                chain.push_back(loop[i % loop.size()]);
            chains.push_back(std::move(chain));
        }
        return chains;
    };
    // A chain in its canonical direction (from its smaller end), which both of its regions share.
    auto forward = [](const std::vector<int> &c) { return c.front() < c.back() || (c.front() == c.back() && c[1] < c[c.size() - 2]); };
    std::map<std::pair<int, int>, int> pairs;
    {
        std::set<std::vector<int>> seen;
        for (const auto &[r, list] : loops)
            for (const std::vector<int> &loop : list)
                for (std::vector<int> chain : chains_of(loop)) {
                    if (!forward(chain))
                        std::reverse(chain.begin(), chain.end());
                    if (seen.insert(chain).second)
                        ++pairs[{ std::min(chain.front(), chain.back()), std::max(chain.front(), chain.back()) }];
                }
    }

    // The surface of each region.
    std::vector<std::vector<int>> faces_of(m_surfaces.size());
    for (int f = 0; f < nf; ++f)
        faces_of[m_region[f]].push_back(f);
    std::vector<Handle(Geom_Surface)> surfaces(m_surfaces.size());
    for (size_t r = 0; r < m_surfaces.size(); ++r)
        if (!faces_of[r].empty()) {
            std::vector<int> verts;
            for (int f : faces_of[r])
                for (int j = 0; j < 3; ++j)
                    verts.push_back(m_its.indices[f][j]);
            sort_remove_duplicates(verts);
            std::vector<Vec3d> points;
            for (int v : verts)
                points.push_back(vertex(m_its, v));
            surfaces[r] = occt_surface(m_surfaces[r], points);
        }

    BRep_Builder                                   B;
    std::unordered_map<int, TopoDS_Vertex>         vertices;
    std::map<std::vector<int>, std::vector<TopoDS_Edge>> edges;
    auto occt_vertex = [&](int v) {
        auto it = vertices.find(v);
        if (it == vertices.end()) {
            TopoDS_Vertex tv;
            const Vec3d   p = vertex(m_its, v);
            B.MakeVertex(tv, gp_Pnt(p.x(), p.y(), p.z()), 1e-7);
            it = vertices.emplace(v, tv).first;
        }
        return it->second;
    };

    // Level 4: an edge on a curve fitted to its chain (a line, an arc, a B-spline), kept when it lies on the surfaces
    // of both its faces; else straight pieces as below.
    const bool   fitted    = m_settings.level >= 4 && fitted_edges;
    const double tolerance = m_settings.tolerance;
    const double edge_tol  = 0.25 * tolerance;
    // The regions on each side of each mesh edge between two of them.
    std::unordered_map<uint64_t, std::pair<int, int>> sides;
    if (fitted)
        for (int f = 0; f < nf; ++f)
            for (int k = 0; k < 3; ++k) {
                const int a = m_its.indices[f][k], b = m_its.indices[f][(k + 1) % 3];
                if (m_region[m_twin[f][k]] != m_region[f])
                    sides[edge_key(std::min(a, b), std::max(a, b))] = { m_region[f], m_region[m_twin[f][k]] };
            }
    // Distance of a point to the surface of a region, and its closest point there.
    std::vector<std::unique_ptr<GeomAPI_ProjectPointOnSurf>> projections(m_surfaces.size());
    auto onto = [&](int r, const Vec3d &p, Vec3d &closest) {
        std::unique_ptr<GeomAPI_ProjectPointOnSurf> &projection = projections[r];
        if (!projection) {
            Standard_Real u0, u1, v0, v1;
            surfaces[r]->Bounds(u0, u1, v0, v1);
            projection = std::make_unique<GeomAPI_ProjectPointOnSurf>();
            projection->Init(surfaces[r], u0, u1, v0, v1, Precision::Confusion());
        }
        projection->Perform(pnt(p));
        if (projection->NbPoints() == 0) {
            closest = p;
            return 1e30;
        }
        closest = vec(projection->NearestPoint());
        return double(projection->LowerDistance());
    };
    struct Fitted
    {
        Handle(Geom_Curve) curve;
        double             first{ 0. };
        double             last{ 0. };
        // A line or an arc.
        bool               simple{ false };
    };
    auto sample = [](const Fitted &fit, int i, int count) { return vec(fit.curve->Value(fit.first + (fit.last - fit.first) * double(i) / double(count - 1))); };
    // A B-spline through points of increasing parameters t, on the range of t (OpenCASCADE gives it on [0, 1]).
    auto approximate = [](const std::vector<Vec3d> &P, const std::vector<double> &t, double tol) {
        Fitted             fit;
        const int          n = int(P.size());
        TColgp_Array1OfPnt points(1, n);
        TColStd_Array1OfReal params(1, n);
        for (int i = 0; i < n; ++i) {
            points.SetValue(i + 1, pnt(P[i]));
            params.SetValue(i + 1, t[i]);
        }
        GeomAPI_PointsToBSpline approximation(points, params, std::min(3, n - 1), 8, GeomAbs_C2, tol);
        if (!approximation.IsDone())
            return fit;
        Handle(Geom_BSplineCurve) curve = approximation.Curve();
        const int                 count = curve->NbKnots();
        const double              k0 = curve->Knot(1), k1 = curve->Knot(count);
        TColStd_Array1OfReal      knots(1, count);
        for (int i = 1; i <= count; ++i)
            knots.SetValue(i, t.front() + (curve->Knot(i) - k0) / (k1 - k0) * (t.back() - t.front()));
        curve->SetKnots(knots);
        fit.curve = curve;
        fit.first = curve->FirstParameter();
        fit.last  = curve->LastParameter();
        return fit;
    };
    // A curve through the vertices of a chain: a segment, an arc, a B-spline, which must stay with its points (an arc
    // through nearly aligned points may take the long way round a huge circle, a B-spline may swing); else the polyline
    // of the points.
    auto fit_chain = [&](const std::vector<int> &ids, bool straight_ok) {
        std::vector<Vec3d> P;
        for (int v : ids)
            P.push_back(vertex(m_its, v));
        const Vec3d  a = P.front(), b = P.back(), ab = b - a;
        const double L      = ab.norm();
        double       length = 0., off = 0.;
        for (size_t i = 0; i + 1 < P.size(); ++i)
            length += (P[i + 1] - P[i]).norm();
        if (L > 1e-12)
            for (const Vec3d &p : P)
                off = std::max(off, (p - a).cross(ab / L).norm());
        if (L > 1e-12 && (P.size() == 2 || (straight_ok && off <= edge_tol))) {
            GC_MakeSegment segment(pnt(a), pnt(b));
            if (segment.IsDone())
                return Fitted{ segment.Value(), segment.Value()->FirstParameter(), segment.Value()->LastParameter(), true };
        }
        Vec3d  center;
        double radius = 0., error = 0.;
        if (P.size() >= 4 && L > 1e-12 && fit_circle_3d(P, center, radius, error) && error <= edge_tol && radius < 100. * length) {
            GC_MakeArcOfCircle arc(pnt(a), pnt(P[P.size() / 2]), pnt(b));
            if (arc.IsDone()) {
                const Handle(Geom_TrimmedCurve) &c      = arc.Value();
                const Handle(Geom_Circle)        circle = Handle(Geom_Circle)::DownCast(c->BasisCurve());
                const double                     t0 = c->FirstParameter(), t1 = c->LastParameter();
                // As long as the points (not the long way round), with the points in their order along it.
                bool   ok   = !circle.IsNull() && std::abs((t1 - t0) * radius - length) <= 0.05 * length + edge_tol && t1 - t0 < 2. * PI - 1e-6;
                double prev = t0;
                for (size_t i = 1; ok && i + 1 < P.size(); ++i) {
                    const double d = ElCLib::Parameter(circle->Circ(), pnt(P[i])) - t0;
                    const double t = t0 + d - 2. * PI * std::floor(d / (2. * PI));
                    ok             = t > prev && t < t1;
                    prev           = t;
                }
                if (ok)
                    return Fitted{ c, t0, t1, true };
            }
        }
        // Chord length parameters; points repeated in place fitted once.
        std::vector<Vec3d>  Pk;
        std::vector<double> tk;
        double              s = 0.;
        for (size_t i = 0; i < P.size(); ++i) {
            const double step = i == 0 ? 0. : (P[i] - P[i - 1]).norm();
            s += step;
            if (i == 0 || step > 1e-9) {
                Pk.push_back(P[i]);
                tk.push_back(s);
            } else if (i + 1 == P.size()) {
                Pk.back() = P[i];
                tk.back() = s;
            }
        }
        if (Pk.size() >= 3) {
            Fitted fit = approximate(Pk, tk, edge_tol);
            if (!fit.curve.IsNull()) {
                BoundingBoxf3 box;
                for (const Vec3d &p : P)
                    box.merge(p);
                const double margin = 0.1 * std::max(box.size().maxCoeff(), 1e-9) + edge_tol;
                const int    count  = 4 * int(P.size()) + 8;
                bool         inside = true;
                for (int i = 0; i < count && inside; ++i) {
                    const Vec3d q = sample(fit, i, count);
                    for (int k = 0; k < 3; ++k)
                        inside = inside && q(k) >= box.min(k) - margin && q(k) <= box.max(k) + margin;
                }
                if (inside)
                    return fit;
            }
        }
        // The polyline itself: a B-spline of degree 1 through the points.
        Fitted fit;
        if (Pk.size() >= 2) {
            const int               n = int(Pk.size());
            TColgp_Array1OfPnt      poles(1, n);
            TColStd_Array1OfReal    knots(1, n);
            TColStd_Array1OfInteger mults(1, n);
            for (int i = 0; i < n; ++i) {
                poles.SetValue(i + 1, pnt(Pk[i]));
                knots.SetValue(i + 1, tk[i]);
                mults.SetValue(i + 1, i == 0 || i == n - 1 ? 2 : 1);
            }
            fit.curve = new Geom_BSplineCurve(poles, knots, mults, 1);
            fit.first = tk.front();
            fit.last  = tk.back();
        }
        return fit;
    };
    // The two regions along a chain, from its first mesh edge.
    auto sides_of = [&](const std::vector<int> &ids) {
        const auto it = sides.find(edge_key(std::min(ids[0], ids[1]), std::max(ids[0], ids[1])));
        return it == sides.end() ? std::make_pair(-1, -1) : it->second;
    };
    // The curve of a chain kept when it lies on the surfaces of both its faces; otherwise points along the chain
    // (between its vertices too, where the mesh cuts the curve of the part by a chord) put on one surface then the
    // other until they lie on both, and a curve fitted through them.
    auto onto_both = [&](const std::vector<int> &ids, Fitted &fit) {
        const auto [ra, rb] = sides_of(ids);
        if (ra < 0)
            return;
        Vec3d      closest;
        double     worst = 0.;
        const int  count = 4 * int(ids.size()) + 5;
        for (int i = 0; i < count; ++i) {
            const Vec3d p = sample(fit, i, count);
            worst         = std::max(worst, std::max(onto(ra, p, closest), onto(rb, p, closest)));
        }
        if (worst <= 0.5 * tolerance)
            return;
        std::vector<Vec3d> Q{ vertex(m_its, ids[0]) };
        for (size_t i = 0; i + 1 < ids.size(); ++i) {
            const Vec3d A = vertex(m_its, ids[i]), Z = vertex(m_its, ids[i + 1]);
            const int   m = int(std::min(8., std::max(1., std::ceil((Z - A).norm() / std::max(20. * tolerance, 1e-6)))));
            for (int k = 1; k <= m; ++k) {
                const double w = double(k) / double(m + 1);
                Vec3d        p = (1. - w) * A + w * Z;
                for (int iteration = 0; iteration < 30; ++iteration) {
                    Vec3d pa, pb;
                    onto(ra, p, pa);
                    onto(rb, pa, pb);
                    const double moved = (pb - p).norm();
                    p                  = pb;
                    if (moved < 1e-3 * tolerance)
                        break;
                }
                if ((p - Q.back()).norm() > 1e-9)
                    Q.push_back(p);
            }
            if ((Z - Q.back()).norm() > 1e-9)
                Q.push_back(Z);
            else
                Q.back() = Z;
        }
        if (Q.size() < 2)
            return;
        std::vector<double> t{ 0. };
        for (size_t i = 1; i < Q.size(); ++i)
            t.push_back(t.back() + (Q[i] - Q[i - 1]).norm());
        Fitted refit = approximate(Q, t, edge_tol);
        if (!refit.curve.IsNull())
            fit = refit;
    };
    auto lies_on_both = [&](const std::vector<int> &ids, const Fitted &fit, double limit) {
        const auto [ra, rb] = sides_of(ids);
        if (ra < 0)
            return true;
        Vec3d     closest;
        const int count = 2 * int(ids.size()) + 7;
        for (int i = 0; i < count; ++i) {
            const Vec3d p = sample(fit, i, count);
            if (onto(ra, p, closest) > limit || onto(rb, p, closest) > limit)
                return false;
        }
        return true;
    };
    // Whether the two faces along a chain meet smoothly: at each of its mesh edges, the normals of the two mesh faces
    // within 20 degrees.
    auto tangent = [&](const std::vector<int> &ids) {
        const double cos_max = std::cos(20. * PI / 180.);
        for (size_t i = 0; i + 1 < ids.size(); ++i) {
            const auto f = edge_at.find(edge_key(ids[i], ids[i + 1]));
            const auto g = edge_at.find(edge_key(ids[i + 1], ids[i]));
            if (f == edge_at.end() || g == edge_at.end() || m_normal[f->second / 3].dot(m_normal[g->second / 3]) < cos_max)
                return false;
        }
        return true;
    };
    // A smooth curve for a chain between two faces meeting smoothly: there the boundary has no place of its own, and the
    // mesh puts it on a staircase of its edges. The points of the chain smoothed (its ends kept), each put midway
    // between the two surfaces, a B-spline through them.
    auto smooth_curve = [&](const std::vector<int> &ids) {
        Fitted         fit;
        const auto [ra, rb] = sides_of(ids);
        if (ra < 0 || ids.size() < 3)
            return fit;
        std::vector<Vec3d> P;
        for (int v : ids)
            P.push_back(vertex(m_its, v));
        for (int pass = 0; pass < 12; ++pass) {
            std::vector<Vec3d> Q = P;
            for (size_t i = 1; i + 1 < P.size(); ++i)
                Q[i] = 0.25 * P[i - 1] + 0.5 * P[i] + 0.25 * P[i + 1];
            P = std::move(Q);
        }
        for (size_t i = 1; i + 1 < P.size(); ++i) {
            Vec3d p = P[i];
            for (int iteration = 0; iteration < 20; ++iteration) {
                Vec3d pa, pb;
                onto(ra, p, pa);
                onto(rb, p, pb);
                const Vec3d  q     = 0.5 * (pa + pb);
                const double moved = (q - p).norm();
                p                  = q;
                if (moved < 1e-3 * tolerance)
                    break;
            }
            P[i] = p;
        }
        std::vector<Vec3d> Q{ P.front() };
        for (size_t i = 1; i < P.size(); ++i)
            if ((P[i] - Q.back()).norm() > 1e-9)
                Q.push_back(P[i]);
            else if (i + 1 == P.size())
                Q.back() = P[i];
        if (Q.size() < 2)
            return fit;
        std::vector<double> t{ 0. };
        for (size_t i = 1; i < Q.size(); ++i)
            t.push_back(t.back() + (Q[i] - Q[i - 1]).norm());
        return approximate(Q, t, edge_tol);
    };
    // An edge on a curve between the vertices of two mesh vertices, their tolerance raised to reach the curve.
    auto make_edge = [&](const Fitted &fit, int v1, int v2) {
        TopoDS_Edge         edge;
        const TopoDS_Vertex a = occt_vertex(v1), b = occt_vertex(v2);
        B.MakeEdge(edge, fit.curve, 1e-7);
        B.Add(edge, a.Oriented(TopAbs_FORWARD));
        B.Add(edge, b.Oriented(TopAbs_REVERSED));
        B.Range(edge, fit.first, fit.last);
        const double da = fit.curve->Value(fit.first).Distance(BRep_Tool::Pnt(a));
        if (da > BRep_Tool::Tolerance(a))
            B.UpdateVertex(a, 1.01 * da);
        const double db = fit.curve->Value(fit.last).Distance(BRep_Tool::Pnt(b));
        if (db > BRep_Tool::Tolerance(b))
            B.UpdateVertex(b, 1.01 * db);
        return edge;
    };

    // Edges of a chain, oriented along it, computed once in its canonical direction. Straight pieces between the mesh
    // vertices kept, to the precision of the file (mesh edges between two planes of a CAD model are straight); two
    // chains between the same corners would become the same straight edge: a point is kept between them; a closed chain
    // keeps two. Level 4: curves fitted to a closed chain in two halves (two arcs for a circle: a whole circle with one
    // vertex, on a face going around an axis, is cut by the repair at its seam into a whole turn and an edge of length
    // zero), to a long chain in pieces of 25 points (a helix of a thread turns several times: OpenCASCADE puts each
    // piece on its faces reliably).
    auto chain_edges = [&](const std::vector<int> &chain) {
        const bool       fwd   = forward(chain);
        std::vector<int> canon = chain;
        if (!fwd)
            std::reverse(canon.begin(), canon.end());
        auto it = edges.find(canon);
        if (it == edges.end()) {
            std::vector<TopoDS_Edge> pieces;
            auto straight = [&](const std::vector<int> &part, int at_least) {
                std::vector<Vec3d> points;
                for (int v : part)
                    points.push_back(vertex(m_its, v));
                const std::vector<int> keep = douglas_peucker(points, 1e-4, at_least);
                for (size_t i = 0; i + 1 < keep.size(); ++i) {
                    BRepBuilderAPI_MakeEdge maker(occt_vertex(part[keep[i]]), occt_vertex(part[keep[i + 1]]));
                    if (!maker.IsDone())
                        throw Slic3r::RuntimeError("An edge of the STEP solid could not be made");
                    pieces.push_back(maker.Edge());
                }
            };
            auto piece = [&](const std::vector<int> &part, bool straight_ok) {
                Fitted fit = fit_chain(part, straight_ok);
                // A line or an arc on both faces is kept as it is; between faces meeting smoothly, a smooth curve rather
                // than one which follows the staircase of the mesh (within 2.5 times the tolerance of both: each
                // surface is within the tolerance of the mesh, not of the other).
                if (fit.simple && lies_on_both(part, fit, tolerance)) {
                    pieces.push_back(make_edge(fit, part.front(), part.back()));
                    return;
                }
                if (tangent(part)) {
                    const Fitted smooth = smooth_curve(part);
                    if (!smooth.curve.IsNull() && lies_on_both(part, smooth, 2.5 * tolerance)) {
                        pieces.push_back(make_edge(smooth, part.front(), part.back()));
                        return;
                    }
                }
                if (!fit.curve.IsNull())
                    onto_both(part, fit);
                if (fit.curve.IsNull() || !lies_on_both(part, fit, tolerance))
                    straight(part, straight_ok ? 0 : 1);
                else
                    pieces.push_back(make_edge(fit, part.front(), part.back()));
            };
            const int shared = pairs[{ std::min(canon.front(), canon.back()), std::max(canon.front(), canon.back()) }];
            // Along a region made of facets (a face which was not valid made again), straight edges as below.
            const auto [ra, rb] = fitted ? sides_of(canon) : std::make_pair(-1, -1);
            const bool facets   = ra >= 0 && (m_surfaces[ra].kind == Kind::Facet || m_surfaces[rb].kind == Kind::Facet);
            if (!fitted || facets)
                straight(canon, canon.front() == canon.back() ? 2 : shared > 1 ? 1 : 0);
            else if (canon.front() == canon.back()) {
                const size_t half = canon.size() / 2;
                piece(std::vector<int>(canon.begin(), canon.begin() + half + 1), true);
                piece(std::vector<int>(canon.begin() + half, canon.end()), true);
            } else if (canon.size() > 25) {
                const int count = int(std::ceil(double(canon.size() - 1) / 24.));
                for (int c = 0; c < count; ++c) {
                    const size_t i = size_t(std::lround(double(canon.size() - 1) * double(c) / double(count)));
                    const size_t j = size_t(std::lround(double(canon.size() - 1) * double(c + 1) / double(count)));
                    piece(std::vector<int>(canon.begin() + i, canon.begin() + j + 1), true);
                }
            } else
                piece(canon, shared <= 1);
            it = edges.emplace(canon, std::move(pieces)).first;
        }
        std::vector<TopoDS_Edge> out;
        if (fwd)
            for (const TopoDS_Edge &e : it->second)
                out.push_back(TopoDS::Edge(e.Oriented(TopAbs_FORWARD)));
        else
            for (auto e = it->second.rbegin(); e != it->second.rend(); ++e)
                out.push_back(TopoDS::Edge(e->Oriented(TopAbs_REVERSED)));
        return out;
    };

    // Connected pieces of the mesh: a solid each.
    std::vector<int> piece(size_t(nf), -1);
    int              pieces = 0;
    for (int s = 0; s < nf; ++s) {
        if (piece[s] >= 0)
            continue;
        std::vector<int> stack{ s };
        piece[s] = pieces;
        while (!stack.empty()) {
            const int f = stack.back();
            stack.pop_back();
            for (int k = 0; k < 3; ++k)
                if (piece[m_twin[f][k]] < 0) {
                    piece[m_twin[f][k]] = pieces;
                    stack.push_back(m_twin[f][k]);
                }
        }
        ++pieces;
    }
    std::vector<TopoDS_Shell> shells(static_cast<size_t>(pieces));
    for (TopoDS_Shell &shell : shells)
        B.MakeShell(shell);

    // A face per region on its surface, its wires turned so that the face looks outward. A piece made of a single closed
    // surface (a ball, a ring) has no boundary: its face is the whole surface, with its seams.
    std::vector<std::pair<TopoDS_Face, int>> made;
    for (int r = 0; r < int(m_surfaces.size()); ++r) {
        // Fitted edges take longer: the progress every 8 regions then.
        if ((r & 255) == 255 || (fitted && (r & 7) == 7)) {
            check(canceled);
            progress(0.25 * double(r) / double(m_surfaces.size()));
        }
        const Surface &s     = m_surfaces[r];
        const auto     bound = loops.find(r);
        if (faces_of[r].empty() || (bound == loops.end() && s.kind != Kind::Sphere && s.kind != Kind::Torus))
            continue;
        const Handle(Geom_Surface) &surface = surfaces[r];
        bool                        outward = true;
        if (s.kind != Kind::Plane && s.kind != Kind::Facet) {
            const int f = *std::max_element(faces_of[r].begin(), faces_of[r].end(), [this](int a, int b) { return m_area[a] < m_area[b]; });
            const Vec3d c = (vertex(m_its, m_its.indices[f][0]) + vertex(m_its, m_its.indices[f][1]) + vertex(m_its, m_its.indices[f][2])) / 3.;
            outward = same_side(surface, c, m_normal[f]);
        }
        TopoDS_Face face;
        if (bound == loops.end()) {
            BRepBuilderAPI_MakeFace whole(surface, Precision::Confusion());
            // Left out: the solid is not valid, and Auto falls back to flat faces.
            if (!whole.IsDone())
                continue;
            face = whole.Face();
        } else {
            B.MakeFace(face, surface, m_settings.tolerance);
            for (const std::vector<int> &loop : bound->second) {
                TopoDS_Wire wire;
                B.MakeWire(wire);
                for (const std::vector<int> &chain : chains_of(loop))
                    for (const TopoDS_Edge &e : chain_edges(chain))
                        B.Add(wire, e);
                wire.Closed(Standard_True);
                B.Add(face, outward ? wire : TopoDS::Wire(wire.Reversed()));
            }
        }
        if (!outward)
            face.Reverse();
        B.Add(shells[piece[faces_of[r].front()]], face);
        made.emplace_back(face, r);
    }
    TopoDS_Shape shape;
    {
        std::vector<TopoDS_Solid> solids;
        for (TopoDS_Shell &shell : shells) {
            shell.Closed(Standard_True);
            TopoDS_Solid solid;
            B.MakeSolid(solid);
            B.Add(solid, shell);
            solids.push_back(solid);
        }
        if (solids.size() == 1)
            shape = solids.front();
        else {
            TopoDS_Compound compound;
            B.MakeCompound(compound);
            for (const TopoDS_Solid &solid : solids)
                B.Add(compound, solid);
            shape = compound;
        }
    }
    check(canceled);
    progress(0.25);

    // Tolerances, curves on the curved surfaces and their seams by the shape healing of OCCT. Level 4: the regions whose
    // face is still not valid, which a caller may make of facets (with_facets()).
    Result result;
    {
        Handle(Indicator) indicator = new Indicator(progress, canceled, 0.25, 0.6);
        ShapeFix_Shape fix(shape);
        fix.Perform(indicator->Start());
        check(canceled);
        shape = fix.Shape();
        if (fitted)
            for (const auto &[face, r] : made) {
                // Facets already: nothing further to do with them.
                if (m_surfaces[r].kind == Kind::Facet)
                    continue;
                // What the repair made of the face at last (it may replace a face more than once).
                TopoDS_Shape now;
                if (fix.Context()->Status(face, now, Standard_True) == 0)
                    now = face;
                if (!now.IsNull() && BRepCheck_Analyzer(now).IsValid() != Standard_True)
                    result.invalid.push_back(r);
            }
        sort_remove_duplicates(result.invalid);
    }
    result.valid = BRepCheck_Analyzer(shape).IsValid() == Standard_True;
    {
        GProp_GProps props;
        BRepGProp::VolumeProperties(shape, props);
        result.volume = props.Mass();
    }
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next())
        ++result.solids;
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next())
        ++result.faces;
    check(canceled);
    progress(0.65);

    // Written through a document, to give the solid its name.
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document)    doc;
    app->NewDocument(TCollection_ExtendedString("BinXCAF"), doc);
    struct Close
    {
        Handle(XCAFApp_Application) &app;
        Handle(TDocStd_Document)    &doc;
        ~Close() { app->Close(doc); }
    } close{ app, doc };
    Handle(XCAFDoc_ShapeTool) tool  = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const TDF_Label           label = tool->AddShape(shape, Standard_False);
    TDataStd_Name::Set(label, TCollection_ExtendedString(name.c_str(), Standard_True));
    // The writer registers the STEP parameters: the unit is set after it is made.
    STEPCAFControl_Writer writer;
    Interface_Static::SetCVal("write.step.unit", "MM");
    writer.SetNameMode(Standard_True);
    Handle(Indicator) indicator   = new Indicator(progress, canceled, 0.65, 0.95);
    const bool        transferred = writer.Transfer(doc, STEPControl_AsIs, nullptr, indicator->Start()) == Standard_True;
    check(canceled);
    // A shape which the writer refuses may do at a lower level; a file which cannot be written will not.
    if (!transferred)
        throw Slic3r::RuntimeError("The STEP file cannot be written: " + path);
    if (writer.Write(path.c_str()) != IFSelect_RetDone)
        throw NotWritten("The STEP file cannot be written: " + path);
    if (m_settings.level >= 4) {
        // Level 4: the file as read back decides (a solid of fitted edges which the analyzer finds wanting in memory
        // may be valid once written, and the other way round).
        STEPControl_Reader reader;
        result.valid = false;
        if (reader.ReadFile(path.c_str()) == IFSelect_RetDone && reader.TransferRoots() > 0) {
            const TopoDS_Shape back = reader.OneShape();
            result.valid            = !back.IsNull() && BRepCheck_Analyzer(back).IsValid() == Standard_True;
        }
        check(canceled);
    }
    progress(1.);
    return result;
} catch (const Standard_Failure &e) {
    // OpenCASCADE throws its own exceptions (a surface it refuses, a shape it cannot heal).
    const char *message = e.GetMessageString();
    throw Slic3r::RuntimeError(std::string("OpenCASCADE: ") + e.DynamicType()->Name() + ": " + (message != nullptr ? message : ""));
}

} // namespace Slic3r

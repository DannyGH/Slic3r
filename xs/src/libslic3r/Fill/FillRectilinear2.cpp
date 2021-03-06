#include <assert.h>
#include <stdlib.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <boost/static_assert.hpp>

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"

#include "FillRectilinear2.hpp"

#define SLIC3R_DEBUG

#ifdef SLIC3R_DEBUG
#include "SVG.hpp"
#endif

// We want our version of assert.
#include "../libslic3r.h"

#ifndef myassert
#define myassert assert
#endif

namespace Slic3r {

#ifndef clamp
template<typename T>
static inline T clamp(T low, T high, T x)
{
    return std::max<T>(low, std::min<T>(high, x));
}
#endif /* clamp */

#ifndef sqr
template<typename T>
static inline T sqr(T x)
{
    return x * x;
}
#endif /* sqr */

#ifndef mag2
static inline coordf_t mag2(const Point &p)
{
    return sqr(coordf_t(p.x)) + sqr(coordf_t(p.y));
}
#endif /* mag2 */

#ifndef mag
static inline coordf_t mag(const Point &p)
{
    return std::sqrt(mag2(p));
}
#endif /* mag */

enum Orientation
{
    ORIENTATION_CCW = 1,
    ORIENTATION_CW = -1,
    ORIENTATION_COLINEAR = 0
};

// Return orientation of the three points (clockwise, counter-clockwise, colinear)
// The predicate is exact for the coord_t type, using 64bit signed integers for the temporaries.
//FIXME Make sure the temporaries do not overflow,
// which means, the coord_t types must not have some of the topmost bits utilized.
static inline Orientation orient(const Point &a, const Point &b, const Point &c)
{
    // BOOST_STATIC_ASSERT(sizeof(coord_t) * 2 == sizeof(int64_t));
    int64_t u = int64_t(b.x) * int64_t(c.y) - int64_t(b.y) * int64_t(c.x);
    int64_t v = int64_t(a.x) * int64_t(c.y) - int64_t(a.y) * int64_t(c.x);
    int64_t w = int64_t(a.x) * int64_t(b.y) - int64_t(a.y) * int64_t(b.x);
    int64_t d = u - v + w;
    return (d > 0) ? ORIENTATION_CCW : ((d == 0) ? ORIENTATION_COLINEAR : ORIENTATION_CW);
}

// Return orientation of the polygon.
// The input polygon must not contain duplicate points.
static inline bool is_ccw(const Polygon &poly)
{
    // The polygon shall be at least a triangle.
    myassert(poly.points.size() >= 3);
    if (poly.points.size() < 3)
        return true;

    // 1) Find the lowest lexicographical point.
    int     imin = 0;
    for (size_t i = 1; i < poly.points.size(); ++ i) {
        const Point &pmin = poly.points[imin];
        const Point &p    = poly.points[i];
        if (p.x < pmin.x || (p.x == pmin.x && p.y < pmin.y))
            imin = i;
    }

    // 2) Detect the orientation of the corner imin.
    size_t iPrev = ((imin == 0) ? poly.points.size() : imin) - 1;
    size_t iNext = ((imin + 1 == poly.points.size()) ? 0 : imin + 1);
    Orientation o = orient(poly.points[iPrev], poly.points[imin], poly.points[iNext]);
    // The lowest bottom point must not be collinear if the polygon does not contain duplicate points
    // or overlapping segments.
    myassert(o != ORIENTATION_COLINEAR);
    return o == ORIENTATION_CCW;
}

// Having a segment of a closed polygon, calculate its Euclidian length.
// The segment indices seg1 and seg2 signify an end point of an edge in the forward direction of the loop,
// therefore the point p1 lies on poly.points[seg1-1], poly.points[seg1] etc.
static inline coordf_t segment_length(const Polygon &poly, size_t seg1, const Point &p1, size_t seg2, const Point &p2)
{
#ifdef SLIC3R_DEBUG
    // Verify that p1 lies on seg1. This is difficult to verify precisely,
    // but at least verify, that p1 lies in the bounding box of seg1.
    for (size_t i = 0; i < 2; ++ i) {
        size_t seg = (i == 0) ? seg1 : seg2;
        Point  px  = (i == 0) ? p1   : p2;
        Point  pa  = poly.points[((seg == 0) ? poly.points.size() : seg) - 1];
        Point  pb  = poly.points[seg];
        if (pa.x > pb.x)
            std::swap(pa.x, pb.x);
        if (pa.y > pb.y)
            std::swap(pa.y, pb.y);
        myassert(px.x >= pa.x && px.x <= pb.x);
        myassert(px.y >= pa.y && px.y <= pb.y);
    }
#endif /* SLIC3R_DEBUG */
    const Point *pPrev = &p1;
    const Point *pThis = NULL;
    coordf_t len = 0;
    if (seg1 <= seg2) {
        for (size_t i = seg1; i < seg2; ++ i, pPrev = pThis)
           len += pPrev->distance_to(*(pThis = &poly.points[i]));
    } else {
        for (size_t i = seg1; i < poly.points.size(); ++ i, pPrev = pThis)
           len += pPrev->distance_to(*(pThis = &poly.points[i]));
        for (size_t i = 0; i < seg2; ++ i, pPrev = pThis)
           len += pPrev->distance_to(*(pThis = &poly.points[i]));
    }
    len += pPrev->distance_to(p2);
    return len;
}

// Append a segment of a closed polygon to a polyline.
// The segment indices seg1 and seg2 signify an end point of an edge in the forward direction of the loop.
// Only insert intermediate points between seg1 and seg2.
static inline void polygon_segment_append(Points &out, const Polygon &polygon, size_t seg1, size_t seg2)
{
    if (seg1 == seg2) {
        // Nothing to append from this segment.
    } else if (seg1 < seg2) {
        // Do not append a point pointed to by seg2.
        out.insert(out.end(), polygon.points.begin() + seg1, polygon.points.begin() + seg2);
    } else {
        out.reserve(out.size() + seg2 + polygon.points.size() - seg1);
        out.insert(out.end(), polygon.points.begin() + seg1, polygon.points.end());
        // Do not append a point pointed to by seg2.
        out.insert(out.end(), polygon.points.begin(), polygon.points.begin() + seg2);
    }
}

// Append a segment of a closed polygon to a polyline.
// The segment indices seg1 and seg2 signify an end point of an edge in the forward direction of the loop,
// but this time the segment is traversed backward.
// Only insert intermediate points between seg1 and seg2.
static inline void polygon_segment_append_reversed(Points &out, const Polygon &polygon, size_t seg1, size_t seg2)
{
    if (seg1 >= seg2) {
        out.reserve(seg1 - seg2);
        for (size_t i = seg1; i > seg2; -- i)
            out.push_back(polygon.points[i - 1]);
    } else {
        // it could be, that seg1 == seg2. In that case, append the complete loop.
        out.reserve(out.size() + seg2 + polygon.points.size() - seg1);
        for (size_t i = seg1; i > 0; -- i)
            out.push_back(polygon.points[i - 1]);
        for (size_t i = polygon.points.size(); i > seg2; -- i)
            out.push_back(polygon.points[i - 1]);
    }
}

// Intersection point of a vertical line with a polygon segment.
class SegmentIntersection
{
public:
    SegmentIntersection() : 
        iContour(0),
        iSegment(0),
        pos(0),
        type(UNKNOWN),
        consumed_vertical_up(false),
        consumed_perimeter_right(false)
        {}

    // Index of a contour in ExPolygonWithOffset, with which this vertical line intersects.
    size_t      iContour;
    // Index of a segment in iContour, with which this vertical line intersects.
    size_t      iSegment;
    // y position of the intersection.
    coord_t     pos;

    // Kind of intersection. With the original contour, or with the inner offestted contour?
    // A vertical segment will be at least intersected by OUTER_LOW, OUTER_HIGH,
    // but it could be intersected with OUTER_LOW, INNER_LOW, INNER_HIGH, OUTER_HIGH,
    // and there may be more than one pair of INNER_LOW, INNER_HIGH between OUTER_LOW, OUTER_HIGH.
    enum SegmentIntersectionType {
        OUTER_LOW   = 0,
        OUTER_HIGH  = 1,
        INNER_LOW   = 2,
        INNER_HIGH  = 3,
        UNKNOWN     = -1
    };
    SegmentIntersectionType type;

    // Was this segment along the y axis consumed?
    // Up means up along the vertical segment.
    bool consumed_vertical_up;
    // Was a segment of the inner perimeter contour consumed?
    // Right means right from the vertical segment.
    bool consumed_perimeter_right;

    // For the INNER_LOW type, this point may be connected to another INNER_LOW point following a perimeter contour.
    // For the INNER_HIGH type, this point may be connected to another INNER_HIGH point following a perimeter contour.
    // If INNER_LOW is connected to INNER_HIGH or vice versa,
    // one has to make sure the vertical infill line does not overlap with the connecting perimeter line.
    bool is_inner() const { return type == INNER_LOW  || type == INNER_HIGH; }
    bool is_outer() const { return type == OUTER_LOW  || type == OUTER_HIGH; }
    bool is_low  () const { return type == INNER_LOW  || type == OUTER_LOW; }
    bool is_high () const { return type == INNER_HIGH || type == OUTER_HIGH; }

    bool operator<(const SegmentIntersection &other) const
        { return pos < other.pos; }
};

// A vertical line with intersection points with polygons.
class SegmentedIntersectionLine
{
public:
    // Index of this vertical intersection line.
    size_t                              idx;
    // x position of this vertical intersection line.
    coord_t                             pos;
    // List of intersection points with polygons, sorted increasingly by the y axis.
    std::vector<SegmentIntersection>    intersections;
};

// A container maintaining an expolygon with its inner offsetted polygon.
// The purpose of the inner offsetted polygon is to provide segments to connect the infill lines.
struct ExPolygonWithOffset
{
public:
    ExPolygonWithOffset(
        const ExPolygon &expolygon,
        float   angle,
        coord_t aoffset1,
        coord_t aoffset2)
    {
        // Copy and rotate the source polygons.
        polygons_src = (Polygons)expolygon;
        for (Polygons::iterator it = polygons_src.begin(); it != polygons_src.end(); ++ it)
            it->rotate(angle);

        double mitterLimit = 3.;
        // for the infill pattern, don't cut the corners.
        // default miterLimt = 3
        //double mitterLimit = 10.;
        myassert(aoffset1 < 0);
        myassert(aoffset2 < 0);
        myassert(aoffset2 < aoffset1);
        polygons_outer = offset(polygons_src, aoffset1,
            CLIPPER_OFFSET_SCALE,
            ClipperLib::jtMiter,
            mitterLimit);
        polygons_inner = offset(polygons_src, aoffset2,
            CLIPPER_OFFSET_SCALE,
            ClipperLib::jtMiter,
            mitterLimit);
        n_contours_outer = polygons_outer.size();
        n_contours_inner = polygons_inner.size();
        n_contours = n_contours_outer + n_contours_inner;
        polygons_ccw.assign(n_contours, false);
        for (size_t i = 0; i < n_contours; ++ i) {
            contour(i).remove_duplicate_points();
            myassert(! contour(i).has_duplicate_points());
            polygons_ccw[i] = is_ccw(contour(i));
        }
    }

    // Any contour with offset1
    bool             is_contour_outer(size_t idx) const { return idx < n_contours_outer; }
    // Any contour with offset2
    bool             is_contour_inner(size_t idx) const { return idx >= n_contours_outer; }

    const Polygon&   contour(size_t idx) const 
        { return is_contour_outer(idx) ? polygons_outer[idx] : polygons_inner[idx - n_contours_outer]; }

    Polygon&         contour(size_t idx)
        { return is_contour_outer(idx) ? polygons_outer[idx] : polygons_inner[idx - n_contours_outer]; }

    bool             is_contour_ccw(size_t idx) const { return polygons_ccw[idx]; }

    BoundingBox      bounding_box_src() const 
        { return _bounding_box_polygons(polygons_src); }
    BoundingBox      bounding_box_outer() const 
        { return _bounding_box_polygons(polygons_outer); }
    BoundingBox      bounding_box_inner() const 
        { return _bounding_box_polygons(polygons_inner); }

    Polygons         polygons_src;
    Polygons         polygons_outer;
    Polygons         polygons_inner;

    size_t           n_contours_outer;
    size_t           n_contours_inner;
    size_t           n_contours;

protected:
    // For each polygon of polygons_inner, remember its orientation.
    std::vector<unsigned char> polygons_ccw;

    static BoundingBox _bounding_box_polygons(const Polygons &poly) {
        BoundingBox bbox;
        if (! poly.empty()) {
            bbox = poly.front().bounding_box();
            for (size_t i = 1; i < poly.size(); ++ i)
                bbox.merge(poly[i]);
        }
        return bbox;
    }
};

static inline int distance_of_segmens(const Polygon &poly, size_t seg1, size_t seg2, bool forward)
{
    int d = int(seg2) - int(seg1);
    if (! forward)
        d = - d;
    if (d < 0)
        d += int(poly.points.size());
    return d;
}

// For a vertical line, an inner contour and an intersection point,
// find an intersection point on the previous resp. next vertical line.
// The intersection point is connected with the prev resp. next intersection point with iInnerContour.
// Return -1 if there is no such point on the previous resp. next vertical line.
static inline int intersection_on_prev_next_vertical_line(
    const ExPolygonWithOffset                     &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iInnerContour,
    size_t                                         iIntersection,
    bool                                           dir_is_next)
{
    size_t iVerticalLineOther = iVerticalLine;
    if (dir_is_next) {
        if (++ iVerticalLineOther == segs.size())
            // No successive vertical line.
            return -1;
    } else if (iVerticalLineOther -- == 0) {
        // No preceding vertical line.
        return -1;
    }

    const SegmentedIntersectionLine &il    = segs[iVerticalLine];
    const SegmentIntersection       &itsct = il.intersections[iIntersection];
    const SegmentedIntersectionLine &il2   = segs[iVerticalLineOther];
    const Polygon                   &poly  = poly_with_offset.contour(iInnerContour);
//    const bool                       ccw   = poly_with_offset.is_contour_ccw(iInnerContour);
    const bool                       forward = itsct.is_low() == dir_is_next;
    // Resulting index of an intersection point on il2.
    int                              out   = -1;
    // Find an intersection point on iVerticalLineOther, intersecting iInnerContour
    // at the same orientation as iIntersection, and being closest to iIntersection
    // in the number of contour segments, when following the direction of the contour.
    int                              dmin  = std::numeric_limits<int>::max();
    for (size_t i = 0; i < il2.intersections.size(); ++ i) {
        const SegmentIntersection &itsct2 = il2.intersections[i];
        if (itsct.iContour == itsct2.iContour && itsct.type == itsct2.type) {
            /*
            if (itsct.is_low()) {
                myassert(itsct.type == SegmentIntersection::INNER_LOW);
                myassert(iIntersection > 0);
                myassert(il.intersections[iIntersection-1].type == SegmentIntersection::OUTER_LOW);                
                myassert(i > 0);
                if (il2.intersections[i-1].is_inner())
                    // Take only the lowest inner intersection point.
                    continue;
                myassert(il2.intersections[i-1].type == SegmentIntersection::OUTER_LOW);
            } else {
                myassert(itsct.type == SegmentIntersection::INNER_HIGH);
                myassert(iIntersection+1 < il.intersections.size());
                myassert(il.intersections[iIntersection+1].type == SegmentIntersection::OUTER_HIGH);
                myassert(i+1 < il2.intersections.size());
                if (il2.intersections[i+1].is_inner())
                    // Take only the highest inner intersection point.
                    continue;
                myassert(il2.intersections[i+1].type == SegmentIntersection::OUTER_HIGH);
            }
            */
            // The intersection points lie on the same contour and have the same orientation.
            // Find the intersection point with a shortest path in the direction of the contour.
            int d = distance_of_segmens(poly, itsct.iSegment, itsct2.iSegment, forward);
            if (d < dmin) {
                out = i;
                dmin = d;
            }
        }
    }
    //FIXME this routine is not asymptotic optimal, it will be slow if there are many intersection points along the line.
    return out;
}

static inline int intersection_on_prev_vertical_line(
    const ExPolygonWithOffset                     &poly_with_offset, 
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iInnerContour,
    size_t                                         iIntersection)
{
    return intersection_on_prev_next_vertical_line(poly_with_offset, segs, iVerticalLine, iInnerContour, iIntersection, false);
}

static inline int intersection_on_next_vertical_line(
    const ExPolygonWithOffset                     &poly_with_offset, 
    const std::vector<SegmentedIntersectionLine>  &segs, 
    size_t                                         iVerticalLine, 
    size_t                                         iInnerContour, 
    size_t                                         iIntersection)
{
    return intersection_on_prev_next_vertical_line(poly_with_offset, segs, iVerticalLine, iInnerContour, iIntersection, true);
}

// Find an intersection on a previous line, but return -1, if the connecting segment of a perimeter was already extruded.
static inline int intersection_unused_on_prev_next_vertical_line(
    const ExPolygonWithOffset                     &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iInnerContour,
    size_t                                         iIntersection,
    bool                                           dir_is_next)
{
    //FIXME This routine will propose a connecting line even if the connecting perimeter segment intersects 
    // iVertical line multiple times before reaching iIntersectionOther.
    int iIntersectionOther = intersection_on_prev_next_vertical_line(poly_with_offset, segs, iVerticalLine, iInnerContour, iIntersection, dir_is_next);
    if (iIntersectionOther == -1)
        return -1;
    myassert(dir_is_next ? (iVerticalLine + 1 < segs.size()) : (iVerticalLine > 0));
    const SegmentedIntersectionLine &il_this      = segs[iVerticalLine];
    const SegmentIntersection       &itsct_this   = il_this.intersections[iIntersection];
    const SegmentedIntersectionLine &il_other     = segs[dir_is_next ? (iVerticalLine+1) : (iVerticalLine-1)];
    const SegmentIntersection       &itsct_other  = il_other.intersections[iIntersectionOther];
    myassert(itsct_other.is_inner());
    myassert(iIntersectionOther > 0);
    myassert(iIntersectionOther + 1 < il_other.intersections.size());
    // Is iIntersectionOther at the boundary of a vertical segment?
    const SegmentIntersection       &itsct_other2 = il_other.intersections[itsct_other.is_low() ? iIntersectionOther - 1 : iIntersectionOther + 1];
    if (itsct_other2.is_inner())
        // Cannot follow a perimeter segment into the middle of another vertical segment.
        // Only perimeter segments connecting to the end of a vertical segment are followed.
        return -1;
    myassert(itsct_other.is_low() == itsct_other2.is_low());
    if (dir_is_next ? itsct_this.consumed_perimeter_right : itsct_other.consumed_perimeter_right)
        // This perimeter segment was already consumed.
        return -1;
    if (itsct_other.is_low() ? itsct_other.consumed_vertical_up : il_other.intersections[iIntersectionOther-1].consumed_vertical_up)
        // This vertical segment was already consumed.
        return -1;
    return iIntersectionOther;
}

static inline int intersection_unused_on_prev_vertical_line(
    const ExPolygonWithOffset                     &poly_with_offset, 
    const std::vector<SegmentedIntersectionLine>  &segs, 
    size_t                                         iVerticalLine, 
    size_t                                         iInnerContour, 
    size_t                                         iIntersection)
{
    return intersection_unused_on_prev_next_vertical_line(poly_with_offset, segs, iVerticalLine, iInnerContour, iIntersection, false);
}

static inline int intersection_unused_on_next_vertical_line(
    const ExPolygonWithOffset                     &poly_with_offset, 
    const std::vector<SegmentedIntersectionLine>  &segs, 
    size_t                                         iVerticalLine, 
    size_t                                         iInnerContour, 
    size_t                                         iIntersection)
{
    return intersection_unused_on_prev_next_vertical_line(poly_with_offset, segs, iVerticalLine, iInnerContour, iIntersection, true);
}

// Measure an Euclidian length of a perimeter segment when going from iIntersection to iIntersection2.
static inline coordf_t measure_perimeter_prev_next_segment_length(
    const ExPolygonWithOffset                     &poly_with_offset, 
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iInnerContour,
    size_t                                         iIntersection,
    size_t                                         iIntersection2,
    bool                                           dir_is_next)
{
    size_t iVerticalLineOther = iVerticalLine;
    if (dir_is_next) {
        if (++ iVerticalLineOther == segs.size())
            // No successive vertical line.
            return coordf_t(-1);
    } else if (iVerticalLineOther -- == 0) {
        // No preceding vertical line.
        return coordf_t(-1);
    }

    const SegmentedIntersectionLine &il     = segs[iVerticalLine];
    const SegmentIntersection       &itsct  = il.intersections[iIntersection];
    const SegmentedIntersectionLine &il2    = segs[iVerticalLineOther];
    const SegmentIntersection       &itsct2 = il2.intersections[iIntersection2];
    const Polygon                   &poly   = poly_with_offset.contour(iInnerContour);
//    const bool                       ccw    = poly_with_offset.is_contour_ccw(iInnerContour);
    myassert(itsct.type == itsct2.type);
    myassert(itsct.iContour == itsct2.iContour);
    myassert(itsct.is_inner());
    const bool                       forward = itsct.is_low() == dir_is_next;

    Point p1(il.pos, itsct.pos);
    Point p2(il2.pos, itsct2.pos);
    return forward ?
        segment_length(poly, itsct .iSegment, p1, itsct2.iSegment, p2) :
        segment_length(poly, itsct2.iSegment, p2, itsct .iSegment, p1);
}

static inline coordf_t measure_perimeter_prev_segment_length(
    const ExPolygonWithOffset                     &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iInnerContour,
    size_t                                         iIntersection,
    size_t                                         iIntersection2)
{
    return measure_perimeter_prev_next_segment_length(poly_with_offset, segs, iVerticalLine, iInnerContour, iIntersection, iIntersection2, false);
}

static inline coordf_t measure_perimeter_next_segment_length(
    const ExPolygonWithOffset                     &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iInnerContour,
    size_t                                         iIntersection,
    size_t                                         iIntersection2)
{
    return measure_perimeter_prev_next_segment_length(poly_with_offset, segs, iVerticalLine, iInnerContour, iIntersection, iIntersection2, true);
}

// Append the points of a perimeter segment when going from iIntersection to iIntersection2.
// The first point (the point of iIntersection) will not be inserted,
// the last point will be inserted.
static inline void emit_perimeter_prev_next_segment(
    const ExPolygonWithOffset                     &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iInnerContour,
    size_t                                         iIntersection,
    size_t                                         iIntersection2,
    Polyline                                      &out,
    bool                                           dir_is_next)
{
    size_t iVerticalLineOther = iVerticalLine;
    if (dir_is_next) {
        ++ iVerticalLineOther;
        myassert(iVerticalLineOther < segs.size());
    } else {
        myassert(iVerticalLineOther > 0);
        -- iVerticalLineOther;
    }

    const SegmentedIntersectionLine &il     = segs[iVerticalLine];
    const SegmentIntersection       &itsct  = il.intersections[iIntersection];
    const SegmentedIntersectionLine &il2    = segs[iVerticalLineOther];
    const SegmentIntersection       &itsct2 = il2.intersections[iIntersection2];
    const Polygon                   &poly   = poly_with_offset.contour(iInnerContour);
//    const bool                       ccw    = poly_with_offset.is_contour_ccw(iInnerContour);
    myassert(itsct.type == itsct2.type);
    myassert(itsct.iContour == itsct2.iContour);
    myassert(itsct.is_inner());
    const bool                       forward = itsct.is_low() == dir_is_next;
    // Do not append the first point.
    // out.points.push_back(Point(il.pos, itsct.pos));
    if (forward)
        polygon_segment_append(out.points, poly, itsct.iSegment, itsct2.iSegment);
    else
        polygon_segment_append_reversed(out.points, poly, itsct.iSegment, itsct2.iSegment);
    // Append the last point.
    out.points.push_back(Point(il2.pos, itsct2.pos));
}

void FillRectilinear2::fill_surface_by_lines(const Surface *surface, const FillParams &params, float angleBase, Polylines &polylines_out)
{
    // At the end, only the new polylines will be rotated back.
    size_t n_polylines_out_initial = polylines_out.size();

    // Shrink the input polygon a bit first to not push the infill lines out of the perimeters.
//    const float INFILL_OVERLAP_OVER_SPACING = 0.3f;
    const float INFILL_OVERLAP_OVER_SPACING = 0.45f;
    myassert(INFILL_OVERLAP_OVER_SPACING > 0 && INFILL_OVERLAP_OVER_SPACING < 0.5f);

    // Rotate polygons so that we can work with vertical lines here
    std::pair<float, Point> rotate_vector = this->_infill_direction(surface);
    rotate_vector.first += angleBase;

    this->_min_spacing = scale_(this->spacing);
    myassert(params.density > 0.0001f && params.density <= 1.f);
    this->_line_spacing = coord_t(coordf_t(this->_min_spacing) / params.density);
    this->_diagonal_distance = this->_line_spacing * 2;

    // On the polygons of poly_with_offset, the infill lines will be connected.
    ExPolygonWithOffset poly_with_offset(
        surface->expolygon, 
        - rotate_vector.first, 
        scale_(- (0.5 - INFILL_OVERLAP_OVER_SPACING) * this->spacing),
        scale_(- 0.5 * this->spacing));
    if (poly_with_offset.n_contours_inner == 0) {
        //FIXME maybe one shall trigger the gap fill here?
        return;
    }

    BoundingBox bounding_box = poly_with_offset.bounding_box_outer();

    // define flow spacing according to requested density
    bool full_infill = params.density > 0.9999f;
    if (full_infill && !params.dont_adjust) {
//        this->_min_spacing = this->_line_spacing = this->_adjust_solid_spacing(bounding_box.size().x, this->_line_spacing);
//        this->spacing = unscale(this->_line_spacing);
    } else {
        // extend bounding box so that our pattern will be aligned with other layers
        // Transform the reference point to the rotated coordinate system.
        bounding_box.merge(_align_to_grid(
            bounding_box.min, 
            Point(this->_line_spacing, this->_line_spacing), 
            rotate_vector.second.rotated(- rotate_vector.first)));
    }

    // Intersect a set of euqally spaced vertical lines wiht expolygon.
    size_t  n_vlines = (bounding_box.max.x - bounding_box.min.x + SCALED_EPSILON) / this->_line_spacing;
    coord_t x0 = bounding_box.min.x + this->_line_spacing;

#ifdef SLIC3R_DEBUG
    char path[2048];
    static int iRun = 0;
    sprintf(path, "out/FillRectilinear2-%d.svg", iRun);
    BoundingBox bbox_svg = poly_with_offset.bounding_box_outer();
    ::Slic3r::SVG svg(path, bbox_svg); // , scale_(1.));
    for (size_t i = 0; i < poly_with_offset.polygons_src.size(); ++ i)
        svg.draw(poly_with_offset.polygons_src[i].lines());
    for (size_t i = 0; i < poly_with_offset.polygons_outer.size(); ++ i)
        svg.draw(poly_with_offset.polygons_outer[i].lines(), "green");
    for (size_t i = 0; i < poly_with_offset.polygons_inner.size(); ++ i)
        svg.draw(poly_with_offset.polygons_inner[i].lines(), "brown");
    {
        char path2[2048];
        sprintf(path2, "out/FillRectilinear2-initial-%d.svg", iRun);
        ::Slic3r::SVG svg(path2, bbox_svg); // , scale_(1.));
        for (size_t i = 0; i < poly_with_offset.polygons_src.size(); ++ i)
            svg.draw(poly_with_offset.polygons_src[i].lines());
        for (size_t i = 0; i < poly_with_offset.polygons_outer.size(); ++ i)
            svg.draw(poly_with_offset.polygons_outer[i].lines(), "green");
            for (size_t i = 0; i < poly_with_offset.polygons_inner.size(); ++ i)
            svg.draw(poly_with_offset.polygons_inner[i].lines(), "brown");
    }
    iRun ++;
#endif /* SLIC3R_DEBUG */

    // For each contour
    // Allocate storage for the segments.
    std::vector<SegmentedIntersectionLine> segs(n_vlines, SegmentedIntersectionLine());
    for (size_t i = 0; i < n_vlines; ++ i) {
        segs[i].idx = i;
        segs[i].pos = x0 + i * this->_line_spacing;
    }
    for (size_t iContour = 0; iContour < poly_with_offset.n_contours; ++ iContour) {
        const Points &contour = poly_with_offset.contour(iContour).points;
        if (contour.size() < 2)
            continue;
        // For each segment
        for (size_t iSegment = 0; iSegment < contour.size(); ++ iSegment) {
            size_t iPrev = ((iSegment == 0) ? contour.size() : iSegment) - 1;
            const Point &p1 = contour[iPrev];
            const Point &p2 = contour[iSegment];
            // Which of the equally spaced vertical lines is intersected by this segment?
            coord_t l = p1.x;
            coord_t r = p2.x;
            if (l > r)
                std::swap(l, r);
            // il, ir are the left / right indices of vertical lines intersecting a segment
            int il = (l - x0) / this->_line_spacing;
            while (il * this->_line_spacing + x0 < l)
                ++ il;
            il = std::max(int(0), il);
            int ir = (r - x0 + this->_line_spacing) / this->_line_spacing;
            while (ir * this->_line_spacing + x0 > r)
                -- ir;
            ir = std::min(int(segs.size()) - 1, ir);
            if (il > ir)
                // No vertical line intersects this segment.
                continue;
            myassert(il >= 0 && il < segs.size());
            myassert(ir >= 0 && ir < segs.size());
            if (l == r) {
                // The segment is vertical.
                // Don't insert vertical segments at all.
                // If the contour is not degenerate, then this vertical line will be crossed
                // by the non-vertical segments preceding resp. succeeding this vertical segment.
                /*
                SegmentIntersection is;
                is.iContour = iContour;
                is.iSegment = iSegment;
                is.pos = p1.y;
                segs[il].intersections.push_back(is);
                is.pos = p2.y;
                segs[il].intersections.push_back(is);
                */
                continue;
            }
            for (int i = il; i <= ir; ++ i) {
                SegmentIntersection is;
                is.iContour = iContour;
                is.iSegment = iSegment;
                myassert(l <= segs[i].pos);
                myassert(r >= segs[i].pos);
                // Calculate the intersection position in y axis. x is known.
                double t = double(segs[i].pos - p1.x) / double(p2.x - p1.x);
                myassert(t > -0.000001 && t < 1.000001);
                t = clamp(0., 1., t);
                coord_t lo = p1.y;
                coord_t hi = p2.y;
                if (lo > hi)
                    std::swap(lo, hi);
                is.pos = p1.y + coord_t(t * double(p2.y - p1.y));
                myassert(is.pos > lo - 0.000001 && is.pos < hi + 0.000001);
                is.pos = clamp(lo, hi, is.pos);
                segs[i].intersections.push_back(is);
            }
        }
    }

    // Sort the intersections along their segments, specify the intersection types.
    for (size_t i_seg = 0; i_seg < segs.size(); ++ i_seg) {
        SegmentedIntersectionLine &sil = segs[i_seg];
        // Sort the intersection points. This needs to be verified, because the intersection points were calculated
        // using imprecise arithmetics.
        std::sort(sil.intersections.begin(), sil.intersections.end());
        // Verify the order, bubble sort the intersections until sorted.
        bool modified = false;
        do {
            modified = false;
            for (size_t i = 1; i < sil.intersections.size(); ++ i) {
                size_t iContour1 = sil.intersections[i-1].iContour;
                size_t iContour2 = sil.intersections[i].iContour;
                const Points &contour1 = poly_with_offset.contour(iContour1).points;
                const Points &contour2 = poly_with_offset.contour(iContour2).points;
                size_t iSegment1 = sil.intersections[i-1].iSegment;
                size_t iPrev1    = ((iSegment1 == 0) ? contour1.size() : iSegment1) - 1;
                size_t iSegment2 = sil.intersections[i].iSegment;
                size_t iPrev2    = ((iSegment2 == 0) ? contour2.size() : iSegment2) - 1;
                bool   swap = false;
                if (iContour1 == iContour2 && iSegment1 == iSegment2) {
                    // The same segment, it has to be vertical.
                    myassert(iPrev1 == iPrev2);
                    swap = contour1[iPrev1].y > contour1[iContour1].y;
                    #ifdef SLIC3R_DEBUG
                    if (swap)
                        printf("Swapping when single vertical segment\n");
                    #endif
                } else {
                    // Segments are in a general position. Here an exact airthmetics may come into play.
                    coord_t y1max = std::max(contour1[iPrev1].y, contour1[iSegment1].y);
                    coord_t y2min = std::min(contour2[iPrev2].y, contour2[iSegment2].y);
                    if (y1max < y2min) {
                        // The segments are separated, nothing to do.
                    } else {
                        // Use an exact predicate to verify, that segment1 is below segment2.
                        const Point *a = &contour1[iPrev1];
                        const Point *b = &contour1[iSegment1];
                        const Point *c = &contour2[iPrev2];
                        const Point *d = &contour2[iSegment2];
#ifdef SLIC3R_DEBUG
                        const Point  x1(sil.pos, sil.intersections[i-1].pos);
                        const Point  x2(sil.pos, sil.intersections[i  ].pos);
                        bool successive = false;
#endif /* SLIC3R_DEBUG */
                        // Sort the points in the two segments by x.
                        if (a->x > b->x)
                        	std::swap(a, b);
                        if (c->x > d->x)
                            std::swap(c, d);
                        myassert(a->x <= sil.pos);
                        myassert(c->x <= sil.pos);
                        myassert(b->x >= sil.pos);
                        myassert(d->x >= sil.pos);
                        // Sort the two segments, so the segment <a,b> will be on the left of <c,d>.
                        bool upper_more_left = false;
                        if (a->x > c->x) {
                            upper_more_left = true;
                            std::swap(a, c);
                            std::swap(b, d);
                        }
                        if (a == c) {
                        	// The segments iSegment1 and iSegment2 are directly connected.
                            myassert(iContour1 == iContour2);
                            myassert(iSegment1 == iPrev2 || iPrev1 == iSegment2);
                            std::swap(c, d);
                            myassert(a != c && b != c);
#ifdef SLIC3R_DEBUG
                            successive = true;
#endif /* SLIC3R_DEBUG */
                        }
#ifdef SLIC3R_DEBUG
                        else if (b == d) {
                            // The segments iSegment1 and iSegment2 are directly connected.
                            myassert(iContour1 == iContour2);
                            myassert(iSegment1 == iPrev2 || iPrev1 == iSegment2);
                            myassert(a != c && b != c);
                            successive = true;
                        }
#endif /* SLIC3R_DEBUG */
                        Orientation o = orient(*a, *b, *c);
                        myassert(o != ORIENTATION_COLINEAR);
                        swap = upper_more_left != (o == ORIENTATION_CW);
#ifdef SLIC3R_DEBUG
                        if (swap)
                            printf(successive ? 
                                "Swapping when iContour1 == iContour2 and successive segments\n" :
                                "Swapping when exact predicate\n");
#endif
                    }
                }
                if (swap) {
                    // Swap the intersection points, but keep the original positions, so they stay sorted by the y axis.
                    std::swap(sil.intersections[i-1], sil.intersections[i]);
                    std::swap(sil.intersections[i-1].pos, sil.intersections[i].pos);
                    modified = true;
                }
            }
        } while (modified);
        // Assign the intersection types.
        size_t j = 0;
        for (size_t i = 0; i < sil.intersections.size(); ++ i) {
            // What is the orientation of the segment at the intersection point?
            size_t iContour = sil.intersections[i].iContour;
            const Points &contour = poly_with_offset.contour(iContour).points;
            size_t iSegment = sil.intersections[i].iSegment;
            size_t iPrev    = ((iSegment == 0) ? contour.size() : iSegment) - 1;
            coord_t dir = contour[iSegment].x - contour[iPrev].x;
            // bool ccw = poly_with_offset.is_contour_ccw(iContour);
            // bool low = (dir > 0) == ccw;
            bool low = dir > 0;
            sil.intersections[i].type = poly_with_offset.is_contour_outer(iContour) ? 
                (low ? SegmentIntersection::OUTER_LOW : SegmentIntersection::OUTER_HIGH) :
                (low ? SegmentIntersection::INNER_LOW : SegmentIntersection::INNER_HIGH);
            if (j > 0 &&
           		sil.intersections[i].pos      == sil.intersections[j-1].pos &&
                sil.intersections[i].type     == sil.intersections[j-1].type &&
                sil.intersections[i].iContour == sil.intersections[j-1].iContour) {
                // This has to be a corner point crossing the vertical line.
                // Remove the second intersection point.
#ifdef SLIC3R_DEBUG
                size_t iSegment2 = sil.intersections[j-1].iSegment;
                size_t iPrev2    = ((iSegment2 == 0) ? contour.size() : iSegment2) - 1;
                myassert(iSegment == iPrev2 || iSegment2 == iPrev);
#endif /* SLIC3R_DEBUG */
            } else {
                if (j < i)
                    sil.intersections[j] = sil.intersections[i];
                ++ j;
            }
        }
        // Shrink the list of intersections, if any of the intersection was removed during the classification.
        if (j < sil.intersections.size())
            sil.intersections.erase(sil.intersections.begin() + j, sil.intersections.end());
    }

#ifdef SLIC3R_DEBUG
    // Verify the segments & paint them.
    for (size_t i_seg = 0; i_seg < segs.size(); ++ i_seg) {
        SegmentedIntersectionLine &sil = segs[i_seg];
        // The intersection points have to be even.
        myassert((sil.intersections.size() & 1) == 0);
        for (size_t i = 0; i < sil.intersections.size();) {
            // An intersection segment crossing the bigger contour may cross the inner offsetted contour even number of times.
            myassert(sil.intersections[i].type == SegmentIntersection::OUTER_LOW);
            size_t j = i + 1;
            myassert(j < sil.intersections.size());
            myassert(sil.intersections[j].type == SegmentIntersection::INNER_LOW || sil.intersections[j].type == SegmentIntersection::OUTER_HIGH);
            for (; j < sil.intersections.size() && sil.intersections[j].is_inner(); ++ j) ;
            myassert(j < sil.intersections.size());
            myassert((j & 1) == 1);
            myassert(sil.intersections[j].type == SegmentIntersection::OUTER_HIGH);
            myassert(i + 1 == j || sil.intersections[j - 1].type == SegmentIntersection::INNER_HIGH);
            if (i + 1 == j) {
                svg.draw(Line(Point(sil.pos, sil.intersections[i].pos), Point(sil.pos, sil.intersections[j].pos)), "blue");
            } else {
                svg.draw(Line(Point(sil.pos, sil.intersections[i].pos), Point(sil.pos, sil.intersections[i+1].pos)), "green");
                svg.draw(Line(Point(sil.pos, sil.intersections[i+1].pos), Point(sil.pos, sil.intersections[j-1].pos)), (j - i + 1 > 4) ? "yellow" : "magenta");
                svg.draw(Line(Point(sil.pos, sil.intersections[j-1].pos), Point(sil.pos, sil.intersections[j].pos)), "green");
            }
            i = j + 1;
        }
    }
    svg.Close();

    // Verify the segments & paint them.
    for (size_t i_seg = 0; i_seg < segs.size(); ++ i_seg) {
        SegmentedIntersectionLine &sil = segs[i_seg];
        // The intersection points have to be even.
        myassert((sil.intersections.size() & 1) == 0);
        for (size_t i = 0; i < sil.intersections.size();) {
            // An intersection segment crossing the bigger contour may cross the inner offsetted contour even number of times.
            myassert(sil.intersections[i].type == SegmentIntersection::OUTER_LOW);
            size_t j = i + 1;
            myassert(j < sil.intersections.size());
            myassert(sil.intersections[j].type == SegmentIntersection::INNER_LOW || sil.intersections[j].type == SegmentIntersection::OUTER_HIGH);
            for (; j < sil.intersections.size() && sil.intersections[j].is_inner(); ++ j) ;
            myassert(j < sil.intersections.size());
            myassert((j & 1) == 1);
            myassert(sil.intersections[j].type == SegmentIntersection::OUTER_HIGH);
            myassert(i + 1 == j || sil.intersections[j - 1].type == SegmentIntersection::INNER_HIGH);
            i = j + 1;
        }
    }
#endif /* SLIC3R_DEBUG */

    // Now construct a graph.
    // Find the first point.
    //FIXME ideally one would plan the initial point to be closest to the current print head position.
    size_t    i_vline = 0;
    size_t    i_intersection = size_t(-1);
    // Follow the line, connect the lines into a graph.
    // Until no new line could be added to the output path:
    Point     pointLast;
    Polyline *polyline_current = NULL;
    if (! polylines_out.empty())
        pointLast = polylines_out.back().points.back();
    for (;;) {
        if (i_intersection == size_t(-1)) {
            // The path has been interrupted. Find a next starting point, closest to the previous extruder position.
            coordf_t dist2min = std::numeric_limits<coordf_t>().max();
            for (size_t i_vline2 = 0; i_vline2 < segs.size(); ++ i_vline2) {
                const SegmentedIntersectionLine &seg = segs[i_vline2];
                if (! seg.intersections.empty()) {
                    myassert(seg.intersections.size() > 1);
                    // Even number of intersections with the loops.
                    myassert((seg.intersections.size() & 1) == 0);
                    myassert(seg.intersections.front().type == SegmentIntersection::OUTER_LOW);
                    for (size_t i = 0; i < seg.intersections.size(); ++ i) {
                        const SegmentIntersection &intrsctn = seg.intersections[i];
                        if (intrsctn.is_outer()) {
                            myassert(intrsctn.is_low() || i > 0);
                            bool consumed = intrsctn.is_low() ? 
                                intrsctn.consumed_vertical_up : 
                                seg.intersections[i-1].consumed_vertical_up;
                            if (! consumed) {
                                coordf_t dist2 = sqr(coordf_t(pointLast.x - seg.pos)) + sqr(coordf_t(pointLast.y - intrsctn.pos));
                                if (dist2 < dist2min) {
                                    dist2min = dist2;
                                    i_vline = i_vline2;
                                    i_intersection = i;
                                    if (polylines_out.empty()) {
                                        // Initial state, take the first line, which is the first from the left.
                                        goto found;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (i_intersection == size_t(-1))
                // We are finished.
                break;
        found:
            // Start a new path.
            polylines_out.push_back(Polyline());
            polyline_current = &polylines_out.back();
            // Emit the first point of a path.
            pointLast = Point(segs[i_vline].pos, segs[i_vline].intersections[i_intersection].pos);
            polyline_current->points.push_back(pointLast);
        }

        // From the initial point (i_vline, i_intersection), follow a path.
        SegmentedIntersectionLine &seg      = segs[i_vline];
        SegmentIntersection       *intrsctn = &seg.intersections[i_intersection];
        bool going_up = intrsctn->is_low();
        bool try_connect = false;
        if (going_up) {
            myassert(! intrsctn->consumed_vertical_up);
            myassert(i_intersection + 1 < seg.intersections.size());
            // Step back to the beginning of the vertical segment to mark it as consumed.
            if (intrsctn->is_inner()) {
                myassert(i_intersection > 0);
                -- intrsctn;
                -- i_intersection;
            }
            // Consume the complete vertical segment up to the outer contour.
            do {
                intrsctn->consumed_vertical_up = true;
                ++ intrsctn;
                ++ i_intersection;
                myassert(i_intersection < seg.intersections.size());
            } while (intrsctn->type != SegmentIntersection::OUTER_HIGH);
            if ((intrsctn - 1)->is_inner()) {
                // Step back.
                -- intrsctn;
                -- i_intersection;
                myassert(intrsctn->type == SegmentIntersection::INNER_HIGH);
                try_connect = true;
            }
        } else {
            // Going down.
            myassert(intrsctn->is_high());
            myassert(i_intersection > 0);
            myassert(! (intrsctn - 1)->consumed_vertical_up);
            // Consume the complete vertical segment up to the outer contour.
            if (intrsctn->is_inner())
                intrsctn->consumed_vertical_up = true;
            do {
                myassert(i_intersection > 0);
                -- intrsctn;
                -- i_intersection;
                intrsctn->consumed_vertical_up = true;
            } while (intrsctn->type != SegmentIntersection::OUTER_LOW);
            if ((intrsctn + 1)->is_inner()) {
                // Step back.
                ++ intrsctn;
                ++ i_intersection;
                myassert(intrsctn->type == SegmentIntersection::INNER_LOW);
                try_connect = true;
            }
        }
        if (try_connect) {
            // Decide, whether to finish the segment, or whether to follow the perimeter.
            int iPrev = intersection_unused_on_prev_vertical_line(poly_with_offset, segs, i_vline, intrsctn->iContour, i_intersection);
            int iNext = intersection_unused_on_next_vertical_line(poly_with_offset, segs, i_vline, intrsctn->iContour, i_intersection);
            if (iPrev != -1 || iNext != -1) {
                // Does the perimeter intersect the current vertical line?
                SegmentIntersection::SegmentIntersectionType type_crossing = (intrsctn->type == SegmentIntersection::INNER_LOW) ?
                    SegmentIntersection::INNER_HIGH : SegmentIntersection::INNER_LOW;
                // Does the perimeter intersect the current vertical line above intrsctn?
                int iSegAbove = -1;
                for (size_t i = i_intersection + 1; i + 1 < seg.intersections.size(); ++ i)
                    if (seg.intersections[i].iContour == intrsctn->iContour &&
                        seg.intersections[i].type == type_crossing) {
                        iSegAbove = seg.intersections[i].iSegment;
                        break;
                    }
                // Does the perimeter intersect the current vertical line below intrsctn?
                int iSegBelow = -1;
                for (size_t i = i_intersection - 1; i > 0; -- i)
                    if (seg.intersections[i].iContour == intrsctn->iContour &&
                        seg.intersections[i].type == type_crossing) {
                        iSegBelow = seg.intersections[i].iSegment;
                        break;
                    }
                if (iSegBelow != -1 || iSegAbove != -1) {
                    // Invalidate iPrev resp. iNext, if the perimeter crosses the current vertical line earlier than iPrev resp. iNext.
                    // The perimeter contour orientation.
                    const bool forward = intrsctn->is_low(); // == poly_with_offset.is_contour_ccw(intrsctn->iContour);
                    const Polygon &poly = poly_with_offset.contour(intrsctn->iContour);
                    if (iPrev != -1) {
                        int d1 = distance_of_segmens(poly, segs[i_vline-1].intersections[iPrev].iSegment, intrsctn->iSegment, forward);
                        int d2 = (iSegBelow == -1) ? std::numeric_limits<int>::max() :
                            distance_of_segmens(poly, iSegBelow, intrsctn->iSegment, forward);
                        if (iSegAbove != -1)
                            d2 = std::min(d2, distance_of_segmens(poly, iSegAbove, intrsctn->iSegment, forward));
                        if (d2 < d1)
                            // The vertical crossing comes eralier than the prev crossing.
                            // Disable the perimeter going back.
                            iPrev = -1;
                    }
                    if (iNext != -1) {
                        int d1 = distance_of_segmens(poly, intrsctn->iSegment, segs[i_vline+1].intersections[iNext].iSegment, forward);
                        int d2 = (iSegBelow == -1) ? std::numeric_limits<int>::max() :
                            distance_of_segmens(poly, intrsctn->iSegment, iSegBelow, forward);
                        if (iSegAbove != -1)
                            d2 = std::min(d2, distance_of_segmens(poly, intrsctn->iSegment, iSegAbove, forward));
                        if (d2 < d1)
                            // The vertical crossing comes eralier than the prev crossing.
                            // Disable the perimeter going forward.
                            iNext = -1;
                    }
                }
                if (iPrev != -1 || iNext != -1) {
                    // Zig zag
                    coord_t distPrev = (iPrev == -1) ? std::numeric_limits<coord_t>::max() :
                        measure_perimeter_prev_segment_length(poly_with_offset, segs, i_vline, intrsctn->iContour, i_intersection, iPrev);
                    coord_t distNext = (iNext == -1) ? std::numeric_limits<coord_t>::max() :
                        measure_perimeter_next_segment_length(poly_with_offset, segs, i_vline, intrsctn->iContour, i_intersection, iNext);
                    // Take the shorter path.
                    bool take_next = (iPrev != -1 && iNext != -1) ? (distNext < distPrev) : iNext != -1;
                    myassert(intrsctn->is_inner());
                    pointLast = Point(seg.pos, intrsctn->pos);
                    polyline_current->points.push_back(pointLast);
                    emit_perimeter_prev_next_segment(poly_with_offset, segs, i_vline, intrsctn->iContour, i_intersection, take_next ? iNext : iPrev, *polyline_current, take_next);
                    // Mark both the left and right connecting segment as consumed, because one cannot go to this intersection point as it has been consumed.
                    if (iPrev != -1)
                        segs[i_vline-1].intersections[iPrev].consumed_perimeter_right = true;
                    if (iNext != -1)
                        intrsctn->consumed_perimeter_right = true;
                    //FIXME consume the left / right connecting segments at the other end of this line? Currently it is not critical because a perimeter segment is not followed if the vertical segment at the other side has already been consumed.
                    // Advance to the neighbor line.
                    if (take_next) {
                        ++ i_vline;
                        i_intersection = iNext;
                    } else {
                        -- i_vline;
                        i_intersection = iPrev;
                    }
                    continue;
                }
            }
            // Take the complete line up to the outer contour.
            if (going_up)
                ++ intrsctn;
            else
                -- intrsctn;
        }
        // Finish the current vertical line,
        // reset the current vertical line to pick a new starting point in the next round.
        myassert(intrsctn->is_outer());
        myassert(intrsctn->is_high() == going_up);
        pointLast = Point(seg.pos, intrsctn->pos);
        polyline_current->points.push_back(pointLast);
        // Handle duplicate points and zero length segments.
        polyline_current->remove_duplicate_points();
        myassert(! polyline_current->has_duplicate_points());
        // Handle nearly zero length edges.
        if (polyline_current->points.size() <= 1 ||
        	(polyline_current->points.size() == 2 &&
        		std::abs(polyline_current->points.front().x - polyline_current->points.back().x) < SCALED_EPSILON &&
				std::abs(polyline_current->points.front().y - polyline_current->points.back().y) < SCALED_EPSILON))
            polylines_out.pop_back();
        intrsctn = NULL;
        i_intersection = -1;
        polyline_current = NULL;
    }

#ifdef SLIC3R_DEBUG
    {
        sprintf(path, "out/FillRectilinear2-final-%d.svg", iRun);
        ::Slic3r::SVG svg(path, bbox_svg); // , scale_(1.));
        for (size_t i = 0; i < poly_with_offset.polygons_src.size(); ++ i)
            svg.draw(poly_with_offset.polygons_src[i].lines());
        for (size_t i = 0; i < poly_with_offset.polygons_outer.size(); ++ i)
            svg.draw(poly_with_offset.polygons_outer[i].lines(), "green");
             for (size_t i = 0; i < poly_with_offset.polygons_inner.size(); ++ i)
            svg.draw(poly_with_offset.polygons_inner[i].lines(), "brown");
        for (size_t i = n_polylines_out_initial; i < polylines_out.size(); ++ i)
            svg.draw(polylines_out[i].lines(), "black");
    }
#endif /* SLIC3R_DEBUG */

    // paths must be rotated back
    for (Polylines::iterator it = polylines_out.begin() + n_polylines_out_initial; it != polylines_out.end(); ++ it) {
        // No need to translate, the absolute position is irrelevant.
        // it->translate(- rotate_vector.second.x, - rotate_vector.second.y);
        myassert(! it->has_duplicate_points());
        it->rotate(rotate_vector.first);
        //FIXME rather simplify the paths to avoid very short edges?
        //myassert(! it->has_duplicate_points());
        it->remove_duplicate_points();
    }

#ifdef SLIC3R_DEBUG
    // Verify, that there are no duplicate points in the sequence.
    for (Polylines::iterator it = polylines_out.begin(); it != polylines_out.end(); ++ it)
        myassert(! it->has_duplicate_points());
#endif /* SLIC3R_DEBUG */
}

Polylines FillRectilinear2::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;
    fill_surface_by_lines(surface, params, 0.f, polylines_out);
    return polylines_out;
}

Polylines FillGrid2::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;
    fill_surface_by_lines(surface, params, 0.f, polylines_out);
    fill_surface_by_lines(surface, params, float(M_PI / 2.), polylines_out);
    return polylines_out;
}

} // namespace Slic3r

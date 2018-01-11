////////////////////////////////////////////////////////////////////////////////
#include "MeshUtils.hpp"
#include <geogram/basic/geometry.h>
#include <geogram/mesh/mesh_preprocessing.h>
#include <geogram/mesh/mesh_topology.h>
#include <geogram/mesh/mesh_geometry.h>
#include <geogram/mesh/mesh_AABB.h>
////////////////////////////////////////////////////////////////////////////////

GEO::vec3 poly_fem::mesh_vertex(const GEO::Mesh &M, GEO::index_t v) {
	using GEO::index_t;
	GEO::vec3 p(0, 0, 0);
	for (index_t d = 0; d < std::min(3u, (index_t) M.vertices.dimension()); ++d) {
		if (M.vertices.double_precision()) {
			p[d] = M.vertices.point_ptr(v)[d];
		} else {
			p[d] = M.vertices.single_precision_point_ptr(v)[d];
		}
	}
	return p;
}

// -----------------------------------------------------------------------------

GEO::vec3 poly_fem::facet_barycenter(const GEO::Mesh &M, GEO::index_t f) {
	using GEO::index_t;
	GEO::vec3 p(0, 0, 0);
	for (index_t lv = 0; lv < M.facets.nb_vertices(f); ++lv) {
		p += poly_fem::mesh_vertex(M, M.facets.vertex(f, lv));
	}
	return p / M.facets.nb_vertices(f);
}

// -----------------------------------------------------------------------------

GEO::index_t poly_fem::mesh_create_vertex(GEO::Mesh &M, const GEO::vec3 &p) {
	using GEO::index_t;
	auto v = M.vertices.create_vertex();
	for (index_t d = 0; d < std::min(3u, (index_t) M.vertices.dimension()); ++d) {
		if (M.vertices.double_precision()) {
			M.vertices.point_ptr(v)[d] = p[d];
		} else {
			M.vertices.single_precision_point_ptr(v)[d] = (float) p[d];
		}
	}
	return v;
}

////////////////////////////////////////////////////////////////////////////////

void poly_fem::compute_element_tags(const GEO::Mesh &M, std::vector<ElementType> &element_tags) {
	using GEO::index_t;
	element_tags.resize(M.facets.nb());

	// Step 0: Compute boundary vertices as true boundary + vertices incident to a polygon
	std::vector<bool> is_boundary_or_interface_vertex(M.vertices.nb(), false);
	{
		GEO::Attribute<bool> boundary_vertices(M.vertices.attributes(), "boundary_vertex");
		for (index_t f = 0; f < M.facets.nb(); ++f) {
			if (M.facets.nb_vertices(f) != 4) {
				// Vertices incident to polygonal facets (triangles or > 4 vertices) are marked as boundary
				for (index_t lv = 0; lv < M.facets.nb_vertices(f); ++lv) {
					is_boundary_or_interface_vertex[M.facets.vertex(f, lv)] = true;
				}
			}
		}
		for (index_t v = 0; v < M.vertices.nb(); ++v) {
			if (boundary_vertices[v]) {
				is_boundary_or_interface_vertex[v] = true;
			}
		}
	}

	// Step 1: Determine which vertices are regular or not
	//
	// Interior vertices are regular if they are incident to exactly 4 quads
	// Boundary vertices are regular if they are incident to at most 2 quads, and no other facets
	std::vector<int> degree(M.vertices.nb(), 0);
	std::vector<bool> is_regular_vertex(M.vertices.nb());
	for (index_t f =  0; f < M.facets.nb(); ++f) {
		if (M.facets.nb_vertices(f) == 4) {
			// Only count incident quads for the degree
			for (index_t lv = 0; lv < M.facets.nb_vertices(f); ++lv) {
				index_t v = M.facets.vertex(f, lv);
				degree[v]++;
			}
		}
	}
	for (index_t v = 0; v < M.vertices.nb(); ++v) {
		// assert(degree[v] > 0); // We assume there are no isolated vertices here
		if (is_boundary_or_interface_vertex[v]) {
			is_regular_vertex[v] = (degree[v] <= 2);
		} else {
			is_regular_vertex[v] = (degree[v] == 4);
		}
	}

	// Step 2: Iterate over the facets and determine the type
	for (index_t f =  0; f < M.facets.nb(); ++f) {
		assert(M.facets.nb_vertices(f) > 2);
		if (M.facets.nb_vertices(f) == 4) {
			// Quad facet

			// a) Determine if it is on the mesh boundary
			bool is_boundary_facet = false;
			for (index_t lv = 0; lv < M.facets.nb_vertices(f); ++lv) {
				if (is_boundary_or_interface_vertex[M.facets.vertex(f, lv)]) {
					is_boundary_facet = true;
					break;
				}
			}

			// b) Determine if it is regular or not
			if (is_boundary_facet) {
				// A boundary quad is regular iff all its vertices are incident to at most 2 other quads
				// We assume that non-boundary vertices of a boundary quads are always regular
				bool is_singular = false;
				for (index_t lv = 0; lv < M.facets.nb_vertices(f); ++lv) {
					index_t v = M.facets.vertex(f, lv);
					if (is_boundary_or_interface_vertex[v]) {
						if (!is_regular_vertex[v]) {
							is_singular = true;
							break;
						}
					} else {
						if (!is_regular_vertex[v]) {
							element_tags[f] = ElementType::Undefined;
							break;
						}
					}
				}

				if (is_singular) {
					element_tags[f] = ElementType::SimpleSingularBoundaryCube;
				} else {
					element_tags[f] = ElementType::RegularBoundaryCube;
				}
			} else {
				// An interior quad is regular if all its vertices are singular
				int nb_singulars = 0;
				for (index_t lv = 0; lv < M.facets.nb_vertices(f); ++lv) {
					if (!is_regular_vertex[M.facets.vertex(f, lv)]) {
						++nb_singulars;
					}
				}

				if (nb_singulars == 0) {
					element_tags[f] = ElementType::RegularInteriorCube;
				} else if (nb_singulars == 1) {
					element_tags[f] = ElementType::SimpleSingularInteriorCube;
				} else {
					element_tags[f] = ElementType::MultiSingularInteriorCube;
				}
			}
		} else {
			// Polygonal facet

			// Note: In this function, we consider triangles as polygonal facets
			ElementType tag = ElementType::InteriorPolytope;
			GEO::Attribute<bool> boundary_vertices(M.vertices.attributes(), "boundary_vertex");
			for (index_t lv = 0; lv < M.facets.nb_vertices(f); ++lv) {
				if (boundary_vertices[M.facets.vertex(f, lv)]) {
					tag = ElementType::BoundaryPolytope;
					std::cout << "foo" << std::endl;
					break;
				}
			}

			element_tags[f] = tag;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////

namespace {

	// Signed area of a polygonal facet
	double signed_area(const GEO::Mesh& M, GEO::index_t f) {
		using namespace GEO;
		double result = 0;
		index_t v0 = M.facet_corners.vertex(M.facets.corners_begin(f));
		const vec3& p0 = Geom::mesh_vertex(M, v0);
		for(index_t c =
			M.facets.corners_begin(f) + 1; c + 1 < M.facets.corners_end(f); c++
		) {
			index_t v1 = M.facet_corners.vertex(c);
			const vec3& p1 = poly_fem::mesh_vertex(M, v1);
			index_t v2 = M.facet_corners.vertex(c + 1);
			const vec3& p2 = poly_fem::mesh_vertex(M, v2);
			result += Geom::triangle_signed_area(vec2(&p0[0]), vec2(&p1[0]), vec2(&p2[0]));
		}
		return result;
	}

} // anonymous namespace

void poly_fem::orient_normals_2d(GEO::Mesh &M) {
	using namespace GEO;
	vector<index_t> component;
	index_t nb_components = get_connected_components(M, component);
	vector<double> comp_signed_volume(nb_components, 0.0);
	for (index_t f = 0; f < M.facets.nb(); ++f) {
		comp_signed_volume[component[f]] += signed_area(M, f);
	}
	for (index_t f = 0; f < M.facets.nb(); ++f) {
		if (comp_signed_volume[component[f]] < 0.0) {
			M.facets.flip(f);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////

void poly_fem::reorder_mesh(Eigen::MatrixXd &V, Eigen::MatrixXi &F, const Eigen::VectorXi &C, Eigen::VectorXi &R) {
	assert(V.rows() == C.size());
	int num_colors = C.maxCoeff() + 1;
	Eigen::VectorXi count(num_colors);
	count.setZero();
	for (int i = 0; i < C.size(); ++i) {
		++count[C(i)];
	}
	R.resize(num_colors + 1);
	R(0) = 0;
	for (int c = 0; c < num_colors; ++c) {
		R(c+1) = R(c) + count(c);
	}
	count.setZero();
	Eigen::VectorXi remap(C.size());
	for (int i = 0; i < C.size(); ++i) {
		remap[i] = R(C(i)) + count[C(i)];
		++count[C(i)];
	}
	// Remap vertices
	Eigen::MatrixXd NV(V.rows(), V.cols());
	for (int v = 0; v < V.rows(); ++v) {
		NV.row(remap(v)) = V.row(v);
	}
	V = NV;
	// Remap face indices
	for (int f = 0; f < F.rows(); ++f) {
		for (int lv = 0; lv < F.cols(); ++lv) {
			F(f, lv) = remap(F(f, lv));
		}
	}
}

////////////////////////////////////////////////////////////////////////////////

namespace {

void compute_unsigned_distance_field(const GEO::Mesh &M,
	const GEO::MeshFacetsAABB &aabb_tree, const Eigen::MatrixXd &P, Eigen::VectorXd &D)
{
	assert(P.cols() == 3);
	D.resize(P.rows());
	#pragma omp parallel for
	for (int i = 0; i < P.rows(); ++i) {
		GEO::vec3 pos(P(i, 0), P(i, 1), P(i, 2));
		double sq_dist = aabb_tree.squared_distance(pos);
		D(i) = sq_dist;
	}
}

// calculate twice signed area of triangle (0,0)-(x1,y1)-(x2,y2)
// return an SOS-determined sign (-1, +1, or 0 only if it's a truly degenerate triangle)
int orientation(
	double x1, double y1, double x2, double y2, double &twice_signed_area)
{
	twice_signed_area=y1*x2-x1*y2;
	if(twice_signed_area>0) return 1;
	else if(twice_signed_area<0) return -1;
	else if(y2>y1) return 1;
	else if(y2<y1) return -1;
	else if(x1>x2) return 1;
	else if(x1<x2) return -1;
	else return 0; // only true when x1==x2 and y1==y2
}

// robust test of (x0,y0) in the triangle (x1,y1)-(x2,y2)-(x3,y3)
// if true is returned, the barycentric coordinates are set in a,b,c.
//
// Note: This function comes from SDFGen by Christopher Batty.
// https://github.com/christopherbatty/SDFGen/blob/master/makelevelset3.cpp
bool point_in_triangle_2d(
	double x0, double y0, double x1, double y1,
	double x2, double y2, double x3, double y3,
	double &a, double &b, double &c)
{
	x1-=x0; x2-=x0; x3-=x0;
	y1-=y0; y2-=y0; y3-=y0;
	int signa=orientation(x2, y2, x3, y3, a);
	if(signa==0) return false;
	int signb=orientation(x3, y3, x1, y1, b);
	if(signb!=signa) return false;
	int signc=orientation(x1, y1, x2, y2, c);
	if(signc!=signa) return false;
	double sum=a+b+c;
	geo_assert(sum!=0); // if the SOS signs match and are nonzero, there's no way all of a, b, and c are zero.
	a/=sum;
	b/=sum;
	c/=sum;
	return true;
}

// -----------------------------------------------------------------------------

// \brief Computes the (approximate) orientation predicate in 2d.
// \details Computes the sign of the (approximate) signed volume of
//  the triangle p0, p1, p2
// \param[in] p0 first vertex of the triangle
// \param[in] p1 second vertex of the triangle
// \param[in] p2 third vertex of the triangle
// \retval POSITIVE if the triangle is oriented positively
// \retval ZERO if the triangle is flat
// \retval NEGATIVE if the triangle is oriented negatively
// \todo check whether orientation is inverted as compared to
//   Shewchuk's version.
// Taken from geogram/src/lib/geogram/delaunay/delaunay_2d.cpp
inline GEO::Sign orient_2d_inexact(GEO::vec2 p0, GEO::vec2 p1, GEO::vec2 p2) {
	double a11 = p1[0] - p0[0] ;
	double a12 = p1[1] - p0[1] ;

	double a21 = p2[0] - p0[0] ;
	double a22 = p2[1] - p0[1] ;

	double Delta = GEO::det2x2(
		a11, a12,
		a21, a22
	);

	return GEO::geo_sgn(Delta);
}

// -----------------------------------------------------------------------------

/**
 * @brief      { Intersect a vertical ray with a triangle }
 *
 * @param[in]  M     { Mesh containing the triangle to intersect }
 * @param[in]  f     { Index of the facet to intersect }
 * @param[in]  q     { Query point (only XY coordinates are used) }
 * @param[out] z     { Intersection }
 *
 * @return     { {-1,0,1} depending on the sign of the intersection. }
 */
template<int X = 0, int Y = 1, int Z = 2>
int intersect_ray_z(const GEO::Mesh &M, GEO::index_t f, const GEO::vec3 &q, double &z) {
	using namespace GEO;

	index_t c = M.facets.corners_begin(f);
	const vec3& p1 = Geom::mesh_vertex(M, M.facet_corners.vertex(c++));
	const vec3& p2 = Geom::mesh_vertex(M, M.facet_corners.vertex(c++));
	const vec3& p3 = Geom::mesh_vertex(M, M.facet_corners.vertex(c));

	double u, v, w;
	if (point_in_triangle_2d(
		q[X], q[Y], p1[X], p1[Y], p2[X], p2[Y], p3[X], p3[Y], u, v, w))
	{
		z = u*p1[Z] + v*p2[Z] + w*p3[Z];
		auto sign = orient_2d_inexact(vec2(p1[X], p1[Y]), vec2(p2[X], p2[Y]), vec2(p3[X], p3[Y]));
		switch (sign) {
		case GEO::POSITIVE: return 1;
		case GEO::NEGATIVE: return -1;
		case GEO::ZERO:
		default: return 0;
		}
	}

	return 0;
}

// -----------------------------------------------------------------------------

void compute_sign(const GEO::Mesh &M, const GEO::MeshFacetsAABB &aabb_tree,
	const Eigen::MatrixXd &P, Eigen::VectorXd &D)
{
	assert(P.cols() == 3);
	assert(D.size() == P.rows());

	GEO::vec3 min_corner, max_corner;
	GEO::get_bbox(M, &min_corner[0], &max_corner[0]);

	#pragma omp parallel for
	for (int k = 0; k < P.rows(); ++k) {
		GEO::vec3 center(P(k, 0), P(k, 1), P(k, 2));

		GEO::Box box;
		box.xyz_min[0] = box.xyz_max[0] = center[0];
		box.xyz_min[1] = box.xyz_max[1] = center[1];
		box.xyz_min[2] = min_corner[2];
		box.xyz_max[2] = max_corner[2];

		std::vector<std::pair<double, int>> inter;
		auto action = [&M, &inter, &center] (GEO::index_t f) {
			double z;
			if (int s = intersect_ray_z(M, f, center, z)) {
				inter.emplace_back(z, s);
			}
		};
		aabb_tree.compute_bbox_facet_bbox_intersections(box, action);
		std::sort(inter.begin(), inter.end());

		std::vector<double> reduced;
		for (int i = 0, s = 0; i < (int) inter.size(); ++i) {
			const int ds = inter[i].second;
			s += ds;
			if ((s == -1 && ds < 0) || (s == 0 && ds > 0)) {
				reduced.push_back(inter[i].first);
			}
		}

		int num_before = 0;
		for (double z : reduced) {
			if (z < center[2]) { ++num_before; }
		}
		if (num_before % 2 == 1) {
			// Point is inside
			D(k) *= -1.0;
		}
	}
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////

void poly_fem::to_geogram_mesh(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F, GEO::Mesh &M) {
	M.clear();
	// Setup vertices
	M.vertices.create_vertices((int) V.rows());
	for (int i = 0; i < (int) M.vertices.nb(); ++i) {
		GEO::vec3 &p = M.vertices.point(i);
		p[0] = V(i, 0);
		p[1] = V(i, 1);
		p[2] = V(i, 2);
	}
	// Setup faces
	M.facets.create_triangles((int) F.rows());
	for (int c = 0; c < (int) M.facets.nb(); ++c) {
		for (int lv = 0; lv < 3; ++lv) {
			M.facets.set_vertex(c, lv, F(c, lv));
		}
	}
}

// -----------------------------------------------------------------------------

void poly_fem::from_geogram_mesh(const GEO::Mesh &M, Eigen::MatrixXd &V, Eigen::MatrixXi &F, Eigen::MatrixXi &T) {
	V.resize(M.vertices.nb(), 3);
	for (int i = 0; i < (int) M.vertices.nb(); ++i) {
		GEO::vec3 p = M.vertices.point(i);
		V.row(i) << p[0], p[1], p[2];
	}
	assert(M.facets.are_simplices());
	F.resize(M.facets.nb(), 3);
	for (int c = 0; c < (int) M.facets.nb(); ++c) {
		for (int lv = 0; lv < 3; ++lv) {
			F(c, lv) = M.facets.vertex(c, lv);
		}
	}
	assert(M.cells.are_simplices());
	T.resize(M.cells.nb(), 4);
	for (int c = 0; c < (int) M.cells.nb(); ++c) {
		for (int lv = 0; lv < 4; ++lv) {
			T(c, lv) = M.cells.vertex(c, lv);
		}
	}
}

// -----------------------------------------------------------------------------

void poly_fem::signed_squared_distances(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F,
	const Eigen::MatrixXd &P, Eigen::VectorXd &D)
{
	GEO::Mesh M;
	to_geogram_mesh(V, F, M);
	GEO::MeshFacetsAABB aabb_tree(M);
	compute_unsigned_distance_field(M, aabb_tree, P, D);
	compute_sign(M, aabb_tree, P, D);
}

// -----------------------------------------------------------------------------

// // Compute volume of a mesh (M can be surface mesh)
// double volume(const GEO::Mesh &M) {
// 	GEO::vec3 t[4];
// 	t[3] = GEO::vec3(0, 0, 0);
// 	double volume_total = 0;
// 	for (int f = 0; f < (int) M.facets.nb(); ++f) {
// 		for(GEO::index_t c = M.facets.corners_begin(f), i = 0;
// 			c < M.facets.corners_end(f); ++c, ++i)
// 		{
// 			geo_assert(i < 3);
// 			t[i] = M.vertices.point(M.facet_corners.vertex(c));
// 		}
// 		double vol = GEO::Geom::tetra_signed_volume(t[0], t[1], t[2], t[3]);
// 		volume_total += vol;
// 	}
// 	return volume_total;
// }
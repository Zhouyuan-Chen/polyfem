#include <polyfem/State.hpp>
#include <polyfem/Common.hpp>

#include <polyfem/io/MatrixIO.hpp>

#include <polyfem/mesh/mesh2D/Mesh2D.hpp>
#include <polyfem/mesh/mesh2D/CMesh2D.hpp>
#include <polyfem/mesh/mesh2D/NCMesh2D.hpp>
#include <polyfem/mesh/mesh3D/Mesh3D.hpp>
#include <polyfem/mesh/mesh3D/CMesh3D.hpp>
#include <polyfem/mesh/mesh3D/NCMesh3D.hpp>

#include <polyfem/basis/FEBasis2d.hpp>
#include <polyfem/basis/FEBasis3d.hpp>

#include <polyfem/refinement/APriori.hpp>

#include <polyfem/basis/SplineBasis2d.hpp>
#include <polyfem/basis/SplineBasis3d.hpp>

#include <polyfem/basis/MVPolygonalBasis2d.hpp>

#include <polyfem/basis/PolygonalBasis2d.hpp>
#include <polyfem/basis/PolygonalBasis3d.hpp>

#include <polyfem/autogen/auto_p_bases.hpp>
#include <polyfem/autogen/auto_q_bases.hpp>

#include <polyfem/quadrature/HexQuadrature.hpp>
#include <polyfem/quadrature/QuadQuadrature.hpp>
#include <polyfem/quadrature/TetQuadrature.hpp>
#include <polyfem/quadrature/TriQuadrature.hpp>

#include <polyfem/utils/Logger.hpp>
#include <polyfem/utils/JSONUtils.hpp>
#include <polyfem/utils/Timer.hpp>

#include <polysolve/LinearSolver.hpp>
#include <polysolve/FEMSolver.hpp>

#include <polyfem/solver/forms/ElasticForm.hpp>

#include <unsupported/Eigen/SparseExtra>

#include <iostream>
#include <algorithm>
#include <memory>
#include <filesystem>

#include <polyfem/utils/autodiff.h>
DECLARE_DIFFSCALAR_BASE();

using namespace Eigen;

namespace polyfem
{
	using namespace assembler;
	using namespace mesh;
	using namespace io;
	using namespace utils;

	namespace
	{
		/// Assumes in nodes are in order vertex, edge, face, then cell nodes.
		void build_in_node_to_in_primitive(const Mesh &mesh, const MeshNodes &mesh_nodes,
										   Eigen::VectorXi &in_node_to_in_primitive,
										   Eigen::VectorXi &in_node_offset)
		{
			const int num_vertex_nodes = mesh_nodes.num_vertex_nodes();
			const int num_edge_nodes = mesh_nodes.num_edge_nodes();
			const int num_face_nodes = mesh_nodes.num_face_nodes();
			const int num_cell_nodes = mesh_nodes.num_cell_nodes();

			const int num_nodes = num_vertex_nodes + num_edge_nodes + num_face_nodes + num_cell_nodes;

			const long n_vertices = num_vertex_nodes;
			const int num_in_primitives = n_vertices + mesh.n_edges() + mesh.n_faces() + mesh.n_cells();
			const int num_primitives = mesh.n_vertices() + mesh.n_edges() + mesh.n_faces() + mesh.n_cells();

			in_node_to_in_primitive.resize(num_nodes);
			in_node_offset.resize(num_nodes);

			// Only one node per vertex, so this is an identity map.
			in_node_to_in_primitive.head(num_vertex_nodes).setLinSpaced(num_vertex_nodes, 0, num_vertex_nodes - 1); // vertex nodes
			in_node_offset.head(num_vertex_nodes).setZero();

			int prim_offset = n_vertices;
			int node_offset = num_vertex_nodes;
			auto foo = [&](const int num_prims, const int num_prim_nodes) {
				if (num_prims <= 0 || num_prim_nodes <= 0)
					return;
				const Eigen::VectorXi range = Eigen::VectorXi::LinSpaced(num_prim_nodes, 0, num_prim_nodes - 1);
				// TODO: This assumes isotropic degree of element.
				const int node_per_prim = num_prim_nodes / num_prims;

				in_node_to_in_primitive.segment(node_offset, num_prim_nodes) =
					range.array() / node_per_prim + prim_offset;

				in_node_offset.segment(node_offset, num_prim_nodes) =
					range.unaryExpr([&](const int x) { return x % node_per_prim; });

				prim_offset += num_prims;
				node_offset += num_prim_nodes;
			};

			foo(mesh.n_edges(), num_edge_nodes);
			foo(mesh.n_faces(), num_face_nodes);
			foo(mesh.n_cells(), num_cell_nodes);
		}

		void build_in_primitive_to_primitive(
			const Mesh &mesh, const MeshNodes &mesh_nodes,
			const Eigen::VectorXi &in_ordered_vertices,
			const Eigen::MatrixXi &in_ordered_edges,
			const Eigen::MatrixXi &in_ordered_faces,
			Eigen::VectorXi &in_primitive_to_primitive)
		{
			const int num_vertex_nodes = mesh_nodes.num_vertex_nodes();
			const int num_edge_nodes = mesh_nodes.num_edge_nodes();
			const int num_face_nodes = mesh_nodes.num_face_nodes();
			const int num_cell_nodes = mesh_nodes.num_cell_nodes();
			const int num_nodes = num_vertex_nodes + num_edge_nodes + num_face_nodes + num_cell_nodes;

			const long n_vertices = num_vertex_nodes;
			const int num_in_primitives = n_vertices + mesh.n_edges() + mesh.n_faces() + mesh.n_cells();
			const int num_primitives = mesh.n_vertices() + mesh.n_edges() + mesh.n_faces() + mesh.n_cells();

			in_primitive_to_primitive.setLinSpaced(num_in_primitives, 0, num_in_primitives - 1);

			igl::Timer timer;

			// ------------
			// Map vertices
			// ------------

			in_primitive_to_primitive.head(n_vertices) = in_ordered_vertices;

			int in_offset = n_vertices;
			int offset = mesh.n_vertices();

			// ---------
			// Map edges
			// ---------

			logger().trace("Building Mesh edges to IDs...");
			timer.start();
			const auto edges_to_ids = mesh.edges_to_ids();
			assert(in_ordered_edges.rows() == edges_to_ids.size());
			timer.stop();
			logger().trace("Done (took {}s)", timer.getElapsedTime());

			logger().trace("Building in-edge to edge mapping...");
			timer.start();
			for (int in_ei = 0; in_ei < in_ordered_edges.rows(); in_ei++)
			{
				const std::pair<int, int> in_edge(
					in_ordered_edges.row(in_ei).minCoeff(),
					in_ordered_edges.row(in_ei).maxCoeff());
				in_primitive_to_primitive[in_offset + in_ei] =
					offset + edges_to_ids.at(in_edge); // offset edge ids
			}
			timer.stop();
			logger().trace("Done (took {}s)", timer.getElapsedTime());

			in_offset += mesh.n_edges();
			offset += mesh.n_edges();

			// ---------
			// Map faces
			// ---------

			if (mesh.is_volume())
			{
				logger().trace("Building Mesh faces to IDs...");
				timer.start();
				const auto faces_to_ids = mesh.faces_to_ids();
				assert(in_ordered_faces.rows() == faces_to_ids.size());
				timer.stop();
				logger().trace("Done (took {}s)", timer.getElapsedTime());

				logger().trace("Building in-face to face mapping...");
				timer.start();
				for (int in_fi = 0; in_fi < in_ordered_faces.rows(); in_fi++)
				{
					std::vector<int> in_face(in_ordered_faces.cols());
					for (int i = 0; i < in_face.size(); i++)
						in_face[i] = in_ordered_faces(in_fi, i);
					std::sort(in_face.begin(), in_face.end());

					in_primitive_to_primitive[in_offset + in_fi] =
						offset + faces_to_ids.at(in_face); // offset face ids
				}
				timer.stop();
				logger().trace("Done (took {}s)", timer.getElapsedTime());

				in_offset += mesh.n_faces();
				offset += mesh.n_faces();
			}

			// NOTE: Assume in_cells_to_cells is identity
		}
	} // namespace

	void State::build_node_mapping()
	{
		if (args["space"]["advanced"]["use_spline"])
		{
			logger().warn("Node ordering disabled, it dosent work for splines!");
			return;
		}

		if (disc_orders.maxCoeff() >= 4 || disc_orders.maxCoeff() != disc_orders.minCoeff())
		{
			logger().warn("Node ordering disabled, it works only for p < 4 and uniform order!");
			return;
		}

		if (!mesh->is_conforming())
		{
			logger().warn("Node ordering disabled, not supported for non-conforming meshes!");
			return;
		}

		if (mesh->has_poly())
		{
			logger().warn("Node ordering disabled, not supported for polygonal meshes!");
			return;
		}

		if (mesh->in_ordered_vertices().size() <= 0 || mesh->in_ordered_edges().size() <= 0 || (mesh->is_volume() && mesh->in_ordered_faces().size() <= 0))
		{
			logger().warn("Node ordering disabled, input vertices/edges/faces not computed!");
			return;
		}

		const int num_vertex_nodes = mesh_nodes->num_vertex_nodes();
		const int num_edge_nodes = mesh_nodes->num_edge_nodes();
		const int num_face_nodes = mesh_nodes->num_face_nodes();
		const int num_cell_nodes = mesh_nodes->num_cell_nodes();

		const int num_nodes = num_vertex_nodes + num_edge_nodes + num_face_nodes + num_cell_nodes;

		const long n_vertices = num_vertex_nodes;
		const int num_in_primitives = n_vertices + mesh->n_edges() + mesh->n_faces() + mesh->n_cells();
		const int num_primitives = mesh->n_vertices() + mesh->n_edges() + mesh->n_faces() + mesh->n_cells();

		igl::Timer timer;

		logger().trace("Building in-node to in-primitive mapping...");
		timer.start();
		Eigen::VectorXi in_node_to_in_primitive;
		Eigen::VectorXi in_node_offset;
		build_in_node_to_in_primitive(*mesh, *mesh_nodes, in_node_to_in_primitive, in_node_offset);
		timer.stop();
		logger().trace("Done (took {}s)", timer.getElapsedTime());

		logger().trace("Building in-primitive to primitive mapping...");
		timer.start();
		build_in_primitive_to_primitive(
			*mesh, *mesh_nodes,
			mesh->in_ordered_vertices(),
			mesh->in_ordered_edges(),
			mesh->in_ordered_faces(),
			in_primitive_to_primitive);
		timer.stop();
		logger().trace("Done (took {}s)", timer.getElapsedTime());

		const auto primitive_offset = [&](int node) {
			if (mesh_nodes->is_vertex_node(node))
				return 0;
			else if (mesh_nodes->is_edge_node(node))
				return mesh->n_vertices();
			else if (mesh_nodes->is_face_node(node))
				return mesh->n_vertices() + mesh->n_edges();
			else if (mesh_nodes->is_cell_node(node))
				return mesh->n_vertices() + mesh->n_edges() + mesh->n_faces();
			throw std::runtime_error("Invalid node ID!");
		};

		logger().trace("Building primitive to node mapping...");
		timer.start();
		std::vector<std::vector<int>> primitive_to_nodes(num_primitives);
		const std::vector<int> &grouped_nodes = mesh_nodes->primitive_to_node();
		int node_count = 0;
		for (int i = 0; i < grouped_nodes.size(); i++)
		{
			int node = grouped_nodes[i];
			assert(node < num_nodes);
			if (node >= 0)
			{
				int primitive = mesh_nodes->node_to_primitive_gid().at(node) + primitive_offset(i);
				assert(primitive < num_primitives);
				primitive_to_nodes[primitive].push_back(node);
				node_count++;
			}
		}
		assert(node_count == num_nodes);
		timer.stop();
		logger().trace("Done (took {}s)", timer.getElapsedTime());

		logger().trace("Combining mappings...");
		timer.start();
		in_node_to_node.resize(num_nodes);
		for (int i = 0; i < num_nodes; i++)
		{
			// input node id -> input primitive -> primitive -> node
			const std::vector<int> &possible_nodes =
				primitive_to_nodes[in_primitive_to_primitive[in_node_to_in_primitive[i]]];
			assert(possible_nodes.size() > 0);
			if (possible_nodes.size() > 1)
			{
				// #ifndef NDEBUG
				// 				// assert(mesh_nodes->is_edge_node(i)); // TODO: Handle P4+
				// 				for (int possible_node : possible_nodes)
				// 					assert(mesh_nodes->is_edge_node(possible_node)); // TODO: Handle P4+
				// #endif

				int e_id = in_primitive_to_primitive[in_node_to_in_primitive[i]] - mesh->n_vertices();
				assert(e_id < mesh->n_edges());
				RowVectorNd v0 = mesh_nodes->node_position(in_node_to_node[mesh->edge_vertex(e_id, 0)]);
				RowVectorNd a = mesh_nodes->node_position(possible_nodes[0]);
				RowVectorNd b = mesh_nodes->node_position(possible_nodes[1]);
				// Assume possible nodes are ordered, so only need to check order of two nodes

				// Input edges are sorted, so if a is closer to v0 then the order is correct
				// otherwise the nodes are flipped.
				assert(mesh->edge_vertex(e_id, 0) < mesh->edge_vertex(e_id, 1));
				int offset = (a - v0).squaredNorm() < (b - v0).squaredNorm()
								 ? in_node_offset[i]
								 : (possible_nodes.size() - in_node_offset[i] - 1);
				in_node_to_node[i] = possible_nodes[offset];
			}
			else
				in_node_to_node[i] = possible_nodes[0];
		}
		timer.stop();
		logger().trace("Done (took {}s)", timer.getElapsedTime());
	}

	std::string State::formulation() const
	{
		if (args["materials"].is_null())
		{
			logger().error("specify some 'materials'");
			assert(!args["materials"].is_null());
			throw "invalid input";
		}

		if (args["materials"].is_array())
		{
			std::string current = "";
			for (const auto &m : args["materials"])
			{
				const std::string tmp = m["type"];
				if (current.empty())
					current = tmp;
				else if (current != tmp)
				{
					if (
						current == "LinearElasticity" || //
						current == "NeoHookean" ||       //
						current == "SaintVenant" ||
						// current == "HookeLinearElasticity" ||
						current == "MooneyRivlin" || //
						current == "Ogden" ||        //
						current == "MultiModels")
					{
						if (tmp == "LinearElasticity" || //
							tmp == "NeoHookean" ||       //
							tmp == "SaintVenant" ||
							// tmp == "HookeLinearElasticity" ||
							tmp == "MooneyRivlin" || //
							tmp == "Ogden")
							current = "MultiModels";
						else
						{
							logger().error("Current material is {}, new material is {}, multimaterial supported only for LinearElasticity and NeoHookean", current, tmp);
							throw "invalid input";
						}
					}
					else
					{
						logger().error("Current material is {}, new material is {}, multimaterial supported only for LinearElasticity and NeoHookean", current, tmp);
						throw "invalid input";
					}
				}
			}

			return current;
		}
		else
			return args["materials"]["type"];
	}

	void State::sol_to_pressure(Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure)
	{
		if (n_pressure_bases <= 0)
		{
			logger().error("No pressure bases defined!");
			return;
		}

		// assert(problem->is_mixed());
		assert(AssemblerUtils::is_mixed(formulation()));
		Eigen::MatrixXd tmp = sol;

		int fluid_offset = use_avg_pressure ? (AssemblerUtils::is_fluid(formulation()) ? 1 : 0) : 0;
		sol = tmp.topRows(tmp.rows() - n_pressure_bases - fluid_offset);
		assert(sol.size() == n_bases * (problem->is_scalar() ? 1 : mesh->dimension()));
		pressure = tmp.middleRows(tmp.rows() - n_pressure_bases - fluid_offset, n_pressure_bases);
		assert(pressure.size() == n_pressure_bases);
	}

	void State::set_materials()
	{
		if (!is_param_valid(args, "materials"))
			return;

		const auto &body_params = args["materials"];

		if (!body_params.is_array())
		{
			assembler.add_multimaterial(0, body_params);
			const auto &cur_lambdas = assembler.lame_params().lambda_mat_;
			const auto &cur_mus = assembler.lame_params().mu_mat_;
			if (cur_lambdas.size() == 0 || cur_mus.size() == 0)
			{
				Eigen::MatrixXd lambdas(mesh->n_elements(), 1), mus(mesh->n_elements(), 1);
				for (int e = 0; e < mesh->n_elements(); e++)
				{
					RowVectorNd barycenter;
					if (!mesh->is_volume())
					{
						const auto &mesh2d = *dynamic_cast<Mesh2D *>(mesh.get());
						barycenter = mesh2d.face_barycenter(e);
					}
					else
					{
						const auto &mesh3d = *dynamic_cast<Mesh3D *>(mesh.get());
						barycenter = mesh3d.cell_barycenter(e);
					}
					assembler.lame_params().lambda_mu(/*not used*/ 0, /*not used*/ 0, /*not used*/ 0, barycenter(0), barycenter(1), barycenter.size() < 3 ? 0.0 : barycenter(2), e, lambdas(e), mus(e));
				}
				assembler.update_lame_params(lambdas, mus);
			}
			return;
		}

		std::map<int, json> materials;
		for (int i = 0; i < body_params.size(); ++i)
		{
			json mat = body_params[i];
			json id = mat["id"];
			if (id.is_array())
			{
				for (int j = 0; j < id.size(); ++j)
					materials[id[j]] = mat;
			}
			else
			{
				const int mid = id;
				materials[mid] = mat;
			}
		}

		std::set<int> missing;

		std::map<int, int> body_element_count;
		std::vector<int> eid_to_eid_in_body(mesh->n_elements());
		for (int e = 0; e < mesh->n_elements(); ++e)
		{
			const int bid = mesh->get_body_id(e);
			body_element_count.try_emplace(bid, 0);
			eid_to_eid_in_body[e] = body_element_count[bid]++;
		}

		for (int e = 0; e < mesh->n_elements(); ++e)
		{
			const int bid = mesh->get_body_id(e);
			const auto it = materials.find(bid);
			if (it == materials.end())
			{
				missing.insert(bid);
				continue;
			}

			const json &tmp = it->second;
			assembler.add_multimaterial(e, tmp);
		}

		const auto &cur_lambdas = assembler.lame_params().lambda_mat_;
		const auto &cur_mus = assembler.lame_params().mu_mat_;
		if (cur_lambdas.size() == 0 || cur_mus.size() == 0)
		{
			Eigen::MatrixXd lambdas(mesh->n_elements(), 1), mus(mesh->n_elements(), 1);
			for (int e = 0; e < mesh->n_elements(); e++)
			{
				RowVectorNd barycenter;
				if (!mesh->is_volume())
				{
					const auto &mesh2d = *dynamic_cast<Mesh2D *>(mesh.get());
					barycenter = mesh2d.face_barycenter(e);
				}
				else
				{
					const auto &mesh3d = *dynamic_cast<Mesh3D *>(mesh.get());
					barycenter = mesh3d.cell_barycenter(e);
				}
				assembler.lame_params().lambda_mu(/*not used*/ 0, /*not used*/ 0, /*not used*/ 0, barycenter(0), barycenter(1), barycenter.size() < 3 ? 0.0 : barycenter(2), e, lambdas(e), mus(e));
			}
			assembler.update_lame_params(lambdas, mus);
		}

		for (int bid : missing)
		{
			logger().warn("Missing material parameters for body {}", bid);
		}
	}

	void compute_integral_constraints(
		const Mesh3D &mesh,
		const int n_bases,
		const std::vector<basis::ElementBases> &bases,
		const std::vector<basis::ElementBases> &gbases,
		Eigen::MatrixXd &basis_integrals)
	{
		if (!mesh.is_volume())
		{
			logger().error("Works only on volumetric meshes!");
			return;
		}
		assert(mesh.is_volume());

		basis_integrals.resize(n_bases, 9);
		basis_integrals.setZero();
		Eigen::MatrixXd rhs(n_bases, 9);
		rhs.setZero();

		const int n_elements = mesh.n_elements();
		for (int e = 0; e < n_elements; ++e)
		{
			// if (mesh.is_polytope(e)) {
			// 	continue;
			// }
			// ElementAssemblyValues vals = values[e];
			// const ElementAssemblyValues &gvals = gvalues[e];
			ElementAssemblyValues vals;
			vals.compute(e, mesh.is_volume(), bases[e], gbases[e]);

			// Computes the discretized integral of the PDE over the element
			const int n_local_bases = int(vals.basis_values.size());
			for (int j = 0; j < n_local_bases; ++j)
			{
				const AssemblyValues &v = vals.basis_values[j];
				const double integral_100 = (v.grad_t_m.col(0).array() * vals.det.array() * vals.quadrature.weights.array()).sum();
				const double integral_010 = (v.grad_t_m.col(1).array() * vals.det.array() * vals.quadrature.weights.array()).sum();
				const double integral_001 = (v.grad_t_m.col(2).array() * vals.det.array() * vals.quadrature.weights.array()).sum();

				const double integral_110 = ((vals.val.col(1).array() * v.grad_t_m.col(0).array() + vals.val.col(0).array() * v.grad_t_m.col(1).array()) * vals.det.array() * vals.quadrature.weights.array()).sum();
				const double integral_011 = ((vals.val.col(2).array() * v.grad_t_m.col(1).array() + vals.val.col(1).array() * v.grad_t_m.col(2).array()) * vals.det.array() * vals.quadrature.weights.array()).sum();
				const double integral_101 = ((vals.val.col(0).array() * v.grad_t_m.col(2).array() + vals.val.col(2).array() * v.grad_t_m.col(0).array()) * vals.det.array() * vals.quadrature.weights.array()).sum();

				const double integral_200 = 2 * (vals.val.col(0).array() * v.grad_t_m.col(0).array() * vals.det.array() * vals.quadrature.weights.array()).sum();
				const double integral_020 = 2 * (vals.val.col(1).array() * v.grad_t_m.col(1).array() * vals.det.array() * vals.quadrature.weights.array()).sum();
				const double integral_002 = 2 * (vals.val.col(2).array() * v.grad_t_m.col(2).array() * vals.det.array() * vals.quadrature.weights.array()).sum();

				const double area = (v.val.array() * vals.det.array() * vals.quadrature.weights.array()).sum();

				for (size_t ii = 0; ii < v.global.size(); ++ii)
				{
					basis_integrals(v.global[ii].index, 0) += integral_100 * v.global[ii].val;
					basis_integrals(v.global[ii].index, 1) += integral_010 * v.global[ii].val;
					basis_integrals(v.global[ii].index, 2) += integral_001 * v.global[ii].val;

					basis_integrals(v.global[ii].index, 3) += integral_110 * v.global[ii].val;
					basis_integrals(v.global[ii].index, 4) += integral_011 * v.global[ii].val;
					basis_integrals(v.global[ii].index, 5) += integral_101 * v.global[ii].val;

					basis_integrals(v.global[ii].index, 6) += integral_200 * v.global[ii].val;
					basis_integrals(v.global[ii].index, 7) += integral_020 * v.global[ii].val;
					basis_integrals(v.global[ii].index, 8) += integral_002 * v.global[ii].val;

					rhs(v.global[ii].index, 6) += -2.0 * area * v.global[ii].val;
					rhs(v.global[ii].index, 7) += -2.0 * area * v.global[ii].val;
					rhs(v.global[ii].index, 8) += -2.0 * area * v.global[ii].val;
				}
			}
		}

		basis_integrals -= rhs;
	}

	bool State::iso_parametric() const
	{
		if (mesh->has_poly())
			return true;

		if (args["space"]["advanced"]["use_spline"])
			return true;

		if (mesh->is_rational())
			return false;

		if (args["space"]["use_p_ref"])
			return false;
		
		if (has_periodic_bc())
			return false;

		if (mesh->orders().size() <= 0)
		{
			return args["space"]["advanced"]["isoparametric"];
		}

		if (mesh->orders().minCoeff() != mesh->orders().maxCoeff())
			return false;

		if (args["space"]["discr_order"] == mesh->orders().minCoeff())
			return true;

		// TODO:
		// if (args["space"]["discr_order"] == 1 && args["force_linear_geometry"])
		// 	return true;

		return args["space"]["advanced"]["isoparametric"];
	}

	void State::build_basis()
	{
		if (!mesh)
		{
			logger().error("Load the mesh first!");
			return;
		}

		mesh->prepare_mesh();

		bases.clear();
		pressure_bases.clear();
		geom_bases_.clear();
		boundary_nodes.clear();
		boundary_gnodes.clear();
		boundary_gnodes_mask.clear();
		input_dirichlet.clear();
		dirichlet_nodes.clear();
		neumann_nodes.clear();
		local_boundary.clear();
		total_local_boundary.clear();
		local_neumann_boundary.clear();
		polys.clear();
		poly_edge_to_data.clear();
		stiffness.resize(0, 0);
		rhs.resize(0, 0);

		if (formulation() == "MultiModels")
		{
			assert(args["materials"].is_array());

			std::vector<std::string> materials(mesh->n_elements());

			std::map<int, std::string> mats;

			for (const auto &m : args["materials"])
				mats[m["id"].get<int>()] = m["type"];

			for (int i = 0; i < materials.size(); ++i)
				materials[i] = mats.at(mesh->get_body_id(i));

			assembler.init_multimodels(materials);
		}

		n_bases = 0;
		n_geom_bases = 0;
		n_pressure_bases = 0;

		stats.reset();

		disc_orders.resize(mesh->n_elements());
		problem->init(*mesh);

		logger().info("Building {} basis...", (iso_parametric() ? "isoparametric" : "not isoparametric"));
		const bool has_polys = mesh->has_poly();

		local_boundary.clear();
		local_neumann_boundary.clear();
		std::map<int, basis::InterfaceData> poly_edge_to_data_geom; // temp dummy variable

		const auto &tmp_json = args["space"]["discr_order"];
		if (tmp_json.is_number_integer())
		{
			disc_orders.setConstant(tmp_json);
		}
		else if (tmp_json.is_string())
		{
			const std::string discr_orders_path = tmp_json;
			Eigen::MatrixXi tmp;
			read_matrix(discr_orders_path, tmp);
			assert(tmp.size() == disc_orders.size());
			assert(tmp.cols() == 1);
			disc_orders = tmp;
		}
		else if (tmp_json.is_array())
		{
			const auto b_discr_orders = tmp_json;

			std::map<int, int> b_orders;
			for (size_t i = 0; i < b_discr_orders.size(); ++i)
			{
				assert(b_discr_orders[i]["id"].is_array() || b_discr_orders[i]["id"].is_number_integer());

				std::vector<int> ids;
				if (b_discr_orders[i]["id"].is_array())
					ids = b_discr_orders[i]["id"].get<decltype(ids)>();
				else
					ids.push_back(b_discr_orders[i]["id"]);

				const int order = b_discr_orders[i]["order"];
				for (const int id : ids)
				{
					b_orders[id] = order;
					logger().trace("bid {}, discr {}", id, order);
				}
			}

			for (int e = 0; e < mesh->n_elements(); ++e)
			{
				const int bid = mesh->get_body_id(e);
				const auto order = b_orders.find(bid);
				if (order == b_orders.end())
				{
					logger().debug("Missing discretization order for body {}; using 1", bid);
					b_orders[bid] = 1;
					disc_orders[e] = 1;
				}
				else
				{
					disc_orders[e] = order->second;
				}
			}
		}
		else
		{
			logger().error("space/discr_order must be either a number a path or an array");
			throw "invalid json";
		}
		// TODO: same for pressure!

		Eigen::MatrixXi geom_disc_orders;
		if (!iso_parametric())
		{
			if (mesh->orders().size() <= 0)
			{
				geom_disc_orders.resizeLike(disc_orders);
				geom_disc_orders.setConstant(1);
			}
			else
				geom_disc_orders = mesh->orders();
		}

		igl::Timer timer;
		timer.start();
		if (args["space"]["use_p_ref"])
		{
			refinement::APriori::p_refine(
				*mesh,
				args["space"]["advanced"]["B"],
				args["space"]["advanced"]["h1_formula"],
				args["space"]["discr_order"],
				args["space"]["advanced"]["discr_order_max"],
				stats,
				disc_orders);

			logger().info("min p: {} max p: {}", disc_orders.minCoeff(), disc_orders.maxCoeff());
		}

		int quadrature_order = args["space"]["advanced"]["quadrature_order"].get<int>();
		const int mass_quadrature_order = args["space"]["advanced"]["mass_quadrature_order"].get<int>();
		if (assembler.is_mixed(formulation()))
		{
			const int disc_order = disc_orders.maxCoeff();
			if (disc_order - disc_orders.minCoeff() != 0)
			{
				logger().error("p refinement not supported in mixed formulation!");
				return;
			}
		}

		if (mesh->is_volume())
		{
			const Mesh3D &tmp_mesh = *dynamic_cast<Mesh3D *>(mesh.get());
			if (args["space"]["advanced"]["use_spline"])
			{
				// if (!iso_parametric())
				// {
				// 	logger().error("Splines must be isoparametric, ignoring...");
				// 	// FEBasis3d::build_bases(tmp_mesh, quadrature_order, geom_disc_orders, has_polys, geom_bases_, local_boundary, poly_edge_to_data_geom, mesh_nodes);
				// 	SplineBasis3d::build_bases(tmp_mesh, quadrature_order, geom_bases_, local_boundary, poly_edge_to_data);
				// }

				n_bases = basis::SplineBasis3d::build_bases(tmp_mesh, formulation(), quadrature_order, mass_quadrature_order, bases, local_boundary, poly_edge_to_data);

				// if (iso_parametric() && args["fit_nodes"])
				// 	SplineBasis3d::fit_nodes(tmp_mesh, n_bases, bases);
			}
			else
			{
				if (!iso_parametric())
				{
					n_geom_bases = basis::FEBasis3d::build_bases(tmp_mesh, formulation(), quadrature_order, mass_quadrature_order, geom_disc_orders, false, has_polys, false, geom_bases_, local_boundary, poly_edge_to_data_geom, geom_mesh_nodes);
					primitive_to_geom_bases_node = geom_mesh_nodes->primitive_to_node();
				}

				n_bases = basis::FEBasis3d::build_bases(tmp_mesh, formulation(), quadrature_order, mass_quadrature_order, disc_orders, args["space"]["advanced"]["serendipity"], has_polys, false, bases, local_boundary, poly_edge_to_data, mesh_nodes);
				primitive_to_bases_node = mesh_nodes->primitive_to_node();
			}

			// if(problem->is_mixed())
			if (assembler.is_mixed(formulation()))
			{
				n_pressure_bases = basis::FEBasis3d::build_bases(tmp_mesh, formulation(), quadrature_order, mass_quadrature_order, int(args["space"]["pressure_discr_order"]), false, has_polys, false, pressure_bases, local_boundary, poly_edge_to_data_geom, pressure_mesh_nodes);
				primitive_to_pressure_bases_node = pressure_mesh_nodes->primitive_to_node();
			}
		}
		else
		{
			const Mesh2D &tmp_mesh = *dynamic_cast<Mesh2D *>(mesh.get());
			if (args["space"]["advanced"]["use_spline"])
			{
				// TODO:
				// if (!iso_parametric())
				// {
				// 	logger().error("Splines must be isoparametric, ignoring...");
				// 	// FEBasis2d::build_bases(tmp_mesh, quadrature_order, disc_orders, has_polys, geom_bases_, local_boundary, poly_edge_to_data_geom, mesh_nodes);
				// 	n_bases = SplineBasis2d::build_bases(tmp_mesh, quadrature_order, geom_bases_, local_boundary, poly_edge_to_data);
				// }

				n_bases = basis::SplineBasis2d::build_bases(tmp_mesh, formulation(), quadrature_order, mass_quadrature_order, bases, local_boundary, poly_edge_to_data);

				// if (iso_parametric() && args["fit_nodes"])
				// 	SplineBasis2d::fit_nodes(tmp_mesh, n_bases, bases);
			}
			else
			{
				if (!iso_parametric())
				{
					n_geom_bases = basis::FEBasis2d::build_bases(tmp_mesh, formulation(), quadrature_order, mass_quadrature_order, geom_disc_orders, false, has_polys, false, geom_bases_, local_boundary, poly_edge_to_data_geom, geom_mesh_nodes);
					primitive_to_geom_bases_node = geom_mesh_nodes->primitive_to_node();
				}

				n_bases = basis::FEBasis2d::build_bases(tmp_mesh, formulation(), quadrature_order, mass_quadrature_order, disc_orders, args["space"]["advanced"]["serendipity"], has_polys, false, bases, local_boundary, poly_edge_to_data, mesh_nodes);
				primitive_to_bases_node = mesh_nodes->primitive_to_node();
			}

			// if(problem->is_mixed())
			if (assembler.is_mixed(formulation()))
			{
				n_pressure_bases = basis::FEBasis2d::build_bases(tmp_mesh, formulation(), quadrature_order, mass_quadrature_order, int(args["space"]["pressure_discr_order"]), false, has_polys, false, pressure_bases, local_boundary, poly_edge_to_data_geom, pressure_mesh_nodes);
				primitive_to_pressure_bases_node = pressure_mesh_nodes->primitive_to_node();
			}
		}

		if (assembler.is_mixed(formulation()))
		{
			assert(bases.size() == pressure_bases.size());
			for (int i = 0; i < pressure_bases.size(); ++i)
			{
				quadrature::Quadrature b_quad;
				bases[i].compute_quadrature(b_quad);
				pressure_bases[i].set_quadrature([b_quad](quadrature::Quadrature &quad) { quad = b_quad; });
			}
		}

		timer.stop();

		if (n_geom_bases == 0)
			n_geom_bases = n_bases;

		auto &gbases = geom_bases();

		build_polygonal_basis();

		// if (is_contact_enabled())
		if (args["optimization"]["enabled"])
		{
			Eigen::SparseMatrix<bool, 0> tmp1(n_geom_bases, n_bases);
			Eigen::SparseMatrix<bool, 0> tmp2(n_geom_bases, n_bases);
			std::vector<Eigen::Triplet<bool>> coeffs1, coeffs2;
			for (int e = 0; e < gbases.size(); e++)
			{
				const auto &gbs = gbases[e].bases;
				const auto &bs = bases[e].bases;

				Eigen::MatrixXd local_pts;
				const int order = bs.front().order();
				assert(order <= 2);
				if (mesh->is_volume())
					if (mesh->is_simplex(e))
						autogen::p_nodes_3d(order, local_pts);
					else
						autogen::q_nodes_3d(order, local_pts);
				else
					if (mesh->is_simplex(e))
						autogen::p_nodes_2d(order, local_pts);
					else
						autogen::q_nodes_2d(order, local_pts);

				ElementAssemblyValues vals;
				vals.compute(e, mesh->is_volume(), local_pts, gbases[e], gbases[e]);

				for (int i = 0; i < bs.size(); i++)
				{
					for (int j = 0; j < gbs.size(); j++)
					{
						if (std::abs(vals.basis_values[j].val(i) - 1) < 1e-7)
							coeffs1.emplace_back(gbs[j].global()[0].index, bs[i].global()[0].index, true);
						else if (std::abs(vals.basis_values[j].val(i) - 0.5) < 1e-7)
							coeffs2.emplace_back(gbs[j].global()[0].index, bs[i].global()[0].index, true);
						else if (std::abs(vals.basis_values[j].val(i)) < 1e-7)
						{
						}
						else
							assert(false);
					}
				}
			}
			tmp1.setFromTriplets(coeffs1.begin(), coeffs1.end());
			tmp2.setFromTriplets(coeffs2.begin(), coeffs2.end());

			std::vector<Eigen::Triplet<double>> coeffs;
			down_sampling_mat.resize(n_geom_bases * mesh->dimension(), n_bases * mesh->dimension());
			for (int k = 0; k < tmp1.outerSize(); ++k)
				for (Eigen::SparseMatrix<bool, 0>::InnerIterator it(tmp1, k); it; ++it)
					if (it.value())
						for (int d = 0; d < mesh->dimension(); d++)
							coeffs.emplace_back(it.row() * mesh->dimension() + d, it.col() * mesh->dimension() + d, 1);
			for (int k = 0; k < tmp2.outerSize(); ++k)
				for (Eigen::SparseMatrix<bool, 0>::InnerIterator it(tmp2, k); it; ++it)
					if (it.value())
						for (int d = 0; d < mesh->dimension(); d++)
							coeffs.emplace_back(it.row() * mesh->dimension() + d, it.col() * mesh->dimension() + d, 0.5);
			down_sampling_mat.setFromTriplets(coeffs.begin(), coeffs.end());
		}

		for (const auto &lb : local_boundary)
			total_local_boundary.emplace_back(lb);

		const int dim = mesh->dimension();
		const int problem_dim = problem->is_scalar() ? 1 : dim;
		
		// handle periodic bc
		if (has_periodic_bc())
		{
			std::vector<bool> periodic_dim;
			for (const auto &r : args["boundary_conditions"]["periodic_boundary"])
				periodic_dim.push_back(r);
			while (periodic_dim.size() < dim)
				periodic_dim.push_back(false);

			RowVectorNd min, max;
			mesh->bounding_box(min, max);
			const double bbox_size = (max - min).maxCoeff();

			periodic_reduce_map.setConstant(n_bases * problem_dim, 1, -1);

			Eigen::VectorXi dependent_map(n_bases);
			dependent_map.setConstant(-1);

			// find corresponding periodic boundary nodes
			Eigen::Vector3i dependent_face({1,2,5}), target_face({3,4,6});
			for (int d = 0; d < dim; d++)
			{
				if (!periodic_dim[d])
					continue;
				
				const int dependent_boundary_id = dependent_face(d);
				const int target_boundary_id = target_face(d);

				std::set<int> dependent_ids, target_ids;

				for (const auto &lb : total_local_boundary)
				{
					const int e = lb.element_id();
					const basis::ElementBases &bs = bases[e];

					for (int i = 0; i < lb.size(); ++i)
					{
						const int primitive_global_id = lb.global_primitive_id(i);
						const auto nodes = bs.local_nodes_for_primitive(primitive_global_id, *mesh);

						for (long n = 0; n < nodes.size(); ++n)
						{
							for (int j = 0; j < bs.bases[nodes(n)].global().size(); j++)
							{
								const int gid = bs.bases[nodes(n)].global()[j].index;
								if (mesh->get_boundary_id(primitive_global_id) == dependent_boundary_id)
									dependent_ids.insert(gid);
								else if (mesh->get_boundary_id(primitive_global_id) == target_boundary_id)
									target_ids.insert(gid);
							}
						}
					}
				}

				for (int i : dependent_ids)
				{
					bool found_target = false;
					for (int j : target_ids)
					{
						RowVectorNd projected_diff = mesh_nodes->node_position(j) - mesh_nodes->node_position(i);
						projected_diff(d) = 0;
						if (projected_diff.norm() < 1e-6 * bbox_size)
						{
							dependent_map(i) = j;
							found_target = true;
							break;
						}
					}
					if (!found_target)
						throw std::runtime_error("Periodic boundary condition failed to find corresponding nodes!");
				}
			}

			// break dependency chains into direct dependency
			for (int d = 0; d < dim; d++)
				for (int i = 0; i < dependent_map.size(); i++)
					if (dependent_map(i) >= 0 && dependent_map(dependent_map(i)) >= 0)
						dependent_map(i) = dependent_map(dependent_map(i));
			
			// new indexing for independent dof
			int independent_dof = 0;
			n_periodic_dependent_dofs = 0;
			for (int i = 0; i < dependent_map.size(); i++)
				if (dependent_map(i) < 0)
				{
					for (int d = 0; d < problem_dim; d++)
						periodic_reduce_map(i * problem_dim + d) = independent_dof * problem_dim + d;
					independent_dof++;
					dependent_map(i) = i;
				}
				else
					n_periodic_dependent_dofs += problem_dim;

			for (int i = 0; i < dependent_map.size(); i++)
				if (dependent_map(i) >= 0)
					for (int d = 0; d < problem_dim; d++)
						periodic_reduce_map(i * problem_dim + d) = periodic_reduce_map(dependent_map(i) * problem_dim + d);

			if (args["space"]["advanced"]["periodic_basis"].get<bool>())
			{
				for (auto &bs : bases)
					for (auto &b : bs.bases)
						for (auto &g : b.global())
						{
							int idx = periodic_reduce_map(g.index * problem_dim) / problem_dim;
							if (idx != g.index)
								g.index = idx;
						}

				n_bases = independent_dof;
			}
		}
		if (args["space"]["advanced"]["count_flipped_els"])
			stats.count_flipped_elements(*mesh, geom_bases());

		const int prev_bases = n_bases;
		n_bases += obstacle.n_vertices();

		logger().info("Building collision mesh...");
		build_collision_mesh(boundary_nodes_pos, collision_mesh, n_bases, bases);
		logger().info("Done!");

		// incompatible with optimization, fix me!
		{
			igl::Timer timer2;
			logger().debug("Building node mapping...");
			timer2.start();
			build_node_mapping();
			problem->update_nodes(in_node_to_node);
			mesh->update_nodes(in_node_to_node);
			timer2.stop();
			logger().debug("Done (took {}s)", timer2.getElapsedTime());
		}

		const int prev_b_size = local_boundary.size();
		problem->setup_bc(*mesh, n_bases,
						  bases, geom_bases(), pressure_bases, boundary_gnodes,
						  local_boundary, boundary_nodes, local_neumann_boundary, pressure_boundary_nodes,
						  dirichlet_nodes, neumann_nodes);
		const bool has_pressure_stablization = (args["materials"].contains("delta2") && (args["materials"]["delta2"].get<double>() > 0));
		const bool has_neumann = local_neumann_boundary.size() > 0 || local_boundary.size() < prev_b_size || has_pressure_stablization;
		use_avg_pressure = !has_neumann;
		if (args["materials"].contains("use_avg_pressure"))
			use_avg_pressure = args["materials"]["use_avg_pressure"];

		boundary_gnodes_mask.assign(n_geom_bases, false);
		for (auto &bnode : boundary_gnodes)
			boundary_gnodes_mask[bnode] = true;

		// setp nodal values
		{
			dirichlet_nodes_position.resize(dirichlet_nodes.size());
			for (int n = 0; n < dirichlet_nodes.size(); ++n)
			{
				const int n_id = dirichlet_nodes[n];
				bool found = false;
				for (const auto &bs : bases)
				{
					for (const auto &b : bs.bases)
					{
						for (const auto &lg : b.global())
						{
							if (lg.index == n_id)
							{
								dirichlet_nodes_position[n] = lg.node;
								found = true;
								break;
							}
						}

						if (found)
							break;
					}

					if (found)
						break;
				}

				assert(found);
			}

			neumann_nodes_position.resize(neumann_nodes.size());
			for (int n = 0; n < neumann_nodes.size(); ++n)
			{
				const int n_id = neumann_nodes[n];
				bool found = false;
				for (const auto &bs : bases)
				{
					for (const auto &b : bs.bases)
					{
						for (const auto &lg : b.global())
						{
							if (lg.index == n_id)
							{
								neumann_nodes_position[n] = lg.node;
								found = true;
								break;
							}
						}

						if (found)
							break;
					}

					if (found)
						break;
				}

				assert(found);
			}
		}

		for (int i = prev_bases; i < n_bases; ++i)
		{
			for (int d = 0; d < problem_dim; ++d)
				boundary_nodes.push_back(i * problem_dim + d);
		}

		std::sort(boundary_nodes.begin(), boundary_nodes.end());
		auto it = std::unique(boundary_nodes.begin(), boundary_nodes.end());
		boundary_nodes.resize(std::distance(boundary_nodes.begin(), it));

		const auto &curret_bases = geom_bases();
		const int n_samples = 10;
		stats.compute_mesh_size(*mesh, curret_bases, n_samples, args["output"]["advanced"]["curved_mesh_size"]);

		if (is_contact_enabled())
		{
			if (!has_dhat && args["contact"]["dhat"] > stats.min_edge_length)
			{
				args["contact"]["dhat"] = double(args["contact"]["dhat_percentage"]) * stats.min_edge_length;
				logger().info("dhat set to {}", double(args["contact"]["dhat"]));
			}
			else
			{
				if (args["contact"]["dhat"] > stats.min_edge_length)
					logger().warn("dhat larger than min edge, {} > {}", double(args["contact"]["dhat"]), stats.min_edge_length);
			}
		}

		logger().info("n_bases {}", n_bases);

		timings.building_basis_time = timer.getElapsedTime();
		logger().info(" took {}s", timings.building_basis_time);

		logger().info("flipped elements {}", stats.n_flipped);
		logger().info("h: {}", stats.mesh_size);
		logger().info("n bases: {}", n_bases);
		logger().info("n pressure bases: {}", n_pressure_bases);

		ass_vals_cache.clear();
		mass_ass_vals_cache.clear();
		if (n_bases <= args["solver"]["advanced"]["cache_size"])
		{
			timer.start();
			logger().info("Building cache...");
			ass_vals_cache.init(mesh->is_volume(), bases, curret_bases);
			mass_ass_vals_cache.init(mesh->is_volume(), bases, curret_bases, true);
			if (assembler.is_mixed(formulation()))
				pressure_ass_vals_cache.init(mesh->is_volume(), pressure_bases, curret_bases);

			logger().info(" took {}s", timer.getElapsedTime());
		}

		out_geom.build_grid(*mesh, args["output"]["advanced"]["sol_on_grid"]);

		if (!problem->is_time_dependent() && boundary_nodes.empty())
		{
			logger().warn("Static problem without Dirichlet nodes, will use Lagrange multipliers to find a unique solution!");
		}
	}

	void State::build_polygonal_basis()
	{
		if (!mesh)
		{
			logger().error("Load the mesh first!");
			return;
		}
		if (n_bases <= 0)
		{
			logger().error("Build the bases first!");
			return;
		}

		stiffness.resize(0, 0);
		rhs.resize(0, 0);

		igl::Timer timer;
		timer.start();
		logger().info("Computing polygonal basis...");

		// std::sort(boundary_nodes.begin(), boundary_nodes.end());

		// mixed not supports polygonal bases
		assert(n_pressure_bases == 0 || poly_edge_to_data.size() == 0);

		int new_bases = 0;

		if (iso_parametric())
		{
			if (mesh->is_volume())
			{
				if (args["space"]["advanced"]["poly_bases"] == "MeanValue")
					logger().error("MeanValue bases not supported in 3D");
				new_bases = basis::PolygonalBasis3d::build_bases(assembler, formulation(), args["space"]["advanced"]["n_harmonic_samples"], *dynamic_cast<Mesh3D *>(mesh.get()), n_bases, args["space"]["advanced"]["quadrature_order"], args["space"]["advanced"]["mass_quadrature_order"], args["space"]["advanced"]["integral_constraints"], bases, bases, poly_edge_to_data, polys_3d);
			}
			else
			{
				if (args["space"]["advanced"]["poly_bases"] == "MeanValue")
				{
					new_bases = basis::MVPolygonalBasis2d::build_bases(formulation(), *dynamic_cast<Mesh2D *>(mesh.get()), n_bases, args["space"]["advanced"]["quadrature_order"], args["space"]["advanced"]["mass_quadrature_order"], bases, bases, poly_edge_to_data, local_boundary, polys);
				}
				else
					new_bases = basis::PolygonalBasis2d::build_bases(assembler, formulation(), args["space"]["advanced"]["n_harmonic_samples"], *dynamic_cast<Mesh2D *>(mesh.get()), n_bases, args["space"]["advanced"]["quadrature_order"], args["space"]["advanced"]["mass_quadrature_order"], args["space"]["advanced"]["integral_constraints"], bases, bases, poly_edge_to_data, polys);
			}
		}
		else
		{
			if (mesh->is_volume())
			{
				if (args["space"]["advanced"]["poly_bases"] == "MeanValue")
				{
					logger().error("MeanValue bases not supported in 3D");
					throw "not implemented";
				}
				new_bases = basis::PolygonalBasis3d::build_bases(assembler, formulation(), args["space"]["advanced"]["n_harmonic_samples"], *dynamic_cast<Mesh3D *>(mesh.get()), n_bases, args["space"]["advanced"]["quadrature_order"], args["space"]["advanced"]["mass_quadrature_order"], args["space"]["advanced"]["integral_constraints"], bases, geom_bases_, poly_edge_to_data, polys_3d);
			}
			else
			{
				if (args["space"]["advanced"]["poly_bases"] == "MeanValue")
					new_bases = basis::MVPolygonalBasis2d::build_bases(formulation(), *dynamic_cast<Mesh2D *>(mesh.get()), n_bases, args["space"]["advanced"]["quadrature_order"], args["space"]["advanced"]["mass_quadrature_order"], bases, geom_bases_, poly_edge_to_data, local_boundary, polys);
				else
					new_bases = basis::PolygonalBasis2d::build_bases(assembler, formulation(), args["space"]["advanced"]["n_harmonic_samples"], *dynamic_cast<Mesh2D *>(mesh.get()), n_bases, args["space"]["advanced"]["quadrature_order"], args["space"]["advanced"]["mass_quadrature_order"], args["space"]["advanced"]["integral_constraints"], bases, geom_bases_, poly_edge_to_data, polys);
			}
		}

		timer.stop();
		timings.computing_poly_basis_time = timer.getElapsedTime();
		logger().info(" took {}s", timings.computing_poly_basis_time);

		n_bases += new_bases;
	}

void State::build_collision_mesh(
		Eigen::MatrixXd &boundary_nodes_pos_,
		ipc::CollisionMesh &collision_mesh_, 
		const int n_bases_, 
		const std::vector<basis::ElementBases> &bases_) const
	{
		Eigen::MatrixXi boundary_edges, boundary_triangles;
		io::OutGeometryData::extract_boundary_mesh(*mesh, n_bases_, bases_, total_local_boundary,
												   boundary_nodes_pos_, boundary_edges, boundary_triangles);

		Eigen::VectorXi codimensional_nodes;
		if (obstacle.n_vertices() > 0)
		{
			// boundary_nodes_pos_ uses n_bases that already contains the obstacle
			const int n_v = boundary_nodes_pos_.rows() - obstacle.n_vertices();

			if (obstacle.v().size())
				boundary_nodes_pos_.bottomRows(obstacle.v().rows()) = obstacle.v();

			if (obstacle.codim_v().size())
			{
				codimensional_nodes.conservativeResize(codimensional_nodes.size() + obstacle.codim_v().size());
				codimensional_nodes.tail(obstacle.codim_v().size()) = obstacle.codim_v().array() + n_v;
			}

			if (obstacle.e().size())
			{
				boundary_edges.conservativeResize(boundary_edges.rows() + obstacle.e().rows(), 2);
				boundary_edges.bottomRows(obstacle.e().rows()) = obstacle.e().array() + n_v;
			}

			if (obstacle.f().size())
			{
				boundary_triangles.conservativeResize(boundary_triangles.rows() + obstacle.f().rows(), 3);
				boundary_triangles.bottomRows(obstacle.f().rows()) = obstacle.f().array() + n_v;
			}
		}

		std::vector<bool> is_on_surface = ipc::CollisionMesh::construct_is_on_surface(boundary_nodes_pos_.rows(), boundary_edges);
		for (int i = 0; i < codimensional_nodes.size(); i++)
		{
			is_on_surface[codimensional_nodes[i]] = true;
		}

		collision_mesh_ = ipc::CollisionMesh(is_on_surface, boundary_nodes_pos_, boundary_edges, boundary_triangles);

		collision_mesh_.can_collide = [&](size_t vi, size_t vj) {
			// obstacles do not collide with other obstacles
			return !this->is_obstacle_vertex(collision_mesh_.to_full_vertex_id(vi))
				   || !this->is_obstacle_vertex(collision_mesh_.to_full_vertex_id(vj));
		};
	}

	void State::assemble_stiffness_mat(bool assemble_mass)
	{
		if (!mesh)
		{
			logger().error("Load the mesh first!");
			return;
		}
		if (n_bases <= 0)
		{
			logger().error("Build the bases first!");
			return;
		}
		if (formulation() == "OperatorSplitting")
		{
			stiffness.resize(1, 1);
			timings.assembling_stiffness_mat_time = 0;
			return;
		}

		stiffness.resize(0, 0);
		mass.resize(0, 0);
		avg_mass = 1;

		igl::Timer timer;
		timer.start();
		logger().info("Assembling stiffness mat...");

		// if(problem->is_mixed())
		if (assembler.is_mixed(formulation()))
		{
			if (assembler.is_linear(formulation()))
			{
				StiffnessMatrix velocity_stiffness, mixed_stiffness, pressure_stiffness;
				assembler.assemble_problem(formulation(), mesh->is_volume(), n_bases, bases, geom_bases(), ass_vals_cache, velocity_stiffness);
				assembler.assemble_mixed_problem(formulation(), mesh->is_volume(), n_pressure_bases, n_bases, pressure_bases, bases, geom_bases(), pressure_ass_vals_cache, ass_vals_cache, mixed_stiffness);
				assembler.assemble_pressure_problem(formulation(), mesh->is_volume(), n_pressure_bases, pressure_bases, geom_bases(), pressure_ass_vals_cache, pressure_stiffness);

				const int problem_dim = problem->is_scalar() ? 1 : mesh->dimension();

				AssemblerUtils::merge_mixed_matrices(n_bases, n_pressure_bases, problem_dim, use_avg_pressure ? assembler.is_fluid(formulation()) : false,
													 velocity_stiffness, mixed_stiffness, pressure_stiffness,
													 stiffness);

				if (problem->is_time_dependent() || assemble_mass)
				{
					StiffnessMatrix velocity_mass;
					assembler.assemble_mass_matrix(formulation(), mesh->is_volume(), n_bases, true, bases, geom_bases(), mass_ass_vals_cache, velocity_mass);

					std::vector<Eigen::Triplet<double>> mass_blocks;
					mass_blocks.reserve(velocity_mass.nonZeros());

					for (int k = 0; k < velocity_mass.outerSize(); ++k)
					{
						for (StiffnessMatrix::InnerIterator it(velocity_mass, k); it; ++it)
						{
							mass_blocks.emplace_back(it.row(), it.col(), it.value());
						}
					}

					mass.resize(stiffness.rows(), stiffness.cols());
					mass.setFromTriplets(mass_blocks.begin(), mass_blocks.end());
					mass.makeCompressed();
				}
			}
		}
		else
		{
			if (!is_contact_enabled()) // collisions are non-linear
				assembler.assemble_problem(formulation(), mesh->is_volume(), n_bases, bases, geom_bases(), ass_vals_cache, stiffness);
			if (problem->is_time_dependent() || assemble_mass)
			{
				assembler.assemble_mass_matrix(formulation(), mesh->is_volume(), n_bases, true, bases, geom_bases(), mass_ass_vals_cache, mass);
			}
		}

		if (mass.size() > 0)
		{
			for (int k = 0; k < mass.outerSize(); ++k)
			{

				for (StiffnessMatrix::InnerIterator it(mass, k); it; ++it)
				{
					assert(it.col() == k);
					avg_mass += it.value();
				}
			}

			avg_mass /= mass.rows();
			logger().info("average mass {}", avg_mass);

			if (args["solver"]["advanced"]["lump_mass_matrix"])
			{
				mass = lump_matrix(mass);
			}
		}

		timer.stop();
		timings.assembling_stiffness_mat_time = timer.getElapsedTime();
		logger().info(" took {}s", timings.assembling_stiffness_mat_time);

		stats.nn_zero = stiffness.nonZeros();
		stats.num_dofs = stiffness.rows();
		stats.mat_size = (long long)stiffness.rows() * (long long)stiffness.cols();
		logger().info("sparsity: {}/{}", stats.nn_zero, stats.mat_size);
	}

	std::shared_ptr<RhsAssembler> State::build_rhs_assembler(
		const int n_bases_,
		const std::vector<basis::ElementBases> &bases_,
		const assembler::AssemblyValsCache &ass_vals_cache_) const
	{
		json rhs_solver_params = args["solver"]["linear"];
		if (!rhs_solver_params.contains("Pardiso"))
			rhs_solver_params["Pardiso"] = {};
		rhs_solver_params["Pardiso"]["mtype"] = -2; // matrix type for Pardiso (2 = SPD)

		const int size = problem->is_scalar() ? 1 : mesh->dimension();

		return std::make_shared<RhsAssembler>(
			assembler, *mesh, obstacle,
			dirichlet_nodes, neumann_nodes,
			dirichlet_nodes_position, neumann_nodes_position,
			n_bases_, size, bases_, geom_bases(), ass_vals_cache_, formulation(), *problem,
			args["space"]["advanced"]["bc_method"], args["solver"]["linear"]["solver"], args["solver"]["linear"]["precond"], rhs_solver_params);
	}

	void State::assemble_rhs()
	{
		if (!mesh)
		{
			logger().error("Load the mesh first!");
			return;
		}
		if (n_bases <= 0)
		{
			logger().error("Build the bases first!");
			return;
		}

		igl::Timer timer;
		// std::string rhs_path = "";
		// if (args["boundary_conditions"]["rhs"].is_string())
		// 	rhs_path = resolve_input_path(args["boundary_conditions"]["rhs"]);

		json p_params = {};
		p_params["formulation"] = formulation();
		p_params["root_path"] = root_path();
		{
			RowVectorNd min, max, delta;
			mesh->bounding_box(min, max);
			delta = (max - min) / 2. + min;
			if (mesh->is_volume())
				p_params["bbox_center"] = {delta(0), delta(1), delta(2)};
			else
				p_params["bbox_center"] = {delta(0), delta(1)};
		}
		problem->set_parameters(p_params);

		// stiffness.resize(0, 0);
		rhs.resize(0, 0);

		timer.start();
		logger().info("Assigning rhs...");

		solve_data.rhs_assembler = build_rhs_assembler();
		solve_data.rhs_assembler->assemble(assembler.density(), rhs);
		rhs *= -1;

		// if(problem->is_mixed())
		if (assembler.is_mixed(formulation()))
		{
			const int prev_size = rhs.rows();
			const int n_larger = n_pressure_bases + (use_avg_pressure ? (assembler.is_fluid(formulation()) ? 1 : 0) : 0);
			rhs.conservativeResize(prev_size + n_larger, rhs.cols());
			if (formulation() == "OperatorSplitting")
			{
				timings.assigning_rhs_time = 0;
				return;
			}
			// Divergence free rhs
			if (formulation() != "Bilaplacian" || local_neumann_boundary.empty())
			{
				rhs.block(prev_size, 0, n_larger, rhs.cols()).setZero();
			}
			else
			{
				Eigen::MatrixXd tmp(n_pressure_bases, 1);
				tmp.setZero();

				std::shared_ptr<RhsAssembler> tmp_rhs_assembler = build_rhs_assembler(
					n_pressure_bases, pressure_bases, pressure_ass_vals_cache);

				tmp_rhs_assembler->set_bc(std::vector<LocalBoundary>(), std::vector<int>(), n_boundary_samples(), local_neumann_boundary, tmp);
				rhs.block(prev_size, 0, n_larger, rhs.cols()) = tmp;
			}
		}

		timer.stop();
		timings.assigning_rhs_time = timer.getElapsedTime();
		logger().info(" took {}s", timings.assigning_rhs_time);
	}

	void State::solve_problem(Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure)
	{
		if (!mesh)
		{
			logger().error("Load the mesh first!");
			return;
		}
		if (n_bases <= 0)
		{
			logger().error("Build the bases first!");
			return;
		}

		if (assembler.is_linear(formulation()) && !is_contact_enabled() && stiffness.rows() <= 0)
		{
			logger().error("Assemble the stiffness matrix first!");
			return;
		}

		// sol.resize(0, 0);
		// pressure.resize(0, 0);
		stats.spectrum.setZero();

		igl::Timer timer;
		timer.start();
		logger().info("Solving {}", formulation());

		const std::string full_mat_path = args["output"]["data"]["full_mat"];
		if (!full_mat_path.empty())
		{
			Eigen::saveMarket(stiffness, full_mat_path);
		}

		init_solve(sol, pressure);

		if (problem->is_time_dependent())
		{
			const double t0 = args["time"]["t0"];
			const int time_steps = args["time"]["time_steps"];
			const double dt = args["time"]["dt"];

			// Pre log the output path for easier watching
			if (args["output"]["advanced"]["save_time_sequence"])
			{
				logger().info("Time sequence of simulation will be written to: {}",
							  resolve_output_path(args["output"]["paraview"]["file_name"]));
			}

			if (formulation() == "NavierStokes")
				solve_transient_navier_stokes(time_steps, t0, dt, sol, pressure);
			else if (formulation() == "OperatorSplitting")
				solve_transient_navier_stokes_split(time_steps, dt, sol, pressure);
			else if (assembler.is_linear(formulation()) && !is_contact_enabled()) // Collisions add nonlinearity to the problem
				solve_transient_linear(time_steps, t0, dt, sol, pressure);
			else if (!assembler.is_linear(formulation()) && problem->is_scalar())
				throw std::runtime_error("Nonlinear scalar problems are not supported yet!");
			else
				solve_transient_tensor_nonlinear(time_steps, t0, dt, sol);
		}
		else
		{
			if (formulation() == "NavierStokes")
				solve_navier_stokes(sol, pressure);
			else if (assembler.is_linear(formulation()) && !is_contact_enabled())
			{
				init_linear_solve(sol);
				solve_linear(sol, pressure);
				if (args["optimization"]["enabled"])
					cache_transient_adjoint_quantities(0, sol);
			}
			else if (!assembler.is_linear(formulation()) && problem->is_scalar())
				throw std::runtime_error("Nonlinear scalar problems are not supported yet!");
			else
			{
				init_nonlinear_tensor_solve(sol);
				solve_tensor_nonlinear(sol);
				if (args["optimization"]["enabled"])
					cache_transient_adjoint_quantities(0, sol);
				const std::string u_path = resolve_output_path(args["output"]["data"]["u_path"]);
				if (!u_path.empty())
					write_matrix(u_path, sol);
			}
		}

		timer.stop();
		timings.solving_time = timer.getElapsedTime();
		logger().info(" took {}s", timings.solving_time);
	}

} // namespace polyfem

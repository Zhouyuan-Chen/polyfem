#pragma once

#include <polyfem/solver/forms/adjoint_forms/ParametrizationForm.hpp>
#include "VariableToSimulation.hpp"

#include <polyfem/solver/forms/ContactForm.hpp>
#include <polyfem/utils/BoundarySampler.hpp>

namespace polyfem::solver
{
	class CollisionBarrierForm : public AdjointForm
	{
	public:
		CollisionBarrierForm(const std::vector<std::shared_ptr<VariableToSimulation>> variable_to_simulation, const State &state, const double dhat, const double dmin = 0) : AdjointForm(variable_to_simulation), state_(state), dhat_(dhat), dmin_(dmin)
		{
			build_collision_mesh();

			Eigen::MatrixXd V;
			state_.get_vertices(V);
			X_init = utils::flatten(V);

			broad_phase_method_ = ipc::BroadPhaseMethod::HASH_GRID;
		}

		double value_unweighted(const Eigen::VectorXd &x) const override
		{
			const Eigen::MatrixXd displaced_surface = collision_mesh_.vertices(utils::unflatten(get_updated_mesh_nodes(x), state_.mesh->dimension()));

			return constraint_set.compute_potential(collision_mesh_, displaced_surface, dhat_);
		}

		void compute_partial_gradient_unweighted(const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const override
		{
			const Eigen::MatrixXd displaced_surface = collision_mesh_.vertices(utils::unflatten(get_updated_mesh_nodes(x), state_.mesh->dimension()));

			Eigen::VectorXd grad = collision_mesh_.to_full_dof(constraint_set.compute_potential_gradient(collision_mesh_, displaced_surface, dhat_));

			gradv.setZero(x.size());
			for (auto &p : variable_to_simulations_)
			{
				for (const auto &state : p->get_states())
					if (state.get() != &state_)
						continue;
				if (p->get_parameter_type() != ParameterType::Shape)
					continue;
				gradv += p->apply_parametrization_jacobian(grad, x);
			}
		}

		void solution_changed(const Eigen::VectorXd &x) override
		{
			AdjointForm::solution_changed(x);

			const Eigen::MatrixXd displaced_surface = collision_mesh_.vertices(utils::unflatten(get_updated_mesh_nodes(x), state_.mesh->dimension()));
			build_constraint_set(displaced_surface);
		}

		Eigen::MatrixXd compute_adjoint_rhs_unweighted(const Eigen::VectorXd &x, const State &state) const override
		{
			return Eigen::MatrixXd::Zero(state.ndof(), state.diff_cached.size());
		}

		bool is_step_collision_free(const Eigen::VectorXd &x0, const Eigen::VectorXd &x1) const override
		{
			const Eigen::MatrixXd V0 = utils::unflatten(get_updated_mesh_nodes(x0), state_.mesh->dimension());
			const Eigen::MatrixXd V1 = utils::unflatten(get_updated_mesh_nodes(x1), state_.mesh->dimension());

			// Skip CCD if the displacement is zero.
			if ((V1 - V0).lpNorm<Eigen::Infinity>() == 0.0)
				return true;

			bool is_valid = ipc::is_step_collision_free(
				collision_mesh_,
				collision_mesh_.vertices(V0),
				collision_mesh_.vertices(V1),
				broad_phase_method_,
				dmin_, 1e-6, 1e6);

			return is_valid;
		}

		double max_step_size(const Eigen::VectorXd &x0, const Eigen::VectorXd &x1) const override
		{
			const Eigen::MatrixXd V0 = utils::unflatten(get_updated_mesh_nodes(x0), state_.mesh->dimension());
			const Eigen::MatrixXd V1 = utils::unflatten(get_updated_mesh_nodes(x1), state_.mesh->dimension());

			double max_step = ipc::compute_collision_free_stepsize(
				collision_mesh_,
				collision_mesh_.vertices(V0),
				collision_mesh_.vertices(V1),
				broad_phase_method_, dmin_, 1e-6, 1e6);

			return max_step;
		}

	protected:
		virtual void build_collision_mesh()
		{
			state_.build_collision_mesh(collision_mesh_, state_.n_geom_bases, state_.geom_bases());
		};

		void build_constraint_set(const Eigen::MatrixXd &displaced_surface)
		{
			static Eigen::MatrixXd cached_displaced_surface;
			if (cached_displaced_surface.size() == displaced_surface.size() && cached_displaced_surface == displaced_surface)
				return;

			constraint_set.build(collision_mesh_, displaced_surface, dhat_, dmin_, broad_phase_method_);

			cached_displaced_surface = displaced_surface;
		}

		Eigen::VectorXd get_updated_mesh_nodes(const Eigen::VectorXd &x) const
		{
			Eigen::VectorXd X = X_init;

			for (auto &p : variable_to_simulations_)
			{
				for (const auto &state : p->get_states())
					if (state.get() != &state_)
						continue;
				if (p->get_parameter_type() != ParameterType::Shape)
					continue;
				auto state_variable = p->get_parametrization().eval(x);
				auto output_indexing = p->get_output_indexing(x);
				for (int i = 0; i < output_indexing.size(); ++i)
					X(output_indexing(i)) = state_variable(i);
			}

			return AdjointTools::map_primitive_to_node_order(state_, X);
		}

		const State &state_;

		Eigen::VectorXd X_init;

		ipc::CollisionMesh collision_mesh_;
		ipc::CollisionConstraints constraint_set;
		const double dhat_;
		const double dmin_;
		ipc::BroadPhaseMethod broad_phase_method_;
	};

	class LayerThicknessForm : public CollisionBarrierForm
	{
	public:
		LayerThicknessForm(const std::vector<std::shared_ptr<VariableToSimulation>> &variable_to_simulations,
						   const State &state,
						   const std::vector<int> &boundary_ids,
						   const double dhat,
						   const double dmin) : CollisionBarrierForm(variable_to_simulations, state, dhat, dmin),
												boundary_ids_(boundary_ids)
		{
		}

	private:
		virtual void build_collision_mesh() override
		{
			Eigen::MatrixXd node_positions;
			Eigen::MatrixXi boundary_edges, boundary_triangles;
			std::vector<Eigen::Triplet<double>> displacement_map_entries;
			io::OutGeometryData::extract_boundary_mesh(*state_.mesh, state_.n_bases, state_.bases, state_.total_local_boundary,
													   node_positions, boundary_edges, boundary_triangles, displacement_map_entries);

			std::vector<bool> is_on_surface;
			is_on_surface.resize(node_positions.rows(), false);

			assembler::ElementAssemblyValues vals;
			Eigen::MatrixXd points, uv, normals;
			Eigen::VectorXd weights;
			Eigen::VectorXi global_primitive_ids;
			for (const auto &lb : state_.total_local_boundary)
			{
				const int e = lb.element_id();
				bool has_samples = utils::BoundarySampler::boundary_quadrature(lb, state_.n_boundary_samples(), *state_.mesh, false, uv, points, normals, weights, global_primitive_ids);

				if (!has_samples)
					continue;

				const basis::ElementBases &gbs = state_.geom_bases()[e];
				const basis::ElementBases &bs = state_.bases[e];

				vals.compute(e, state_.mesh->is_volume(), points, bs, gbs);

				for (int i = 0; i < lb.size(); ++i)
				{
					const int primitive_global_id = lb.global_primitive_id(i);
					const auto nodes = bs.local_nodes_for_primitive(primitive_global_id, *state_.mesh);
					const int boundary_id = state_.mesh->get_boundary_id(primitive_global_id);

					if (!std::count(boundary_ids_.begin(), boundary_ids_.end(), boundary_id))
						continue;

					for (long n = 0; n < nodes.size(); ++n)
					{
						const assembler::AssemblyValues &v = vals.basis_values[nodes(n)];
						is_on_surface[v.global[0].index] = true;
					}
				}
			}

			Eigen::SparseMatrix<double> displacement_map;
			if (!displacement_map_entries.empty())
			{
				displacement_map.resize(node_positions.rows(), state_.n_bases);
				displacement_map.setFromTriplets(displacement_map_entries.begin(), displacement_map_entries.end());
			}

			collision_mesh_ = ipc::CollisionMesh(is_on_surface,
												 node_positions,
												 boundary_edges,
												 boundary_triangles,
												 displacement_map);

			collision_mesh_.init_area_jacobians();
		}

		std::vector<int> boundary_ids_;
	};
} // namespace polyfem::solver
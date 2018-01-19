#ifndef PROBLEM_HPP
#define PROBLEM_HPP

#include "ElementBases.hpp"
#include "LocalBoundary.hpp"
#include "Mesh.hpp"

#include <vector>
#include <Eigen/Dense>

namespace poly_fem
{
	enum ProblemType
	{
		Linear = 0,
		Quadratic,
		Franke,
		Elastic,
		Zero_BC,
		Franke3d
	};

	class Problem
	{
	public:
		void rhs(const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const;
		void bc(const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const;

		void exact(const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const;

		inline void set_problem_num(const int num) { problem_num_ = num; }
		inline int problem_num() const { return problem_num_; }

		bool has_exact_sol() const;

		void remove_neumann_nodes(const Mesh &mesh, const std::vector< ElementBases > &bases, std::vector< LocalBoundary > &local_boundary, std::vector< int > &boundary_nodes);

	private:
		int problem_num_;
	};
}

#endif //PROBLEM_HPP


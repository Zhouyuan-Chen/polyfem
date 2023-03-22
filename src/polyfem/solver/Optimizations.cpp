#include "Optimizations.hpp"

#include <polyfem/mesh/GeometryReader.hpp>

#include <polyfem/solver/forms/adjoint_forms/SpatialIntegralForms.hpp>
#include <polyfem/solver/forms/adjoint_forms/SumCompositeForm.hpp>
#include <polyfem/solver/forms/adjoint_forms/CompositeForms.hpp>
#include <polyfem/solver/forms/adjoint_forms/TransientForm.hpp>
#include <polyfem/solver/forms/adjoint_forms/SmoothingForms.hpp>

#include <polyfem/solver/forms/parametrization/Parametrizations.hpp>
#include <polyfem/solver/forms/parametrization/SDFParametrizations.hpp>
#include <polyfem/solver/forms/parametrization/NodeCompositeParametrizations.hpp>

namespace polyfem::solver
{
	namespace
	{
		bool load_json(const std::string &json_file, json &out)
		{
			std::ifstream file(json_file);

			if (!file.is_open())
				return false;

			file >> out;

			out["root_path"] = json_file;

			return true;
		}
	} // namespace

	std::shared_ptr<AdjointForm> create_form(const json &args, const std::vector<std::shared_ptr<VariableToSimulation>> &var2sim, const std::vector<std::shared_ptr<State>> &states)
	{
		std::shared_ptr<AdjointForm> obj;
		if (args.is_array())
		{
			std::vector<std::shared_ptr<AdjointForm>> forms;
			for (const auto &arg : args)
				forms.push_back(create_form(arg, var2sim, states));
			
			obj = std::make_shared<SumCompositeForm>(var2sim, forms);
		}
		else
		{
			const std::string type = args["type"];
			if (type == "transient_integral")
			{
				std::shared_ptr<StaticForm> static_obj = std::dynamic_pointer_cast<StaticForm>(create_form(args["static_objective"], var2sim, states));
				const auto &state = states[args["state"]];
				obj = std::make_shared<TransientForm>(var2sim, state->args["time"]["time_steps"], state->args["time"]["dt"], args["integral_type"], static_obj);
			}
			else if (type == "power")
			{
				std::shared_ptr<AdjointForm> obj_aux = create_form(args["objective"], var2sim, states);
				obj = std::make_shared<PowerForm>(obj_aux, args["power"]);
			}
			else if (type == "divide")
			{
				std::shared_ptr<AdjointForm> obj1 = create_form(args["objective"][0], var2sim, states);
				std::shared_ptr<AdjointForm> obj2 = create_form(args["objective"][1], var2sim, states);
				std::vector<std::shared_ptr<AdjointForm>> objs({obj1, obj2});
				obj = std::make_shared<DivideForm>(objs);
			}
			else if (type == "compliance")
			{
				obj = std::make_shared<ComplianceForm>(var2sim, *(states[args["state"]]), args);
			}
			else if (type == "acceleration")
			{
				obj = std::make_shared<AccelerationForm>(var2sim, *(states[args["state"]]), args);
			}
			else if (type == "kinetic")
			{
				obj = std::make_shared<AccelerationForm>(var2sim, *(states[args["state"]]), args);
			}
			else if (type == "target")
			{
				std::shared_ptr<TargetForm> tmp = std::make_shared<TargetForm>(var2sim, *(states[args["state"]]), args);
				auto reference_cached = args["reference_cached_body_ids"].get<std::vector<int>>();
				tmp->set_reference(states[args["target_state"]], std::set(reference_cached.begin(), reference_cached.end()));
				obj = tmp;
			}
			else if (type == "sdf-target")
			{
				log_and_throw_error("Objective not implemented!");
			}
			else if (type == "function-target")
			{
				std::shared_ptr<TargetForm> tmp = std::make_shared<TargetForm>(var2sim, *(states[args["state"]]), args);
				tmp->set_reference(args["target_function"], args["target_function_gradient"]);
				obj = tmp;
			}
			else if (type == "position")
			{
				obj = std::make_shared<PositionForm>(var2sim, *(states[args["state"]]), args);
			}
			else if (type == "stress")
			{
				obj = std::make_shared<StressForm>(var2sim, *(states[args["state"]]), args);
			}
			else if (type == "stress_norm")
			{
				obj = std::make_shared<StressNormForm>(var2sim, *(states[args["state"]]), args);
			}
			else if (type == "max_stress")
			{
				obj = std::make_shared<MaxStressForm>(var2sim, *(states[args["state"]]), args);
			}
			else if (type == "volume")
			{
				obj = std::make_shared<VolumeForm>(var2sim, *(states[args["state"]]), args);
			}
			else if (type == "soft_constraint")
			{
				std::vector<std::shared_ptr<AdjointForm>> forms({create_form(args["objective"], var2sim, states)});
				Eigen::VectorXd bounds;
				nlohmann::adl_serializer<Eigen::VectorXd>::from_json(args["soft_bound"], bounds);
				obj = std::make_shared<InequalityConstraintForm>(forms, bounds, args["power"]);
			}
			else
				log_and_throw_error("Objective not implemented!");

			obj->set_weight(args["weight"]);
		}

		return obj;
	}

	std::shared_ptr<Parametrization> create_parametrization(const json &args, const std::vector<std::shared_ptr<State>> &states)
	{
		std::shared_ptr<Parametrization> map;
		const std::string type = args["type"];
		if (type == "per-body-to-per-elem")
		{
			map = std::make_shared<PerBody2PerElem>(*(states[args["state"]]->mesh));
		}
		else if (type == "E-nu-to-lambda-mu")
		{
			map = std::make_shared<ENu2LambdaMu>(args["is_volume"]);
		}
		else if (type == "slice")
		{
			map = std::make_shared<SliceMap>(args["from"], args["to"]);
		}
		else if (type == "exp")
		{
			map = std::make_shared<ExponentialMap>();
		}
		else if (type == "scale")
		{
			map = std::make_shared<Scaling>(args["value"]);
		}
		else if (type == "power")
		{
			map = std::make_shared<PowerMap>(args["power"]);
		}
		else if (type == "append-values")
		{
			Eigen::VectorXd vals;
			nlohmann::adl_serializer<Eigen::VectorXd>::from_json(args["values"], vals);
			map = std::make_shared<AppendConstantMap>(vals);
		}
		else if (type == "append-const")
		{
			map = std::make_shared<AppendConstantMap>(args["size"], args["value"]);
		}
		else if (type == "linear-filter")
		{
			map = std::make_shared<LinearFilter>(*(states[args["state"]]->mesh), args["radius"]);
		}
		else if (type == "custom-symmetric")
		{
			map = std::make_shared<CustomSymmetric>(args);
		}
		else if (type == "sdf-to-mesh")
		{
			map = std::make_shared<SDF2Mesh>(args["wire_path"], args["output_path"], args["options"]);
		}
		else if (type == "periodic-mesh-tile")
		{
			Eigen::VectorXi dims;
			nlohmann::adl_serializer<Eigen::VectorXi>::from_json(args["dimensions"], dims);
			map = std::make_shared<MeshTiling>(dims, args["input_path"], args["output_path"]);
		}
		else if (type == "mesh-affine")
		{
			MatrixNd A;
			VectorNd b;
			mesh::construct_affine_transformation(
				args["transformation"],
				VectorNd::Ones(args["dimension"]),
				A, b);
			map = std::make_shared<MeshAffine>(A, b, args["input_path"], args["output_path"]);
		}
		else
			log_and_throw_error("Unkown parametrization!");
		
		return map;
	}

	std::shared_ptr<VariableToSimulation> create_variable_to_simulation(const json &args, const std::vector<std::shared_ptr<State>> &states)
	{
		std::shared_ptr<VariableToSimulation> var2sim;
		const std::string type = args["type"];

		std::vector<std::shared_ptr<Parametrization>> map_list;
		for (const auto &arg : args["composition"])
			map_list.push_back(create_parametrization(arg, states));
		CompositeParametrization composite_map(map_list);

		std::vector<std::shared_ptr<State>> cur_states;
		if (args["state"].is_array())
			for (int i : args["state"])
				cur_states.push_back(states[i]);
		else
			cur_states.push_back(states[args["state"]]);

        if (type == "shape")
		{
			var2sim = std::make_shared<ShapeVariableToSimulation>(cur_states, composite_map);
		}
        else if (type == "elastic")
		{
			var2sim = std::make_shared<ElasticVariableToSimulation>(cur_states, composite_map);
		}
        else if (type == "friction")
		{
			var2sim = std::make_shared<FrictionCoeffientVariableToSimulation>(cur_states, composite_map);
		}
        else if (type == "damping")
		{
			var2sim = std::make_shared<DampingCoeffientVariableToSimulation>(cur_states, composite_map);
		}
        else if (type == "macro-strain")
		{
			var2sim = std::make_shared<MacroStrainVariableToSimulation>(cur_states, composite_map);
		}
        else if (type == "initial")
		{
			var2sim = std::make_shared<InitialConditionVariableToSimulation>(cur_states, composite_map);
		}
        else if (type == "sdf-shape")
		{
			var2sim = std::make_shared<SDFShapeVariableToSimulation>(cur_states, composite_map, args);
		}
		else if (type == "dirichlet")
		{
			var2sim = std::make_shared<DirichletVariableToSimulation>(cur_states, composite_map);
		}

		return var2sim;
	}

	std::shared_ptr<State> create_state(const json &args, spdlog::level::level_enum log_level, const int max_threads)
	{
		std::shared_ptr<State> state = std::make_shared<State>();
		state->set_max_threads(max_threads);

		json in_args = args;
		in_args["solver"]["max_threads"] = max_threads;
		{
			auto tmp = R"({
					"output": {
						"log": {
							"level": -1
						}
					}
				})"_json;

			tmp["output"]["log"]["level"] = int(log_level);

			in_args.merge_patch(tmp);
		}

		in_args["optimization"]["enabled"] = true;
		state->init(in_args, false);
		state->load_mesh();
		Eigen::MatrixXd sol, pressure;
		state->build_basis();
		state->assemble_rhs();
		state->assemble_stiffness_mat();

		return state;
	}

	void solve_pde(State &state)
	{
		state.assemble_rhs();
		state.assemble_stiffness_mat();
		Eigen::MatrixXd sol, pressure;
		state.solve_problem(sol, pressure);
	}

	json apply_opt_json_spec(const json &input_args, bool strict_validation)
	{
		json args_in = input_args;

		// CHECK validity json
		json rules;
		jse::JSE jse;
		{
			jse.strict = strict_validation;
			const std::string polyfem_input_spec = POLYFEM_OPT_INPUT_SPEC;
			std::ifstream file(polyfem_input_spec);

			if (file.is_open())
				file >> rules;
			else
			{
				logger().error("unable to open {} rules", polyfem_input_spec);
				throw std::runtime_error("Invald spec file");
			}
		}

		const bool valid_input = jse.verify_json(args_in, rules);

		if (!valid_input)
		{
			logger().error("invalid input json:\n{}", jse.log2str());
			throw std::runtime_error("Invald input json file");
		}

		json args = jse.inject_defaults(args_in, rules);
		return args;
	}

} // namespace polyfem::solver
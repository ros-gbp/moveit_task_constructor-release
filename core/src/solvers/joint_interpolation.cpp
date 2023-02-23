/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2017, Bielefeld University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Bielefeld University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Authors: Robert Haschke
   Desc:    Plan using simple interpolation in joint-space + validity checking
*/

#include <moveit/task_constructor/solvers/joint_interpolation.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/trajectory_processing/time_parameterization.h>

#include <chrono>

namespace moveit {
namespace task_constructor {
namespace solvers {

using namespace trajectory_processing;

JointInterpolationPlanner::JointInterpolationPlanner() {
	auto& p = properties();
	p.declare<double>("max_step", 0.1, "max joint step");
}

void JointInterpolationPlanner::init(const core::RobotModelConstPtr& /*robot_model*/) {}

bool JointInterpolationPlanner::plan(const planning_scene::PlanningSceneConstPtr& from,
                                     const planning_scene::PlanningSceneConstPtr& to,
                                     const moveit::core::JointModelGroup* jmg, double /*timeout*/,
                                     robot_trajectory::RobotTrajectoryPtr& result,
                                     const moveit_msgs::Constraints& /*path_constraints*/) {
	const auto& props = properties();

	// Get maximum joint distance
	double d = 0.0;
	const moveit::core::RobotState& from_state = from->getCurrentState();
	const moveit::core::RobotState& to_state = to->getCurrentState();
	for (const moveit::core::JointModel* jm : from_state.getRobotModel()->getActiveJointModels())
		d = std::max(d, jm->getDistanceFactor() * from_state.distance(to_state, jm));

	result = std::make_shared<robot_trajectory::RobotTrajectory>(from->getRobotModel(), jmg);

	// add first point
	result->addSuffixWayPoint(from->getCurrentState(), 0.0);
	if (from->isStateColliding(from_state, jmg->getName()))
		return false;

	moveit::core::RobotState waypoint(from_state);
	double delta = d < 1e-6 ? 1.0 : props.get<double>("max_step") / d;
	for (double t = delta; t < 1.0; t += delta) {  // NOLINT(clang-analyzer-security.FloatLoopCounter)
		from_state.interpolate(to_state, t, waypoint);
		result->addSuffixWayPoint(waypoint, t);

		if (from->isStateColliding(waypoint, jmg->getName()))
			return false;
	}

	// add goal point
	result->addSuffixWayPoint(to_state, 1.0);
	if (from->isStateColliding(to_state, jmg->getName()))
		return false;

	auto timing = props.get<TimeParameterizationPtr>("time_parameterization");
	timing->computeTimeStamps(*result, props.get<double>("max_velocity_scaling_factor"),
	                          props.get<double>("max_acceleration_scaling_factor"));

	return true;
}

bool JointInterpolationPlanner::plan(const planning_scene::PlanningSceneConstPtr& from,
                                     const moveit::core::LinkModel& link, const Eigen::Isometry3d& offset,
                                     const Eigen::Isometry3d& target, const moveit::core::JointModelGroup* jmg,
                                     double timeout, robot_trajectory::RobotTrajectoryPtr& result,
                                     const moveit_msgs::Constraints& path_constraints) {
	const auto start_time = std::chrono::steady_clock::now();

	auto to{ from->diff() };

	kinematic_constraints::KinematicConstraintSet constraints{ to->getRobotModel() };
	constraints.add(path_constraints, from->getTransforms());

	auto is_valid{ [&constraints, &to](moveit::core::RobotState* robot_state, const moveit::core::JointModelGroup* jmg,
		                                const double* joint_values) -> bool {
		robot_state->setJointGroupPositions(jmg, joint_values);
		robot_state->update();
		return to->isStateValid(*robot_state, constraints, jmg->getName());
	} };

	if (!to->getCurrentStateNonConst().setFromIK(jmg, target * offset.inverse(), link.getName(), timeout, is_valid)) {
		// TODO(v4hn): planners need a way to add feedback to failing plans
		// in case of an invalid solution feedback should include unwanted collisions or violated constraints
		ROS_WARN_NAMED("JointInterpolationPlanner", "IK failed for pose target");
		return false;
	}
	to->getCurrentStateNonConst().update();

	timeout = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
	if (timeout <= 0.0)
		return false;

	return plan(from, to, jmg, timeout, result, path_constraints);
}
}  // namespace solvers
}  // namespace task_constructor
}  // namespace moveit

// Copyright 2021 Tier IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <scene_module/occlusion_spot/occlusion_spot_utils.hpp>
#include <scene_module/occlusion_spot/risk_predictive_braking.hpp>
#include <utilization/util.hpp>

#include <algorithm>
#include <vector>

namespace behavior_velocity_planner
{
namespace occlusion_spot_utils
{
void applySafeVelocityConsideringPossibleCollision(
  PathWithLaneId * inout_path, std::vector<PossibleCollisionInfo> & possible_collisions,
  const PlannerParam & param)
{
  const auto logger{rclcpp::get_logger("behavior_velocity_planner").get_child("occlusion_spot")};
  rclcpp::Clock clock{RCL_ROS_TIME};
  // return nullptr or too few points
  if (!inout_path || inout_path->points.size() < 2) {
    return;
  }
  const double v0 = param.v.v_ego;
  const double a0 = param.v.a_ego;
  const double j_min = param.v.max_slow_down_jerk;
  const double a_min = param.v.max_slow_down_accel;
  const double v_min = param.v.min_allowed_velocity;
  for (auto & possible_collision : possible_collisions) {
    const double l_obs = possible_collision.arc_lane_dist_at_collision.length;
    const double original_vel = possible_collision.collision_with_margin.longitudinal_velocity_mps;

    // safe velocity : Consider ego emergency braking deceleration
    const double v_safe = possible_collision.obstacle_info.safe_motion.safe_velocity;

    // min allowed velocity : min allowed velocity consider maximum allowed braking
    const double v_slow_down =
      (l_obs < 0 && v0 <= v_safe)
        ? v_safe
        : planning_utils::calcDecelerationVelocityFromDistanceToTarget(j_min, a_min, a0, v0, l_obs);
    // compare safe velocity consider EBS, minimum allowed velocity and original velocity
    const double safe_velocity = calculateInsertVelocity(v_slow_down, v_safe, v_min, original_vel);
    possible_collision.obstacle_info.safe_motion.safe_velocity = safe_velocity;
    const auto & pose = possible_collision.collision_with_margin.pose;
    insertSafeVelocityToPath(pose, safe_velocity, param, inout_path);
  }
}

bool isAheadOf(const geometry_msgs::msg::Pose & target, const geometry_msgs::msg::Pose & origin)
{
  geometry_msgs::msg::Pose p = planning_utils::transformRelCoordinate2D(target, origin);
  bool is_target_ahead = (p.position.x > 0.0);
  return is_target_ahead;
}

bool setVelocityFrom(const size_t idx, const double vel, PathWithLaneId * input)
{
  for (size_t i = idx; i < input->points.size(); ++i) {
    input->points.at(i).point.longitudinal_velocity_mps =
      std::min(static_cast<float>(vel), input->points.at(i).point.longitudinal_velocity_mps);
  }
  return true;
}

int insertSafeVelocityToPath(
  const geometry_msgs::msg::Pose & in_pose, const double safe_vel, const PlannerParam & param,
  PathWithLaneId * inout_path)
{
  int closest_idx = -1;
  if (!planning_utils::calcClosestIndex(
        *inout_path, in_pose, closest_idx, param.dist_thr, param.angle_thr)) {
    return -1;
  }
  PathPointWithLaneId inserted_point;
  inserted_point = inout_path->points.at(closest_idx);
  int insert_idx = closest_idx;
  // insert velocity to path if distance is not too close else insert new collision point
  // if original path has narrow points it's better to set higher distance threshold
  if (planning_utils::calcDist2d(in_pose, inserted_point.point) > 0.3) {
    if (isAheadOf(in_pose, inout_path->points.at(closest_idx).point.pose)) {
      ++insert_idx;
    }
    // return if index is after the last path point
    if (insert_idx == static_cast<int>(inout_path->points.size())) {
      return -1;
    }
    auto it = inout_path->points.begin() + insert_idx;
    inserted_point = inout_path->points.at(closest_idx);
    inserted_point.point.pose = in_pose;
    inout_path->points.insert(it, inserted_point);
  }
  setVelocityFrom(insert_idx, safe_vel, inout_path);
  return 0;
}

double calculateLateralDistanceFromTTC(
  const double longitudinal_distance, const PlannerParam & param)
{
  const auto & v = param.v;
  const auto & p = param;
  double v_min = 1.0;
  const double lateral_buffer = 0.5;
  const double min_distance = p.half_vehicle_width + lateral_buffer;
  const double max_distance = p.detection_area.max_lateral_distance;
  if (longitudinal_distance <= 0) return min_distance;
  if (v_min < param.v.min_allowed_velocity) v_min = param.v.min_allowed_velocity;
  // use min velocity if ego velocity is below min allowed
  const double v0 = (v.v_ego > v_min) ? v.v_ego : v_min;
  // here is a part where ego t(ttc) can be replaced by calculation of velocity smoother or ?
  double t = longitudinal_distance / v0;
  double lateral_distance = t * param.pedestrian_vel + p.half_vehicle_width;
  return std::min(max_distance, std::max(min_distance, lateral_distance));
}

SafeMotion calculateSafeMotion(const Velocity & v, const double ttc)
{
  SafeMotion sm;
  const double j_max = v.safety_ratio * v.max_stop_jerk;
  const double a_max = v.safety_ratio * v.max_stop_accel;
  const double t1 = v.delay_time;
  double t2 = a_max / j_max;
  double & v_safe = sm.safe_velocity;
  double & stop_dist = sm.stop_dist;
  if (ttc <= t1) {
    // delay
    v_safe = 0;
    stop_dist = 0;
  } else if (ttc <= t2 + t1) {
    // delay + const jerk
    t2 = ttc - t1;
    v_safe = -0.5 * j_max * t2 * t2;
    stop_dist = v_safe * t1 - j_max * t2 * t2 * t2 / 6;
  } else {
    const double t3 = ttc - t2 - t1;
    // delay + const jerk + const accel
    const double v2 = -0.5 * j_max * t2 * t2;
    v_safe = v2 - a_max * t3;
    stop_dist = v_safe * t1 - j_max * t2 * t2 * t2 / 6 + v2 * t3 - 0.5 * a_max * t3 * t3;
  }
  stop_dist += v.safe_margin;
  return sm;
}

double calculateInsertVelocity(
  const double min_allowed_vel, const double safe_vel, const double min_vel,
  const double original_vel)
{
  const double max_vel_noise = 0.05;
  // ensure safe velocity doesn't exceed maximum allowed pbs deceleration
  double cmp_safe_vel = std::max(min_allowed_vel + max_vel_noise, safe_vel);
  // ensure safe path velocity is also above ego min velocity
  cmp_safe_vel = std::max(cmp_safe_vel, min_vel);
  // ensure we only lower the original velocity (and do not increase it)
  cmp_safe_vel = std::min(cmp_safe_vel, original_vel);
  return cmp_safe_vel;
}
}  // namespace occlusion_spot_utils
}  // namespace behavior_velocity_planner

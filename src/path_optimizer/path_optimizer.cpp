//
// Created by ljn on 19-8-16.
//

#include <path_optimizer/solver/solver_kp_as_input_constrained.hpp>
#include "path_optimizer/path_optimizer.hpp"
#include "path_optimizer/reference_path_smoother/reference_path_smoother.hpp"
#include "path_optimizer/reference_path_smoother/frenet_reference_path_smoother.hpp"
#include "path_optimizer/reference_path_smoother/cartesian_reference_path_smoother.hpp"
#include "path_optimizer/solver/solver_k_as_input.hpp"
#include "path_optimizer/solver/solver_kp_as_input.hpp"
#include "path_optimizer/solver/solver_kp_as_input_constrained.hpp"
#include "path_optimizer/tools/tools.hpp"

namespace PathOptimizationNS {

PathOptimizer::PathOptimizer(const State &start_state,
                             const State &end_state,
                             const grid_map::GridMap &map) :
    grid_map_(map),
    collision_checker_(map),
    vehicle_state_(start_state, end_state, 0, 0),
    N_(0) {
    setConfig();
    collision_checker_.init(config_);
}

void PathOptimizer::setConfig() {
    // TODO: read from config file.
    config_.car_type_ = ACKERMANN_STEERING;
    config_.car_width_ = 2.0; //1.6;//
    config_.car_length_ = 4.9; //4.0;//
    config_.circle_radius_ = sqrt(pow(config_.car_length_ / 8, 2) + pow(config_.car_width_ / 2, 2));
    config_.wheel_base_ = 2.85; //2.35;
    config_.rear_axle_to_center_distance_ = 1.45; //1.0;
    config_.d1_ = -3.0 / 8.0 * config_.car_length_;
    config_.d2_ = -1.0 / 8.0 * config_.car_length_;
    config_.d3_ = 1.0 / 8.0 * config_.car_length_;
    config_.d4_ = 3.0 / 8.0 * config_.car_length_;
    config_.max_steer_angle_ = 30 * M_PI / 180;
    config_.max_curvature_rate_ = 0.1; // TODO: verify this.

    config_.smoothing_method_ = FRENET;
    config_.modify_input_points_ = true;
    config_.a_star_lateral_range_ = 10;
    config_.a_star_longitudinal_interval_ = 1.5;
    config_.a_star_lateral_interval_ = 0.6;
    config_.mu_ = 0.4;

    //
    config_.frenet_curvature_rate_w_ = 1500;
    config_.frenet_curvature_w_ = 200;
    config_.frenet_deviation_w_ = 4;
    //
    config_.cartesian_curvature_w_ = 10;
    config_.cartesian_deviation_w_ = 0.001;
    //
    config_.opt_curvature_w_ = 10;
    config_.opt_curvature_rate_w_ = 200;
    config_.opt_deviation_w_ = 0;
    config_.opt_bound_slack_w_ = 3;
    config_.constraint_end_heading_ = true;
    // TODO: use this condition.
    config_.exact_end_position_ = false;
    config_.expected_safety_margin_ = 1.3;

    //
    config_.raw_result_ = true;
    config_.output_interval_ = 0.3;
    config_.info_output_ = true;
}

bool PathOptimizer::solve(const std::vector<State> &reference_points, std::vector<State> *final_path) {
    std::cout << "------" << std::endl;
    if (!final_path) {
        LOG(WARNING) << "[PathOptimizer] No place for result!";
        return false;
    }
    auto t1 = std::clock();
    if (reference_points.empty()) {
        LOG(WARNING) << "[PathOptimizer] empty input, quit path optimization";
        return false;
    }
    // Smooth reference path.
    ReferencePathSmoother
        reference_path_smoother(reference_points, vehicle_state_.start_state_, grid_map_, config_);
    bool smoothing_ok = false;
    if (config_.smoothing_method_ == FRENET) {
        smoothing_ok = reference_path_smoother.solve<FrenetReferencePathSmoother>(&reference_path_, &smoothed_path_);
    } else if (config_.smoothing_method_ == CARTESIAN) {
        smoothing_ok = reference_path_smoother.solve<CartesianReferencePathSmoother>(&reference_path_, &smoothed_path_);
    }
    a_star_display_ = reference_path_smoother.display();
    if (!smoothing_ok) {
        LOG(WARNING) << "[PathOptimizer] Reference Smoothing failed!";
        return false;
    }
    auto t2 = std::clock();
    // Divide reference path into segments;
    if (!divideSmoothedPath()) {
        printf("divide stage failed, quit path optimization.\n");
        LOG(WARNING) << "[PathOptimizer] Reference path segmentation failed!";
        return false;
    }
    auto t3 = std::clock();
    // Optimize.
    if (optimizePath(final_path)) {
        auto t4 = std::clock();
        if (config_.info_output_) {
            time_ms_out(t1, t2, "Reference smoothing");
            time_ms_out(t2, t3, "Reference segmentation");
            time_ms_out(t3, t4, "Optimization phase");
            time_ms_out(t1, t4, "All");
        }
        LOG(INFO) << "[PathOptimizer] Solved!";
        delete reference_path_.reference_states;
        return true;
    } else {
        LOG(WARNING) << "[PathOptimizer] Failed!";
        delete reference_path_.reference_states;
        return false;
    }
}

bool PathOptimizer::solveWithoutSmoothing(const std::vector<PathOptimizationNS::State> &reference_points,
                                          std::vector<PathOptimizationNS::State> *final_path) {
    std::cout << "------" << std::endl;
    if (!final_path) {
        LOG(WARNING) << "[PathOptimizer] No place for result!";
        return false;
    }
    auto t1 = std::clock();
    if (reference_points.empty()) {
        LOG(WARNING) << "[PathOptimizer] Empty input, quit path optimization!";
        return false;
    }
    vehicle_state_.initial_heading_error_ = vehicle_state_.initial_offset_ = 0;
    reference_path_.reference_states = &reference_points;
    reference_path_.updateBounds(grid_map_, config_);
    reference_path_.updateLimits(config_);
    N_ = reference_path_.bounds.size();
    if (optimizePath(final_path)) {
        auto t2 = std::clock();
        if (config_.info_output_) {
            time_ms_out(t1, t2, "Solve without smoothing");
        }
        LOG(INFO) << "[PathOptimizer] Solved without smoothing!";
        return true;
    } else {
        LOG(WARNING) << "[PathOptimizer] Solving without smoothing failed!";
        return false;
    }
}

bool PathOptimizer::divideSmoothedPath() {
    if (reference_path_.max_s_ == 0) {
        LOG(INFO) << "[PathOptimizer] Smoothed path is empty!";
        return false;
    }
    // Calculate the initial deviation and angle difference.
    State first_point;
    first_point.x = reference_path_.x_s_(0);
    first_point.y = reference_path_.y_s_(0);
    first_point.z = getHeading(reference_path_.x_s_, reference_path_.y_s_, 0);
    auto first_point_local = global2Local(vehicle_state_.start_state_, first_point);
    double min_distance = distance(vehicle_state_.start_state_, first_point);
    if (first_point_local.y < 0) {
        vehicle_state_.initial_offset_ = min_distance;
    } else {
        vehicle_state_.initial_offset_ = -min_distance;
    }
    vehicle_state_.initial_heading_error_ = constraintAngle(vehicle_state_.start_state_.z - first_point.z);
    // If the start heading differs a lot with the ref path, quit.
    if (fabs(vehicle_state_.initial_heading_error_) > 75 * M_PI / 180) {
        LOG(WARNING) << "[PathOptimizer] Initial epsi is larger than 90°, quit path optimization!";
        return false;
    }

    double end_distance =
        sqrt(pow(vehicle_state_.end_state_.x - reference_path_.x_s_(reference_path_.max_s_), 2) +
            pow(vehicle_state_.end_state_.y - reference_path_.y_s_(reference_path_.max_s_), 2));
    if (end_distance > 0.001) {
        // If the goal position is not the same as the end position of the reference line,
        // then find the closest point to the goal and change max_s of the reference line.
        double search_delta_s = 0;
        if (config_.exact_end_position_) {
            search_delta_s = 0.1;
        } else {
            search_delta_s = 0.3;
        }
        double tmp_s = reference_path_.max_s_ - search_delta_s;
        auto min_dis_to_goal = end_distance;
        double min_dis_s = reference_path_.max_s_;
        while (tmp_s > 0) {
            double x = reference_path_.x_s_(tmp_s);
            double y = reference_path_.y_s_(tmp_s);
            double tmp_dis = sqrt(pow(x - vehicle_state_.end_state_.x, 2) + pow(y - vehicle_state_.end_state_.y, 2));
            if (tmp_dis < min_dis_to_goal) {
                min_dis_to_goal = tmp_dis;
                min_dis_s = tmp_s;
            }
            tmp_s -= search_delta_s;
        }
        reference_path_.max_s_ = min_dis_s;
    }

    // Divide the reference path. Intervals are smaller at the beginning.
    double delta_s_smaller = 0.3;
    // If we want to make the result path dense later, the interval here is 1.0m. This makes computation faster;
    // If we want to output the result directly, the interval is controlled by config_.output_interval..
    double delta_s_larger = config_.raw_result_ ? config_.output_interval_ : 1.0;
    // If the initial heading error with the reference path is small, then set intervals equal.
    if (fabs(vehicle_state_.initial_heading_error_) < 20 * M_PI / 180) delta_s_smaller = delta_s_larger;

    auto reference_states = new std::vector<State>;
    double tmp_s = 0;
    while (tmp_s <= reference_path_.max_s_) {
        double x = reference_path_.x_s_(tmp_s);
        double y = reference_path_.y_s_(tmp_s);
        double h = getHeading(reference_path_.x_s_, reference_path_.y_s_, tmp_s);
        double k = getCurvature(reference_path_.x_s_, reference_path_.y_s_, tmp_s);
        reference_states->emplace_back(x, y, h, k, tmp_s);
        if (tmp_s <= 2) tmp_s += delta_s_smaller;
        else tmp_s += delta_s_larger;
    }
    reference_path_.reference_states = reference_states;
    reference_path_.updateBounds(grid_map_, config_);
    if (reference_path_.reference_states->size() != reference_path_.bounds.size()) {
        reference_states->resize(reference_path_.bounds.size());
    }
    N_ = reference_path_.bounds.size();
    reference_path_.max_k_list.clear();
    reference_path_.max_kp_list.clear();
    for (size_t i = 0; i != N_; ++i) {
        reference_path_.max_k_list.emplace_back(tan(config_.max_steer_angle_) / config_.wheel_base_);
        reference_path_.max_kp_list.emplace_back(DBL_MAX);
    }
    return true;
}

bool PathOptimizer::optimizePath(std::vector<State> *final_path) {
    // Solve problem.
    SolverKpAsInputConstrained solver_interface(config_,
                                     reference_path_,
                                     vehicle_state_,
                                     N_);
    if (!solver_interface.solve(final_path)) {
        return false;
    }

    // Output. Choose from:
    // 1. set the interval smaller and output the result directly.
    // 2. set the interval larger and use interpolation to make the result dense.
    if (config_.raw_result_) {
        for (auto iter = final_path->begin(); iter != final_path->end(); ++iter) {
            if (!collision_checker_.isSingleStateCollisionFreeImproved(*iter)) {
                final_path->erase(iter, final_path->end());
                LOG(INFO) << "[PathOptimizer] collision checker failed at " << final_path->back().s << "m.";
                return final_path->back().s >= 20;
            }
        }
        return true;
    } else {
        std::vector<double> result_x, result_y, result_s;
        for (const auto &p : *final_path) {
            result_x.emplace_back(p.x);
            result_y.emplace_back(p.y);
            result_s.emplace_back(p.s);
        }
        tk::spline x_s, y_s;
        x_s.set_points(result_s, result_x);
        y_s.set_points(result_s, result_y);
        final_path->clear();
        double delta_s = config_.output_interval_;
        for (int i = 0; i * delta_s <= result_s.back(); ++i) {
            double tmp_s = i * delta_s;
            State tmp_state{x_s(tmp_s),
                            y_s(tmp_s),
                            getHeading(x_s, y_s, tmp_s),
                            getCurvature(x_s, y_s, tmp_s),
                            tmp_s};
            if (collision_checker_.isSingleStateCollisionFreeImproved(tmp_state)) {
                final_path->emplace_back(tmp_state);
            } else {
                LOG(INFO) << "[PathOptimizer] collision checker failed at " << final_path->back().s << "m.";
                return final_path->back().s >= 20;
            }
        }
    }
    return true;
}

const std::vector<State> &PathOptimizer::getRearBounds() const {
    return this->rear_bounds_;
}

const std::vector<State> &PathOptimizer::getCenterBounds() const {
    return this->center_bounds_;
}

const std::vector<State> &PathOptimizer::getFrontBounds() const {
    return this->front_bounds_;
}

const std::vector<State> &PathOptimizer::getSmoothedPath() const {
    return this->smoothed_path_;
}
}

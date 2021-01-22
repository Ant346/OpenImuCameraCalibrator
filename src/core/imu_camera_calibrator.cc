/* Copyright (C) 2021 Steffen Urban
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "OpenCameraCalibrator/core/imu_camera_calibrator.h"

using namespace theia;

namespace OpenICC {
namespace core {

void ImuCameraCalibrator::InitSpline(
    const theia::Reconstruction &calib_dataset,
    const Sophus::SE3<double> &T_i_c_init,
    const SplineWeightingData &spline_weight_data,
    const double time_offset_imu_to_cam, const Eigen::Vector3d &gyro_bias,
    const Eigen::Vector3d &accl_bias,
    const OpenICC::CameraTelemetryData &telemetry_data) {

  spline_weight_data_ = spline_weight_data;

  T_i_c_init_ = T_i_c_init;
  const auto &view_ids = calib_dataset.ViewIds();
  // get all timestamps and find smallest one
  // Output each camera.
  for (const ViewId view_id : view_ids) {
    cam_timestamps_.push_back(calib_dataset.View(view_id)->GetTimestamp());
  }
  std::sort(cam_timestamps_.begin(), cam_timestamps_.end());

  // initialize readout with 1/fps * 1/image_rows
  if (calibrate_cam_line_delay_) {
    inital_cam_line_delay_s_ =
        (1. / spline_weight_data.cam_fps) *
        (1. / calib_dataset.View(view_ids[0])->Camera().ImageHeight());
  } else {
    // this will set it constant 0.0 during optimization
    inital_cam_line_delay_s_ = 0.0;
  }
  trajectory_.SetInitialRSLineDelay(inital_cam_line_delay_s_);

  std::cout << "Initialized Line Delay to: " << inital_cam_line_delay_s_ * 1e6
            << "ns\n";

  // find smallest timestamp
  auto result =
      std::minmax_element(cam_timestamps_.begin(), cam_timestamps_.end());
  t0_s_ = cam_timestamps_[result.first - cam_timestamps_.begin()];
  tend_s_ = cam_timestamps_[result.second - cam_timestamps_.begin()];
  const int64_t start_t_ns = t0_s_ * 1e9;
  const int64_t end_t_ns = tend_s_ * 1e9;
  const int64_t dt_so3_ns = spline_weight_data_.dt_so3 * 1e9;
  const int64_t dt_r3_ns = spline_weight_data_.dt_r3 * 1e9;
  LOG(INFO) << "Spline initialized with. Start/End: " << t0_s_ << "/" << tend_s_
            << " knots spacing r3/so3: " << spline_weight_data_.dt_r3 << "/"
            << spline_weight_data_.dt_so3;

  trajectory_.init_times(dt_so3_ns, dt_r3_ns, start_t_ns);
  trajectory_.setCalib(calib_dataset);
  trajectory_.setT_i_c(T_i_c_init);

  for (const ViewId view_id : view_ids) {
    const View &view = *calib_dataset.View(view_id);
    double timestamp = view.GetTimestamp();
    if (timestamp >= tend_s_ || timestamp < t0_s_)
      continue;
    TimeCamId t_c_id(timestamp * 1e9, 0);
    CalibCornerData corner_data;
    const std::vector<theia::TrackId> trackIds = view.TrackIds();
    for (size_t t = 0; t < trackIds.size(); ++t) {
      corner_data.corners.push_back(*view.GetFeature(trackIds[t]));
    }
    corner_data.track_ids = trackIds;
    calib_corners_[t_c_id] = corner_data;
    CalibInitPoseData pose_data;
    pose_data.T_a_c = Sophus::SE3<double>(
        view.Camera().GetOrientationAsRotationMatrix().transpose(),
        view.Camera().GetPosition());
    calib_init_poses_[t_c_id] = pose_data;

    Sophus::SE3d T_w_i_init =
        calib_init_poses_.at(t_c_id).T_a_c * T_i_c_init.inverse();
    CalibInitPoseData spline_pose_data;
    spline_pose_data.T_a_c = T_w_i_init;
    spline_init_poses_[t_c_id] = spline_pose_data;
  }

  nr_knots_so3_ = (end_t_ns - start_t_ns) / dt_so3_ns + SPLINE_N;
  nr_knots_r3_ = (end_t_ns - start_t_ns) / dt_r3_ns + SPLINE_N;

  std::cout << "Initializing " << nr_knots_so3_ << " SO3 knots.\n";
  std::cout << "Initializing " << nr_knots_r3_ << " R3 knots.\n";

  trajectory_.initAll(spline_init_poses_, nr_knots_so3_, nr_knots_r3_);

  // add corners
  for (const auto &kv : calib_corners_) {
    if (kv.first.frame_id >= start_t_ns && kv.first.frame_id < end_t_ns) {
      trajectory_.addRSCornersMeasurement(&kv.second, &calib_dataset,
                                          &calib_dataset.View(0)->Camera(),
                                          kv.first.frame_id);
    }
  }

  // Add Accelerometer
  for (size_t i = 0; i < telemetry_data.accelerometer.measurement.size(); ++i) {
    const double t = telemetry_data.accelerometer.timestamp_ms[i] * 1e-3 +
                     time_offset_imu_to_cam;
    if (t < t0_s_ || t >= tend_s_)
      continue;

    const Eigen::Vector3d accl_unbiased =
        telemetry_data.accelerometer.measurement[i] + accl_bias;
    trajectory_.addAccelMeasurement(accl_unbiased, t * 1e9,
                                    1. / spline_weight_data_.var_r3,
                                    reestimate_biases_);
    accl_measurements[t] = accl_unbiased;
  }

  // Add Gyroscope
  for (size_t i = 0; i < telemetry_data.gyroscope.measurement.size(); ++i) {
    const double t = telemetry_data.gyroscope.timestamp_ms[i] * 1e-3 +
                     time_offset_imu_to_cam;
    if (t < t0_s_ || t >= tend_s_)
      continue;

    const Eigen::Vector3d gyro_unbiased =
        telemetry_data.gyroscope.measurement[i] + gyro_bias;
    trajectory_.addGyroMeasurement(gyro_unbiased, t * 1e9,
                                   1. / spline_weight_data_.var_so3,
                                   reestimate_biases_);
    gyro_measurements[t] = gyro_unbiased;
  }
}

// void ImuCameraCalibrator::InitSplinePosesFromSpline(
//    const theia::Reconstruction &calib_dataset,
//    const SplineWeightingData &spline_weight_data,
//    const double time_offset_imu_to_cam, const Eigen::Vector3d &gyro_bias,
//    const Eigen::Vector3d &accl_bias,
//    const OpenICC::CameraTelemetryData &telemetry_data,
//    CeresCalibrationSplineSplit<SPLINE_N, USE_OLD_TIME_DERIV> &trajectory) {

//  spline_weight_data_ = spline_weight_data;

//  const auto &view_ids = calib_dataset.ViewIds();
//  // get all timestamps and find smallest one
//  // Output each camera.
//  for (const ViewId view_id : view_ids) {
//    cam_timestamps_.push_back(calib_dataset.View(view_id)->GetTimestamp());
//  }

//  // initialize readout with 1/fps * 1/image_rows
//  inital_cam_line_delay_s_ =
//      (1. / spline_weight_data.cam_fps) *
//      (1. / calib_dataset.View(view_ids[0])->Camera().ImageHeight());
//  if (calibrate_cam_line_delay_) {
//    trajectory_.SetInitialRSLineDelay(inital_cam_line_delay_s_);
//  } else {
//    // this will set it constant 0.0 during optimization
//    trajectory_.SetInitialRSLineDelay(0.0);
//  }
//  std::cout << "Initialized Line Delay to: " << inital_cam_line_delay_s_ * 1e6
//            << "ns\n";

//  // find smallest timestamp
//  auto result =
//      std::minmax_element(cam_timestamps_.begin(), cam_timestamps_.end());
//  t0_s_ = cam_timestamps_[result.first - cam_timestamps_.begin()];
//  tend_s_ = cam_timestamps_[result.second - cam_timestamps_.begin()] +
//            2.0 / spline_weight_data.cam_fps;
//  const int64_t start_t_ns = t0_s_ * 1e9;
//  const int64_t end_t_ns = tend_s_ * 1e9;
//  const int64_t dt_so3_ns = spline_weight_data_.dt_so3 * 1e9;
//  const int64_t dt_r3_ns = spline_weight_data_.dt_r3 * 1e9;
//  LOG(INFO) << "Spline initialized with. Start/End: " << t0_s_ << "/" <<
//  tend_s_
//            << " knots spacing r3/so3: " << spline_weight_data_.dt_r3 << "/"
//            << spline_weight_data_.dt_so3;
//  trajectory_.init_times(dt_so3_ns, dt_r3_ns, start_t_ns);
//  trajectory_.setCalib(calib_dataset);

//  for (const ViewId view_id : view_ids) {
//    const View &view = *calib_dataset.View(view_id);
//    double timestamp = view.GetTimestamp();
//    if (timestamp >= tend_s_ || timestamp < t0_s_)
//      continue;
//    TimeCamId t_c_id(timestamp * 1e9, 0);
//    CalibCornerData corner_data;
//    const std::vector<theia::TrackId> trackIds = view.TrackIds();
//    for (size_t t = 0; t < trackIds.size(); ++t) {
//      corner_data.corners.push_back(*view.GetFeature(trackIds[t]));
//    }
//    corner_data.track_ids = trackIds;
//    calib_corners_[t_c_id] = corner_data;
//    CalibInitPoseData pose_data;
//    pose_data.T_a_c = Sophus::SE3<double>(
//        view.Camera().GetOrientationAsRotationMatrix().transpose(),
//        view.Camera().GetPosition());
//    calib_init_poses_[t_c_id] = pose_data;

//    Sophus::SE3d T_w_i_init =
//        calib_init_poses_.at(t_c_id).T_a_c * trajectory.getT_i_c().inverse();
//    CalibInitPoseData spline_pose_data;
//    spline_pose_data.T_a_c = T_w_i_init;
//    spline_init_poses_[t_c_id] = spline_pose_data;
//  }

//  nr_knots_so3_ = (end_t_ns - start_t_ns) / dt_so3_ns + SPLINE_N;
//  nr_knots_r3_ = (end_t_ns - start_t_ns) / dt_r3_ns + SPLINE_N;

//  std::cout << "Initializing " << nr_knots_so3_ << " SO3 knots.\n";
//  std::cout << "Initializing " << nr_knots_r3_ << " R3 knots.\n";

//  // init spline from another spline (eg already calibrated)
//  OpenICC::so3_vector so3_knots;
//  OpenICC::vec3_vector r3_knots;
//  trajectory.GetSO3Knots(so3_knots);
//  trajectory.GetR3Knots(r3_knots);
//  trajectory_.initFromSpline(so3_knots, r3_knots, trajectory.getG(),
//                             trajectory.getT_i_c());

//  // add corners
//  for (const auto &kv : calib_corners_) {
//    if (kv.first.frame_id >= start_t_ns && kv.first.frame_id < end_t_ns) {
//      // if (calibrate_cam_line_delay_) {
//      trajectory_.addRSCornersMeasurement(&kv.second, &calib_dataset,
//                                          &calib_dataset.View(0)->Camera(),
//                                          kv.first.frame_id);
//      //      } else {
//      //        trajectory_.addCornersMeasurement(&kv.second, &calib_dataset,
//      // &calib_dataset.View(0)->Camera(),
//      //                                          kv.first.frame_id);
//      //      }
//    }
//  }

//  // Add Accelerometer
//  for (size_t i = 0; i < telemetry_data.accelerometer.measurement.size(); ++i)
//  {
//    const double t = telemetry_data.accelerometer.timestamp_ms[i] * 1e-3 +
//                     time_offset_imu_to_cam;
//    if (t < t0_s_ || t >= tend_s_)
//      continue;

//    const Eigen::Vector3d accl_unbiased =
//        telemetry_data.accelerometer.measurement[i] + accl_bias;
//    trajectory_.addAccelMeasurement(accl_unbiased, t * 1e9,
//                                    1. / spline_weight_data_.var_r3,
//                                    reestimate_biases_);
//    accl_measurements[t] = accl_unbiased;
//  }

//  // Add Gyroscope
//  for (size_t i = 0; i < telemetry_data.gyroscope.measurement.size(); ++i) {
//    const double t = telemetry_data.gyroscope.timestamp_ms[i] * 1e-3 +
//                     time_offset_imu_to_cam;
//    if (t < t0_s_ || t >= tend_s_)
//      continue;

//    const Eigen::Vector3d gyro_unbiased =
//        telemetry_data.gyroscope.measurement[i] + gyro_bias;
//    trajectory_.addGyroMeasurement(gyro_unbiased, t * 1e9,
//                                   1. / spline_weight_data_.var_so3,
//                                   reestimate_biases_);
//    gyro_measurements[t] = gyro_unbiased;
//  }
//}

void ImuCameraCalibrator::InitializeGravity(
    const OpenICC::CameraTelemetryData &telemetry_data,
    const Eigen::Vector3d &accl_bias) {
  for (size_t j = 0; j < cam_timestamps_.size(); ++j) {
    int64_t timestamp_ns = cam_timestamps_[j] * 1e9;

    TimeCamId tcid(timestamp_ns, 0);
    const auto cp_it = calib_init_poses_.find(tcid);

    if (cp_it != calib_init_poses_.end()) {
      Sophus::SE3d T_a_i = cp_it->second.T_a_c * T_i_c_init_.inverse();

      if (!gravity_initialized_) {
        for (size_t i = 0;
             i < telemetry_data.accelerometer.measurement.size() &&
             !gravity_initialized_;
             i++) {
          const Eigen::Vector3d ad =
              telemetry_data.accelerometer.measurement[i] + accl_bias;
          const int64_t accl_t =
              telemetry_data.accelerometer.timestamp_ms[i] * 1e-3;
          if (std::abs(accl_t - cam_timestamps_[j]) < 1 / 30.) {
            gravity_init_ = T_a_i.so3() * ad;
            gravity_initialized_ = true;
            std::cout << "g_a initialized with " << gravity_init_.transpose()
                      << " at timestamp: " << accl_t << std::endl;
          }
        }
      }
    }
  }
  trajectory_.setG(gravity_init_);
}

std::vector<double> ImuCameraCalibrator::Optimize(const int iterations) {
  ceres::Solver::Summary summary = trajectory_.optimize(iterations);
  std::vector<double> gl_shut_rol_shut_errors = {
      trajectory_.meanReprojection(calib_corners_),
      trajectory_.meanRSReprojection(calib_corners_)};
  return gl_shut_rol_shut_errors;
}

void ImuCameraCalibrator::ToTheiaReconDataset(Reconstruction &output_recon) {
  // convert spline to theia output
  for (size_t i = 0; i < cam_timestamps_.size(); ++i) {
    const int64_t t_ns = cam_timestamps_[i] * 1e9;
    Sophus::SE3d spline_pose = trajectory_.getPose(t_ns);
    theia::ViewId v_id_theia =
        output_recon.AddView(std::to_string(t_ns), 0, t_ns);
    theia::View *view = output_recon.MutableView(v_id_theia);
    view->SetEstimated(true);
    theia::Camera *camera = view->MutableCamera();
    camera->SetOrientationFromRotationMatrix(
        spline_pose.rotationMatrix().transpose());
    camera->SetPosition(spline_pose.translation());
  }
}

void ImuCameraCalibrator::ClearSpline() {
  cam_timestamps_.clear();
  gyro_measurements.clear();
  accl_measurements.clear();
  calib_corners_.clear();
  calib_init_poses_.clear();
  spline_init_poses_.clear();
  trajectory_.Clear();
}

} // namespace core
} // namespace OpenICC

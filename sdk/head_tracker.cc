/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "head_tracker.h"

#include "include/cardboard.h"
#include "sensors/neck_model.h"
#include "util/logging.h"
#include "util/rotation.h"
#include "util/vector.h"
#include "util/vectorutils.h"

namespace cardboard {
// @{ Hold rotations to adapt the pose estimation to the viewport and head
// poses. Use the following indexing for each viewport orientation:
// [0]: Landscape left.
// [1]: Landscape right.
// [2]: Portrait.
// [3]: Portrait upside down.
static std::array<Rotation, 4> SensorToDisplayRotations() {
  static std::array<Rotation, 4> kSensorToDisplayRotations{
      // LandscapeLeft: This is the same than initializing the rotation from
      // Rotation::FromAxisAndAngle(Vector3(0., 0., 1.), M_PI / 2.).
      Rotation::FromQuaternion(Rotation::QuaternionType(
          0., 0., 0.7071067811865476, 0.7071067811865476)),
      // LandscapeRight: This is the same than initializing the rotation from
      // Rotation::FromAxisAndAngle(Vector3(0., 0., 1.), -M_PI / 2.).
      Rotation::FromQuaternion(Rotation::QuaternionType(
          0., 0., -0.7071067811865476, 0.7071067811865476)),
      // Portrait: This is the same than initializing the rotation from
      // Rotation::FromAxisAndAngle(Vector3(0., 0., 1.), 0.).
      Rotation::FromQuaternion(Rotation::QuaternionType(0., 0., 0., 1.)),
      // PortaitUpsideDown: This is the same than initializing the rotation from
      // Rotation::FromAxisAndAngle(Vector3(0., 0., 1.), M_PI).
      Rotation::FromQuaternion(Rotation::QuaternionType(0., 0., 1., 0.))};
  return kSensorToDisplayRotations;
}

static std::array<Rotation, 4> EkfToHeadTrackerRotations() {
  static std::array<Rotation, 4> kEkfToHeadTrackerRotations{
      // LandscapeLeft: This is the same than initializing the rotation from
      // Rotation::FromYawPitchRoll(-M_PI / 2., 0, -M_PI / 2.).
      Rotation::FromQuaternion(Rotation::QuaternionType(0.5, -0.5, -0.5, 0.5)),
      // LandscapeRight: This is the same than initializing the rotation from
      // Rotation::FromYawPitchRoll(M_PI / 2., 0, M_PI / 2.).
      Rotation::FromQuaternion(Rotation::QuaternionType(0.5, 0.5, 0.5, 0.5)),
      // Portrait: This is the same than initializing the rotation from
      // Rotation::FromYawPitchRoll(M_PI / 2., M_PI / 2., M_PI / 2.).
      Rotation::FromQuaternion(Rotation::QuaternionType(
          0.7071067811865476, 0., 0., 0.7071067811865476)),
      // Portrait upside down: This is the same than initializing the rotation
      // from Rotation::FromYawPitchRoll(-M_PI / 2., -M_PI / 2., -M_PI / 2.).
      Rotation::FromQuaternion(Rotation::QuaternionType(
          0., -0.7071067811865476, -0.7071067811865476, 0.))};
  return kEkfToHeadTrackerRotations;
}
// @}

// Contains the necessary rotations to account for changes in reported head
// pose when the tracker starts/resets in a certain viewport and then changes
// to another.
//
// The rows contain the current viewport orientation, the columns contain the
// transformed viewport orientation. See below:
//
// @code
// kViewportChangeRotationCompensation[current_viewport_orientation]
//                                    [new_viewport_orientation]
// @endcode
//
// Roll angle needs to change. The following table shows the correction angle
// for each combination:
//
// | Current\New     | LL  | LR  |  P  | PUD |
// |-----------------|-----|-----|-----|-----|
// | Landscape Left  | 0   | π   |-π/2 | π/2 |
// | Landscape Right | π   | 0   | π/2 |-π/2 |
// | Portrait        | π/2 |-π/2 | 0   | π   |
// | Portrait UD     |-π/2 | π/2 | π   | 0   |
static std::array<std::array<Rotation, 4>, 4>
ViewportChangeRotationCompensation() {
  static std::array<std::array<Rotation, 4>, 4>
      kViewportChangeRotationCompensation{{
          // Landscape left.
          {Rotation::Identity(), Rotation::FromYawPitchRoll(0, 0, M_PI),
           Rotation::FromYawPitchRoll(0, 0, -M_PI / 2),
           Rotation::FromYawPitchRoll(0, 0, M_PI / 2)},
          // Landscape Right.
          {Rotation::FromYawPitchRoll(0, 0, M_PI), Rotation::Identity(),
           Rotation::FromYawPitchRoll(0, 0, M_PI / 2),
           Rotation::FromYawPitchRoll(0, 0, -M_PI / 2)},
          // Portrait.
          {Rotation::FromYawPitchRoll(0, 0, M_PI / 2),
           Rotation::FromYawPitchRoll(0, 0, -M_PI / 2), Rotation::Identity(),
           Rotation::FromYawPitchRoll(0, 0, M_PI)},
          // Portrait Upside Down.
          {Rotation::FromYawPitchRoll(0, 0, -M_PI / 2),
           Rotation::FromYawPitchRoll(0, 0, M_PI / 2),
           Rotation::FromYawPitchRoll(0, 0, M_PI), Rotation::Identity()},
      }};
  return kViewportChangeRotationCompensation;
}

// Aryzon 6DoF
const int rotation_samples = 10;
const int position_samples = 3;
const int64_t max6DoFTimeDifference = 200000000; // Maximum time difference between last pose state timestamp and last 6DoF timestamp, if it takes longer than this the last known location of sixdof will be used

HeadTracker::HeadTracker()
    : is_tracking_(false),
      sensor_fusion_(new SensorFusionEkf()),
      latest_gyroscope_data_({0, 0, Vector3::Zero()}),
      accel_sensor_(new SensorEventProducer<AccelerometerData>()),
      gyro_sensor_(new SensorEventProducer<GyroscopeData>()),
	  // Aryzon 6DoF
      rotation_data_(new RotationData(rotation_samples)),
      position_data_(new PositionData(position_samples)),
      is_viewport_orientation_initialized_(false) {
  on_accel_callback_ = [&](const AccelerometerData& event) {
    OnAccelerometerData(event);
  };
  on_gyro_callback_ = [&](const GyroscopeData& event) {
    OnGyroscopeData(event);
  };
}

HeadTracker::~HeadTracker() { UnregisterCallbacks(); }

void HeadTracker::Pause() {
  if (!is_tracking_) {
    return;
  }

  UnregisterCallbacks();

  // Create a gyro event with zero velocity. This effectively stops the
  // prediction.
  GyroscopeData event = latest_gyroscope_data_;
  event.data = Vector3::Zero();

  OnGyroscopeData(event);

  is_tracking_ = false;
}

void HeadTracker::Resume() {
  is_tracking_ = true;
  RegisterCallbacks();
}

void HeadTracker::GetPose(int64_t timestamp_ns,
                          CardboardViewportOrientation viewport_orientation,
                          std::array<float, 3>& out_position,
                          std::array<float, 4>& out_orientation) {


  const RotationState latest_rotation_state = sensor_fusion_->GetLatestRotationState();

#if 0

    Rotation orientation;
  const PoseState pose_state = sensor_fusion_->GetLatestPoseState();
    orientation = pose_prediction::PredictPose(timestamp_ns, pose_state);

  // In order to update our pose as the sensor changes, we begin with the
  // inverse default orientation (the orientation returned by a reset sensor),
  // apply the current sensor transformation, and then transform into display
  // space.
  Rotation sensor_to_display;
  // Aryzon multiple orientations
  // Very fast implementation on iOS, pretty fast for Android
  screen_params::ScreenOrientation orientation = screen_params::getScreenOrientation();
  if (orientation == screen_params::LandscapeLeft) {
      sensor_to_display = Rotation::FromAxisAndAngle(Vector3(0, 0, 1), M_PI / 2.0);
  } else  if (orientation == screen_params::LandscapeRight) {
      sensor_to_display = Rotation::FromAxisAndAngle(Vector3(0, 0, 1), -M_PI / 2.0);
  } else { // Portrait
      sensor_to_display = Rotation::FromAxisAndAngle(Vector3(0, 0, 1), 0.0);
  }
#endif

  Rotation rotation = GetRotation(viewport_orientation, timestamp_ns);
  Vector4 orientation = rotation.GetQuaternion();

  if (is_viewport_orientation_initialized_ &&
      viewport_orientation != viewport_orientation_) {
    sensor_fusion_->RotateSensorSpaceToStartSpaceTransformation(
        ViewportChangeRotationCompensation()[viewport_orientation_]
                                            [viewport_orientation]);
  }
  viewport_orientation_ = viewport_orientation;
  is_viewport_orientation_initialized_ = true;

  // Aryzon 6DoF
  // Save rotation sample with timestamp to be used in AddSixDoFData()
  rotation_data_->AddSample(orientation, timestamp_ns);

  if (position_data_->IsValid() && latest_rotation_state.timestamp - position_data_->GetLatestTimestamp() < max6DoFTimeDifference) {
      // 6DoF is recently updated

      rotation = rotation * -difference_to_6DoF_;
      orientation = rotation.GetQuaternion();

	  out_orientation[0] = static_cast<float>(orientation[0]);
	  out_orientation[1] = static_cast<float>(orientation[1]);
	  out_orientation[2] = static_cast<float>(orientation[2]);
	  out_orientation[3] = static_cast<float>(orientation[3]);

      Vector3 p = position_data_->GetExtrapolatedForTimeStamp(timestamp_ns);
      std::array<float, 3> predicted_position_ = {(float)p[0], (float)p[1], (float)p[2]};

      out_position = predicted_position_;

  } else {
      // 6DoF is not recently updated

	  out_orientation[0] = static_cast<float>(orientation[0]);
	  out_orientation[1] = static_cast<float>(orientation[1]);
	  out_orientation[2] = static_cast<float>(orientation[2]);
	  out_orientation[3] = static_cast<float>(orientation[3]);

      out_position = ApplyNeckModel(out_orientation, 1.0);

      if (position_data_->IsValid()) {
          // Apply last known 6DoF position if 6DoF was data previously added, while still applying neck model.

          Vector3 last_known_position_ = position_data_->GetLatestData();
          out_position[0] += (float)last_known_position_[0];
          out_position[1] += (float)last_known_position_[1];
          out_position[2] += (float)last_known_position_[2];
      }
  }
}

void HeadTracker::Recenter() {
  sensor_fusion_->Reset();
}

void HeadTracker::RegisterCallbacks() {
  accel_sensor_->StartSensorPolling(&on_accel_callback_);
  gyro_sensor_->StartSensorPolling(&on_gyro_callback_);
}

void HeadTracker::UnregisterCallbacks() {
  accel_sensor_->StopSensorPolling();
  gyro_sensor_->StopSensorPolling();
}

void HeadTracker::OnAccelerometerData(const AccelerometerData& event) {
  if (!is_tracking_) {
    return;
  }
  sensor_fusion_->ProcessAccelerometerSample(event);
}

void HeadTracker::OnGyroscopeData(const GyroscopeData& event) {
  if (!is_tracking_) {
    return;
  }
  latest_gyroscope_data_ = event;
  sensor_fusion_->ProcessGyroscopeSample(event);
}

// Aryzon 6DoF
void HeadTracker::AddSixDoFData(int64_t timestamp_ns, float* pos, float* orientation) {
  if (!is_tracking_) {
    return;
  }
    position_data_->AddSample(Vector3(pos[0], pos[1], pos[2]), timestamp_ns);

    if (position_data_->IsValid() && rotation_data_->IsValid()) {
        // 6DoF timestamp needs to be before the latest rotation_data timestamp
        Rotation gyroAtTimeOfSixDoF = Rotation::FromQuaternion(rotation_data_->GetInterpolatedForTimeStamp(timestamp_ns));
        Rotation sixDoFRotation = Rotation::FromQuaternion(Vector4(0, orientation[1], 0, orientation[3]));

        Rotation difference = gyroAtTimeOfSixDoF * -sixDoFRotation;

        // Only synchronize rotation around the y axis
        Vector4 diffQ = difference.GetQuaternion();
        diffQ[0] = 0;
        diffQ[2] = 0;

        // Quaternion will be normalized in this call:
        difference_to_6DoF_.SetQuaternion(diffQ);
    }
}


Rotation HeadTracker::GetRotation(
    CardboardViewportOrientation viewport_orientation,
    int64_t timestamp_ns) const {
  const Rotation predicted_rotation =
      sensor_fusion_->PredictRotation(timestamp_ns);

  // In order to update our pose as the sensor changes, we begin with the
  // inverse default orientation (the orientation returned by a reset sensor,
  // i.e. since the last Reset() call), apply the current sensor transformation,
  // and then transform into display space.
  return SensorToDisplayRotations()[viewport_orientation] * predicted_rotation *
         EkfToHeadTrackerRotations()[viewport_orientation];
}

}  // namespace cardboard

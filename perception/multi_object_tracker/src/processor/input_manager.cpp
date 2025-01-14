// Copyright 2024 TIER IV, Inc.
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

#include "multi_object_tracker/processor/input_manager.hpp"

#include <cassert>

namespace multi_object_tracker
{
///////////////////////////
/////// InputStream ///////
///////////////////////////
InputStream::InputStream(rclcpp::Node & node, uint & index) : node_(node), index_(index)
{
}

void InputStream::init(const InputChannel & input_channel)
{
  // Initialize parameters
  input_topic_ = input_channel.input_topic;
  long_name_ = input_channel.long_name;
  short_name_ = input_channel.short_name;
  is_spawn_enabled_ = input_channel.is_spawn_enabled;

  // Initialize queue
  objects_que_.clear();

  // Initialize latency statistics
  latency_mean_ = 0.2;  // [s] (initial value)
  latency_var_ = 0.0;
  interval_mean_ = 0.0;  // [s] (initial value)
  interval_var_ = 0.0;

  latest_measurement_time_ = node_.now();
  latest_message_time_ = node_.now();
}

bool InputStream::getTimestamps(
  rclcpp::Time & latest_measurement_time, rclcpp::Time & latest_message_time) const
{
  if (!isTimeInitialized()) {
    return false;
  }
  latest_measurement_time = latest_measurement_time_;
  latest_message_time = latest_message_time_;
  return true;
}

void InputStream::onMessage(
  const autoware_auto_perception_msgs::msg::DetectedObjects::ConstSharedPtr msg)
{
  const DetectedObjects objects = *msg;
  objects_que_.push_back(objects);
  if (objects_que_.size() > que_size_) {
    objects_que_.pop_front();
  }

  // update the timing statistics
  rclcpp::Time now = node_.now();
  rclcpp::Time objects_time(objects.header.stamp);
  updateTimingStatus(now, objects_time);

  // trigger the function if it is set
  if (func_trigger_) {
    func_trigger_(index_);
  }
}

void InputStream::updateTimingStatus(const rclcpp::Time & now, const rclcpp::Time & objects_time)
{
  // Define constants
  constexpr int SKIP_COUNT = 4;             // Skip the initial messages
  constexpr int INITIALIZATION_COUNT = 16;  // Initialization process count

  // Update latency statistics
  // skip initial messages for the latency statistics
  if (initial_count_ > SKIP_COUNT) {
    const double latency = (now - objects_time).seconds();
    if (initial_count_ < INITIALIZATION_COUNT) {
      // set higher gain for the initial messages
      constexpr double initial_gain = 0.5;
      latency_mean_ = (1.0 - initial_gain) * latency_mean_ + initial_gain * latency;
    } else {
      constexpr double gain = 0.05;
      latency_mean_ = (1.0 - gain) * latency_mean_ + gain * latency;
      const double latency_delta = latency - latency_mean_;
      latency_var_ = (1.0 - gain) * latency_var_ + gain * latency_delta * latency_delta;
    }
  }

  // Calculate interval, Update interval statistics
  if (initial_count_ > SKIP_COUNT) {
    const double interval = (now - latest_message_time_).seconds();
    if (interval < 0.0) {
      RCLCPP_WARN(
        node_.get_logger(),
        "InputManager::updateTimingStatus %s: Negative interval detected, now: %f, "
        "latest_message_time_: %f",
        long_name_.c_str(), now.seconds(), latest_message_time_.seconds());
    } else if (initial_count_ < INITIALIZATION_COUNT) {
      // Initialization
      constexpr double initial_gain = 0.5;
      interval_mean_ = (1.0 - initial_gain) * interval_mean_ + initial_gain * interval;
    } else {
      // The interval is considered regular if it is within 0.5 and 1.5 times the mean interval
      bool update_statistics = interval > 0.5 * interval_mean_ && interval < 1.5 * interval_mean_;
      if (update_statistics) {
        constexpr double gain = 0.05;
        interval_mean_ = (1.0 - gain) * interval_mean_ + gain * interval;
        const double interval_delta = interval - interval_mean_;
        interval_var_ = (1.0 - gain) * interval_var_ + gain * interval_delta * interval_delta;
      }
    }
  }

  // Update time
  latest_message_time_ = now;
  constexpr double delay_threshold = 3.0;  // [s]
  if (std::abs((latest_measurement_time_ - objects_time).seconds()) > delay_threshold) {
    // Reset the latest measurement time if the time difference is too large
    latest_measurement_time_ = objects_time;
    RCLCPP_WARN(
      node_.get_logger(),
      "InputManager::updateTimingStatus %s: Resetting the latest measurement time to %f",
      long_name_.c_str(), objects_time.seconds());
  } else {
    // Aware reversed message arrival
    latest_measurement_time_ =
      latest_measurement_time_ < objects_time ? objects_time : latest_measurement_time_;
  }

  // Update the initial count
  if (initial_count_ < INITIALIZATION_COUNT) {
    initial_count_++;
  }
}

void InputStream::getObjectsOlderThan(
  const rclcpp::Time & object_latest_time, const rclcpp::Time & object_oldest_time,
  ObjectsList & objects_list)
{
  assert(object_latest_time.nanoseconds() > object_oldest_time.nanoseconds());

  for (const auto & object : objects_que_) {
    const rclcpp::Time object_time = rclcpp::Time(object.header.stamp);

    // remove objects older than the specified duration
    if (object_time < object_oldest_time) {
      objects_que_.pop_front();
      continue;
    }

    // Add the object if the object is older than the specified latest time
    if (object_latest_time >= object_time) {
      std::pair<uint, DetectedObjects> object_pair(index_, object);
      objects_list.push_back(object_pair);
      // remove the object from the queue
      objects_que_.pop_front();
    }
  }
}

////////////////////////////
/////// InputManager ///////
////////////////////////////
InputManager::InputManager(rclcpp::Node & node) : node_(node)
{
  latest_object_time_ = node_.now();
}

void InputManager::init(const std::vector<InputChannel> & input_channels)
{
  // Check input sizes
  input_size_ = input_channels.size();
  if (input_size_ == 0) {
    RCLCPP_ERROR(node_.get_logger(), "InputManager::init No input streams");
    return;
  }

  // Initialize input streams
  sub_objects_array_.resize(input_size_);
  bool is_any_spawn_enabled = false;
  for (size_t i = 0; i < input_size_; i++) {
    uint index(i);
    InputStream input_stream(node_, index);
    input_stream.init(input_channels[i]);
    input_stream.setTriggerFunction(
      std::bind(&InputManager::onTrigger, this, std::placeholders::_1));
    input_streams_.push_back(std::make_shared<InputStream>(input_stream));
    is_any_spawn_enabled |= input_streams_.at(i)->isSpawnEnabled();

    // Set subscription
    RCLCPP_INFO(
      node_.get_logger(), "InputManager::init Initializing %s input stream from %s",
      input_channels[i].long_name.c_str(), input_channels[i].input_topic.c_str());
    std::function<void(const DetectedObjects::ConstSharedPtr msg)> func =
      std::bind(&InputStream::onMessage, input_streams_.at(i), std::placeholders::_1);
    sub_objects_array_.at(i) = node_.create_subscription<DetectedObjects>(
      input_channels[i].input_topic, rclcpp::QoS{1}, func);
  }

  // Check if any spawn enabled input streams
  if (!is_any_spawn_enabled) {
    RCLCPP_ERROR(node_.get_logger(), "InputManager::init No spawn enabled input streams");
    return;
  }
  is_initialized_ = true;
}

void InputManager::onTrigger(const uint & index) const
{
  // when the target stream triggers, call the trigger function
  if (index == target_stream_idx_ && func_trigger_) {
    func_trigger_();
  }
}

void InputManager::getObjectTimeInterval(
  const rclcpp::Time & now, rclcpp::Time & object_latest_time,
  rclcpp::Time & object_oldest_time) const
{
  object_latest_time =
    now - rclcpp::Duration::from_seconds(
            target_stream_latency_ -
            0.1 * target_stream_latency_std_);  // object_latest_time with 0.1 sigma safety margin
  // check the target stream can be included in the object time interval
  if (input_streams_.at(target_stream_idx_)->isTimeInitialized()) {
    rclcpp::Time latest_measurement_time =
      input_streams_.at(target_stream_idx_)->getLatestMeasurementTime();

    // if the object_latest_time is older than the latest measurement time, set it as the latest
    // object time
    object_latest_time =
      object_latest_time < latest_measurement_time ? latest_measurement_time : object_latest_time;
  }

  object_oldest_time = object_latest_time - rclcpp::Duration::from_seconds(1.0);
  // if the object_oldest_time is older than the latest object time, set it to the latest object
  // time
  object_oldest_time =
    object_oldest_time > latest_object_time_ ? object_oldest_time : latest_object_time_;
}

void InputManager::optimizeTimings()
{
  double max_latency_mean = 0.0;
  uint selected_stream_idx = 0;
  double selected_stream_latency_std = 0.1;
  double selected_stream_interval = 0.1;
  double selected_stream_interval_std = 0.01;

  {
    // ANALYSIS: Get the streams statistics
    // select the stream that has the maximum latency
    double latency_mean, latency_var, interval_mean, interval_var;
    for (const auto & input_stream : input_streams_) {
      if (!input_stream->isTimeInitialized()) continue;
      input_stream->getTimeStatistics(latency_mean, latency_var, interval_mean, interval_var);
      if (latency_mean > max_latency_mean) {
        max_latency_mean = latency_mean;
        selected_stream_idx = input_stream->getIndex();
        selected_stream_latency_std = std::sqrt(latency_var);
        selected_stream_interval = interval_mean;
        selected_stream_interval_std = std::sqrt(interval_var);
      }
    }
  }

  // Set the target stream index, which has the maximum latency
  // trigger will be called next time
  // if no stream is initialized, the target stream index will be 0 and wait for the initialization
  target_stream_idx_ = selected_stream_idx;
  target_stream_latency_ = max_latency_mean;
  target_stream_latency_std_ = selected_stream_latency_std;
  target_stream_interval_ = selected_stream_interval;
  target_stream_interval_std_ = selected_stream_interval_std;
}

bool InputManager::getObjects(const rclcpp::Time & now, ObjectsList & objects_list)
{
  if (!is_initialized_) {
    RCLCPP_INFO(node_.get_logger(), "InputManager::getObjects Input manager is not initialized");
    return false;
  }

  // Clear the objects
  objects_list.clear();

  // Get the time interval for the objects
  rclcpp::Time object_latest_time, object_oldest_time;
  getObjectTimeInterval(now, object_latest_time, object_oldest_time);

  // Optimize the target stream, latency, and its band
  // The result will be used for the next time, so the optimization is after getting the time
  // interval
  optimizeTimings();

  // Get objects from all input streams
  // adds up to the objects vector for efficient processing
  for (const auto & input_stream : input_streams_) {
    input_stream->getObjectsOlderThan(object_latest_time, object_oldest_time, objects_list);
  }

  // Sort objects by timestamp
  std::sort(
    objects_list.begin(), objects_list.end(),
    [](const std::pair<uint, DetectedObjects> & a, const std::pair<uint, DetectedObjects> & b) {
      return (rclcpp::Time(a.second.header.stamp) - rclcpp::Time(b.second.header.stamp)).seconds() <
             0;
    });

  // Update the latest object time
  bool is_any_object = !objects_list.empty();
  if (is_any_object) {
    latest_object_time_ = rclcpp::Time(objects_list.back().second.header.stamp);
  } else {
    // check time jump
    if (now < latest_object_time_) {
      RCLCPP_WARN(
        node_.get_logger(),
        "InputManager::getObjects Time jump detected, now: %f, latest_object_time_: %f",
        now.seconds(), latest_object_time_.seconds());
      latest_object_time_ =
        now - rclcpp::Duration::from_seconds(3.0);  // reset the latest object time to 3 seconds ago
    } else {
      RCLCPP_DEBUG(
        node_.get_logger(),
        "InputManager::getObjects No objects in the object list, object time band from %f to %f",
        (now - object_oldest_time).seconds(), (now - object_latest_time).seconds());
    }
  }

  return is_any_object;
}

}  // namespace multi_object_tracker

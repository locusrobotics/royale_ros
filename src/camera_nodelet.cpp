/*
 * Copyright (C) 2017 Love Park Robotics, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distribted on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <royale_ros/camera_nodelet.h>
#include <royale_ros/contrib/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <nodelet/nodelet.h>
#include <opencv2/opencv.hpp>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/point_cloud.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>
#include <royale_ros/ExposureTimes.h>
#include <royale_ros/Config.h>
#include <royale_ros/Dump.h>
#include <royale.hpp>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>

namespace enc = sensor_msgs::image_encodings;
using json = nlohmann::json;
constexpr auto OK_ = royale::CameraStatus::SUCCESS;

void
royale_ros::CameraNodelet::onInit()
{
  NODELET_INFO_STREAM("onInit(): " << this->getName());

  // Currently, we only support LEVEL 1 access to the device.
  this->access_level_ = 0;

  // flag indicating that we have not yet created our image publishers
  this->instantiated_publishers_ = false;

  // used for detecting a disconnected camera
  this->last_frame_ = ros::Time::now();

  this->np_ = getMTPrivateNodeHandle();
  this->it_.reset(new image_transport::ImageTransport(this->np_));
  this->np_.param<std::string>("serial_number", this->serial_number_, "-");
  this->np_.param<float>("poll_bus_secs", this->poll_bus_secs_, 1.);
  this->np_.param<float>("timeout_secs", this->timeout_secs_, 1.);
  this->np_.param<std::string>("optical_frame", this->optical_frame_,
                               "camera_optical_link");

  //------------------------------------------------------------
  // Instantiate the underlying camera device by polling the bus
  //------------------------------------------------------------
  this->timer_ =
    this->np_.createTimer(ros::Duration(.001),
                          [this](const ros::TimerEvent& t)
                          { this->InitCamera(); },
                          true); // oneshot timer

  //---------------------
  // Advertised Services
  //---------------------
  this->dump_srv_ =
    this->np_.advertiseService<royale_ros::Dump::Request,
                               royale_ros::Dump::Response>
    ("Dump", std::bind(&CameraNodelet::Dump, this,
                              std::placeholders::_1,
                              std::placeholders::_2));

  this->config_srv_ =
    this->np_.advertiseService<royale_ros::Config::Request,
                               royale_ros::Config::Response>
    ("Config", std::bind(&CameraNodelet::Config, this,
                         std::placeholders::_1,
                         std::placeholders::_2));
}

void
royale_ros::CameraNodelet::InitCamera()
{
  std::lock_guard<std::mutex> lock(this->cam_mutex_);

  // For an already initialized camera, this acts as a heartbeat
  if (this->cam_ != nullptr)
    {
      {
        // New scope to check to see if the camera has timedout
        std::lock_guard<std::mutex> lock(this->last_frame_mutex_);
        if ((ros::Time::now() - this->last_frame_).toSec() >
            this->timeout_secs_)
          {
            NODELET_WARN_STREAM("Camera timeout!");
            this->cam_.reset();
          }
      }

      this->RescheduleTimer();
      return;
    }


  NODELET_INFO_STREAM("Probing for available royale cameras...");
  royale::CameraManager manager;
  auto camlist = manager.getConnectedCameraList();

  if (! camlist.empty())
    {
      if (this->serial_number_ == "-")
        {
          // grab the first camera found
          this->cam_ = manager.createCamera(camlist.at(0));
          this->serial_number_ = std::string(camlist.at(0).c_str());
          this->np_.setParam("serial_number", this->serial_number_);
        }
      else
        {
          // see if the specific camera was detected
          auto result = std::find(std::begin(camlist), std::end(camlist),
                                  this->serial_number_);
          if (result != std::end(camlist))
            {
              // the specific camera is available
              this->cam_ = manager.createCamera(*result);
            }
          else
            {
              // the specific camera is not available
              NODELET_WARN_STREAM("Could not find royale camera: "
                                  << this->serial_number_);
            }
        }
    }
  else
    {
      NODELET_WARN_STREAM("No royale cameras found on bus!");
    }

  if (this->cam_ != nullptr)
    {
      if (this->cam_->initialize() != OK_)
        {
          NODELET_INFO_STREAM("Failed to initialize() camera: "
                              << this->serial_number_);
          this->cam_.reset();
        }
      else
        {
          NODELET_INFO_STREAM("Instantiated royale camera: "
                              << this->serial_number_);

          royale::CameraAccessLevel level;
          if (this->cam_->getAccessLevel(level) == OK_)
            {
              this->access_level_ = (std::uint32_t) level;
            }
          NODELET_INFO_STREAM("Access level: " << this->access_level_);

          //
          // we only create our image publishers once regardless
          // of how many times the node polls the bus for a camera
          //
          if (! this->instantiated_publishers_)
            {
              //
              // Dynamically create our image publishers based on the max
              // number of streams available across all camera use-cases
              //
              royale::Vector<royale::String> use_cases;
              if (this->cam_->getUseCases(use_cases) == OK_)
                {
                  std::uint32_t max_num_streams = 1;
                  for(auto& uc : use_cases)
                    {
                      std::uint32_t nstreams = 0;
                      if (this->cam_->getNumberOfStreams(uc, nstreams) != OK_)
                        {
                          NODELET_WARN_STREAM("Could not get stream count: "
                                              << uc.c_str());
                        }
                      else
                        {
                          if (nstreams > max_num_streams)
                            {
                              max_num_streams = nstreams;
                            }
                        }

                      // add the use-case to our stream id LUT
                      this->stream_id_lut_.emplace(
                        std::make_pair(std::string(uc.c_str()),
                                       std::vector<std::uint16_t>()));

                    } // end: for(auto& uc : use_cases)

                  NODELET_INFO_STREAM("Max number of streams: "
                                      << max_num_streams);
                  for (std::uint32_t i = 0; i < max_num_streams; ++i)
                    {
                      this->exposure_pubs_.push_back(
                        this->np_.advertise<royale_ros::ExposureTimes>(
                          "stream/" + std::to_string(i+1) +
                          "/exposure_times", 1));

                      this->cloud_pubs_.push_back(
                        this->np_.advertise<pcl::PointCloud<pcl::PointXYZI> >(
                          "stream/" + std::to_string(i+1) + "/cloud", 1));

                      this->noise_pubs_.push_back(
                        this->it_->advertise(
                          "stream/" + std::to_string(i+1) + "/noise", 1));

                      this->gray_pubs_.push_back(
                        this->it_->advertise(
                          "stream/" + std::to_string(i+1) + "/gray", 1));

                      this->conf_pubs_.push_back(
                        this->it_->advertise(
                          "stream/" + std::to_string(i+1) + "/conf", 1));
                    }

                  this->instantiated_publishers_ = true;
                }
            } // end: if (! this->instantiated_publishers_)

          //
          // This whole block is a candidate to be a function for starting the
          // camera stream
          //
          {
            std::lock_guard<std::mutex> lock(this->current_use_case_mutex_);
            royale::String current_use_case;
            if (this->cam_->getCurrentUseCase(current_use_case) == OK_)
              {
                this->current_use_case_ = std::string(current_use_case.c_str());
              }
            else
              {
                NODELET_WARN_STREAM("Could not discover current use case!");
                this->current_use_case_ = "UNKNOWN";
              }
          }

          this->cam_->registerDataListener(this);
          this->cam_->startCapture();
          {
            std::lock_guard<std::mutex> lock(this->last_frame_mutex_);
            this->last_frame_ = ros::Time::now();
          }
          //
          // END OF BLOCK THAT SHOULD BE BROKEN OUT INTO FUNCTION
          //
        }
    }

  this->RescheduleTimer();
}

void
royale_ros::CameraNodelet::RescheduleTimer()
{
  this->timer_.stop();
  this->timer_.setPeriod(ros::Duration(this->poll_bus_secs_));
  this->timer_.start();
}

bool
royale_ros::CameraNodelet::Config(royale_ros::Config::Request& req,
                                  royale_ros::Config::Response& resp)
{
  std::lock_guard<std::mutex> lock(this->cam_mutex_);
  if (this->cam_ == nullptr)
    {
      NODELET_ERROR_STREAM("No camera instantiated with serial number: "
                           << this->serial_number_);
      return false;
    }

  NODELET_INFO_STREAM("Handling Config request...");

  json j;
  try
    {
      j = json::parse(req.json);
    }
  catch (const std::exception& ex)
    {
      NODELET_ERROR_STREAM("Failed to parse json: " << ex.what());
      NODELET_INFO_STREAM("json was:\n" << req.json);
      resp.status = -1;
      resp.msg = ex.what();
      return true;
    }

  if (! j.is_object())
    {
      NODELET_ERROR_STREAM("The passed in json should be an object!");
      NODELET_INFO_STREAM("json was:\n" << j.dump());
      resp.status = -1;
      resp.msg = "The passed in json shuld be an object";
      return true;
    }

  //
  // Imager parameters
  //
  // XXX: Ultimately, we will generalize this, however, for now,
  // there is really only 1 mutable imager parameter (the so called "Use Case")
  // at Level 1 access and so we simply process it in-line.
  //
  //
  royale::CameraStatus status = OK_;
  json j_img = j["Imager"];
  if (! j_img.is_null())
    {
      if (j_img.count("CurrentUseCase") == 1)
        {
          json uc_root = j_img["CurrentUseCase"];

          for (auto it = uc_root.begin(); it != uc_root.end(); ++it)
            {
              std::string key = it.key();
              // NODELET_INFO_STREAM("Processing: key="
              //                      << key << ", val="
              //                      << uc_root[key].dump(2));

              if (key == "Name")
                {
                  status =
                    this->cam_->setUseCase(
                      royale::String(uc_root[key].get<std::string>()));

                  if (status == OK_)
                    {
                      {
                        std::lock_guard<std::mutex>
                          lock(this->current_use_case_mutex_);
                        royale::String current_use_case;
                        if (this->cam_->getCurrentUseCase(
                              current_use_case) == OK_)
                          {
                            this->current_use_case_ =
                              std::string(current_use_case.c_str());
                          }
                        else
                          {
                            NODELET_WARN_STREAM("current_use_case is stale!");
                          }
                      }
                    }
                }
              else
                {
                  // read-only parameter
                  continue;
                }

              //
              // Hard stop!
              //
              if (status != OK_)
                {
                  resp.status = (int) status;
                  resp.msg =
                    std::string(royale::getErrorString(status).c_str());

                  NODELET_WARN_STREAM("While processing: "
                                      << key << " -> (" << resp.status
                                      << ") " << resp.msg);
                  NODELET_INFO_STREAM("json was:\n" << req.json);
                  return true;
                }
            }
        }
    }

  resp.status = 0;
  resp.msg = "OK";
  return true;
}

bool
royale_ros::CameraNodelet::Dump(royale_ros::Dump::Request& req,
                                royale_ros::Dump::Response& resp)
{
  std::lock_guard<std::mutex> lock(this->cam_mutex_);
  if (this->cam_ == nullptr)
    {
      NODELET_ERROR_STREAM("No camera instantiated with serial number: "
                           << this->serial_number_);
      return false;
    }

  //
  // "Device" information
  //
  royale::String r_string;
  std::unordered_map<std::string, std::string> device_info;
  if (this->cam_->getId(r_string) == OK_)
    {
      device_info.emplace(std::make_pair("Id", std::string(r_string.c_str())));
    }

  if (this->cam_->getCameraName(r_string) == OK_)
    {
      device_info.emplace(
        std::make_pair("Name", std::string(r_string.c_str())));
    }

  //
  // "Imager" information
  //

  royale::Vector<royale::String> use_cases;
  json uc_vec; // list
  if (this->cam_->getUseCases(use_cases) == OK_)
    {
      std::transform(use_cases.begin(), use_cases.end(),
                     std::back_inserter(uc_vec),
                     [](royale::String& s) -> std::string
                     { return std::string(s.c_str()); });
    }

  std::string current_use_case;
  {
    std::lock_guard<std::mutex> lock(this->current_use_case_mutex_);
    current_use_case = this->current_use_case_;
  }

  std::uint32_t nstreams = 0;
  this->cam_->getNumberOfStreams(royale::String(current_use_case.c_str()),
                                 nstreams);

  royale::Vector<royale::StreamId> streamids;
  json streams; // list
  if (this->cam_->getStreams(streamids) == OK_)
    {
      std::transform(streamids.begin(), streamids.end(),
                     std::back_inserter(streams),
                     [](royale::StreamId& s) -> std::string
                     { return std::to_string((std::uint16_t) s); });
    }

  std::map<std::string, std::vector<std::string> > exp_limits;
  for (auto& sid : streamids)
    {
      royale::Pair<std::uint32_t, std::uint32_t> limits;
      if (this->cam_->getExposureLimits(limits, sid) == OK_)
        {
          std::vector<std::string> l;
          l.push_back(std::to_string(limits.first));
          l.push_back(std::to_string(limits.second));
          exp_limits.emplace(
            std::make_pair(std::to_string((std::uint16_t) sid), l));
        }
    }

  std::uint16_t max_width = 0;
  std::uint16_t max_height = 0;
  this->cam_->getMaxSensorWidth(max_width);
  this->cam_->getMaxSensorHeight(max_height);

  std::uint16_t max_fps = 0;
  std::uint16_t fps;
  this->cam_->getMaxFrameRate(max_fps);
  this->cam_->getFrameRate(fps);

  //
  // Serialize to JSON
  //
  json j =
    {
      {"Device", json(device_info)},
      {"Imager",
       {
         {"MaxSensorWidth", std::to_string(max_width)},
         {"MaxSensorHeight", std::to_string(max_height)},
         {"UseCases", uc_vec},
         {"CurrentUseCase",
          {
            {"Name", current_use_case},
            {"NumberOfStreams", std::to_string(nstreams)},
            {"Streams", streams},
            {"ExposureLimits", json(exp_limits)},
            {"FrameRate", std::to_string(fps)},
            {"MaxFrameRate", std::to_string(max_fps)}
          }
         }
       }
      }
    };

  resp.config = j.dump(2);
  return true;
}

void
royale_ros::CameraNodelet::onNewData(const royale::DepthData *data)
{
  auto stamp = ros::Time((double) data->timeStamp.count()/1e6);
  {
    std::lock_guard<std::mutex> lock(this->last_frame_mutex_);
    this->last_frame_ = stamp;
  }

  // Determine the index into the publishers vector that we will push this
  // image stream out to -- we do this so the function generalizes to mixed-mode
  // use cases
  int idx = 0;
  try
    {
      auto& stream_ids = this->stream_id_lut_.at(this->current_use_case_);
      auto result = std::find(stream_ids.begin(), stream_ids.end(),
                              (std::uint16_t) data->streamId);
      if (result != stream_ids.end())
        {
          //NODELET_INFO_STREAM("Cache Hit!");
          idx = std::distance(stream_ids.begin(), result);
        }
      else
        {
          NODELET_INFO_STREAM("StreamId cache miss: " << (int) data->streamId);
          stream_ids.push_back(data->streamId);
          idx = stream_ids.size() - 1;
        }
    }
  catch (const std::out_of_range& ex)
    {
      NODELET_ERROR_STREAM(ex.what());
      NODELET_WARN_STREAM("No publisher for use case: "
                          << this->current_use_case_);
      return;
    }

  //
  // NOTE: we will publish all data in the optical frame and just rely on a
  // static tf to convert to a standard ROS sensor frame. In our example launch
  // file we will provide a static_transform_publisher for this.
  //
  std_msgs::Header head = std_msgs::Header();
  head.stamp = stamp;
  head.frame_id = this->optical_frame_;

  //
  // Exposure times
  //
  royale_ros::ExposureTimes exposure_msg;
  exposure_msg.header = head;
  std::copy(data->exposureTimes.begin(), data->exposureTimes.end(),
            std::back_inserter(exposure_msg.usec));
  try
    {
      auto& ex_pub = this->exposure_pubs_.at(idx);
      ex_pub.publish(exposure_msg);
    }
  catch (const std::out_of_range& ex)
    {
      NODELET_ERROR_STREAM("Could not publish exposures: " << ex.what());
    }

  //
  // Loop over the pixel data and publish the images
  //
  pcl::PointCloud<pcl::PointXYZI>::Ptr
    cloud_(new pcl::PointCloud<pcl::PointXYZI>());

  cv::Mat gray_, conf_, noise_;
  gray_.create(data->height, data->width, CV_16UC1);
  conf_.create(data->height, data->width, CV_8UC1);
  noise_.create(data->height, data->width, CV_32FC1);

  std::uint16_t* gray_ptr;
  std::uint8_t* conf_ptr;
  float* noise_ptr;

  std::size_t npts = data->points.size();
  int col = 0;
  int row = -1;

  cloud_->header.frame_id = this->optical_frame_;
  cloud_->width = data->width;
  cloud_->height = data->height;
  cloud_->is_dense = true;
  cloud_->points.resize(npts);

  for (std::size_t i = 0; i < npts; ++i)
    {
      pcl::PointXYZI& pt = cloud_->points[i];

      col = i % data->width;

      if (col == 0)
        {
          row += 1;
          gray_ptr = gray_.ptr<std::uint16_t>(row);
          conf_ptr = conf_.ptr<std::uint8_t>(row);
          noise_ptr = noise_.ptr<float>(row);
        }

      gray_ptr[col] = data->points[i].grayValue;
      conf_ptr[col] = data->points[i].depthConfidence;
      noise_ptr[col] = data->points[i].noise;

      pt.x = data->points[i].x;
      pt.y = data->points[i].y;
      pt.z = data->points[i].z;
      pt.data_c[0] = pt.data_c[1] = pt.data_c[2] = pt.data_c[3] = 0;
      pt.intensity = data->points[i].grayValue;
    }

  //
  // Create the image messages
  //
  sensor_msgs::ImagePtr gray_msg =
    cv_bridge::CvImage(head, enc::TYPE_16UC1, gray_).toImageMsg();
  sensor_msgs::ImagePtr conf_msg =
    cv_bridge::CvImage(head, enc::TYPE_8UC1, conf_).toImageMsg();
  sensor_msgs::ImagePtr noise_msg =
    cv_bridge::CvImage(head, enc::TYPE_32FC1, noise_).toImageMsg();
  cloud_->header = pcl_conversions::toPCL(head);

  //
  // Publish the data
  //
  try
    {
      this->gray_pubs_.at(idx).publish(gray_msg);
      this->conf_pubs_.at(idx).publish(conf_msg);
      this->noise_pubs_.at(idx).publish(noise_msg);
      this->cloud_pubs_.at(idx).publish(cloud_);
    }
  catch (const std::out_of_range& ex)
    {
      // If this happens, it is a bug. Please report it at:
      // https://github.com/lovepark/royale-ros/issues
      NODELET_ERROR_STREAM("Could not publish image message: " << ex.what());
    }
}

PLUGINLIB_EXPORT_CLASS(royale_ros::CameraNodelet, nodelet::Nodelet)
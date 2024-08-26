/*******************************************************************************
* Copyright 2020 ROBOTIS CO., LTD.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
/* Authors: Hye-Jong KIM, Yong-Ho Na */

#include "dynamixel_hardware_interface/dynamixel_hardware_interface.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include <string>

#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "rclcpp/rclcpp.hpp"

namespace dynamixel_hardware_interface
{

DynamixelHardware::DynamixelHardware()
: rclcpp::Node("dynamixel_hardware_interface"),
  logger_(rclcpp::get_logger("dynamixel_hardware_interface"))
{
  // init. error state
  dxl_status_ = DXL_OK;
  dxl_torque_status_ = TORQUE_ENABLED;
  err_timeout_sec_ = 3.0;
}

DynamixelHardware::~DynamixelHardware()
{
  if (ros_update_thread_.joinable()) {
    ros_update_thread_.join();
  }
  stop();
}

hardware_interface::CallbackReturn DynamixelHardware::on_init(
  const hardware_interface::HardwareInfo & info)
{
  if (hardware_interface::SystemInterface::on_init(info) != hardware_interface::CallbackReturn::SUCCESS) {
    return hardware_interface::CallbackReturn::ERROR;
  }

  num_of_joints_ = static_cast<size_t>(stoi(info_.hardware_parameters["number_of_joints"]));
  num_of_transmissions_ =
    static_cast<size_t>(stoi(info_.hardware_parameters["number_of_transmissions"]));
  SetMatrix();

  ////////// communication setting
  // Dynamixel Communication Setting
  port_name_ = info_.hardware_parameters["port_name"];
  baud_rate_ = info_.hardware_parameters["baud_rate"];
  err_timeout_sec_ = stod(info_.hardware_parameters["error_timeout_sec"]);

  RCLCPP_INFO_STREAM(
    logger_,
    "port_name " << port_name_.c_str() << " / baudrate " << baud_rate_.c_str());

  // Dynamixel Model Setting
  std::string dxl_model_folder = info_.hardware_parameters["dynamixel_model_folder"];
  dxl_comm_ = std::unique_ptr<Dynamixel>(
    new Dynamixel(
      (ament_index_cpp::get_package_share_directory("dynamixel_hardware_interface") +
      dxl_model_folder).c_str()));

  ////////// gpio (dxl) setting
  // init communication
  RCLCPP_INFO_STREAM(logger_, "$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$");
  RCLCPP_INFO_STREAM(logger_, "$$$$$ Init Dxl Comm Port");
  for (const hardware_interface::ComponentInfo & gpio : info_.gpios) {
    if (gpio.parameters.at("type") == "dxl") {
      dxl_id_.push_back(static_cast<uint8_t>(stoi(gpio.parameters.at("ID"))));
    } else if (gpio.parameters.at("type") == "sensor") {
      sensor_id_.push_back(static_cast<uint8_t>(stoi(gpio.parameters.at("ID"))));
    } else {
      RCLCPP_ERROR_STREAM(logger_, "Invalid DXL / Sensoe type");
      exit(-1);
    }
  }

  bool trying_connect = true;
  int trying_cnt = 60;  // second
  int cnt = 0;

  while (trying_connect) {
    std::vector<uint8_t> id_arr;
    for (auto dxl : dxl_id_) {
      id_arr.push_back(dxl);
    }
    for (auto sensor : sensor_id_) {
      id_arr.push_back(sensor);
    }
    // init communication
    if (dxl_comm_->InitDxlComm(id_arr, port_name_, baud_rate_) == DxlError::OK) {
      RCLCPP_INFO_STREAM(logger_, "Trying to connect to the communication port...");
      trying_connect = false;
    } else {
      sleep(1);
      cnt++;
      if (cnt > trying_cnt) {
        RCLCPP_ERROR_STREAM(logger_, "Cannot connect communication port! :(");
        cnt = 0;
      }
    }
  }  // end of while

  // item initialization
  if (!InitDxlItems()) {
    return hardware_interface::CallbackReturn::ERROR;
  }

  // set read items & transmissions handler initialization
  if (!InitDxlReadItems()) {
    return hardware_interface::CallbackReturn::ERROR;
  }

  // set write items & transmissions handler initialization
  if (!InitDxlWriteItems()) {
    return hardware_interface::CallbackReturn::ERROR;
  }

  if (num_of_transmissions_ != hdl_trans_commands_.size() &&
    num_of_transmissions_ != hdl_trans_states_.size())
  {
    RCLCPP_ERROR_STREAM(
      logger_, "Error: number of transmission " << num_of_transmissions_ << ", " <<
        hdl_trans_commands_.size() << ", " << hdl_trans_states_.size());
    return hardware_interface::CallbackReturn::ERROR;
  }

  ////////// set comm reset flag
  dxl_status_ = DXL_OK;

  ////////// joint handler setting
  hdl_joint_states_.clear();
  for (const hardware_interface::ComponentInfo & joint : info_.joints) {
    HandlerVarType temp_state;
    temp_state.name = joint.name;

    // position
    temp_state.interface_name_vec.push_back(hardware_interface::HW_IF_POSITION);
    temp_state.value_ptr_vec.push_back(std::make_shared<double>(0.0));

    // velocity
    temp_state.interface_name_vec.push_back(hardware_interface::HW_IF_VELOCITY);
    temp_state.value_ptr_vec.push_back(std::make_shared<double>(0.0));

    // effort
    temp_state.interface_name_vec.push_back(hardware_interface::HW_IF_EFFORT);
    temp_state.value_ptr_vec.push_back(std::make_shared<double>(0.0));

    for (auto it : joint.state_interfaces) {
      if (hardware_interface::HW_IF_POSITION != it.name &&
        hardware_interface::HW_IF_VELOCITY != it.name &&
        hardware_interface::HW_IF_ACCELERATION != it.name &&
        hardware_interface::HW_IF_EFFORT != it.name &&
        HW_IF_HARDWARE_STATE != it.name &&
        HW_IF_TORQUE_ENABLE != it.name)
      {
        return hardware_interface::CallbackReturn::ERROR;
      }
      if (it.name != hardware_interface::HW_IF_POSITION &&
        it.name != hardware_interface::HW_IF_VELOCITY &&
        it.name != hardware_interface::HW_IF_EFFORT)
      {
        temp_state.interface_name_vec.push_back(it.name);
        temp_state.value_ptr_vec.push_back(std::make_shared<double>(0.0));
      }
    }
    hdl_joint_states_.push_back(temp_state);
  }

  hdl_joint_commands_.clear();
  for (const hardware_interface::ComponentInfo & joint : info_.joints) {
    HandlerVarType temp_cmd;
    temp_cmd.name = joint.name;

    for (auto it : joint.command_interfaces) {
      if (hardware_interface::HW_IF_POSITION != it.name &&
        hardware_interface::HW_IF_VELOCITY != it.name &&
        hardware_interface::HW_IF_ACCELERATION != it.name &&
        hardware_interface::HW_IF_EFFORT != it.name)
      {
        return hardware_interface::CallbackReturn::ERROR;
      }
      temp_cmd.interface_name_vec.push_back(it.name);
      temp_cmd.value_ptr_vec.push_back(std::make_shared<double>(0.0));
    }
    hdl_joint_commands_.push_back(temp_cmd);
  }

  if (num_of_joints_ != hdl_joint_commands_.size() &&
    num_of_joints_ != hdl_joint_states_.size())
  {
    RCLCPP_ERROR_STREAM(
      logger_, "Error: number of joints " << num_of_joints_ << ", " <<
        hdl_joint_commands_.size() << ", " << hdl_joint_commands_.size());
    return hardware_interface::CallbackReturn::ERROR;
  }

  if (num_of_joints_ != hdl_joint_commands_.size() &&
    num_of_joints_ != hdl_joint_states_.size())
  {
    RCLCPP_ERROR_STREAM(
      logger_, "Error: number of joints " << num_of_joints_ << ", " <<
        hdl_joint_commands_.size() << ", " << hdl_joint_commands_.size());
    return hardware_interface::CallbackReturn::ERROR;
  }

  ////////// sensor handler setting
  hdl_sensor_states_.clear();
  for (const hardware_interface::ComponentInfo & sensor : info_.sensors) {
    HandlerVarType temp_state;
    temp_state.name = sensor.name;

    for (auto it : sensor.state_interfaces) {
      temp_state.interface_name_vec.push_back(it.name);
      temp_state.value_ptr_vec.push_back(std::make_shared<double>(0.0));
    }
    hdl_sensor_states_.push_back(temp_state);
  }

  ///// ROS param
  // ros msg pub
  std::string str_dxl_state_pub_name =
    info_.hardware_parameters["dynamixel_state_pub_msg_name"];
  dxl_state_pub_ = this->create_publisher<DynamixelStateMsg>(
    str_dxl_state_pub_name, rclcpp::SystemDefaultsQoS());
  dxl_state_pub_uni_ptr_ = std::make_unique<StatePublisher>(dxl_state_pub_);

  size_t num_of_pub_data = hdl_trans_states_.size();
  dxl_state_pub_uni_ptr_->lock();
  dxl_state_pub_uni_ptr_->msg_.id.resize(num_of_pub_data);
  dxl_state_pub_uni_ptr_->msg_.dxl_hw_state.resize(num_of_pub_data);
  dxl_state_pub_uni_ptr_->msg_.torque_state.resize(num_of_pub_data);
  dxl_state_pub_uni_ptr_->unlock();

  // ros srv server
  using namespace std::placeholders;
  std::string str_get_dxl_data_srv_name =
    info_.hardware_parameters["get_dynamixel_data_srv_name"];
  get_dxl_data_srv_ = create_service<dynamixel_interfaces::srv::GetDataFromDxl>(
    str_get_dxl_data_srv_name,
    std::bind(&DynamixelHardware::get_dxl_data_srv_callback, this, _1, _2));

  std::string str_set_dxl_data_srv_name =
    info_.hardware_parameters["set_dynamixel_data_srv_name"];
  set_dxl_data_srv_ = create_service<dynamixel_interfaces::srv::SetDataToDxl>(
    str_set_dxl_data_srv_name,
    std::bind(&DynamixelHardware::set_dxl_data_srv_callback, this, _1, _2));

  std::string str_reboot_dxl_srv_name =
    info_.hardware_parameters["reboot_dxl_srv_name"];
  reboot_dxl_srv_ = create_service<dynamixel_interfaces::srv::RebootDxl>(
    str_reboot_dxl_srv_name,
    std::bind(&DynamixelHardware::reboot_dxl_srv_callback, this, _1, _2));

  std::string str_set_dxl_torque_srv_name =
    info_.hardware_parameters["set_dxl_torque_srv_name"];
  set_dxl_torque_srv_ = create_service<std_srvs::srv::SetBool>(
    str_set_dxl_torque_srv_name,
    std::bind(&DynamixelHardware::set_dxl_torque_srv_callback, this, _1, _2));

  ///// ros publish & ros spin thread
  ros_update_freq_ = stoi(info_.hardware_parameters["ros_update_freq"]);
  ros_update_thread_ = std::thread(
    [this]() {
      RCLCPP_INFO_STREAM(logger_, "ros_update rate is " << ros_update_freq_ << "hz");
      while (rclcpp::ok()) {
        // dxl state pub
        size_t index = 0;
        if (dxl_state_pub_uni_ptr_ && dxl_state_pub_uni_ptr_->trylock()) {
          dxl_state_pub_uni_ptr_->msg_.header.stamp = this->now();
          dxl_state_pub_uni_ptr_->msg_.comm_state = dxl_comm_err_;
          for (auto it : hdl_trans_states_) {
            dxl_state_pub_uni_ptr_->msg_.id.at(index) = it.id;
            dxl_state_pub_uni_ptr_->msg_.dxl_hw_state.at(index) = dxl_hw_err_[it.id];
            dxl_state_pub_uni_ptr_->msg_.torque_state.at(index) = dxl_torque_state_[it.id];
            index++;
          }
          dxl_state_pub_uni_ptr_->unlockAndPublish();
        }

        // ros spin
        rclcpp::spin_some(this->get_node_base_interface());

        // ros_update thread time sleep
        struct timespec duration{};
        duration.tv_nsec = 1000000000 / ros_update_freq_;
        clock_nanosleep(CLOCK_MONOTONIC, 0, &duration, nullptr);
      }
    }
  );

  return hardware_interface::CallbackReturn::SUCCESS;
}

std::vector<hardware_interface::StateInterface>
DynamixelHardware::export_state_interfaces()
{
  std::vector<hardware_interface::StateInterface> state_interfaces;

  // transmissions
  for (auto it : hdl_trans_states_) {
    for (size_t i = 0; i < it.value_ptr_vec.size(); i++) {
      state_interfaces.emplace_back(
        hardware_interface::StateInterface(
          it.name, it.interface_name_vec.at(i), it.value_ptr_vec.at(i).get()));
    }
  }
  // joints
  for (auto it : hdl_joint_states_) {
    for (size_t i = 0; i < it.value_ptr_vec.size(); i++) {
      state_interfaces.emplace_back(
        hardware_interface::StateInterface(
          it.name, it.interface_name_vec.at(i), it.value_ptr_vec.at(i).get()));
    }
  }
  // sensors
  for (auto it : hdl_sensor_states_) {
    for (size_t i = 0; i < it.value_ptr_vec.size(); i++) {
      state_interfaces.emplace_back(
        hardware_interface::StateInterface(
          it.name, it.interface_name_vec.at(i), it.value_ptr_vec.at(i).get()));
    }
  }
  return state_interfaces;
}

std::vector<hardware_interface::CommandInterface>
DynamixelHardware::export_command_interfaces()
{
  std::vector<hardware_interface::CommandInterface> command_interfaces;

  // transmissions
  for (auto it : hdl_trans_commands_) {
    for (size_t i = 0; i < it.value_ptr_vec.size(); i++) {
      command_interfaces.emplace_back(
        hardware_interface::CommandInterface(
          it.name, it.interface_name_vec.at(i), it.value_ptr_vec.at(i).get()));
    }
  }
  // joints
  for (auto it : hdl_joint_commands_) {
    for (size_t i = 0; i < it.value_ptr_vec.size(); i++) {
      command_interfaces.emplace_back(
        hardware_interface::CommandInterface(
          it.name, it.interface_name_vec.at(i), it.value_ptr_vec.at(i).get()));
    }
  }
  return command_interfaces;
}

hardware_interface::CallbackReturn DynamixelHardware::on_activate(
  const rclcpp_lifecycle::State & previous_state)
{
  return start();
}

hardware_interface::CallbackReturn DynamixelHardware::on_deactivate(
  const rclcpp_lifecycle::State & previous_state)
{
  return stop();
}

hardware_interface::CallbackReturn DynamixelHardware::start()
{
  // read present state from dxl
  dxl_comm_err_ = CheckError(dxl_comm_->ReadMultiDxlData());
  if (dxl_comm_err_ != DxlError::OK) {
    RCLCPP_ERROR_STREAM(
      logger_,
      "Dynamixel Start Fail :" << Dynamixel::DxlErrorToString(dxl_comm_err_));
    return hardware_interface::CallbackReturn::ERROR;
  }

  // actuator to handler
  CalcTransmissionToJoint();

  // sync commands = states joint
  for (auto it_states : hdl_joint_states_) {
    for (auto it_commands : hdl_joint_commands_) {
      if (it_states.name == it_commands.name) {
        for (size_t i = 0; i < it_states.interface_name_vec.size(); i++) {
          if (it_commands.interface_name_vec.at(0) == it_states.interface_name_vec.at(i)) {
            *it_commands.value_ptr_vec.at(0) = *it_states.value_ptr_vec.at(i);
            RCLCPP_INFO_STREAM(
              logger_, "Sync joint state to command (" <<
                it_commands.interface_name_vec.at(0).c_str() << ", " <<
                *it_commands.value_ptr_vec.at(0) * 180.0 / 3.141592 << " <- " <<
                it_states.interface_name_vec.at(i).c_str() << ", " <<
                *it_states.value_ptr_vec.at(i) * 180.0 / 3.141592);
          }
        }
      }
    }
  }

  usleep(500 * 1000);  // 500ms

  // torque on
  dxl_comm_->DynamixelEnable(dxl_id_);

  RCLCPP_INFO_STREAM(logger_, "Dynamixel Hardware Start!");

  return hardware_interface::CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DynamixelHardware::stop()
{
  // torque off
  dxl_comm_->DynamixelDisable(dxl_id_);

  RCLCPP_INFO_STREAM(logger_, "Dynamixel Hardware Stop!");

  return hardware_interface::CallbackReturn::SUCCESS;
}

hardware_interface::return_type DynamixelHardware::read(
    const rclcpp::Time & time, const rclcpp::Duration & period)
{
  if (dxl_status_ == REBOOTING) {
    return hardware_interface::return_type::ERROR;
  } else if (dxl_status_ == DXL_OK || dxl_status_ == COMM_ERROR) {
    // read dxl
    dxl_comm_err_ = CheckError(dxl_comm_->ReadMultiDxlData());
    if (dxl_comm_err_ != DxlError::OK) {
      RCLCPP_ERROR_STREAM(
        logger_,
        "Dynamixel Read Fail :" << Dynamixel::DxlErrorToString(dxl_comm_err_));
      return hardware_interface::return_type::ERROR;
    }
  } else if (dxl_status_ == HW_ERROR) {
    // read dxl
    dxl_comm_err_ = CheckError(dxl_comm_->ReadMultiDxlData());
    if (dxl_comm_err_ != DxlError::OK) {
      RCLCPP_ERROR_STREAM(
        logger_,
        "Dynamixel Read Fail :" << Dynamixel::DxlErrorToString(dxl_comm_err_));
    }
  }

  // actuator to handler
  CalcTransmissionToJoint();

  // read sensor
  for (auto sensor : hdl_gpio_sensor_states_) {
    ReadSensorData(sensor);
  }

  // read item buffer
  dxl_comm_->ReadItemBuf();

  return hardware_interface::return_type::OK;
}

hardware_interface::return_type DynamixelHardware::write(
    const rclcpp::Time & time, const rclcpp::Duration & period)
{
  if (dxl_status_ == DXL_OK || dxl_status_ == HW_ERROR) {
    // write item buffer
    dxl_comm_->WriteItemBuf();

    // write torque state
    ChangeDxlTorqueState();

    // handler to actuator
    CalcJointToTransmission();

    // write dxl
    dxl_comm_->WriteMultiDxlData();
    // USB에서 tx가 성공하면 error 코드를 발생시키지 않으므로 dxl에 tx가 성공했는지 알 수 없음
    // -> error 체크는 read 함수에서만 실행

    return hardware_interface::return_type::OK;
  } else {
    // REBOOTING and COMM_ERROR
    return hardware_interface::return_type::ERROR;
  }
}

DxlError DynamixelHardware::CheckError(DxlError dxl_comm_err)
{
  DxlError error_state = DxlError::OK;

  // check comm error
  if (dxl_comm_err != DxlError::OK) {
    RCLCPP_ERROR_STREAM(
      logger_,
      "Communication Fail --> " << Dynamixel::DxlErrorToString(dxl_comm_err));
    dxl_status_ = COMM_ERROR;
    return dxl_comm_err;
  }
  // check hardware error
  for (size_t i = 0; i < num_of_transmissions_; i++) {
    for (size_t j = 0; j < hdl_trans_states_.at(i).interface_name_vec.size(); j++) {
      if (hdl_trans_states_.at(i).interface_name_vec.at(j) == "Hardware Error Status") {
        dxl_hw_err_[hdl_trans_states_.at(i).id] = *hdl_trans_states_.at(i).value_ptr_vec.at(j);
        std::string error_string = "";
        if (dxl_hw_err_[hdl_trans_states_.at(i).id] & 0x01) {  // input voltage error
          error_string += "input voltage error/ ";
        }
        if (dxl_hw_err_[hdl_trans_states_.at(i).id] & 0x04) {  // overheating
          error_string += "overheating/ ";
        }
        if (dxl_hw_err_[hdl_trans_states_.at(i).id] & 0x08) {  // motor encoder
          error_string += "motor encoder/ ";
        }
        if (dxl_hw_err_[hdl_trans_states_.at(i).id] & 0x16) {  // electrical shork
          error_string += "electrical shork/ ";
        }
        if (dxl_hw_err_[hdl_trans_states_.at(i).id] & 0x32) {  // Overload
          error_string += "Overload/ ";
        }

        if (!error_string.empty()) {
          RCLCPP_WARN_STREAM(
            logger_, "Dynamixel Hardware Error States [ ID:" <<
              static_cast<int>(hdl_trans_states_.at(i).id) << "] --> " <<
              static_cast<int>(dxl_hw_err_[hdl_trans_states_.at(i).id]) <<
              "/ " << error_string);
          dxl_status_ = HW_ERROR;
          error_state = DxlError::DLX_HARDWARE_ERROR;
        }
      }
    }
  }

  // Setup joint state handlers
  for (size_t i = 0; i < num_of_joints_; i++) {
    for (size_t j = 0; j < hdl_joint_states_.at(i).interface_name_vec.size(); j++) {
      if (hdl_joint_states_.at(i).interface_name_vec.at(j) == HW_IF_HARDWARE_STATE) {
        *hdl_joint_states_.at(i).value_ptr_vec.at(j) = error_state;
      }
    }
  }

  return error_state;
}

bool DynamixelHardware::CommReset()
{
  dxl_status_ = REBOOTING;
  stop();

  RCLCPP_INFO_STREAM(logger_, "Communication Reset Start");
  // clear comm port handler clear
  dxl_comm_->RWDataReset();

  // Get the start time
  auto start_time = this->now();
  while ((this->now() - start_time) < rclcpp::Duration(3, 0)) {
    usleep(200 * 1000);
    RCLCPP_INFO_STREAM(logger_, "Reset Start");
    // reboot dxl
    bool result = true;
    for (auto id : dxl_id_) {
      if (dxl_comm_->Reboot(id) != DxlError::OK) {
        RCLCPP_ERROR_STREAM(logger_, "Cannot reboot dynamixel! :(");
        result = false;
        break;
      }
      usleep(200 * 1000);
    }
    if (!result) {continue;}
    // 다이나믹셀의 초기 설정값을 세팅합니다.
    if (!InitDxlItems()) {continue;}
    // 다이나믹셀의 read item을 설정합니다.
    if (!InitDxlReadItems()) {continue;}
    // 다이나믹셀의 write item을 설정합니다.
    if (!InitDxlWriteItems()) {continue;}

    // Reboot Success
    RCLCPP_INFO_STREAM(logger_, "RESET Success");
    usleep(1000 * 1000);
    start();
    dxl_status_ = DXL_OK;
    return true;
  }
  RCLCPP_ERROR_STREAM(logger_, "RESET Failure");
  usleep(1000 * 1000);
  start();
  return false;
}

bool DynamixelHardware::InitDxlItems()
{
  RCLCPP_INFO_STREAM(logger_, "$$$$$ Init Dxl Items");
  for (const hardware_interface::ComponentInfo & gpio : info_.gpios) {
    uint8_t id = static_cast<uint8_t>(stoi(gpio.parameters.at("ID")));
    for (auto it : gpio.parameters) {
      if (it.first != "ID" && it.first != "type") {
        if (dxl_comm_->WriteItem(
            id, it.first,
            static_cast<uint32_t>(stoi(it.second))) != DxlError::OK)
        {
          RCLCPP_ERROR_STREAM(logger_, "[ID:" << std::to_string(id) << "] Wtite Item error");
          return false;
        }
        RCLCPP_INFO_STREAM(
          logger_,
          "[ID:" << std::to_string(id) << "] item_name:" << it.first.c_str() << "\tdata:" <<
            stoi(it.second));
      }
    }
  }
  return true;
}

bool DynamixelHardware::InitDxlReadItems()
{
  RCLCPP_INFO_STREAM(logger_, "$$$$$ Init Dxl Read Items");
  static bool is_set_hdl = false;

  if (!is_set_hdl) {
    hdl_trans_states_.clear();
    hdl_gpio_sensor_states_.clear();
    for (const hardware_interface::ComponentInfo & gpio : info_.gpios) {
      if (gpio.state_interfaces.size() && gpio.parameters.at("type") == "dxl") {
        // read item
        uint8_t id = static_cast<uint8_t>(stoi(gpio.parameters.at("ID")));
        HandlerVarType temp_read;

        temp_read.id = id;
        temp_read.name = gpio.name;

        // Present Position
        temp_read.interface_name_vec.push_back("Present Position");
        temp_read.value_ptr_vec.push_back(std::make_shared<double>(0.0));

        // Present Velocity
        temp_read.interface_name_vec.push_back("Present Velocity");
        temp_read.value_ptr_vec.push_back(std::make_shared<double>(0.0));

        // effort third
        for (auto it : gpio.state_interfaces) {
          if (it.name == "Present Current" || it.name == "Present Load") {
            temp_read.interface_name_vec.push_back(it.name);
            temp_read.value_ptr_vec.push_back(std::make_shared<double>(0.0));
          }
        }

        for (auto it : gpio.state_interfaces) {
          if (it.name != "Present Position" && it.name != "Present Velocity" &&
            it.name != "Present Current" && it.name != "Present Load")
          {
            temp_read.interface_name_vec.push_back(it.name);
            temp_read.value_ptr_vec.push_back(std::make_shared<double>(0.0));

            // init. hardware error state
            if (it.name == "Hardware Error Status") {
              dxl_hw_err_[id] = 0x00;
            }
          }
        }
        hdl_trans_states_.push_back(temp_read);

      } else if (gpio.state_interfaces.size() && gpio.parameters.at("type") == "sensor") {
        HandlerVarType temp_sensor;
        for (auto it : gpio.state_interfaces) {
          uint8_t id = static_cast<uint8_t>(stoi(gpio.parameters.at("ID")));

          temp_sensor.id = id;
          temp_sensor.name = gpio.name;
          for (auto it : gpio.state_interfaces) {
            temp_sensor.interface_name_vec.push_back(it.name);
            temp_sensor.value_ptr_vec.push_back(std::make_shared<double>(0.0));
          }
        }
        hdl_gpio_sensor_states_.push_back(temp_sensor);
      }
    }
    is_set_hdl = true;
  }
  for (auto it : hdl_trans_states_) {
    if (dxl_comm_->SetDxlReadItems(
        it.id, it.interface_name_vec,
        it.value_ptr_vec) != DxlError::OK)
    {
      return false;
    }
  }
  if (dxl_comm_->SetMultiDxlRead() != DxlError::OK) {
    return false;
  }
  return true;
}

bool DynamixelHardware::InitDxlWriteItems()
{
  RCLCPP_INFO_STREAM(logger_, "$$$$$ Init Dxl Write Items");
  static bool is_set_hdl = false;

  if (!is_set_hdl) {
    hdl_trans_commands_.clear();
    for (const hardware_interface::ComponentInfo & gpio : info_.gpios) {
      if (gpio.command_interfaces.size()) {
        // write item
        uint8_t id = static_cast<uint8_t>(stoi(gpio.parameters.at("ID")));
        for (auto it : gpio.command_interfaces) {
          HandlerVarType temp_write;
          temp_write.id = id;
          temp_write.name = gpio.name;

          temp_write.interface_name_vec.push_back(it.name);
          temp_write.value_ptr_vec.push_back(std::make_shared<double>(0.0));
          hdl_trans_commands_.push_back(temp_write);
        }
      }
    }
    is_set_hdl = true;
  }

  for (auto it : hdl_trans_commands_) {
    if (dxl_comm_->SetDxlWriteItems(
        it.id, it.interface_name_vec,
        it.value_ptr_vec) != DxlError::OK)
    {
      return false;
    }
  }

  if (dxl_comm_->SetMultiDxlWrite() != DxlError::OK) {
    return false;
  }

  return true;
}

void DynamixelHardware::ReadSensorData(const HandlerVarType & sensor)
{
  for (auto item : sensor.interface_name_vec) {
    uint32_t data;
    dxl_comm_->ReadItem(sensor.id, item, data);
    for (size_t i = 0; i < hdl_sensor_states_.size(); i++) {
      for (size_t j = 0; j < hdl_sensor_states_.at(i).interface_name_vec.size(); j++) {
        if (hdl_sensor_states_.at(i).name == sensor.name &&
          hdl_sensor_states_.at(i).interface_name_vec.at(j) == item)
        {
          *hdl_sensor_states_.at(i).value_ptr_vec.at(j) = data;
        }
      }
    }
  }
}

void DynamixelHardware::SetMatrix()
{
  std::string str;
  std::vector<double> d_vec;

  /////////////////////
  // dynamic allocation (number_of_transmissions x number_of_joint)
  transmission_to_joint_matrix_ = new double *[num_of_joints_];
  for (size_t i = 0; i < num_of_joints_; i++) {
    transmission_to_joint_matrix_[i] = new double[num_of_transmissions_];
  }

  d_vec.clear();
  std::stringstream ss_tj(info_.hardware_parameters["transmission_to_joint_matrix"]);
  while (std::getline(ss_tj, str, ',')) {
    d_vec.push_back(stod(str));
  }
  for (size_t i = 0; i < num_of_joints_; i++) {
    for (size_t j = 0; j < num_of_transmissions_; j++) {
      transmission_to_joint_matrix_[i][j] = d_vec.at(i * num_of_transmissions_ + j);
    }
  }

  fprintf(stderr, "transmission_to_joint_matrix_ \n");
  for (size_t i = 0; i < num_of_joints_; i++) {
    for (size_t j = 0; j < num_of_transmissions_; j++) {
      fprintf(stderr, "[%zu][%zu] %lf, ", i, j, transmission_to_joint_matrix_[i][j]);
    }
    fprintf(stderr, "\n");
  }


  /////////////////////
  // dynamic allocation (number_of_joint x number_of_transmissions)
  joint_to_transmission_matrix_ = new double *[num_of_transmissions_];
  for (size_t i = 0; i < num_of_transmissions_; i++) {
    joint_to_transmission_matrix_[i] = new double[num_of_joints_];
  }

  d_vec.clear();
  std::stringstream ss_jt(info_.hardware_parameters["joint_to_transmission_matrix"]);
  while (std::getline(ss_jt, str, ',')) {
    d_vec.push_back(stod(str));
  }
  for (size_t i = 0; i < num_of_transmissions_; i++) {
    for (size_t j = 0; j < num_of_joints_; j++) {
      joint_to_transmission_matrix_[i][j] = d_vec.at(i * num_of_joints_ + j);
    }
  }


  fprintf(stderr, "joint_to_transmission_matrix_ \n");
  for (size_t i = 0; i < num_of_transmissions_; i++) {
    for (size_t j = 0; j < num_of_joints_; j++) {
      fprintf(stderr, "[%zu][%zu] %lf, ", i, j, joint_to_transmission_matrix_[i][j]);
    }
    fprintf(stderr, "\n");
  }
}
void DynamixelHardware::CalcTransmissionToJoint()
{
  // read
  // position
  for (size_t i = 0; i < num_of_joints_; i++) {
    double value = 0.0;
    for (size_t j = 0; j < num_of_transmissions_; j++) {
      value += transmission_to_joint_matrix_[i][j] *
        (*hdl_trans_states_.at(j).value_ptr_vec.at(PRESENT_POSITION_INDEX));
    }
    *hdl_joint_states_.at(i).value_ptr_vec.at(PRESENT_POSITION_INDEX) = value;
  }

  // velocity
  for (size_t i = 0; i < num_of_joints_; i++) {
    double value = 0.0;
    for (size_t j = 0; j < num_of_transmissions_; j++) {
      value += transmission_to_joint_matrix_[i][j] *
        (*hdl_trans_states_.at(j).value_ptr_vec.at(PRESENT_VELOCITY_INDEX));
    }
    *hdl_joint_states_.at(i).value_ptr_vec.at(PRESENT_VELOCITY_INDEX) = value;
  }

  // effort
  for (size_t i = 0; i < num_of_joints_; i++) {
    double value = 0.0;
    for (size_t j = 0; j < num_of_transmissions_; j++) {
      value += transmission_to_joint_matrix_[i][j] *
        (*hdl_trans_states_.at(j).value_ptr_vec.at(PRESENT_ERROT_INDEX));
    }
    *hdl_joint_states_.at(i).value_ptr_vec.at(PRESENT_ERROT_INDEX) = value;
  }
}

void DynamixelHardware::CalcJointToTransmission()
{
  // write
  for (size_t i = 0; i < num_of_transmissions_; i++) {
    double value = 0.0;
    for (size_t j = 0; j < num_of_joints_; j++) {
      value += joint_to_transmission_matrix_[i][j] *
        (*hdl_joint_commands_.at(j).value_ptr_vec.at(0));
    }
    *hdl_trans_commands_.at(i).value_ptr_vec.at(0) = value;
  }
}

void DynamixelHardware::SyncJointCommandWithStates()
{
  for (auto it_states : hdl_joint_states_) {
    for (auto it_commands : hdl_joint_commands_) {
      if (it_states.name == it_commands.name) {
        for (size_t i = 0; i < it_states.interface_name_vec.size(); i++) {
          if (it_commands.interface_name_vec.at(0) == it_states.interface_name_vec.at(i)) {
            *it_commands.value_ptr_vec.at(0) = *it_states.value_ptr_vec.at(i);
            RCLCPP_INFO_STREAM(
              logger_, "Sync joint state to command (" <<
                it_commands.interface_name_vec.at(0).c_str() << ", " <<
                *it_commands.value_ptr_vec.at(0) << " <- " <<
                it_states.interface_name_vec.at(i).c_str() << ", " <<
                *it_states.value_ptr_vec.at(i));
          }
        }
      }
    }
  }
}

void DynamixelHardware::ChangeDxlTorqueState()
{
  if (dxl_torque_status_ == REQUESTED_TO_ENABLE) {
    std::cout << "torque enalble" << std::endl;
    dxl_comm_->DynamixelEnable(dxl_id_);
    SyncJointCommandWithStates();
  } else if (dxl_torque_status_ == REQUESTED_TO_DISABLE) {
    std::cout << "torque disalble" << std::endl;
    dxl_comm_->DynamixelDisable(dxl_id_);
    SyncJointCommandWithStates();
  }

  dxl_torque_state_ = dxl_comm_->GetDxlTorqueState();
  for (auto single_torque_state : dxl_torque_state_) {
    if (single_torque_state.second == false) {
      dxl_torque_status_ = TORQUE_DISABLED;
      return;
    }
  }
  dxl_torque_status_ = TORQUE_ENABLED;
}

void DynamixelHardware::get_dxl_data_srv_callback(
  const std::shared_ptr<dynamixel_interfaces::srv::GetDataFromDxl::Request> request,
  std::shared_ptr<dynamixel_interfaces::srv::GetDataFromDxl::Response> response)
{
  uint8_t id = static_cast<uint8_t>(request->id);
  std::string name = request->item_name;

  if (dxl_comm_->InsertReadItemBuf(id, name) != DxlError::OK) {
    RCLCPP_ERROR_STREAM(logger_, "get_dxl_data_srv_callback InsertReadItemBuf");

    response->result = false;
    return;
  }
  double timeout_sec = request->timeout_sec;
  if (timeout_sec == 0.0) {
    timeout_sec = 1.0;
  }
  rclcpp::Time t_start = rclcpp::Clock().now();
  while (dxl_comm_->CheckReadItemBuf(id, name) == false) {
    // time check
    if ((rclcpp::Clock().now() - t_start).seconds() > timeout_sec) {
      RCLCPP_ERROR_STREAM(
        logger_,
        "get_dxl_data_srv_callback Timeout : " << (rclcpp::Clock().now() - t_start).seconds() );
      response->result = false;
      return;
    }
  }

  response->item_data = dxl_comm_->GetReadItemDataBuf(id, name);
  response->result = true;
}

void DynamixelHardware::set_dxl_data_srv_callback(
  const std::shared_ptr<dynamixel_interfaces::srv::SetDataToDxl::Request> request,
  std::shared_ptr<dynamixel_interfaces::srv::SetDataToDxl::Response> response)
{
  uint8_t dxl_id = static_cast<uint8_t>(request->id);
  uint32_t dxl_data = static_cast<uint32_t>(request->item_data);
  if (dxl_comm_->InsertWriteItemBuf(dxl_id, request->item_name, dxl_data) == DxlError::OK) {
    response->result = true;
  } else {
    response->result = false;
  }
}

void DynamixelHardware::reboot_dxl_srv_callback(
  const std::shared_ptr<dynamixel_interfaces::srv::RebootDxl::Request> request,
  std::shared_ptr<dynamixel_interfaces::srv::RebootDxl::Response> response)
{
  if (CommReset()) {
    response->result = true;
    RCLCPP_INFO_STREAM(logger_, "[reboot_dxl_srv_callback] SUCCESS");
  } else {
    response->result = false;
    RCLCPP_INFO_STREAM(logger_, "[reboot_dxl_srv_callback] FAIL");
  }
}

void DynamixelHardware::set_dxl_torque_srv_callback(
  const std::shared_ptr<std_srvs::srv::SetBool::Request> request,
  std::shared_ptr<std_srvs::srv::SetBool::Response> response)
{
  if (request->data) {
    if (dxl_torque_status_ == TORQUE_ENABLED) {
      response->success = true;
      response->message = "Already enabled.";
      return;
    } else {
      dxl_torque_status_ = REQUESTED_TO_ENABLE;
    }
  } else {
    if (dxl_torque_status_ == TORQUE_DISABLED) {
      response->success = true;
      response->message = "Already disabled.";
      return;
    } else {
      dxl_torque_status_ = REQUESTED_TO_DISABLE;
    }
  }

  // Get the start time
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(1)) {
    // std::cout << "dxl_torque_status_ : " << dxl_torque_status_ << std::endl;
    if (dxl_torque_status_ == TORQUE_ENABLED) {
      if (request->data) {
        response->success = true;
        response->message = "Success to enable.";
      } else {
        response->success = false;
        response->message = "Fail to enable.";
      }
      return;
    } else if (dxl_torque_status_ == TORQUE_DISABLED) {
      if (!request->data) {
        response->success = true;
        response->message = "Success to disable.";
      } else {
        response->success = false;
        response->message = "Fail to disable.";
      }
      return;
    }
    // Wait for 50ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  response->success = false;
  response->message = "Fail to write requeset. main thread is not running.";
}

}  // namespace dynamixel_hardware_interface

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(
  dynamixel_hardware_interface::DynamixelHardware,
  hardware_interface::SystemInterface
)

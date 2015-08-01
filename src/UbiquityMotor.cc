#include "UbiquityMotor.h"

#include <fcntl.h>
#include <pthread.h>
#include <termios.h>
#include <unistd.h>

#include "crc8.h"
#include "protocol.h"

#include <ros/ros.h>
#include <sensor_msgs/JointState.h>

const float UbiquityMotor::time_units_per_second = 1.0e-6 * 0xffffff;

// 330mm between the center of each wheel.  Measured with ruler.
const float UbiquityMotor::wheel_base = 0.33f;

// Wheel diameter is 8 inches (203mm).  Verified with wooden caliper.
const float UbiquityMotor::wheel_diameter = 0.203f;

// 20 magnets counted by looking at photo.
const unsigned int UbiquityMotor::magnet_count = 20;

// Gear teeth counted with a flat screwdriver in a curvy motor.
const unsigned int UbiquityMotor::sun_gear_teeth = 17;
const unsigned int UbiquityMotor::planet_gear_teeth = 28;
const unsigned int UbiquityMotor::ring_gear_teeth = 73;

const float UbiquityMotor::wheel_circumference =
    UbiquityMotor::wheel_diameter * M_PI;

const float UbiquityMotor::gear_ratio =
    UbiquityMotor::ring_gear_teeth / UbiquityMotor::sun_gear_teeth;

const float UbiquityMotor::transitions_per_magnet = 3.0;

const float UbiquityMotor::sensor_distance =
    UbiquityMotor::wheel_circumference / UbiquityMotor::gear_ratio /
    UbiquityMotor::magnet_count /
    UbiquityMotor::transitions_per_magnet;

const float UbiquityMotor::odom_covariance[] = { 0.2, 0.0, 0.0, 0.0, 0.0, 0.0,
                                                 0.0, 0.2, 0.0, 0.0, 0.0, 0.0,
                                                 0.0, 0.0, 0.2, 0.0, 0.0, 0.0,
                                                 0.0, 0.0, 0.0, 0.2, 0.0, 0.0,
                                                 0.0, 0.0, 0.0, 0.0, 0.2, 0.0,
                                                 0.0, 0.0, 0.0, 0.0, 0.0, 0.2 };
namespace {

void WriteAll(int fd, const void *data_, size_t size) {
  const char *data = reinterpret_cast<const char *>(data_);
  size_t offset = 0;

  while (offset < size) {
    ssize_t ret;

    ret = write(fd, data + offset, size - offset);

    if (ret < 0) {
      ROS_FATAL("Failed to write %zu bytes", size - offset);
      ros::shutdown();
    } else if (!ret) {
      ROS_FATAL("EOF while attempting to write %zu bytes", size - offset);
      ros::shutdown();
    }

    offset += ret;
  }
}

}  // namespace

void *UbiquityMotorReaderThread(void *arg) {
  UbiquityMotor *hc = reinterpret_cast<UbiquityMotor *>(arg);
  struct motor_message message;
  size_t fill = 0;
  unsigned char c;

  ros::Time last_odom = ros::Time::now();
  ros::Duration odom_rate(1/50.0);
  ROS_INFO("reader thread started");

  for (;; fill = 0) {
    while (fill < sizeof(message)) {
      int rc = read(hc->fd_, &c, 1);
//      ROS_INFO("read returned %d %02x", rc, c);
      if (1 != rc) {
        ROS_FATAL("Read error: %s (fd = %d)", strerror(errno), hc->fd_);
        ros::shutdown();
      }
      *((char *)&message + fill) = c;

      ++fill;

      // Synchronize byte stream.
      if (fill == 1 && message.sync[0] != 0x7E)
        fill = 0;
      else if (fill == 2 && message.sync[1] != 0x02)
        fill = 0;
    }


  ROS_INFO("received msg %x %x %x %x %x %x %x %x %x", 
	   message.sync[0],
	   message.sync[1],
	   message.type,
	   message.addr,
	   message.data[0],
	   message.data[1],
	   message.data[2], 
	   message.data[3],
	   message.checksum);

  int checksum;
  int sum = message.sync[1] + message.type + message.addr + message.data[0] + message.data[1] + message.data[2] + message.data[3];
  if (sum > 0xFF) {
    int tmp;
    tmp = sum >> 8;
    tmp = tmp << 8;
   checksum = 0xFF - static_cast<uint8_t>(roundf(sum-tmp));
  }
  else {
    checksum = 0xFF - sum;
  }

/*
    // Discard corrupted messages.
    uint8_t checksum = crc8(
        &message.type, sizeof(message) - offsetof(struct motor_message, type));
*/
    if (checksum != message.checksum) {
      ROS_INFO("checksum %d != %d", checksum, message.checksum);
      continue;
    }


    ros::Time current_time = ros::Time::now();
#if 0
    switch (message.type) {
      case MOTOR_MSG_ODOMETER:

        if (hc->has_odometer_) {
          double delta_time = (current_time - hc->last_odometry_).toSec();

          int32_t left_delta =
              (int32_t) message.u.odometer.motor0 - hc->odometer_[0];
          int32_t right_delta =
              (int32_t) message.u.odometer.motor1 - hc->odometer_[1];

          /* Handle 16 bit arithmetic overflow.  */
          if (left_delta <= -0x8000)
            left_delta += 0x10000;
          else if (left_delta >= 0x8000)
            left_delta -= 0x10000;
          if (right_delta <= -0x8000)
            right_delta += 0x10000;
          else if (right_delta >= 0x8000)
            right_delta -= 0x10000;

          hc->position_[0] += left_delta;
          hc->position_[1] += right_delta;

          // compute our odometry position from deltas
          double d_left = left_delta * UbiquityMotor::sensor_distance;
          double d_right = -right_delta * UbiquityMotor::sensor_distance;

          double dist = (d_left + d_right) / 2.0f;

          double dx = dist * cos(hc->odom_theta_);
          double dy = dist * sin(hc->odom_theta_);
          // baseline between wheels is wheel_base
          double d_theta = (d_right - d_left) / UbiquityMotor::wheel_base;

          hc->odom_x_ += dx;
          hc->odom_y_ += dy;
          hc->odom_theta_ += d_theta;

          // publish joint states, odometry and tf
          if(current_time >= last_odom + odom_rate) {
              last_odom = current_time;
              double left_angle =
                  hc->position_[0] * UbiquityMotor::sensor_distance /
                  (UbiquityMotor::wheel_diameter * 0.5f);
              double right_angle =
                  hc->position_[1] * UbiquityMotor::sensor_distance /
                  (UbiquityMotor::wheel_diameter * 0.5f);
              double left_vel =
                  left_delta * UbiquityMotor::sensor_distance / delta_time;
              double right_vel =
                  right_delta * UbiquityMotor::sensor_distance / delta_time;

              sensor_msgs::JointState joints;

              joints.header.stamp = current_time;

              joints.name.push_back(hc->left_motor_joint_name_);
              joints.position.push_back(left_angle);
              joints.velocity.push_back(left_vel);
              joints.effort.push_back(0.0);

              joints.name.push_back(hc->right_motor_joint_name_);
              joints.position.push_back(right_angle);
              joints.velocity.push_back(right_vel);
              joints.effort.push_back(0.0);

              hc->odometry_pub_.publish(joints);

              // Generate a nav_msgs::Odometry for position and publish to /odom
              nav_msgs::Odometry odom;
              odom.header.stamp = current_time;
              odom.header.frame_id = "odom";
              odom.child_frame_id = "base_link";

              odom.pose.pose.position.x = hc->odom_x_;
              odom.pose.pose.position.y = hc->odom_y_;
              
              // compute a quaternion for our yaw
              geometry_msgs::Quaternion odom_quat = tf::createQuaternionMsgFromYaw(
                  hc->odom_theta_);

              odom.pose.pose.orientation = odom_quat;

              for (int i=0; i<36; i++) {
                 odom.pose.covariance[i] = UbiquityMotor::odom_covariance[i];
                 odom.twist.covariance[i] = UbiquityMotor::odom_covariance[i];
              }

              hc->nav_odom_pub_.publish(odom);

              // generate a transform for postion and publish
              geometry_msgs::TransformStamped odom_trans;
              odom_trans.header.stamp = current_time;
              odom_trans.header.frame_id = "odom";
              odom_trans.child_frame_id = "base_link";
              odom_trans.transform.translation.x = hc->odom_x_;
              odom_trans.transform.translation.y = hc->odom_y_;

              odom_trans.transform.rotation = odom_quat;

              hc->tf_pub_.sendTransform(odom_trans);
          }

        } else if (message.u.odometer.motor0 == hc->odometer_[0] &&
                   message.u.odometer.motor1 == hc->odometer_[1]) {
          hc->has_odometer_ = true;
        }

        hc->odometer_[0] = message.u.odometer.motor0;
        hc->odometer_[1] = message.u.odometer.motor1;

        hc->last_odometry_ = current_time;

        break;
    }
#endif

  }

  return NULL;
}

UbiquityMotor::UbiquityMotor() : done_(false), has_odometer_(false) {
  memset(position_, 0, sizeof position_);

  if (!nh_.getParam("/motor/controller_tty_name", controller_tty_name_))
    controller_tty_name_ = "/dev/ttyUSB0";

  if (!nh_.getParam("/motor/left/name", left_motor_joint_name_))
    left_motor_joint_name_ = "base_l_wheel_joint";

  if (!nh_.getParam("/motor/right/name", right_motor_joint_name_))
    right_motor_joint_name_ = "base_r_wheel_joint";

  // save odometry information to the parameter server between runs
  if(!nh_.getParam("/motor/x", odom_x_))
    odom_x_ = 0.0;
  if(!nh_.getParam("/motor/y", odom_y_))
    odom_y_ = 0.0;
  if(!nh_.getParam("/motor/theta", odom_theta_))
    odom_theta_ = 0.0;

  ROS_INFO("Opening TTY %s", controller_tty_name_.c_str());

  if (-1 == (fd_ = open(controller_tty_name_.c_str(), O_RDWR | O_NOCTTY))) {
    ROS_FATAL("Failed to open '%s' in read/write mode: %s",
              controller_tty_name_.c_str(), strerror(errno));
    ros::shutdown();
  }

  if (-1 == tcflush(fd_, TCIOFLUSH)) {
    ROS_FATAL("tcflush failed");
    ros::shutdown();
  }

  ROS_INFO("Setting baud rate to 38400 on %s", controller_tty_name_.c_str());

  struct termios tty;
  memset(&tty, 0, sizeof tty);
  cfsetospeed(&tty, B38400);
  cfsetispeed(&tty, B38400);
  cfmakeraw(&tty);

  if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
    ROS_FATAL("tcsetattr failed");
    ros::shutdown();
  }

  vel_sub_ = nh_.subscribe<geometry_msgs::Twist>(
      "/cmd_vel", 1 /* queue size */, &UbiquityMotor::CmdVelCallback,
      this);
  odometry_pub_ = nh_.advertise<sensor_msgs::JointState>("/joint_states",
                                                         1 /* queue size */);
  nav_odom_pub_ = nh_.advertise<nav_msgs::Odometry>("/odom",
                                                         1 /* queue size */);

  pthread_create(&reader_thread_, NULL, UbiquityMotorReaderThread, this);

  ROS_INFO("Initialization complete");
}

UbiquityMotor::~UbiquityMotor() {
  done_ = true;
  pthread_cancel(reader_thread_);
  pthread_join(reader_thread_, NULL);
  
  // save out odometry x, y, theta to the parameter server on shutdown
  nh_.setParam("/motor/x", odom_x_);
  nh_.setParam("/motor/y", odom_y_);
  nh_.setParam("/motor/theta", odom_theta_);
}

void UbiquityMotor::CmdVelCallback(
    const geometry_msgs::Twist::ConstPtr &movement) {
  // movement.angular.z                     robot rotation in radians/s
  // 0.5 * movement.angular.z / M_PI        robot rotation in revolutions/s
  // wheel_base * M_PI                      turn circumference
  // 0.5 * wheel_base * movement.angular.z  robot rotation in m/s
  float angular_vel = -0.5 * wheel_base * movement->angular.z;

  SetWheelVelocities(movement->linear.x + angular_vel,
                     movement->linear.x - angular_vel);
}

void UbiquityMotor::SetWheelVelocities(float left_vel, float right_vel) {
  // Convert from m/s to sensors/robot_time_unit
  right_vel *= time_units_per_second / sensor_distance;
  left_vel *= time_units_per_second / sensor_distance;

#define CLAMP_TO_RANGE(v, max, min) \
  (((v) < min) ? (min) : ((v) > max) ? (max) : (v))
  right_vel = CLAMP_TO_RANGE(right_vel, 32767, -32767);
  left_vel = CLAMP_TO_RANGE(left_vel, 32767, -32767);
#undef CLAMP_TO_RANGE

  motor_message msg;

  msg.sync[0] = 0x7E;
  msg.sync[1] = 0x02;
  msg.type = 0xBB;
  msg.addr = 0x07;
  msg.data[3] = (static_cast<int32_t>(roundf(left_vel)) >> 0) & 0xFF;
  msg.data[2] = (static_cast<int32_t>(roundf(left_vel)) >> 8) & 0xFF;
  msg.data[1] = (static_cast<int32_t>(roundf(left_vel)) >> 16) & 0xFF;
  msg.data[0] = (static_cast<int32_t>(roundf(left_vel)) >> 24) & 0xFF;

  int sum = msg.sync[1] + msg.type + msg.addr + msg.data[0] + msg.data[1] + msg.data[2] + msg.data[3];
  if (sum > 0xFF) {
    int tmp;
    tmp = sum >> 8;
    tmp = tmp << 8;
    msg.checksum = 0xFF - static_cast<uint8_t>(roundf(sum-tmp));
  }
  else {
    msg.checksum = 0xFF - sum;
  }

  ROS_INFO("msg %x %x %x %x %x %x %x %x %x", 
    msg.sync[0],
    msg.sync[1],
    msg.type,
    msg.addr,
    msg.data[0],
    msg.data[1],
    msg.data[2], 
    msg.data[3],
    msg.checksum);

  WriteAll(fd_, &msg, sizeof(msg));
  
  msg.sync[0] = 0x7E;
  msg.sync[1] = 0x02;
  msg.type = 0xBB;
  msg.addr = 0x08;
  msg.data[3] = (static_cast<int32_t>(roundf(right_vel)) >> 0) & 0xFF;
  msg.data[2] = (static_cast<int32_t>(roundf(right_vel)) >> 8) & 0xFF;
  msg.data[1] = (static_cast<int32_t>(roundf(right_vel)) >> 16) & 0xFF;
  msg.data[0] = (static_cast<int32_t>(roundf(right_vel)) >> 24) & 0xFF;

  sum = msg.sync[1] + msg.type + msg.addr + msg.data[0] + msg.data[1] + msg.data[2] + msg.data[3];
  if (sum > 0xFF) {
    int tmp;
    tmp = sum >> 8;
    tmp = tmp << 8;
    msg.checksum = 0xFF - static_cast<uint8_t>(roundf(sum-tmp));
  }
  else {
    msg.checksum = 0xFF - sum;
  }

  ROS_INFO("msg %x %x %x %x %x %x %x %x %x", 
    msg.sync[0],
    msg.sync[1],
    msg.type,
    msg.addr,
    msg.data[0],
    msg.data[1],
    msg.data[2], 
    msg.data[3],
    msg.checksum);

  WriteAll(fd_, &msg, sizeof(msg));
}

void UbiquityMotor::requestOdometry()
{
  motor_message msg;

  msg.sync[0] = 0x7E;
  msg.sync[1] = 0x02;
  msg.type = 0xAA;
  msg.addr = 0x0b;
  msg.data[3] = 0x00;
  msg.data[2] = 0x00;
  msg.data[1] = 0x00;
  msg.data[0] = 0x00;

  int sum = msg.sync[1] + msg.type + msg.addr + msg.data[0] + msg.data[1] + msg.data[2] + msg.data[3];
  if (sum > 0xFF) {
    int tmp;
    tmp = sum >> 8;
    tmp = tmp << 8;
    msg.checksum = 0xFF - static_cast<uint8_t>(roundf(sum-tmp));
  }
  else {
    msg.checksum = 0xFF - sum;
  }

  ROS_INFO("msg %x %x %x %x %x %x %x %x %x", 
    msg.sync[0],
    msg.sync[1],
    msg.type,
    msg.addr,
    msg.data[0],
    msg.data[1],
    msg.data[2], 
    msg.data[3],
    msg.checksum);

  WriteAll(fd_, &msg, sizeof(msg));
}

void UbiquityMotor::run() 
{
  ros::Rate r(10.0);
  //ros::spin();
  while (ros::ok()) {
    r.sleep();
    requestOdometry();
  }
}

int main(int argc, char **argv) {
  ros::init(argc, argv, "motor_node");
  UbiquityMotor hc;
  hc.run();
}

/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
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
*   * Neither the name of the Willow Garage nor the names of its
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
*
* Author: Eitan Marder-Eppstein
*********************************************************************/

#include <base_local_planner/trajectory_planner_ros.h>

#include <sys/time.h>
#include <boost/tokenizer.hpp>

#include <Eigen/Core>
#include <cmath>

#include <ros/console.h>

#include <pluginlib/class_list_macros.h>

#include <base_local_planner/goal_functions.h>
#include <nav_msgs/Path.h>
#include <geometry_msgs/Twist.h>




//register this planner as a BaseLocalPlanner plugin
PLUGINLIB_EXPORT_CLASS(base_local_planner::Winter_TrajectoryPlannerROS, nav_core::BaseLocalPlanner)

namespace base_local_planner {

  void Winter_TrajectoryPlannerROS::reconfigureCB(BaseLocalPlannerConfig &config, uint32_t level) {
      if (setup_ && config.restore_defaults) {
        config = default_config_;
        //Avoid looping
        config.restore_defaults = false;
      }
      if ( ! setup_) {
        default_config_ = config;
        setup_ = true;
      }
      tc_->mreconfigure(config);
      reached_goal_ = false;
  }

  Winter_TrajectoryPlannerROS::Winter_TrajectoryPlannerROS() :
      world_model_(NULL), tc_(NULL), costmap_ros_(NULL), tf_(NULL), setup_(false), initialized_(false), odom_helper_("odom") {}

  Winter_TrajectoryPlannerROS::Winter_TrajectoryPlannerROS(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros) :
      world_model_(NULL), tc_(NULL), costmap_ros_(NULL), tf_(NULL), setup_(false), initialized_(false), odom_helper_("odom") {

      //initialize the planner
      initialize(name, tf, costmap_ros);
  }

  void Winter_TrajectoryPlannerROS::initialize(
      std::string name,
      tf::TransformListener* tf,
      costmap_2d::Costmap2DROS* costmap_ros){
    if (! isInitialized()) {

      ros::NodeHandle n;
      ros::NodeHandle private_nh("~/" + name);
      g_plan_pub_ = private_nh.advertise<nav_msgs::Path>("global_plan", 1);
      l_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);
      
       n.param("speed_topic", speed_topic_, std::string("cmd_vel"));
      vel_pub_ = n.advertise<geometry_msgs::Twist>(speed_topic_, 1);
      
      ultrosonicdata_sub_ = n.subscribe<sensor_msgs::Range>("UltraSoundPublisher", 1, boost::bind(&Winter_TrajectoryPlannerROS::ultrosonicdata_callback, this, _1));
      //初始化超声波距离
      ultro_distance=10.0;

      tf_ = tf;
      costmap_ros_ = costmap_ros;
      rot_stopped_velocity_ = 1e-2;
      trans_stopped_velocity_ = 1e-2;
      double sim_time, sim_granularity, angular_sim_granularity;
      int vx_samples, vtheta_samples;
      double pdist_scale, gdist_scale, occdist_scale, heading_lookahead, oscillation_reset_dist, escape_reset_dist, escape_reset_theta;
      bool holonomic_robot, dwa, simple_attractor, heading_scoring;
      double heading_scoring_timestep;
      double max_vel_x, min_vel_x;
      double backup_vel;
      double stop_time_buffer;
      std::string world_model_type;
      rotating_to_goal_ = false;
      turning_flag=0;
	  
	  NewPath=false;
	  
      //initialize the copy of the costmap the controller will use
      costmap_ = costmap_ros_->getCostmap();


      global_frame_ = costmap_ros_->getGlobalFrameID();
      robot_base_frame_ = costmap_ros_->getBaseFrameID();
      private_nh.param("prune_plan", prune_plan_, true);

      private_nh.param("yaw_goal_tolerance", yaw_goal_tolerance_, 0.05);
      private_nh.param("xy_goal_tolerance", xy_goal_tolerance_, 0.10);
      private_nh.param("acc_lim_x", acc_lim_x_, 2.5);
      private_nh.param("acc_lim_y", acc_lim_y_, 2.5);
      //this was improperly set as acc_lim_th -- TODO: remove this when we get to J turtle
      acc_lim_theta_ = 3.2;
      if (private_nh.hasParam("acc_lim_th"))
      {
        ROS_WARN("%s/acc_lim_th should be acc_lim_theta, this param will be removed in J-turtle", private_nh.getNamespace().c_str());
        private_nh.param("acc_lim_th", acc_lim_theta_, 3.2);
      }
      private_nh.param("acc_lim_theta", acc_lim_theta_, acc_lim_theta_);

      private_nh.param("stop_time_buffer", stop_time_buffer, 0.2);

      private_nh.param("latch_xy_goal_tolerance", latch_xy_goal_tolerance_, false);

      //Since I screwed up nicely in my documentation, I'm going to add errors
      //informing the user if they've set one of the wrong parameters
      if(private_nh.hasParam("acc_limit_x"))
        ROS_ERROR("You are using acc_limit_x where you should be using acc_lim_x. Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");

      if(private_nh.hasParam("acc_limit_y"))
        ROS_ERROR("You are using acc_limit_y where you should be using acc_lim_y. Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");

      if(private_nh.hasParam("acc_limit_th"))
        ROS_ERROR("You are using acc_limit_th where you should be using acc_lim_th. Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");

      //Assuming this planner is being run within the navigation stack, we can
      //just do an upward search for the frequency at which its being run. This
      //also allows the frequency to be overwritten locally.
      //tag 机器人控制频率 sim_period_ 机器人速度控制周期
      std::string controller_frequency_param_name;
      if(!private_nh.searchParam("controller_frequency", controller_frequency_param_name))
        sim_period_ = 0.05;
      else
      {
        double controller_frequency = 0;
        private_nh.param(controller_frequency_param_name, controller_frequency, 20.0);
        if(controller_frequency > 0)
          sim_period_ = 1.0 / controller_frequency;
        else
        {
          ROS_WARN("A controller_frequency less than 0 has been set. Ignoring the parameter, assuming a rate of 20Hz");
          sim_period_ = 0.05;
        }
      }
      ROS_INFO("Sim period is set to %.2f", sim_period_);

      private_nh.param("sim_time", sim_time, 1.0);
      private_nh.param("sim_granularity", sim_granularity, 0.025);
      private_nh.param("angular_sim_granularity", angular_sim_granularity, sim_granularity);
      private_nh.param("vx_samples", vx_samples, 3);
      private_nh.param("vtheta_samples", vtheta_samples, 20);

      private_nh.param("path_distance_bias", pdist_scale, 0.6);
      private_nh.param("goal_distance_bias", gdist_scale, 0.8);
      private_nh.param("occdist_scale", occdist_scale, 0.01);

      bool meter_scoring;
      if ( ! private_nh.hasParam("meter_scoring")) {
        ROS_WARN("Trajectory Rollout planner initialized with param meter_scoring not set. Set it to true to make your settins robust against changes of costmap resolution.");
      } else {
        private_nh.param("meter_scoring", meter_scoring, false);

        if(meter_scoring) {
          //if we use meter scoring, then we want to multiply the biases by the resolution of the costmap
          double resolution = costmap_->getResolution();
          gdist_scale *= resolution;
          pdist_scale *= resolution;
          occdist_scale *= resolution;
        } else {
          ROS_WARN("Trajectory Rollout planner initialized with param meter_scoring set to false. Set it to true to make your settins robust against changes of costmap resolution.");
        }
      }

      private_nh.param("heading_lookahead", heading_lookahead, 0.325);
      private_nh.param("oscillation_reset_dist", oscillation_reset_dist, 0.05);
      private_nh.param("escape_reset_dist", escape_reset_dist, 0.10);
      private_nh.param("escape_reset_theta", escape_reset_theta, M_PI_4);
      private_nh.param("holonomic_robot", holonomic_robot, true);
      private_nh.param("max_vel_x", max_vel_x, 0.5);
      private_nh.param("min_vel_x", min_vel_x, 0.1);

      double max_rotational_vel;
      private_nh.param("max_rotational_vel", max_rotational_vel, 1.0);
      max_vel_th_ = max_rotational_vel;
      min_vel_th_ = -1.0 * max_rotational_vel;
      private_nh.param("min_in_place_rotational_vel", min_in_place_vel_th_, 0.4);
      reached_goal_ = false;
      backup_vel = -0.1;
      if(private_nh.getParam("backup_vel", backup_vel))
        ROS_WARN("The backup_vel parameter has been deprecated in favor of the escape_vel parameter. To switch, just change the parameter name in your configuration files.");

      //if both backup_vel and escape_vel are set... we'll use escape_vel
      private_nh.getParam("escape_vel", backup_vel);

      if(backup_vel >= 0.0)
        ROS_WARN("You've specified a positive escape velocity. This is probably not what you want and will cause the robot to move forward instead of backward. You should probably change your escape_vel parameter to be negative");

      private_nh.param("world_model", world_model_type, std::string("costmap"));
      private_nh.param("dwa", dwa, true);
      private_nh.param("heading_scoring", heading_scoring, false);
      private_nh.param("heading_scoring_timestep", heading_scoring_timestep, 0.8);

      simple_attractor = false;

      //parameters for using the freespace controller
      double min_pt_separation, max_obstacle_height, grid_resolution;
      private_nh.param("point_grid/max_sensor_range", max_sensor_range_, 2.0);
      private_nh.param("point_grid/min_pt_separation", min_pt_separation, 0.01);
      private_nh.param("point_grid/max_obstacle_height", max_obstacle_height, 2.0);
      private_nh.param("point_grid/grid_resolution", grid_resolution, 0.2);

      ROS_ASSERT_MSG(world_model_type == "costmap", "At this time, only costmap world models are supported by this controller");
      world_model_ = new CostmapModel(*costmap_);
      std::vector<double> y_vels = loadYVels(private_nh);
		
      footprint_spec_ = costmap_ros_->getRobotFootprint();
	  
	  isMoveingBack=false;
	  ifPublishMessage=true;
	  
	  ROS_INFO("acc_lim_x_: %f  acc_lim_y_:%f acc_lim_theta_%f",acc_lim_x_, acc_lim_y_, acc_lim_theta_);
	  ROS_INFO("pdist_scale:%f gdist_scale:%f occdist_scale:%f",pdist_scale,gdist_scale, occdist_scale);
	  ROS_INFO("max_vel_x:%f max_vel_th_:%f backup_vel:%f",max_vel_x,max_vel_th_, backup_vel);
	  ROS_INFO("sim_period_:%f sim_granularity:%f angular_sim_granularity:%f",sim_period_,sim_granularity, angular_sim_granularity);
      tc_ = new Winter_TrajectoryPlanner(*world_model_, *costmap_, footprint_spec_,
          acc_lim_x_, acc_lim_y_, acc_lim_theta_, sim_time, sim_granularity, vx_samples, vtheta_samples, pdist_scale,
          gdist_scale, occdist_scale, heading_lookahead, oscillation_reset_dist, escape_reset_dist, escape_reset_theta, holonomic_robot,
          max_vel_x, min_vel_x, max_vel_th_, min_vel_th_, min_in_place_vel_th_, backup_vel,
          dwa, heading_scoring, heading_scoring_timestep, meter_scoring, simple_attractor, y_vels, stop_time_buffer, sim_period_, angular_sim_granularity);

      map_viz_.initialize(name, global_frame_, boost::bind(&Winter_TrajectoryPlanner::getCellCosts, tc_, _1, _2, _3, _4, _5, _6));
      initialized_ = true;

      dsrv_ = new dynamic_reconfigure::Server<BaseLocalPlannerConfig>(private_nh);
      dynamic_reconfigure::Server<BaseLocalPlannerConfig>::CallbackType cb = boost::bind(&Winter_TrajectoryPlannerROS::reconfigureCB, this, _1, _2);
      dsrv_->setCallback(cb);
      
      globalPathvalid=true;

    } else {
      ROS_WARN("This planner has already been initialized, doing nothing");
    }
  }

  std::vector<double> Winter_TrajectoryPlannerROS::loadYVels(ros::NodeHandle node){
    std::vector<double> y_vels;

    std::string y_vel_list;
    if(node.getParam("y_vels", y_vel_list)){
      typedef boost::tokenizer<boost::char_separator<char> > tokenizer;
      boost::char_separator<char> sep("[], ");
      tokenizer tokens(y_vel_list, sep);

      for(tokenizer::iterator i = tokens.begin(); i != tokens.end(); i++){
        y_vels.push_back(atof((*i).c_str()));
      }
    }
    else{
      //if no values are passed in, we'll provide defaults
      y_vels.push_back(-0.3);
      y_vels.push_back(-0.1);
      y_vels.push_back(0.1);
      y_vels.push_back(0.3);
    }

    return y_vels;
  }

  Winter_TrajectoryPlannerROS::~Winter_TrajectoryPlannerROS() {
    //make sure to clean things up
    delete dsrv_;

    if(tc_ != NULL)
      delete tc_;

    if(world_model_ != NULL)
      delete world_model_;
  }

  bool Winter_TrajectoryPlannerROS::stopWithAccLimits(const tf::Stamped<tf::Pose>& global_pose, const tf::Stamped<tf::Pose>& robot_vel, geometry_msgs::Twist& cmd_vel){
    //slow down with the maximum possible acceleration... we should really use the frequency that we're running at to determine what is feasible
    //but we'll use a tenth of a second to be consistent with the implementation of the local planner.
    //tag仿真速度
    double vx = sign(robot_vel.getOrigin().x()) * std::max(0.0, (fabs(robot_vel.getOrigin().x()) - acc_lim_x_ * sim_period_));
    double vy = sign(robot_vel.getOrigin().y()) * std::max(0.0, (fabs(robot_vel.getOrigin().y()) - acc_lim_y_ * sim_period_));

    double vel_yaw = tf::getYaw(robot_vel.getRotation());
    double vth = sign(vel_yaw) * std::max(0.0, (fabs(vel_yaw) - acc_lim_theta_ * sim_period_));

    //we do want to check whether or not the command is valid
    double yaw = tf::getYaw(global_pose.getRotation());
    bool valid_cmd = tc_->mcheckTrajectory(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), yaw, 
        robot_vel.getOrigin().getX(), robot_vel.getOrigin().getY(), vel_yaw, vx, vy, vth);

    //if we have a valid command, we'll pass it on, otherwise we'll command all zeros
    if(valid_cmd){
      ROS_DEBUG("Slowing down... using vx, vy, vth: %.2f, %.2f, %.2f", vx, vy, vth);
      cmd_vel.linear.x = vx;
      cmd_vel.linear.y = vy;
      cmd_vel.angular.z = vth;
      return true;
    }

    cmd_vel.linear.x = 0.0;
    cmd_vel.linear.y = 0.0;
    cmd_vel.angular.z = 0.0;
    return false;
  }
  void  Winter_TrajectoryPlannerROS::ultrosonicdata_callback(const sensor_msgs::Range::ConstPtr& data)
  {
	ultro_distance=data->range;
  }
  double Winter_TrajectoryPlannerROS::canculateDistance(double GoalX,double GoalY,double CurrentX,double CurrentY)
{
	return sqrt(pow((GoalX-CurrentX),2)+pow((GoalY-CurrentY),2));
}

double Winter_TrajectoryPlannerROS::normalize_angle(double angle)
{
    if( angle > M_PI)
        angle -= 2.0 * M_PI;
    if( angle < -M_PI)
        angle += 2.0 * M_PI;
    return angle;
}
  double Winter_TrajectoryPlannerROS::canculateAngle(double GoalX,double GoalY,double CurrentX,double CurrentY)
{
	double ERR_X=GoalX-CurrentX;
	double ERR_Y=GoalY-CurrentY;
	if (ERR_X>0)
	{
		return atan(ERR_Y/ERR_X);
	}
	else if( ERR_X<0)
	{
		if (ERR_Y>0)
			{return atan(ERR_Y/ERR_X)+M_PI;}
		else
			{return atan(ERR_Y/ERR_X)-M_PI;}
	}
	else
	{
		if (ERR_Y>0)
			return M_PI/2.0;
		else
			return 0-M_PI/2.0;
	}
}
/*
  * 转到指定的方向上*/
   /*
  * 可以用的速度全局变量
  　　acc_lim_x_, acc_lim_y_, acc_lim_theta_, 
          max_vel_x, min_vel_x, max_vel_th_, min_vel_th_, min_in_place_vel_th_, backup_vel,
          * //直接加速然后减速后的角度值
	MODE1_ANGLE=MAX_ANGULAR_Z*MAX_ANGULAR_Z/ACC_ANGULAR_Z;
	//#直接加速或者减速产生的角度
	MODE2_ANGLE=MODE1_ANGLE/2.0;
  * */
 bool Winter_TrajectoryPlannerROS::rotateToAngle(double goalAngle,double err_angle)
{
	
	double MAX_ANGULAR_Z=max_vel_th_;
	double ACC_ANGULAR_Z=acc_lim_theta_;
	if (MAX_ANGULAR_Z>2.0)  MAX_ANGULAR_Z=1.3;
	double MIN_ANGULAR_Z=0.3;
	if (ACC_ANGULAR_Z>0.8) ACC_ANGULAR_Z=0.8;
	
	double max_z=0.0;//运行中机器人旋转的最大速度
	double MODE1_ANGLE=MAX_ANGULAR_Z*MAX_ANGULAR_Z/ACC_ANGULAR_Z; //减速的距离
	double sharke_dis=MODE1_ANGLE/2.0; //减速的距离

	double current_angle;
	double ANGULAR_Z_ERR=err_angle;
	//获取当前的角度
	tf::Stamped<tf::Pose> global_pose;
    //获得机器人的全局坐标
    if (!costmap_ros_->getRobotPose(global_pose)) {
      return false;
    }
    current_angle=tf::getYaw(global_pose.getRotation());
    
	//获取两个角度之间的差值
	double turn_angle=normalize_angle(goalAngle-current_angle);
	
	//设置旋转加速度  这个值为正的 所以要判断一下旋转方向
	double rotate_acc=ACC_ANGULAR_Z;
	if (turn_angle<0.0)
	{
		rotate_acc=0.0-rotate_acc;
	}
	//记录最初的旋转角
	double O_turn_angle=turn_angle;
	if(fabs(O_turn_angle)<sharke_dis)
	{
		max_z=sqrt(fabs(O_turn_angle));
	}
	else
	{
		max_z=MAX_ANGULAR_Z;
	}

	
	double RATE=10;
	geometry_msgs::Twist move_cmd;
	ros::Rate r(RATE);
	double cAngle;
	ROS_INFO("MODE1_ANGLE %f",MODE1_ANGLE);
	ROS_INFO("shark angle %f",sharke_dis);
	while ((fabs(turn_angle)>ANGULAR_Z_ERR)  and ros::ok())
	{
		ros::spinOnce();
		vel_pub_.publish(move_cmd);
		r.sleep();
		 if (!costmap_ros_->getRobotPose(global_pose)) {
			return false;
		}
		cAngle=tf::getYaw(global_pose.getRotation());
		turn_angle=normalize_angle(goalAngle-cAngle);
		ROS_INFO("tangle %f O_turn_angle %f speed %f",turn_angle,O_turn_angle,move_cmd.angular.z);
		bool up=true;
		if (fabs(O_turn_angle)<MODE1_ANGLE)
		{
			//第一种模式 加速然后减速
			if(fabs(turn_angle)>(fabs(O_turn_angle)/2.0))	{	up=true;	}
			else {	up=false;}
		}
		else
		{
				//第二种模式 加速 匀速 减速
			if(fabs(turn_angle)>sharke_dis)  { up=true;}
			else {up=false;}
		}
		if(up)
		{
			if (fabs(move_cmd.angular.z)<(max_z-0.3))
					move_cmd.angular.z+=rotate_acc/RATE;
		}
		else
		{
			if (fabs(move_cmd.angular.z)>MIN_ANGULAR_Z)
					move_cmd.angular.z-=rotate_acc/RATE*1.5;
			else
			{
				if (turn_angle<0.0)
				move_cmd.angular.z=-0.3;
				else move_cmd.angular.z=0.3;
			}
		}
			
	}
	PublishMoveStopCMD();
	return true;
}
void Winter_TrajectoryPlannerROS::PublishMoveStopCMD()
{
		geometry_msgs::Twist move_stop;
	    vel_pub_.publish(move_stop);
}
bool  Winter_TrajectoryPlannerROS::MoveBack(const tf::Stamped<tf::Pose>& global_pose,const tf::Stamped<tf::Pose>& goal_pose,
																						  const tf::Stamped<tf::Pose>& robot_vel,geometry_msgs::Twist& cmd_vel)
 {
		
		double left_dis=getGoalPositionDistance(global_pose,goal_pose.getOrigin().x(),goal_pose.getOrigin().y());
		ROS_INFO("cx %f cy %f left_dis %f ",global_pose.getOrigin().x(),global_pose.getOrigin().y(),left_dis);
		
		double sharke_dis=0.1*0.1/(2*acc_lim_x_);
		double vel_yaw = tf::getYaw(robot_vel.getRotation());
		double yaw = tf::getYaw(global_pose.getRotation());
		//采样速度
		double vx=-0.1;
		double vy=0.0;
		double vth=0.0;
		
		if(left_dis>sharke_dis)
		{
			if(left_dis<0.035)  
			{
				//将机器人停下
				cmd_vel.linear.x = 0.0;
				cmd_vel.linear.y = 0.0;
				cmd_vel.angular.z = 0.0;
				//将机器人后退状态清除 机器人将接着前进
				isMoveingBack=false;
				return true; 
			}
			
			bool valid_cmd = tc_->mcheckTrajectory(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), yaw, 
															robot_vel.getOrigin().getX(), robot_vel.getOrigin().getY(), vel_yaw, vx, vy, vth);

			//if we have a valid command, we'll pass it on, otherwise we'll command all zeros
			if(valid_cmd){
				ROS_DEBUG("Slowing down... using vx, vy, vth: %.2f, %.2f, %.2f", vx, vy, vth);
				cmd_vel.linear.x = vx;
				cmd_vel.linear.y = vy;
				cmd_vel.angular.z = vth;
				return true;
			}
		}
	  
	  cmd_vel.linear.x = 0.0;
	  return false;
	
 }

  bool Winter_TrajectoryPlannerROS::rotateToGoal(const tf::Stamped<tf::Pose>& global_pose, const tf::Stamped<tf::Pose>& robot_vel, double goal_th, geometry_msgs::Twist& cmd_vel){
    
    double yaw = tf::getYaw(global_pose.getRotation());
    double vel_yaw = tf::getYaw(robot_vel.getRotation());
    cmd_vel.linear.x = 0;
    cmd_vel.linear.y = 0;
    double ang_diff = angles::shortest_angular_distance(yaw, goal_th);

    double v_theta_samp = ang_diff > 0.0 ? std::min(max_vel_th_,
        std::max(min_in_place_vel_th_, ang_diff)) : std::max(min_vel_th_,
        std::min(-1.0 * min_in_place_vel_th_, ang_diff));

    //take the acceleration limits of the robot into account
    //double max_acc_vel = fabs(vel_yaw) + acc_lim_theta_ * sim_period_;
    //double min_acc_vel = fabs(vel_yaw) - acc_lim_theta_ * sim_period_;
    
    double max_acc_vel = fabs(vel_yaw) + 0.2 * sim_period_;
    double min_acc_vel = fabs(vel_yaw) - 0.2 * sim_period_;

    v_theta_samp = sign(v_theta_samp) * std::min(std::max(fabs(v_theta_samp), min_acc_vel), max_acc_vel);

    //we also want to make sure to send a velocity that allows us to stop when we reach the goal given our acceleration limits
    double max_speed_to_stop = sqrt(2 * acc_lim_theta_ * fabs(ang_diff)); 

    v_theta_samp = sign(v_theta_samp) * std::min(max_speed_to_stop, fabs(v_theta_samp));

    // Re-enforce min_in_place_vel_th_.  It is more important than the acceleration limits.
    v_theta_samp = v_theta_samp > 0.0
      ? std::min( max_vel_th_, std::max( min_in_place_vel_th_, v_theta_samp ))
      : std::max( min_vel_th_, std::min( -1.0 * min_in_place_vel_th_, v_theta_samp ));

    //we still want to lay down the footprint of the robot and check if the action is legal
    bool valid_cmd = tc_->mcheckTrajectory(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), yaw, 
        robot_vel.getOrigin().getX(), robot_vel.getOrigin().getY(), vel_yaw, 0.0, 0.0, v_theta_samp);

    ROS_DEBUG("Moving to desired goal orientation, th cmd: %.2f, valid_cmd: %d", v_theta_samp, valid_cmd);

    if(valid_cmd){
      cmd_vel.angular.z = v_theta_samp;
      return true;
    }

    cmd_vel.angular.z = 0.0;
    return false;

  }

//这个plan 被外界move base 调用
 bool Winter_TrajectoryPlannerROS::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan){
    if (! isInitialized()) {
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }
	NewPath=true;
    //reset the global plan
    global_plan_.clear();
    global_plan_ = orig_global_plan;
    
    //when we get a new plan, we also want to clear any latch we may have on goal tolerances
    xy_tolerance_latch_ = false;
    //reset the at goal flag
    reached_goal_ = false;
    return true;
  }

  bool Winter_TrajectoryPlannerROS::computeVelocityCommands(geometry_msgs::Twist& cmd_vel){
    if (! isInitialized()) {
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }

    std::vector<geometry_msgs::PoseStamped> local_plan;
    tf::Stamped<tf::Pose> global_pose;
    //获得机器人的全局坐标
    if (!costmap_ros_->getRobotPose(global_pose)) {
      return false;
    }

    std::vector<geometry_msgs::PoseStamped> transformed_plan;
    //get the global plan in our frame
    if (!transformGlobalPlan(*tf_, global_plan_, global_pose, *costmap_, global_frame_, transformed_plan)) {
      ROS_WARN("Could not transform the global plan to the frame of the controller");
      return false;
    }

	//删减 prune 
    //now we'll prune the plan based on the position of the robot
    if(prune_plan_)
      prunePlan(global_pose, transformed_plan, global_plan_);
	
    tf::Stamped<tf::Pose> drive_cmds;
    drive_cmds.frame_id_ = robot_base_frame_;

	//tag得到机器人的速度
    tf::Stamped<tf::Pose> robot_vel;
    odom_helper_.getRobotVel(robot_vel);
    
    //判断全局路径是否有效
	if(tc_->checkPath(global_pose,transformed_plan))
	{
		 globalPathvalid=true;
	}
	else
	{
		globalPathvalid=false;
	}
	
    double cx=global_pose.getOrigin().x();
    double cy=global_pose.getOrigin().y();
    double disFromStart=canculateDistance(transformed_plan[0].pose.position.x,transformed_plan[0].pose.position.y,cx,cy);
    double current_angle=tf::getYaw(global_pose.getRotation());
    double goalAngle=canculateAngle(transformed_plan[5].pose.position.x,transformed_plan[5].pose.position.y,transformed_plan[0].pose.position.x,transformed_plan[0].pose.position.y);
   
   double  ang_diff = normalize_angle(goalAngle-current_angle);
    //在起点且朝向角度与方向偏差很大　则转向到目标方向　0.523 30
    geometry_msgs::Twist cmd_v;
    ros::Rate loop_rate(10);
   
    //是否处于后退的状态中
    if(!isMoveingBack)
    {
		if(ultro_distance<0.1)
		{
			ROS_INFO("get ulrtosonic data range: %f",ultro_distance);
			isMoveingBack=true;
			//获取机器人朝向
			double angle=tf::getYaw(global_pose.getRotation());
			//应该移动的距离
			double move_dis=0.15;
			//设置临时后退的目标点
			temp_goal_point.stamp_ = global_pose.stamp_;
			temp_goal_point.frame_id_ = global_pose.frame_id_;
			double x = global_pose.getOrigin().x()-move_dis*cos(angle);
			double y = global_pose.getOrigin().y()-move_dis*sin(angle);;
			temp_goal_point.setOrigin(tf::Vector3(x,y,0));
			temp_goal_point.setRotation(global_pose.getRotation());
			ROS_INFO("ox %f oy %f gx %f gy %f ",global_pose.getOrigin().x(),global_pose.getOrigin().y(),x,y);
		}
	}
	if(isMoveingBack)
	{
			double current_speed=robot_vel.getOrigin().getX();
		    if(current_speed>0)
		    {
				//处于正在刹车的状态中
				if ( ! stopWithAccLimits(global_pose, robot_vel, cmd_vel)) {
						return false;
					}
				else
				{
					    ROS_INFO("Robot stoping ");
						return true;
				}
			}
			else
			{
				//机器人开始向后移动并停止
				if (Winter_TrajectoryPlannerROS::MoveBack(global_pose,temp_goal_point,robot_vel,cmd_vel))
				{ 
					ROS_INFO("Robot Moing Back");
					return true;
				}
				else
				{
					return false;
				}
			}
	  }
	 
	//得到机器人的速度
	//vel 机器人的速度
    //Eigen::Vector3f vel(global_vel.getOrigin().getX(), global_vel.getOrigin().getY(), tf::getYaw(global_vel.getRotation()));
    
    /* For timing uncomment
    struct timeval start, end;
    double start_t, end_t, t_diff;
    gettimeofday(&start, NULL);
    */

    //if the global plan passed in is empty... we won't do anything
    if(transformed_plan.empty())
      return false;

	 //局部地图中的目标点
    tf::Stamped<tf::Pose> goal_point;
    tf::poseStampedMsgToTF(transformed_plan.back(), goal_point);
    //we assume the global goal is the last point in the global plan
    double goal_x = goal_point.getOrigin().getX();
    double goal_y = goal_point.getOrigin().getY();
   
	//ROS_INFO("the goal point x is %f y is %f ",goal_x,goal_y);
    double yaw = tf::getYaw(goal_point.getRotation());

    double goal_th = yaw;
	
	
	
    //check to see if we've reached the goal position
    /*****************以下是到达目的地的动作**************************************/
    if (xy_tolerance_latch_ || (getGoalPositionDistance(global_pose, goal_x, goal_y) <= xy_goal_tolerance_)) {

      //if the user wants to latch goal tolerance, if we ever reach the goal location, we'll
      //just rotate in place
      if (latch_xy_goal_tolerance_) {
        xy_tolerance_latch_ = true;
      }

      double angle = getGoalOrientationAngleDifference(global_pose, goal_th);
      //check to see if the goal orientation has been reached
      if (fabs(angle) <= yaw_goal_tolerance_) {
        //set the velocity command to zero
        cmd_vel.linear.x = 0.0;
        cmd_vel.linear.y = 0.0;
        cmd_vel.angular.z = 0.0;
        rotating_to_goal_ = false;
        xy_tolerance_latch_ = false;
        reached_goal_ = true;
      } else {
        //we need to call the next two lines to make sure that the trajectory
        //planner updates its path distance and goal distance grids
        tc_->mupdatePlan(transformed_plan);
        //#tag得到速度的入口
        
        Trajectory path = tc_->mfindBestPath(global_pose, robot_vel, drive_cmds);
        if(ifPublishMessage)
        {
        map_viz_.publishCostCloud(costmap_);
		}
        //copy over the odometry information
        nav_msgs::Odometry base_odom;
        odom_helper_.getOdom(base_odom);

        //if we're not stopped yet... we want to stop... taking into account the acceleration limits of the robot
        if ( ! rotating_to_goal_ && !base_local_planner::stopped(base_odom, rot_stopped_velocity_, trans_stopped_velocity_)) {
          if ( ! stopWithAccLimits(global_pose, robot_vel, cmd_vel)) {
            return false;
          }
        }
        //if we're stopped... then we want to rotate to goal
        else{
          //set this so that we know its OK to be moving
          rotating_to_goal_ = true;
          if(!rotateToGoal(global_pose, robot_vel, goal_th, cmd_vel)) {
            return false;
          }
        }
      }
	  if(ifPublishMessage)
      //publish an empty plan because we've reached our goal position
      {publishPlan(transformed_plan, g_plan_pub_);
      publishPlan(local_plan, l_plan_pub_);
		}
	 
      //we don't actually want to run the controller when we're just rotating to goal
      return true;
    }
    
    /**************以上是到达目的地的动作*****************************************/

	/*************出发状态****************************/
	
	//用turning flag 防止反复转向
	 //ROS_INFO("dis %f  anglediff %f ",disFromStart,fabs(ang_diff));
	 if(((disFromStart<0.3)&&(turning_flag==0))||NewPath)
	 {
		 /*double cvx=robot_vel.getOrigin().getX();
		 double cvy=robot_vel.getOrigin().getY();
		 double vtheta=tf::getYaw(robot_vel.getRotation());
		 if (tc_->mcheckTrajectory(cx,cy,current_angle,cvx,cvy,vtheta,cvx,cvy,vtheta))
		 {*/
			ROS_INFO("Ratating start");
			rotateToAngle(goalAngle,0.2); //0.2/3.14*180=11 degree
			ROS_INFO("Ratating stop");
			turning_flag=1;
			NewPath=false;
		/*}
		else
			return false;*/
	}
	if(disFromStart>0.4) turning_flag=0;
	
	
	
	/******************************************/
    tc_->mupdatePlan(transformed_plan);

    //compute what trajectory to drive along
    
    Trajectory path = tc_->mfindBestPath(global_pose, robot_vel, drive_cmds);

	if(ifPublishMessage)
	{
    map_viz_.publishCostCloud(costmap_);
	}
    /* For timing uncomment
    gettimeofday(&end, NULL);
    start_t = start.tv_sec + double(start.tv_usec) / 1e6;
    end_t = end.tv_sec + double(end.tv_usec) / 1e6;
    t_diff = end_t - start_t;
    ROS_INFO("Cycle time: %.9f", t_diff);
    */

    //pass along drive commands
    cmd_vel.linear.x = drive_cmds.getOrigin().getX();
    cmd_vel.linear.y = drive_cmds.getOrigin().getY();
    cmd_vel.angular.z = tf::getYaw(drive_cmds.getRotation());

    //if we cannot move... tell someone
    if (path.cost_ < 0) {
      ROS_DEBUG_NAMED("trajectory_planner_ros",
          "The rollout planner failed to find a valid plan. This means that the footprint of the robot was in collision for all simulated trajectories.");
      local_plan.clear();
      if(ifPublishMessage)
      {
      publishPlan(transformed_plan, g_plan_pub_);
      publishPlan(local_plan, l_plan_pub_);
	  }
      return false;
    }

    ROS_DEBUG_NAMED("trajectory_planner_ros", "A valid velocity command of (%.2f, %.2f, %.2f) was found for this cycle.",
        cmd_vel.linear.x, cmd_vel.linear.y, cmd_vel.angular.z);

    // Fill out the local plan
    for (unsigned int i = 0; i < path.getPointsSize(); ++i) {
      double p_x, p_y, p_th;
      path.getPoint(i, p_x, p_y, p_th);
      tf::Stamped<tf::Pose> p =
          tf::Stamped<tf::Pose>(tf::Pose(
              tf::createQuaternionFromYaw(p_th),
              tf::Point(p_x, p_y, 0.0)),
              ros::Time::now(),
              global_frame_);
      geometry_msgs::PoseStamped pose;
      tf::poseStampedTFToMsg(p, pose);
      local_plan.push_back(pose);
    }

    //publish information to the visualizer
    if(ifPublishMessage)
    {
    publishPlan(transformed_plan, g_plan_pub_);
    publishPlan(local_plan, l_plan_pub_);
	}
    return true;
  }

  bool Winter_TrajectoryPlannerROS::checkTrajectory(double vx_samp, double vy_samp, double vtheta_samp, bool update_map){
    tf::Stamped<tf::Pose> global_pose;
    if(costmap_ros_->getRobotPose(global_pose)){
      if(update_map){
        //we need to give the planne some sort of global plan, since we're only checking for legality
        //we'll just give the robots current position
        std::vector<geometry_msgs::PoseStamped> plan;
        geometry_msgs::PoseStamped pose_msg;
        tf::poseStampedTFToMsg(global_pose, pose_msg);
        plan.push_back(pose_msg);
        tc_->mupdatePlan(plan, true);
      }

      //copy over the odometry information
      nav_msgs::Odometry base_odom;
      {
        boost::recursive_mutex::scoped_lock lock(odom_lock_);
        base_odom = base_odom_;
      }

      return tc_->mcheckTrajectory(global_pose.getOrigin().x(), global_pose.getOrigin().y(), tf::getYaw(global_pose.getRotation()),
          base_odom.twist.twist.linear.x,
          base_odom.twist.twist.linear.y,
          base_odom.twist.twist.angular.z, vx_samp, vy_samp, vtheta_samp);

    }
    ROS_WARN("Failed to get the pose of the robot. No trajectories will pass as legal in this case.");
    return false;
  }


  double Winter_TrajectoryPlannerROS::scoreTrajectory(double vx_samp, double vy_samp, double vtheta_samp, bool update_map){
    // Copy of checkTrajectory that returns a score instead of True / False
    tf::Stamped<tf::Pose> global_pose;
    if(costmap_ros_->getRobotPose(global_pose)){
      if(update_map){
        //we need to give the planne some sort of global plan, since we're only checking for legality
        //we'll just give the robots current position
        std::vector<geometry_msgs::PoseStamped> plan;
        geometry_msgs::PoseStamped pose_msg;
        tf::poseStampedTFToMsg(global_pose, pose_msg);
        plan.push_back(pose_msg);
        tc_->mupdatePlan(plan, true);
      }

      //copy over the odometry information
      nav_msgs::Odometry base_odom;
      {
        boost::recursive_mutex::scoped_lock lock(odom_lock_);
        base_odom = base_odom_;
      }

      return tc_->mscoreTrajectory(global_pose.getOrigin().x(), global_pose.getOrigin().y(), tf::getYaw(global_pose.getRotation()),
          base_odom.twist.twist.linear.x,
          base_odom.twist.twist.linear.y,
          base_odom.twist.twist.angular.z, vx_samp, vy_samp, vtheta_samp);

    }
    ROS_WARN("Failed to get the pose of the robot. No trajectories will pass as legal in this case.");
    return -1.0;
  }

bool Winter_TrajectoryPlannerROS::checkGlobalPath(void)
	{
		return globalPathvalid;
	}
 bool Winter_TrajectoryPlannerROS::isGoalReached() {
    if (! isInitialized()) {
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }
    //return flag set in controller
    return reached_goal_; 
  }
};

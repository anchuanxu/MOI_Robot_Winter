cmake_minimum_required(VERSION 2.8.3)
project(winter_base_local_planner)

find_package(catkin REQUIRED
        COMPONENTS
            angles
            cmake_modules
            costmap_2d
            dynamic_reconfigure
            geometry_msgs
            sensor_msgs
            message_generation
            nav_core
            nav_msgs
            pcl_conversions
            pcl_ros
            pluginlib
            roscpp
            rospy
            std_msgs
            tf
            voxel_grid
        )

find_package(Boost REQUIRED
    COMPONENTS
        thread
        )

find_package(Eigen REQUIRED)
find_package(PCL REQUIRED)
include_directories(
    include
    ${catkin_INCLUDE_DIRS}
    ${EIGEN_INCLUDE_DIRS}
    ${PCL_INCLUDE_DIRS}
)
add_definitions(${EIGEN_DEFINITIONS})

catkin_python_setup()

# messages
add_message_files(
    DIRECTORY msg
    FILES
    Position2DInt.msg
)

generate_messages(
    DEPENDENCIES
        std_msgs
)

# dynamic reconfigure
generate_dynamic_reconfigure_options(
    cfg/BaseLocalPlanner.cfg
)

catkin_package(
    INCLUDE_DIRS include
    LIBRARIES
        winter_base_local_planner
        winter_trajectory_planner_ros
    CATKIN_DEPENDS
        angles
        costmap_2d
        dynamic_reconfigure
        geometry_msgs
        sensor_msgs
        message_runtime
        nav_core
        nav_msgs
        pluginlib
        roscpp
        std_msgs
        tf
)

#uncomment for profiling
#set(ROS_COMPILE_FLAGS "-g -pg" ${ROS_COMPILE_FLAGS})
#set(ROS_LINK_FLAGS "-g -pg" ${ROS_LINK_FLAGS})
#set(ROS_COMPILE_FLAGS "-g" ${ROS_COMPILE_FLAGS})
#set(ROS_LINK_FLAGS "-g" ${ROS_LINK_FLAGS})

add_library(winter_base_local_planner
	src/footprint_helper.cpp
	src/goal_functions.cpp
	src/map_cell.cpp
	src/map_grid.cpp
	src/map_grid_visualizer.cpp
	src/map_grid_cost_function.cpp
	src/latched_stop_rotate_controller.cpp
	src/local_planner_util.cpp
	src/odometry_helper_ros.cpp
	src/obstacle_cost_function.cpp
	src/oscillation_cost_function.cpp
	src/prefer_forward_cost_function.cpp
	src/point_grid.cpp
	src/costmap_model.cpp
	src/simple_scored_sampling_planner.cpp
	src/simple_trajectory_generator.cpp
	src/trajectory.cpp
	src/voxel_grid_model.cpp)
add_dependencies(winter_base_local_planner winter_base_local_planner_gencfg)
add_dependencies(winter_base_local_planner winter_base_local_planner_gencpp)
add_dependencies(winter_base_local_planner nav_msgs_gencpp)
target_link_libraries(winter_base_local_planner
    ${catkin_LIBRARIES}
    ${PCL_LIBRARIES}
    ${Boost_LIBRARIES}
    ${Eigen_LIBRARIES}
    )

add_library(winter_trajectory_planner_ros
	src/trajectory_planner.cpp
	src/trajectory_planner_ros.cpp)
add_dependencies(winter_trajectory_planner_ros ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
target_link_libraries(winter_trajectory_planner_ros
     winter_base_local_planner)

add_executable(point_grid src/point_grid.cpp)
add_dependencies(point_grid ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
target_link_libraries(point_grid ${catkin_LIBRARIES})



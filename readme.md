# RCTA Manipulation

RCTA Manipulation is a set of catkin packages compatible with the ROS Indigo
distribution on Ubuntu 14.04 that provide planning capabilities for
manipulation on RCTA robotic platforms.

## Building

Set up a catkin workspace as instructed in the [ROS tutorials](www.ros.org) or
determine an existing catkin workspace to host the `rcta_manipulation` packages.

Source dependencies are listed in `rcta_manipulation/rcta.rosinstall` and may
be downloaded simultaneously via [wstool](wiki.ros.org/wstool). Alternatively,
you can manually clone all the packages listed in that file and place them
alongside the `rcta_manipulation` package in your catkin workspace.

System dependencies may be installed using [rosdep](wiki.ros.org/rosdep). At
the root of the workspace, executing the following command will prompt to
install all required system dependencies:

```sh
rosdep install --from-paths rcta_manipulation -i -y
```

The recommended way to build the packages in this repository is to use the
`catkin` from the `python-catkin-tools` package. A workspace containing these
packages may be built using the `catkin build` command.

## Running

There are multiple GNU screen configuration scripts in the `roman_manipulation`
package for running the system under a few configurations. The system can be
run under any configuration using rosrun. For example,

```sh
rosrun roman_manipulation screenrc_fake
```

will run the complete system with an offline (fake) simulation of the RoMan
robot.

Executing a configuration script will start a screen session with tabs preloaded
with the required commands. Launch the system by executing the command within
each tab. Note that the default screen accelerator key C-a is modified to C-x.
Ctrl + the arrow keys can be used to cycle through tabs.

## Frequently used commands

Make sure that the time on robot's computer and time in the docker container/your 
computer are the same. The server is currently set in the `/etc/hosts` file to the 
IP of the robot. 
```sh
sudo ntpdate -u tl1-1-am1 
```

To align the time zones in docker to the same as that on the robot, add this 
environment variable to the docker startup file.
```sh
--env="TZ=America/New_York" \
```

SSH into the robot computer
```sh
ssh rcta@tl1-1-am1
```

Run the `export ROS_IP` command on each of the docker tabs(set it to your 
computer's IP address). This is to inform the robot of the IP of the host computer.

Set the ROS_MASTER_URI if running the planner on the robot. 
```sh
--env="ROS_MASTER_URI=http://tl1-1-am1:11311" \
```

If there is a need to manually move the robot's joints around, using the scripts 
from `$RS_LIMB_ROOT/sbin`.

Running visualization in RViz - 
Run this `export LD_LIBRARY_PATH=/usr/local/nvidia/lib:/usr/local/nvidia/lib64:$LD_LIBRARY_PATH `
to make sure RViz is running with the right libraries. The rviz file with the correct configs set 
is `rcta-roman1.rviz`. Run it with the namespace `__ns:=roman1`

Running batch tests on the robot -  

While running the Realsense, launch the `rs_rgbd.launch` file with the it with the argument 
`camera:=roman1/center_realsense`

Moving the fingers
```sh
rostopic pub -1 /roman1/rcta_right_robotiq_controller/command std_msgs/Float64MultiArray 'layout:
  dim:
  - label: joint
    size: 4
    stride: 1
  data_offset: 0
data: [6.0, 6.0,6.0,137.0]'
```

Moving the dynamixel that supports the camera - 
```sh
rcta@tl1-1-am1:~/rcta/install$ more share/rcta_dynamixel_publisher/config/roman1
```

To run PERCH
```sh
rostopic pub /requested_object std_msgs/String "data: 'test'" 
```
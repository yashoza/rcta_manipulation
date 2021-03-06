#include <actionlib/server/simple_action_server.h>
#include <actionlib/client/simple_action_client.h>
#include <control_msgs/FollowJointTrajectoryAction.h>
#include <ros/ros.h>
#include <sensor_msgs/JointState.h>

using FollowJointTrajectoryActionServer =
        actionlib::SimpleActionServer<control_msgs::FollowJointTrajectoryAction>;

struct TrajectoryServerConfig
{
    std::string name;
    std::vector<std::string> input_joint_names;
};

template <class InputIt, class T>
auto index_of(InputIt first, InputIt last, const T& e) -> int
{
    auto it = std::find(first, last, e);
    if (it == last) return -1;
    else return std::distance(first, it);
}

template <class Container>
auto index_of(Container& container, const typename Container::value_type& elem) -> std::ptrdiff_t
{
    auto first = begin(container);
    auto last = end(container);
    auto it = std::find(first, last, elem);
    if (it == last) return -1;
    else return std::distance(first, it);
}

template <class InputIt, class T>
auto contains(InputIt first, InputIt last, const T& e) -> bool
{
    auto it = std::find(first, last, e);
    return it != last;
}

template <class Container>
bool contains(Container& container, const typename Container::value_type& elem)
{
    auto first = begin(container);
    auto last = end(container);
    auto it = std::find(first, last, elem);
    return it != last;
}

void execute(
    FollowJointTrajectoryActionServer* server,
    const std::vector<TrajectoryServerConfig>* servers,
    const sensor_msgs::JointState* curr_state,
    const control_msgs::FollowJointTrajectoryGoal* msg)
{
    ROS_INFO("Execute trajectory");

    // 1. select a trajectory server to use
    auto traj_server_index = -1;
    for (auto i = 0; i < servers->size(); ++i) {
        auto& server = (*servers)[i];
        // if server contains all joints specified in the goal
        auto all = true;
        for (auto& name : msg->trajectory.joint_names) {
            if (!contains(server.input_joint_names, name)) {
                all = false;
                break;
            }
        }

        if (!all) continue;

        traj_server_index = i;
    }

    if (traj_server_index == -1) {
        FollowJointTrajectoryActionServer::Result result;
        result.error_code = FollowJointTrajectoryActionServer::Result::INVALID_JOINTS;
        result.error_string = "No suitable trajectory server configured";
        server->setAborted(result);
        return;
    }

    auto& srv_desc = (*servers)[traj_server_index];

    ///////////////////////////////////////
    // Convert to acceptable goal format //
    ///////////////////////////////////////

    // figure out which output joints are required but are missing from the
    // input trajectory

    auto missing = std::vector<std::string>();
    for (auto& joint_name : srv_desc.input_joint_names) {
        if (!contains(msg->trajectory.joint_names, joint_name)) {
            missing.push_back(joint_name);
        }
    }

    ROS_INFO("Missing %zu joints", missing.size());

    control_msgs::FollowJointTrajectoryGoal goal;
    goal.goal_time_tolerance = msg->goal_time_tolerance;
    goal.goal_tolerance = msg->goal_tolerance;
    goal.path_tolerance = msg->path_tolerance;
    goal.trajectory.header = msg->trajectory.header;
    goal.trajectory.joint_names = srv_desc.input_joint_names;
    goal.trajectory.points.reserve(msg->trajectory.points.size());
    for (auto& point : msg->trajectory.points) {
        trajectory_msgs::JointTrajectoryPoint p;

        p.positions.reserve(srv_desc.input_joint_names.size());

        // reserve space for velocity, acceleration, effort if specified in
        // the trajectory
        if (!point.velocities.empty()) {
            p.velocities.reserve(srv_desc.input_joint_names.size());
        }
        if (!point.accelerations.empty()) {
            p.accelerations.reserve(srv_desc.input_joint_names.size());
        }
        if (!point.effort.empty()) {
            p.effort.reserve(srv_desc.input_joint_names.size());
        }

        // construct points in the order defined by input_joint_names, in case
        // that matters
        for (auto& joint_name : srv_desc.input_joint_names) {
            // look for the joint position in the input trajectory
            auto index = index_of(msg->trajectory.joint_names, joint_name);
            if (index >= 0) {
                p.positions.push_back(point.positions[index]);
                if (!point.velocities.empty()) {
                    p.velocities.push_back(point.velocities[index]);
                }
                if (!point.accelerations.empty()) {
                    p.accelerations.push_back(point.accelerations[index]);
                }
                if (!point.effort.empty()) {
                    p.effort.push_back(point.effort[index]);
                }
                continue;
            }

            // look for the joint position in the current state
            if (curr_state != NULL) {
                auto j = index_of(curr_state->name, joint_name);
                if (j >= 0) {
                    p.positions.push_back(curr_state->position[j]);
                    if (!point.velocities.empty()) {
                        p.velocities.push_back(0.0);
                    }
                    if (!point.accelerations.empty()) {
                        p.accelerations.push_back(0.0);
                    }
                    if (!point.effort.empty()) {
                        p.effort.push_back(0.0);
                    }
                    continue;
                }
            }

            // Failed to find the position for this joint
            FollowJointTrajectoryActionServer::Result result;
            result.error_code = FollowJointTrajectoryActionServer::Result::INVALID_JOINTS;
            result.error_string = "No joint state for missing joint";
            server->setAborted(result);
            return;
        }

        p.time_from_start = point.time_from_start;

        goal.trajectory.points.push_back(std::move(p));
    }

    using FollowJointTrajectoryActionClient =
            actionlib::SimpleActionClient<control_msgs::FollowJointTrajectoryAction>;

    auto spin_thread = true;
    FollowJointTrajectoryActionClient traj_client(srv_desc.name, spin_thread);
    if (!traj_client.waitForServer(ros::Duration(10.0))) {
        FollowJointTrajectoryActionServer::Result result;
        result.error_code = FollowJointTrajectoryActionServer::Result::INVALID_GOAL;
        result.error_string = "Trajectory server took too long to come up";
        server->setAborted(result);
        return;
    }

    auto res = traj_client.sendGoalAndWait(goal);
    if (res.state_ == res.SUCCEEDED) {
        FollowJointTrajectoryActionServer::Result result;
        server->setSucceeded(result);
    } else {
        FollowJointTrajectoryActionServer::Result result;
        // TODO: error code
        server->setAborted(result);
    }
}

// This node serves as a bridge between control_msgs::FollowTrajectoryAction
// clients and servers which send and receive different lists of joints. When
// a trajectory is received, it is relayed to one of a set of available action
// servers, specified via configuration, which accepts the same or more
// input joints. If joints are missing from the client's output, they are
// initialized to their current positions at each point throughout the
// trajectory.

// parameters:
// * configuration of each available trajectory server (no way to query this?)
// when a trajectory is received, select a suitable output trajectory server.
// This server should accept the same or more joints (and match as closely as
// possible to the input set of joints)
//
// When a server is selected that requires more joints than are specified, fill
// in values for that joint using the current robot state
int main(int argc, char* argv[])
{
    ros::init(argc, argv, "trajectory_dispatcher");
    ros::NodeHandle nh;
    ros::NodeHandle ph("~");

    auto traj_server_configs = std::vector<TrajectoryServerConfig>();

    XmlRpc::XmlRpcValue clients;
    if (!ph.getParam("clients", clients)) {
        ROS_ERROR("Failed to retrieve 'clients' from the param server");
        return 1;
    }

    if (clients.getType() != XmlRpc::XmlRpcValue::Type::TypeArray) {
        ROS_ERROR("'clients' must be an array");
        return 1;
    }

    for (auto i = 0; i < clients.size(); ++i) {
        auto config = TrajectoryServerConfig();
        auto& value = clients[i];
        if (value.getType() != XmlRpc::XmlRpcValue::Type::TypeStruct) {
            ROS_WARN("Skipping 'clients' element. Expected a struct");
            continue;
        }
        if (!value.hasMember("name") || !value.hasMember("joints")) {
            ROS_WARN("Skipping 'clients' elements. Expected 'name' and 'joints' fields");
            continue;
        }

        auto& name = (std::string&)value["name"];
        auto& joints = value["joints"];

        if (value["name"].getType() != XmlRpc::XmlRpcValue::Type::TypeString) {
            ROS_WARN("Expected string");
            continue;
        }

        config.name = name;

        if (value["joints"].getType() != XmlRpc::XmlRpcValue::Type::TypeArray) {
            ROS_WARN("Expected array");
            continue;
        }

        config.input_joint_names.reserve(joints.size());
        for (auto j = 0; j < joints.size(); ++j) {
            auto& v = joints[j];
            if (v.getType() != XmlRpc::XmlRpcValue::Type::TypeString) {
                ROS_WARN("Expected string");
                continue;
            }
            config.input_joint_names.push_back((std::string&)v);
        }

        traj_server_configs.push_back(std::move(config));
    }

    // Subscribe to and maintain the current joint state
    sensor_msgs::JointState::ConstPtr curr_state;

    using JointStateCallback =
            boost::function<void(const sensor_msgs::JointState::ConstPtr&)>;
     auto joint_state_callback =
        [&](const sensor_msgs::JointState::ConstPtr& msg) {
            curr_state = msg;
        };

    auto joint_state_sub = nh.subscribe<sensor_msgs::JointState>(
            "joint_states", 10, JointStateCallback(joint_state_callback));

    // Advertise the action server. Incoming goal requests will dispatch to the
    // appropriate available trajectory server and fill in any required input
    // joints using the current state.
    auto autostart = false;
    FollowJointTrajectoryActionServer server(
            "trajectory_dispatcher/follow_joint_trajectory",
            FollowJointTrajectoryActionServer::ExecuteCallback(
                [&](const control_msgs::FollowJointTrajectoryGoal::ConstPtr& msg) {
                    return execute(&server, &traj_server_configs, curr_state.get(), msg.get());
                }),
            autostart);

    server.start();

    ros::spin();
    return 0;
}


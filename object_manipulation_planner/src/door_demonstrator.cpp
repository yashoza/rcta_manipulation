// standard includes
#include <math.h>

// system includes
#include <Eigen/Dense>
#include <ros/ros.h>
#include <visualization_msgs/MarkerArray.h>
#include <leatherman/utils.h>
#include <eigen_conversions/eigen_msg.h>
#include <moveit_msgs/RobotState.h>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/robot_model/robot_model.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/robot_state/conversions.h>
#include <moveit_msgs/GetStateValidity.h>
#include <smpl/angles.h>
#include <smpl/console/console.h>
#include <smpl/debug/marker_utils.h>
#include <smpl/debug/marker_conversions.h>
#include <smpl/console/nonstd.h>
#include <urdf_parser/urdf_parser.h>
#include <smpl_urdf_robot_model/robot_model.h>
#include <smpl_urdf_robot_model/robot_state.h>
#include <smpl_urdf_robot_model/robot_state_visualization.h>

// project includes
#include "cabinet_model.h"

////////////////////
// STEALING DEFER //
////////////////////

namespace scdl {

template <class Callable>
struct CallOnDestruct
{
    Callable c;
    CallOnDestruct(Callable c) : c(c) { }
    ~CallOnDestruct() { c(); }
};

template <class Callable>
CallOnDestruct<Callable> MakeCallOnDestruct(Callable c) {
    return CallOnDestruct<Callable>(c);
}

} // namespace scdl

////////////////////

// preprocessor magic to get a prefix to concatenate with the __LINE__ macro
#define MAKE_LINE_IDENT_JOIN_(a, b) a##b
#define MAKE_LINE_IDENT_JOIN(a, b) MAKE_LINE_IDENT_JOIN_(a, b)
#define MAKE_LINE_IDENT(prefix) MAKE_LINE_IDENT_JOIN(prefix, __LINE__)

// create an obscurely named CallOnDestruct with an anonymous lambda that
// executes the given statement sequence
#define DEFER(fun) auto MAKE_LINE_IDENT(tmp_call_on_destruct_) = scdl::MakeCallOnDestruct([&](){ fun; })

auto MakeContactVisualization(
    const smpl::urdf::RobotState* state,
    const smpl::urdf::Link* contact_link,
    const char* frame,
    const char* ns,
    bool contact,
    int32_t* id = NULL)
    -> std::vector<smpl::visual::Marker>
{
    std::vector<smpl::visual::Marker> markers;

    auto first_id = id == NULL ? 0 : *id;
    for (auto& link : Links(GetRobotModel(state))) {
        auto* pose = GetLinkTransform(state, &link);
        for (auto& visual : link.visual) {
            smpl::visual::Marker m;

            m.pose = *GetVisualBodyTransform(state, &visual);
            m.shape = MakeShapeVisualization(visual.shape);
            if (&link == contact_link) {
                if (contact) {
                    m.color = smpl::visual::Color{ 0.0f, 0.0f, 1.0f, 1.0f };
                } else {
                    m.color = smpl::visual::Color{ 1.0f, 0.0f, 0.0f, 1.0f };
                }
            } else {
                m.color = smpl::visual::Color{ 1.0f, 1.0f, 1.0f, 1.0f };
            }
            m.frame_id = frame;
            m.ns = ns;
            m.lifetime = 0;
            m.id = first_id++;
            markers.push_back(m);
        }
    }

    if (id != NULL) *id = first_id;
    return markers;
}

template <class T>
bool GetParam(const ros::NodeHandle& nh, const std::string& key, T& value)
{
    if (!nh.getParam(key, value)) {
        SMPL_ERROR("Failed to retrieve '%s' from the param server", key.c_str());
        return false;
    }

    return true;
}

bool SetCabinetFromIK(
    smpl::urdf::RobotState* state,
    const Eigen::Affine3d* pose,
    const smpl::urdf::Link* link)
{
    auto world_jidx = 0;
    auto hinge_jidx = 1;
    auto handle_jidx = 2;

    // T_world_hinge, rotated so x is to the cabinet's left and y is to
    // the cabinet's back
    UpdateLinkTransform(state, GetRootLink(GetRobotModel(state)));
    Eigen::Affine3d hinge_frame =
            // T_world_cabinet
            *GetLinkTransform(state, GetRootLink(GetRobotModel(state))) *
            // T_cabinet_hinge
            *GetJointOrigin(GetJoint(GetRobotModel(state), hinge_jidx));

    // T_hinge_world * T_world_tool
    Eigen::Affine3d tool_in_hinge = hinge_frame.inverse() * (*pose);

    SMPL_INFO("tool position (hinge frame) = (%f, %f, %f)",
            tool_in_hinge.translation().x(),
            tool_in_hinge.translation().y(),
            tool_in_hinge.translation().z());

    auto GetHandleRotationRadius = [&]()
    {
        auto* J2 = GetJointOrigin(GetJoint(GetRobotModel(state), handle_jidx));
        auto dx = J2->translation().x();
        auto dy = J2->translation().y();
        return std::sqrt(dx * dx + dy * dy);
    };

    auto radius = GetHandleRotationRadius();

    auto GetHandleRotationOffset = [&]()
    {
        auto* J2 = GetJointOrigin(GetJoint(GetRobotModel(state), handle_jidx));
        auto dx = J2->translation().x();
        auto dy = J2->translation().y();
        return std::atan2(std::fabs(dx), std::fabs(dy));
    };

    // closest point on the circle mapped by the handle radius
    Eigen::Vector3d nearest = radius * tool_in_hinge.translation().normalized();


    auto ccw_dist = [](double ai, double af)
    {
        auto diff = smpl::angles::shortest_angle_diff(af, ai);
        if (diff >= 0.0) {
            return diff;
        } else {
            return 2.0 * M_PI - std::fabs(diff);
        }
    };


    double theta;
    SMPL_INFO("  theta = %f", atan2(nearest.y(), nearest.x()));
    theta = atan2(nearest.y(), nearest.x());

    // clamp to the boundaries for the hinge, theta is now the angle
    // the handle
    theta = std::max(theta, -0.5 * M_PI + GetHandleRotationOffset());
    theta = std::min(theta, 0.25 * M_PI + GetHandleRotationOffset());

    theta -= GetHandleRotationOffset();
    theta += 0.5 * M_PI;

    SMPL_INFO("  theta adjusted = %f", theta);

    SetVariablePosition(state, GetVariable(GetRobotModel(state), "door_joint"), theta);
    return true;
}

bool SetCrateFromIK(
    smpl::urdf::RobotState* state,
    const Eigen::Affine3d* pose,
    const smpl::urdf::Link* link)
{
    auto* robot = GetRobotModel(state);

    // T_world_crate
    auto* crate_link = GetLinkTransform(state, GetRootLink(robot));

    // transform from the world to the frame of the first joint:
    // T_world_J1 = T_world_L0 * T_L0_J2
    Eigen::Affine3d lid_frame = (*crate_link) * (*GetJointOrigin(GetJoint(robot, "lid_joint")));

    ROS_INFO("lid frame[world] = (%f, %f, %f)",
            lid_frame.translation().x(),
            lid_frame.translation().y(),
            lid_frame.translation().z());

    auto best_lid = std::numeric_limits<double>::max();
    auto best_dist = std::numeric_limits<double>::max();
    auto best_handle = std::numeric_limits<double>::quiet_NaN();

    // sample orientations for the handle and choose the configuration that
    // (1) TODO: best reaches the target point
    // (2) TODO: minimizes the lid opening
    auto sample_count = 100;
    for (int i = 0; i < sample_count; ++i) {
        auto alpha = (double)i / (double)(sample_count - 1);
        auto pitch =
                (1.0 - alpha) * GetVariableLimits(GetVariable(robot, "handle_joint"))->min_position +
                alpha *         GetVariableLimits(GetVariable(robot, "handle_joint"))->max_position;

        // desired pose of the tool in the frame of the first joint:
        // T_J1_L3 = (T_world_J1)^-1 * T_world_L3
        Eigen::Affine3d T_J1_L3 = lid_frame.inverse() *
                Eigen::Translation3d(pose->translation()) *
                Eigen::AngleAxisd(pitch, Eigen::Vector3d::UnitY());

        ROS_DEBUG("tool frame[lid frame] = (%f, %f, %f)", T_J1_L3.translation().x(), T_J1_L3.translation().y(), T_J1_L3.translation().z());

        // note: J3 fixed => L3 = J3

        // desired pose of the handle in the frame of the first joint:
        // T_J1_L2 = T_J1_L3 * T_L3_L2
        auto* J3_local = GetJointOrigin(GetJoint(robot, "tool_joint"));
        Eigen::Affine3d T_J1_L2 = T_J1_L3 * J3_local->inverse();

        ROS_DEBUG("handle frame[lid frame] = (%f, %f, %f)",
                T_J1_L2.translation().x(),
                T_J1_L2.translation().y(),
                T_J1_L2.translation().z());

        // revolute J2 => position(J2) = position(L2)

        auto J1_J2_x = T_J1_L2.translation().x();
        auto J1_J2_y = T_J1_L2.translation().z();

        auto J2_ox = GetJointOrigin(GetJoint(robot, "handle_joint"))->translation().x();
        auto J2_oy = GetJointOrigin(GetJoint(robot, "handle_joint"))->translation().z();

//        J1_J2_x = J2_ox * cos(th1) - J2_oy * sin(th1);
//        J1_J2_y = J2_ox * sin(th1) + J2_oy * cos(th1);

        auto x = J2_ox;
        auto y = J2_oy;
        auto xp = J1_J2_x;
        auto yp = J1_J2_y;

        ROS_DEBUG("x' = %f", xp);
        ROS_DEBUG("y' = %f", yp);
        ROS_DEBUG("x = %f", x);
        ROS_DEBUG("y = %f", y);

        auto th1 = std::atan2(x * yp - y * xp, y * yp + x * xp);

        // put the lid in its place
        th1 = -th1;
        th1 = std::min(th1, GetVariableLimits(GetVariable(robot, "lid_joint"))->max_position);
        th1 = std::max(th1, GetVariableLimits(GetVariable(robot, "lid_joint"))->min_position);

        SetVariablePosition(state, GetVariable(robot, "lid_joint"), th1);
        UpdateLinkTransform(state, GetLink(robot, "lid"));

        Eigen::Affine3d T_J1_J2 =
                lid_frame.inverse() * // (T_world_J1^1) = T_J1_world
                (*GetLinkTransform(state, GetLink(robot, "lid"))) * // T_world_L1
                (*GetJointOrigin(GetJoint(robot, "handle_joint"))); // T_L1_J2

        // get the transform from J2 to L2
        Eigen::Affine3d T_J2_L2 = T_J1_J2.inverse() * T_J1_L2;

        auto angle = Eigen::AngleAxisd(T_J2_L2.rotation()).angle();
        angle = std::min(angle, GetVariableLimits(GetVariable(robot, "handle_joint"))->max_position);
        angle = std::max(angle, GetVariableLimits(GetVariable(robot, "handle_joint"))->min_position);
        SetVariablePosition(state, GetVariable(robot, "handle_joint"), angle);
        UpdateLinkTransform(state, GetLink(robot, "tool"));

        // a configuration is better if...
        // ...the tool pose is closer to the ee pose
        // ...the joint variable is higher (lid more closed)
        auto margin = 0.01;
        auto* tip_trans = GetLinkTransform(state, GetLink(robot, "tool"));
        auto ee_dist = (pose->translation() - tip_trans->translation()).norm();

        if (ee_dist < best_dist) { // - margin) {
            ROS_INFO("Better distance! %f -> %f @ (%f, %f)", best_dist, ee_dist, th1, angle);
            best_lid = th1;
            best_handle = angle;
            best_dist = ee_dist;
        }
#if 0
        else if (ee_dist < best_dist + margin) {
            if (th1 > best_lid) {
                ROS_INFO("Heavier lid! %f -> %f @ (%f, %f)", best_lid, th1, th1, angle);
                best_lid = th1;
                best_handle = angle;
                best_dist = ee_dist;
            }
        }
#endif
        else {
        }
    }

    SetVariablePosition(state, GetVariable(robot, "lid_joint"), best_lid);
    SetVariablePosition(state, GetVariable(robot, "handle_joint"), best_handle);

    return true;
}

auto ConvertMarkersToMarkersMsg(const std::vector<smpl::visual::Marker>& markers)
    -> visualization_msgs::MarkerArray
{
    visualization_msgs::MarkerArray ma;
    ma.markers.reserve(markers.size());
    for (auto& marker : markers) {
        visualization_msgs::Marker m;
        ConvertMarkerToMarkerMsg(marker, m);
        ma.markers.push_back(std::move(m));
    }
    return ma;
}

int main(int argc, char* argv[])
{
    ros::init(argc, argv, "door_demonstrator", ros::init_options::AnonymousName);
    ros::NodeHandle nh;
    ros::NodeHandle ph("~");

    ////////////////
    // Parameters //
    ////////////////

    std::string group_name;
    std::string demo_filename;
    std::string tip_link;
    std::string object_tip_link_name;
    std::string object_description;
    double contact_error_z;
    double contact_error;

    if (!GetParam(ph, "group_name", group_name)) return 1;
    if (!GetParam(ph, "demo_filename", demo_filename)) return 1;
    if (!GetParam(ph, "robot_tip_link", tip_link)) return 1;
    if (!GetParam(ph, "object_tip_link", object_tip_link_name)) return 1;
    if (!GetParam(nh, "object_description", object_description)) return 1;
    if (!GetParam(ph, "contact_error_z", contact_error_z)) return 1;
    if (!GetParam(ph, "contact_error", contact_error)) return 1;

    // initialize object pose
    Eigen::Affine3d object_pose;
    {
        double ox, oy, oz, oyaw, opitch, oroll;
        if (!GetParam(ph, "object_x", ox)) return 1;
        if (!GetParam(ph, "object_y", oy)) return 1;
        if (!GetParam(ph, "object_z", oz)) return 1;
        if (!GetParam(ph, "object_yaw", oyaw)) return 1;
        if (!GetParam(ph, "object_pitch", opitch)) return 1;
        if (!GetParam(ph, "object_roll", oroll)) return 1;
        object_pose =
                Eigen::Translation3d(ox, oy, oz) *
                Eigen::AngleAxisd(oyaw, Eigen::Vector3d::UnitZ()) *
                Eigen::AngleAxisd(opitch, Eigen::Vector3d::UnitY()) *
                Eigen::AngleAxisd(oroll, Eigen::Vector3d::UnitX());
    }

    ///////////////
    // Arguments //
    ///////////////

    printf("Usage: door_demonstrator [record] [record_object]\n");

    auto record = argc > 1 ? true : false;
    auto record_object = argc > 2 ? true: false;

    SMPL_INFO("Record: %s", record ? "true" : "false");
    SMPL_INFO("Record Object: %s", record_object ? "true" : "false");

    ////////////////////
    // Initialization //
    ////////////////////

    // initialize robot model...
    robot_model_loader::RobotModelLoader loader;
    auto model = loader.getModel();

    // initialize object model...
    auto object_urdf = urdf::parseURDF(object_description);
    if (!object_urdf) {
        SMPL_ERROR("Failed to parse object URDF");
        return 1;
    }

    smpl::urdf::RobotModel object_model;
    if (!InitRobotModel(&object_model, object_urdf.get())) {
        SMPL_ERROR("Failed to initialize object model");
        return 1;
    }

    // initialize robot state...
    moveit::core::RobotState robot_state(model);
    robot_state.setToDefaultValues();

    // initialize object state...
    smpl::urdf::RobotState object_state;
    if (!Init(&object_state, &object_model)) {
        SMPL_ERROR("Failed to initialize object state");
        return 1;
    }
    SetToDefaultValues(&object_state);
    UpdateTransforms(&object_state);

    {
        double world_joint_state[7];
        world_joint_state[0] = object_pose.translation().x();
        world_joint_state[1] = object_pose.translation().y();
        world_joint_state[2] = object_pose.translation().z();
        Eigen::Quaterniond q(object_pose.rotation());
        world_joint_state[3] = q.x();
        world_joint_state[4] = q.y();
        world_joint_state[5] = q.z();
        world_joint_state[6] = q.w();
        SetJointPositions(&object_state, GetRootJoint(&object_model), world_joint_state);
    }

    // initialize demonstration joint group...
    if (!model->hasJointModelGroup(group_name)) {
        SMPL_ERROR("Demonstration group '%s' does not exist in the robot model", group_name.c_str());
        return 1;
    }

    auto* demo_group = model->getJointModelGroup(group_name);

    // initialize object tip...
    auto* object_tip_link = GetLink(&object_model, object_tip_link_name.c_str());
    if (object_tip_link == NULL) {
        SMPL_ERROR("Tip link '%s' not found in object model", object_tip_link_name.c_str());
        return 1;
    }

    // initialize the demonstration recording...
    FILE* fdemo = NULL;
    if (record) {
        fdemo = fopen(demo_filename.c_str(), "w");
        if (!fdemo) {
            SMPL_ERROR("Failed to open '%s' for writing", demo_filename.c_str());
            return 1;
        }

        // write group variable names to the header
        for (size_t vidx = 0; vidx < demo_group->getVariableCount(); ++vidx) {
            auto& name = demo_group->getVariableNames()[vidx];
            fprintf(fdemo, "%s", name.c_str());
            if (record_object) {
                fprintf(fdemo, ",");
            } else {
                if (vidx != demo_group->getVariableCount() - 1) {
                    fprintf(fdemo, ",");
                } else {
                    fprintf(fdemo, "\n");
                }
            }
        }
        if (record_object) {
            fprintf(fdemo, "hinge\n");
        }
    }

    // close the demonstration file when we kill the program...
    DEFER(
        if (fdemo != NULL) {
            fclose(fdemo);
            fdemo = NULL;
        }
    );

    // initialize object state variables...
    std::vector<double> prev_object_state(
            GetVariablePositions(&object_state),
            GetVariablePositions(&object_state) + GetVariableCount(&object_model));
    auto curr_object_state = prev_object_state;

    // initialize contact state...
    auto contacted = false; // ever came into contact?
    auto contact = false;   // in contact at the last frame

    auto state_received = false;
    moveit_msgs::RobotState::ConstPtr curr_robot_state;

    //////////////////////
    // Fire up the node //
    //////////////////////

    boost::function<void(const moveit_msgs::RobotState::ConstPtr&)>
    robot_state_callback = [&](const moveit_msgs::RobotState::ConstPtr& msg) {
        curr_robot_state = msg;
        state_received = true;
    };

    auto state_sub = nh.subscribe("robot_state", 1, robot_state_callback);

    // to perform usual validity checking
    auto check_state_service = nh.serviceClient<moveit_msgs::GetStateValidity>(
            "check_state_validity");

    auto marker_pub = nh.advertise<visualization_msgs::MarkerArray>(
            "visualization_markers", 1);

    boost::function<bool(moveit_msgs::GetStateValidity::Request& req, moveit_msgs::GetStateValidity::Response& res)> service_fun;

    auto IsStateValid = [&](
        moveit_msgs::GetStateValidity::Request& req,
        moveit_msgs::GetStateValidity::Response& res)
        -> bool
    {
        if (contacted) {
            bool result = check_state_service.call(req, res);
            if (result) {
                res.valid = res.valid && contact;
            }
            return result;
        } else {
            return check_state_service.call(req, res);
        }
    };

    service_fun = IsStateValid;

    auto check_state_server =
            nh.advertiseService("check_state_validity_manipulation", service_fun);

    ////////////////////////
    // demonstration loop //
    ////////////////////////

    ros::Rate loop_rate(30.0);
    while (ros::ok()) {
        ros::spinOnce();

        DEFER(state_received = false);

        // 0 to 3*pi/4
//        auto amplitude = 0.5 * GetHingeSpan(&cabinet);
//        auto median = 0.5 * (GetHingeLowerLimit(&cabinet) + GetHingeUpperLimit(&cabinet));

        if (!std::equal(
                begin(prev_object_state),
                end(prev_object_state),
                begin(curr_object_state)))
        {
            SMPL_INFO_STREAM("object state = " << curr_object_state);
            prev_object_state = curr_object_state;
        }


        if (state_received) {
            moveit::core::robotStateMsgToRobotState(*curr_robot_state, robot_state);
        }

        ////////////////////////////////////////////////////////////////
        // record the motion of the robot and the state of the object //
        ////////////////////////////////////////////////////////////////

        auto robot_moved = state_received;
        if (robot_moved && record) {
            assert(curr_robot_state && "State shouldn't be null if we received it");

            std::vector<double> group_state;
            robot_state.copyJointGroupPositions(demo_group, group_state);

            for (size_t vidx = 0; vidx < demo_group->getVariableCount(); ++vidx) {
                auto pos = group_state[vidx];
                fprintf(fdemo, "%f", pos);
                if (record_object) {
                    fprintf(fdemo, ",");
                } else {
                    if (vidx != demo_group->getVariableCount() - 1) {
                        fprintf(fdemo, ",");
                    } else {
                        fprintf(fdemo, "\n");
                    }
                }
            }
            if (record_object) {
                for (auto i = 0; i < curr_object_state.size(); ++i) {
                    fprintf(fdemo, "%f", curr_object_state[i]);
                    if (i != curr_object_state.size() - 1) {
                        fprintf(fdemo, ",");
                    }
                }
            }
        }

        //////////////////////////
        // update contact state //
        //////////////////////////

        UpdateLinkTransform(&object_state, object_tip_link);
        auto* handle_pose = GetLinkTransform(&object_state, object_tip_link);
        SMPL_DEBUG("  handle @ (%f, %f, %f)",
                handle_pose->translation().x(),
                handle_pose->translation().y(),
                handle_pose->translation().z());

        contact = false;
        Eigen::Affine3d curr_tool_pose;
        if (curr_robot_state) {
            curr_tool_pose = robot_state.getGlobalLinkTransform(tip_link);
            SMPL_DEBUG("  curr tool @ (%f, %f, %f)",
                    curr_tool_pose.translation().x(),
                    curr_tool_pose.translation().y(),
                    curr_tool_pose.translation().z());

            Eigen::Vector3d dp = curr_tool_pose.translation() - handle_pose->translation();
            if (std::fabs(dp.z()) < contact_error_z &&
                dp.x() * dp.x() + dp.y() * dp.y() < contact_error * contact_error)
            {
                contact = true;
                contacted = true; // never reset to false
            }

            auto markers = smpl::visual::MakeFrameMarkers(curr_tool_pose, "map", "tool");
            visualization_msgs::MarkerArray ma;
            for (auto& marker : markers) {
                visualization_msgs::Marker m;
                ConvertMarkerToMarkerMsg(marker, m);
                ma.markers.push_back(std::move(m));
            }
            marker_pub.publish(ma);
        }

        /////////////////////////////////
        // update object state from ik //
        /////////////////////////////////

        if (robot_moved || contact) {
            if (object_urdf->getName() == "cabinet") {
                if (!SetCabinetFromIK(&object_state, &curr_tool_pose, object_tip_link)) {
//                    SMPL_WARN("Failed to set cabinet from IK");
                }
            } else if (object_urdf->getName() == "crate") {
                if (!SetCrateFromIK(&object_state, &curr_tool_pose, object_tip_link)) {

                }
            }
        }

        //////////////////////////
        // visualize the object //
        //////////////////////////

        UpdateCollisionBodyTransforms(&object_state);
        UpdateVisualBodyTransforms(&object_state);
#if 1
        auto markers = ConvertMarkersToMarkersMsg(MakeContactVisualization(
                &object_state, object_tip_link, "map", "cabinet", contact));
#else
        auto markers = ConvertMarkersToMarkersMsg(MakeCollisionVisualization(
                    &object_state,
                    smpl::visual::Color{ 0.8f, 0.8f, 0.8f, 1.0f },
                    "map",
                    "cabinet"));
#endif

        // TODO: anonymize ids to allow visualizations from multiple anonymous nodes

        marker_pub.publish(markers);
        loop_rate.sleep();
    }

    return 0;
}

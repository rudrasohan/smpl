#include "sbpl_planner_manager.h"

#include <leatherman/print.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/robot_state/conversions.h>

#include "sbpl_planning_context.h"
#include <moveit_planners_sbpl/collision_detector_allocator_sbpl.h>
#include <moveit_planners_sbpl/collision_world_sbpl.h>

static const char* xmlTypeToString(XmlRpc::XmlRpcValue::Type type)
{
    switch (type) {
    case XmlRpc::XmlRpcValue::TypeInvalid:
        return "Invalid";
    case XmlRpc::XmlRpcValue::TypeBoolean:
        return "Boolean";
    case XmlRpc::XmlRpcValue::TypeInt:
        return "Int";
    case XmlRpc::XmlRpcValue::TypeDouble:
        return "Double";
    case XmlRpc::XmlRpcValue::TypeString:
        return "String";
    case XmlRpc::XmlRpcValue::TypeDateTime:
        return "DateTime";
    case XmlRpc::XmlRpcValue::TypeBase64:
        return "Base64";
    case XmlRpc::XmlRpcValue::TypeArray:
        return "Array";
    case XmlRpc::XmlRpcValue::TypeStruct:
        return "Struct";
    default:
        return "Unrecognized";
    }
}

namespace sbpl_interface {

// pp = planning plugin
static const char* PP_LOGGER = "planning";

const std::string DefaultPlanningAlgorithm = "arastar";

SBPLPlannerManager::SBPLPlannerManager() :
    Base(),
    m_robot_model(),
    m_ns(),
    m_viz()
{
    ROS_DEBUG_NAMED(PP_LOGGER, "Constructed SBPL Planner Manager");
    sbpl::viz::set_visualizer(&m_viz);
}

SBPLPlannerManager::~SBPLPlannerManager()
{
    ROS_DEBUG_NAMED(PP_LOGGER, "Destructed SBPL Planner Manager");
    if (sbpl::viz::visualizer() == &m_viz) {
        sbpl::viz::unset_visualizer();
    }
}

bool SBPLPlannerManager::initialize(
    const robot_model::RobotModelConstPtr& model,
    const std::string& ns)
{
    ROS_INFO_NAMED(PP_LOGGER, "Initializing SBPL Planner Manager");
    ROS_INFO_NAMED(PP_LOGGER, "  Robot Model: %s", model->getName().c_str());
    ROS_INFO_NAMED(PP_LOGGER, "  Namespace: %s", ns.c_str());

    m_robot_model = model;
    m_ns = ns;

    if (!loadPlannerConfigurationMapping(*model)) {
        ROS_ERROR_NAMED(PP_LOGGER, "Failed to load planner configurations");
        return false;
    }

    ROS_INFO_NAMED(PP_LOGGER, "Initialized SBPL Planner Manager");
    return true;
}

std::string SBPLPlannerManager::getDescription() const
{
    return "Search-Based Planning Algorithms";
}

void SBPLPlannerManager::getPlanningAlgorithms(
    std::vector<std::string>& algs) const
{
    algs.push_back("ARA*");
    algs.push_back("MHA*");
    algs.push_back("LARA*");
}

planning_interface::PlanningContextPtr SBPLPlannerManager::getPlanningContext(
    const planning_scene::PlanningSceneConstPtr& planning_scene,
    const planning_interface::MotionPlanRequest& req,
    moveit_msgs::MoveItErrorCodes& error_code) const
{
    ROS_DEBUG_NAMED(PP_LOGGER, "Getting SBPL Planning Context");

    planning_interface::PlanningContextPtr context;

    if (!canServiceRequest(req)) {
        ROS_WARN_NAMED(PP_LOGGER, "Unable to service request");
        return context;
    }

    if (!planning_scene) {
        ROS_WARN_NAMED(PP_LOGGER, "Planning Scene is null");
        return context;
    }

    // create a child planning scene so we can set a different collision checker
    planning_scene::PlanningScenePtr diff_scene = planning_scene->diff();

    ///////////////////////////
    // Setup SBPL Robot Model
    ///////////////////////////

    SBPLPlannerManager* mutable_me = const_cast<SBPLPlannerManager*>(this);
    MoveItRobotModel* sbpl_model = mutable_me->getModelForGroup(req.group_name);
    if (!sbpl_model) {
        ROS_WARN_NAMED(PP_LOGGER, "No SBPL Robot Model available for group '%s'", req.group_name.c_str());
        return context;
    }

    // TODO: reevaluate these assumptions when different goal types are added
    if (!req.goal_constraints.empty()) {
        const auto& goal_constraint = req.goal_constraints.front();
        // should've received one pose constraint for a single link, o/w
        // canServiceRequest would have complained
        assert(!goal_constraint.position_constraints.empty());
        const auto& position_constraint = goal_constraint.position_constraints.front();
        const std::string& planning_link = position_constraint.link_name;
        ROS_INFO_NAMED(PP_LOGGER, "Setting planning link to '%s'", planning_link.c_str());
        if (!sbpl_model->setPlanningLink(planning_link)) {
            ROS_ERROR_NAMED(PP_LOGGER, "Failed to set planning link to '%s'", planning_link.c_str());
            return context;
        }
    }

    bool res = true;
    res &= sbpl_model->setPlanningScene(diff_scene);
    res &= sbpl_model->setPlanningFrame(diff_scene->getPlanningFrame());
    if (!res) {
        ROS_WARN_NAMED(PP_LOGGER, "Failed to set SBPL Robot Model's planning scene or planning frame");
        return context;
    }

    // get the planner configuration for this group
    const auto& pcs = getPlannerConfigurations().find(req.group_name)->second;

    ///////////////////////////////////////////
    // Initialize a new SBPL Planning Context
    ///////////////////////////////////////////

//    logPlanningScene(*diff_scene);
    logMotionPlanRequest(req);

    SBPLPlanningContext* sbpl_context = new SBPLPlanningContext(
            sbpl_model, "sbpl_planning_context", req.group_name);

    // find a configuration for this group + planner_id
    const planning_interface::PlannerConfigurationMap& pcm = getPlannerConfigurations();

    // merge group parameters and planning configuration parameters of the
    // appropriate planner type
    std::map<std::string, std::string> all_params;
    for (auto it = pcm.begin(); it != pcm.end(); ++it) {
        const std::string& name = it->first;
        const planning_interface::PlannerConfigurationSettings& pcs = it->second;
        const std::string& group_name = req.group_name;
        if (name.find(group_name) != std::string::npos) {
            auto iit = pcs.config.find("type");
            if (iit == pcs.config.end() || iit->second == req.planner_id) {
                all_params.insert(pcs.config.begin(), pcs.config.end());
            }
        }
    }
    if (!sbpl_context->init(all_params)) {
        ROS_ERROR_NAMED(PP_LOGGER, "Failed to initialize SBPL Planning Context");
        delete sbpl_context;
        return context;
    }

    sbpl_context->setPlanningScene(diff_scene);
    sbpl_context->setMotionPlanRequest(req);

    context.reset(sbpl_context);
    return context;
}

bool SBPLPlannerManager::canServiceRequest(
    const planning_interface::MotionPlanRequest& req) const
{
    ROS_DEBUG_NAMED(PP_LOGGER, "SBPLPlannerManager::canServiceRequest()");

    // TODO: Most of this is just duplicate of
    // SBPLArmPlannerInterface::canServiceRequest. Can we make that static and
    // just call that here?

    if (req.allowed_planning_time < 0.0) {
        ROS_WARN_NAMED(PP_LOGGER, "allowed_planning_time must be non-negative");
        return false;
    }

    // check for a configuration for the requested group
    auto pcit = getPlannerConfigurations().find(req.group_name);
    if (pcit == getPlannerConfigurations().end()) {
        ROS_WARN_NAMED(PP_LOGGER, "No planner configuration found for group '%s'", req.group_name.c_str());
        return false;
    }

    std::vector<std::string> available_algs;
    getPlanningAlgorithms(available_algs);
    if (std::find(
            available_algs.begin(), available_algs.end(), req.planner_id) ==
                    available_algs.end())
    {
        ROS_WARN_NAMED(PP_LOGGER, "SBPL planner does not support the '%s' algorithm", req.planner_id.c_str());
        return false;
    }

    // guard against unsupported constraints

    if (req.goal_constraints.size() > 1) {
        ROS_WARN_NAMED(PP_LOGGER, "SBPL planner does not currently support more than one goal constraint");
        return false;
    }

    for (const moveit_msgs::Constraints& constraints : req.goal_constraints) {
        if (!constraints.joint_constraints.empty()) {
            ROS_WARN_NAMED(PP_LOGGER, "SBPL planner does not currently support goal constraints on joint positions");
            return false;
        }

        if (!constraints.visibility_constraints.empty()) {
            ROS_WARN_NAMED(PP_LOGGER, "SBPL planner does not currently support goal constraints on visibility");
            return false;
        }

        if (constraints.position_constraints.size() != 1 ||
            constraints.orientation_constraints.size() != 1)
        {
            ROS_WARN_NAMED(PP_LOGGER, "SBPL planner only supports goal constraints with exactly one position constraint and one orientation constraint");
            return false;
        }
    }

    if (!req.path_constraints.position_constraints.empty() ||
        !req.path_constraints.orientation_constraints.empty() ||
        !req.path_constraints.joint_constraints.empty() ||
        !req.path_constraints.visibility_constraints.empty())
    {
        ROS_WARN_NAMED(PP_LOGGER, "SBPL planner does not support path constraints");
        return false;
    }

    if (!req.trajectory_constraints.constraints.empty()) {
        ROS_WARN_NAMED(PP_LOGGER, "SBPL planner does not support trajectory constraints");
        return false;
    }

    // TODO: check start state for existence of state for our robot model

    // TODO: check for existence of workspace parameters frame? Would this make
    // this call tied to an explicit planning scene?
    if (req.workspace_parameters.header.frame_id.empty()) {
        ROS_WARN_NAMED(PP_LOGGER, "SBPL planner requires workspace parameters to have a valid frame");
        return false;
    }

    // check for positive workspace volume
    const auto& min_corner = req.workspace_parameters.min_corner;
    const auto& max_corner = req.workspace_parameters.max_corner;
    if (min_corner.x > max_corner.x ||
        min_corner.y > max_corner.y ||
        min_corner.z > max_corner.z)
    {
        std::stringstream reasons;
        if (min_corner.x > max_corner.x) {
            reasons << "negative length";
        }
        if (min_corner.y > max_corner.y) {
            reasons << (reasons.str().empty() ? "" : " ") << "negative width";
        }
        if (min_corner.z > max_corner.z) {
            reasons << (reasons.str().empty() ? "" : " ") << "negative height";
        }
        ROS_WARN_NAMED(PP_LOGGER, "SBPL planner requires valid workspace (%s)", reasons.str().c_str());
        return false;
    }

    return true;
}

void SBPLPlannerManager::setPlannerConfigurations(
    const planning_interface::PlannerConfigurationMap& pcs)
{
    Base::setPlannerConfigurations(pcs);

    ROS_INFO_NAMED(PP_LOGGER, "Planner Configurations");
    for (const auto& entry : pcs) {
        ROS_INFO_NAMED(PP_LOGGER, "  %s: { name: %s, group: %s }", entry.first.c_str(), entry.second.group.c_str(), entry.second.name.c_str());
        for (const auto& e : entry.second.config) {
            ROS_INFO_NAMED(PP_LOGGER, "    %s: %s", e.first.c_str(), e.second.c_str());
        }
    }
}

void SBPLPlannerManager::logPlanningScene(
    const planning_scene::PlanningScene& scene) const
{
    ROS_INFO_NAMED(PP_LOGGER, "Planning Scene");
    ROS_INFO_NAMED(PP_LOGGER, "  Name: %s", scene.getName().c_str());
    ROS_INFO_NAMED(PP_LOGGER, "  Has Parent: %s", scene.getParent() ? "true" : "false");
    ROS_INFO_NAMED(PP_LOGGER, "  Has Robot Model: %s", scene.getRobotModel() ? "true" : "false");
    ROS_INFO_NAMED(PP_LOGGER, "  Planning Frame: %s", scene.getPlanningFrame().c_str());
    ROS_INFO_NAMED(PP_LOGGER, "  Active Collision Detector Name: %s", scene.getActiveCollisionDetectorName().c_str());
    ROS_INFO_NAMED(PP_LOGGER, "  Has World: %s", scene.getWorld() ? "true" : "false");
    if (scene.getWorld()) {
        ROS_INFO_NAMED(PP_LOGGER, "    size:  %zu", scene.getWorld()->size());
        ROS_INFO_NAMED(PP_LOGGER, "    Object IDs: %zu", scene.getWorld()->getObjectIds().size());
        for (auto oit = scene.getWorld()->begin();
            oit != scene.getWorld()->end(); ++oit)
        {
            const std::string& object_id = oit->first;
            ROS_INFO_NAMED(PP_LOGGER, "      %s", object_id.c_str());
        }
    }
    ROS_INFO_NAMED(PP_LOGGER, "  Has Collision World: %s", scene.getCollisionWorld() ? "true" : "false");
    ROS_INFO_NAMED(PP_LOGGER, "  Has Collision Robot: %s", scene.getCollisionRobot() ? "true" : "false");
    ROS_INFO_NAMED(PP_LOGGER, "  Current State:");

    const moveit::core::RobotState& current_state = scene.getCurrentState();
    for (size_t vind = 0; vind < current_state.getVariableCount(); ++vind) {
        ROS_INFO_NAMED(PP_LOGGER, "    %s: %0.3f", current_state.getVariableNames()[vind].c_str(), current_state.getVariablePosition(vind));
    }

//    ROS_INFO_NAMED(PP_LOGGER, "Allowed collision matrix");
//    scene.getAllowedCollisionMatrix().print(std::cout);
}

void SBPLPlannerManager::logMotionPlanRequest(
    const planning_interface::MotionPlanRequest& req) const
{
    ROS_DEBUG_NAMED(PP_LOGGER, "Motion Plan Request");

    ROS_DEBUG_NAMED(PP_LOGGER, "  workspace_parameters");
    ROS_DEBUG_NAMED(PP_LOGGER, "    header");
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      seq: " << req.workspace_parameters.header.seq);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      stamp: " << req.workspace_parameters.header.stamp);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      frame_id: \"" << req.workspace_parameters.header.frame_id.c_str() << "\"");
    ROS_DEBUG_NAMED(PP_LOGGER, "    min_corner");
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      x: " << req.workspace_parameters.min_corner.x);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      y: " << req.workspace_parameters.min_corner.y);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      z: " << req.workspace_parameters.min_corner.z);
    ROS_DEBUG_NAMED(PP_LOGGER, "    max_corner");
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      x: " << req.workspace_parameters.max_corner.x);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      y: " << req.workspace_parameters.max_corner.y);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "      z: " << req.workspace_parameters.max_corner.z);

    ROS_DEBUG_NAMED(PP_LOGGER, "  start_state");
    ROS_DEBUG_NAMED(PP_LOGGER, "    joint_state:");
    const sensor_msgs::JointState& joint_state = req.start_state.joint_state;
    for (size_t jidx = 0; jidx < joint_state.name.size(); ++jidx) {
        ROS_DEBUG_NAMED(PP_LOGGER, "      { name: %s, position: %0.3f }", joint_state.name[jidx].c_str(), joint_state.position[jidx]);
    }
    ROS_DEBUG_NAMED(PP_LOGGER, "    multi_dof_joint_state");
    const sensor_msgs::MultiDOFJointState& multi_dof_joint_state = req.start_state.multi_dof_joint_state;
    ROS_DEBUG_NAMED(PP_LOGGER, "      header: { seq: %d, stamp: %0.3f, frame_id: \"%s\" }",
            multi_dof_joint_state.header.seq,
            multi_dof_joint_state.header.stamp.toSec(),
            multi_dof_joint_state.header.frame_id.c_str());
    for (size_t jidx = 0; jidx < multi_dof_joint_state.joint_names.size(); ++jidx) {
        const std::string& joint_name = multi_dof_joint_state.joint_names[jidx];
        const geometry_msgs::Transform& transform = multi_dof_joint_state.transforms[jidx];
        ROS_DEBUG_NAMED(PP_LOGGER, "      { joint_names: %s, transform: %s }", joint_name.c_str(), to_string(transform).c_str());
    }

    ROS_DEBUG_NAMED(PP_LOGGER, "    attached_collision_objects: %zu", req.start_state.attached_collision_objects.size());
    ROS_DEBUG_NAMED(PP_LOGGER, "    is_diff: %s", req.start_state.is_diff ? "true" : "false");

    ROS_DEBUG_NAMED(PP_LOGGER, "  goal_constraints: %zu", req.goal_constraints.size());
    for (size_t cind = 0; cind < req.goal_constraints.size(); ++cind) {
        const moveit_msgs::Constraints& constraints = req.goal_constraints[cind];

        // joint constraints
        ROS_DEBUG_NAMED(PP_LOGGER, "    joint_constraints: %zu", constraints.joint_constraints.size());
        for (size_t jcind = 0; jcind < constraints.joint_constraints.size(); ++jcind) {
            const moveit_msgs::JointConstraint& joint_constraint =
                    constraints.joint_constraints[jcind];
            ROS_DEBUG_NAMED(PP_LOGGER, "      joint_name: %s, position: %0.3f, tolerance_above: %0.3f, tolerance_below: %0.3f, weight: %0.3f",
                    joint_constraint.joint_name.c_str(),
                    joint_constraint.position,
                    joint_constraint.tolerance_above,
                    joint_constraint.tolerance_below,
                    joint_constraint.weight);
        }

        // position constraints
        ROS_DEBUG_NAMED(PP_LOGGER, "    position_constraints: %zu", constraints.position_constraints.size());
        for (size_t pcind = 0; pcind < constraints.position_constraints.size(); ++pcind) {
            const moveit_msgs::PositionConstraint pos_constraint =
                    constraints.position_constraints[pcind];
            ROS_DEBUG_NAMED(PP_LOGGER, "      header: { frame_id: %s, seq: %u, stamp: %0.3f }", pos_constraint.header.frame_id.c_str(), pos_constraint.header.seq, pos_constraint.header.stamp.toSec());
            ROS_DEBUG_NAMED(PP_LOGGER, "      link_name: %s", pos_constraint.link_name.c_str());
            ROS_DEBUG_NAMED(PP_LOGGER, "      target_point_offset: (%0.3f, %0.3f, %0.3f)", pos_constraint.target_point_offset.x, pos_constraint.target_point_offset.y, pos_constraint.target_point_offset.z);
            ROS_DEBUG_NAMED(PP_LOGGER, "      constraint_region:");
            ROS_DEBUG_NAMED(PP_LOGGER, "        primitives: %zu", pos_constraint.constraint_region.primitives.size());
            for (size_t pind = 0; pind < pos_constraint.constraint_region.primitives.size(); ++pind) {
                const shape_msgs::SolidPrimitive& prim = pos_constraint.constraint_region.primitives[pind];
                const geometry_msgs::Pose& pose = pos_constraint.constraint_region.primitive_poses[pind];
                ROS_DEBUG_NAMED(PP_LOGGER, "          { type: %d, pose: { position: (%0.3f, %0.3f, %0.3f), orientation: (%0.3f, %0.3f, %0.3f, %0.3f) } }", prim.type, pose.position.x, pose.position.y, pose.position.y, pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
            }
            ROS_DEBUG_NAMED(PP_LOGGER, "        meshes: %zu", pos_constraint.constraint_region.meshes.size());
        }

        // orientation constarints
        ROS_DEBUG_NAMED(PP_LOGGER, "    orientation_constraints: %zu", constraints.orientation_constraints.size());
        for (size_t ocind = 0; ocind < constraints.orientation_constraints.size(); ++ocind) {
            const moveit_msgs::OrientationConstraint rot_constraint =
                    constraints.orientation_constraints[ocind];
                ROS_DEBUG_NAMED(PP_LOGGER, "      header: { frame_id: %s, seq: %u, stamp: %0.3f }", rot_constraint.header.frame_id.c_str(), rot_constraint.header.seq, rot_constraint.header.stamp.toSec());
                ROS_DEBUG_NAMED(PP_LOGGER, "      orientation: (%0.3f, %0.3f, %0.3f, %0.3f)", rot_constraint.orientation.w, rot_constraint.orientation.x, rot_constraint.orientation.y, rot_constraint.orientation.z);
                ROS_DEBUG_NAMED(PP_LOGGER, "      link_name: %s", rot_constraint.link_name.c_str());
                ROS_DEBUG_NAMED(PP_LOGGER, "      absolute_x_axis_tolerance: %0.3f", rot_constraint.absolute_x_axis_tolerance);
                ROS_DEBUG_NAMED(PP_LOGGER, "      absolute_y_axis_tolerance: %0.3f", rot_constraint.absolute_y_axis_tolerance);
                ROS_DEBUG_NAMED(PP_LOGGER, "      absolute_z_axis_tolerance: %0.3f", rot_constraint.absolute_z_axis_tolerance);
                ROS_DEBUG_NAMED(PP_LOGGER, "      weight: %0.3f", rot_constraint.weight);
        }

        // visibility constraints
        ROS_DEBUG_NAMED(PP_LOGGER, "    visibility_constraints: %zu", constraints.visibility_constraints.size());
    }

    ROS_DEBUG_NAMED(PP_LOGGER, "  path_constraints");
    ROS_DEBUG_NAMED(PP_LOGGER, "    joint_constraints: %zu", req.path_constraints.joint_constraints.size());
    ROS_DEBUG_NAMED(PP_LOGGER, "    position_constraints: %zu", req.path_constraints.position_constraints.size());
    ROS_DEBUG_NAMED(PP_LOGGER, "    orientation_constraints: %zu", req.path_constraints.orientation_constraints.size());
    ROS_DEBUG_NAMED(PP_LOGGER, "    visibility_constraints: %zu", req.path_constraints.visibility_constraints.size());

    ROS_DEBUG_NAMED(PP_LOGGER, "  trajectory_constraints");
    for (size_t cind = 0; cind < req.trajectory_constraints.constraints.size(); ++cind) {
        const moveit_msgs::Constraints& constraints = req.trajectory_constraints.constraints[cind];
        ROS_DEBUG_NAMED(PP_LOGGER, "    joint_constraints: %zu", constraints.joint_constraints.size());
        ROS_DEBUG_NAMED(PP_LOGGER, "    position_constraints: %zu", constraints.position_constraints.size());
        ROS_DEBUG_NAMED(PP_LOGGER, "    orientation_constraints: %zu", constraints.orientation_constraints.size());
        ROS_DEBUG_NAMED(PP_LOGGER, "    visibility_constraints: %zu", constraints.visibility_constraints.size());
    }

    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "  planner_id: " << req.planner_id);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "  group_name: " << req.group_name);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "  num_planning_attempts: " << req.num_planning_attempts);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "  allowed_planning_time: " << req.allowed_planning_time);
    ROS_DEBUG_STREAM_NAMED(PP_LOGGER, "  max_velocity_scaling_factor: " << req.max_velocity_scaling_factor);
}

bool SBPLPlannerManager::loadPlannerConfigurationMapping(
    const moveit::core::RobotModel& model)
{
    ros::NodeHandle nh(m_ns);

    // map<string, pcs>
    planning_interface::PlannerConfigurationMap pcm;

    // gather settings for each planner
    PlannerSettingsMap planner_settings_map;
    if (!loadPlannerSettings(planner_settings_map)) {
        ROS_ERROR_NAMED(PP_LOGGER, "Failed to load planner settings");
        return false;
    }

    ROS_DEBUG_NAMED(PP_LOGGER, "Successfully loaded planner settings");

    // TODO: implement defaults for parameters
    const bool DefaultUseCollisionCheckingSBPL = true;
    const bool DefaultUseSnapMprimXYZRPY = false;
    const bool DefaultUseSnapMprimXYZ = false;
    const double DefaultIkMprimDistThresh = 0.2;
    const bool DefaultUseSnapRPY = false;
    const bool DefaultUseMprimsMultiRes = true;
    const double DefaultShortDistMprimsThresh = 0.4;
    const bool DefaultDiscretization = 2.0 * M_PI / 360.0;

    const char* known_group_param_names[] = {
        "discretization",
        "mprim_filename",
        "use_xyz_snap_mprim",
        "use_rpy_snap_mprim",
        "use_xyzrpy_snap_mprim",
        "use_short_dist_mprims",
        "xyz_snap_dist_thresh",
        "rpy_snap_dist_thresh",
        "xyzrpy_snap_dist_thresh",
        "short_dist_mprims_thresh",
        "shortcut_path",
        "shortcut_type",
        "interpolate_path"
    };

    const std::vector<std::string>& joint_group_names =
            model.getJointModelGroupNames();
    for (size_t jind = 0; jind < joint_group_names.size(); ++jind) {
        const std::string& joint_group_name = joint_group_names[jind];
        if (!nh.hasParam(joint_group_name)) {
            ROS_WARN_NAMED(PP_LOGGER, "No planning configuration for joint group '%s'",
                    joint_group_name.c_str());
            continue;
        }

        ROS_DEBUG_NAMED(PP_LOGGER, "Reading configuration for joint group '%s'",
                joint_group_name.c_str());

        XmlRpc::XmlRpcValue joint_group_cfg;
        if (!nh.getParam(joint_group_name, joint_group_cfg)) {
            ROS_ERROR_NAMED(PP_LOGGER, "Failed to retrieve '%s'", joint_group_name.c_str());
            return false;
        }

        if (joint_group_cfg.getType() != XmlRpc::XmlRpcValue::TypeStruct) {
            ROS_ERROR_NAMED(PP_LOGGER, "'%s' should be a map of parameter names to parameter values",
                    joint_group_name.c_str());
            return false;
        }

        ROS_DEBUG_NAMED(PP_LOGGER, "Creating (group, planner) configurations");

        if (joint_group_cfg.hasMember("planner_configs")) {
            XmlRpc::XmlRpcValue& group_planner_configs_cfg =
                    joint_group_cfg["planner_configs"];
            if (group_planner_configs_cfg.getType() !=
                XmlRpc::XmlRpcValue::TypeArray)
            {
                ROS_ERROR_NAMED(PP_LOGGER, "'planner_configs' should be an array of strings (actual: %s)",
                        xmlTypeToString(group_planner_configs_cfg.getType()));
                return false;
            }

            for (int pcind = 0;
                pcind < group_planner_configs_cfg.size();
                ++pcind)
            {
                XmlRpc::XmlRpcValue& group_planner_config =
                        group_planner_configs_cfg[pcind];
                if (group_planner_config.getType() !=
                    XmlRpc::XmlRpcValue::TypeString)
                {
                    ROS_ERROR_NAMED(PP_LOGGER, "group planner config should be the name of a planner config");
                    return false;
                }

                std::string planner_config_name =
                        (std::string)group_planner_configs_cfg[pcind];

                auto it = planner_settings_map.find(planner_config_name);
                if (it == planner_settings_map.end()) {
                    ROS_WARN_NAMED(PP_LOGGER, "No planner settings exist for configuration '%s'",
                            planner_config_name.c_str());
                }
                else {
                    // create a separate group of planner configuration settings for
                    // the joint group with this specific planner
                    planning_interface::PlannerConfigurationSettings pcs;
                    pcs.name =
                            joint_group_name + "[" + planner_config_name + "]";
                    pcs.group = joint_group_name;
                    pcs.config = it->second;
                    pcm[pcs.name] = pcs;
                }
            }
        }

        ROS_DEBUG_NAMED(PP_LOGGER, "Creating group configuration");

        bool found_all = true;
        PlannerSettings known_settings;
        for (size_t pind = 0;
            pind < sizeof(known_group_param_names) / sizeof(const char*);
            ++pind)
        {
            const char* param_name = known_group_param_names[pind];
            if (!joint_group_cfg.hasMember(param_name)) {
                ROS_WARN_NAMED(PP_LOGGER, "Group '%s' lacks parameter '%s'",
                        joint_group_name.c_str(), param_name);
                found_all = false;
                break;
            }

            ROS_DEBUG_NAMED(PP_LOGGER, "Converting parameter '%s' to string representation",
                    param_name);
            XmlRpc::XmlRpcValue& param = joint_group_cfg[param_name];
            if (!xmlToString(param, known_settings[param_name])) {
                ROS_ERROR_NAMED(PP_LOGGER, "Unsupported parameter type");
            }
            else {
                ROS_DEBUG_NAMED(PP_LOGGER, "Converted parameter to '%s'",
                        known_settings[param_name].c_str());
            }
        }

        if (found_all) {
            planning_interface::PlannerConfigurationSettings pcs;
            pcs.name = joint_group_name;
            pcs.group = joint_group_name;
            pcs.config = known_settings;
            pcm[pcs.name] = pcs;
        }
    }

    setPlannerConfigurations(pcm);
    return true;
}

bool SBPLPlannerManager::loadPlannerSettings(PlannerSettingsMap& out)
{
    ros::NodeHandle nh(m_ns);
    if (!nh.hasParam("planner_configs")) {
        return true;
    }

    PlannerSettingsMap planner_configs;

    XmlRpc::XmlRpcValue planner_configs_cfg;
    if (!nh.getParam("planner_configs", planner_configs_cfg)) {
        ROS_ERROR_NAMED(PP_LOGGER, "Failed to retrieve 'planner_configs'");
        return false;
    }

    // planner_configs should be a mapping of planner configuration names to
    // another struct which is a mapping of parameter names (strings) to
    // parameter values (type known per-parameter)
    if (planner_configs_cfg.getType() != XmlRpc::XmlRpcValue::TypeStruct) {
        ROS_ERROR_NAMED(PP_LOGGER, "'planner_configs' section should be a map of planner configuration names to planner configurations (found type '%s')", xmlTypeToString(planner_configs_cfg.getType()));
        return false;
    }

    for (auto it = planner_configs_cfg.begin();
        it != planner_configs_cfg.end();
        ++it)
    {
        const std::string& planner_config_name = it->first;
        XmlRpc::XmlRpcValue& planner_settings_cfg = it->second;

        ROS_DEBUG_NAMED(PP_LOGGER, "Reading configuration for '%s'", planner_config_name.c_str());

        if (planner_settings_cfg.getType() != XmlRpc::XmlRpcValue::TypeStruct) {
            ROS_ERROR_NAMED(PP_LOGGER, "Planner configuration should be a map of parameter names to values");
            return false;
        }

        PlannerSettings planner_settings;
        for (auto iit = planner_settings_cfg.begin();
            iit != planner_settings_cfg.end();
            ++iit)
        {
            const std::string& planner_setting_name = iit->first;
            XmlRpc::XmlRpcValue& planner_setting = iit->second;
            ROS_DEBUG_NAMED(PP_LOGGER, "Reading value for parameter '%s'", planner_setting_name.c_str());
            if (!xmlToString(
                    planner_setting, planner_settings[planner_setting_name]))
            {
                ROS_ERROR_NAMED(PP_LOGGER, "Unsupported parameter type");
            }
            // planner_settings filled if no error above
        }

        planner_configs[planner_config_name] = planner_settings;
    }

    out = std::move(planner_configs);
    return true;
}

bool SBPLPlannerManager::xmlToString(
    XmlRpc::XmlRpcValue& value, std::string& out) const
{
    switch (value.getType()) {
    case XmlRpc::XmlRpcValue::TypeString:
    {
        std::string string_param = (std::string)value;
        out = string_param;
    }   break;
    case XmlRpc::XmlRpcValue::TypeBoolean:
    {
        bool bool_param = (bool)value;
        out = bool_param ? "true" : "false";
    }   break;
    case XmlRpc::XmlRpcValue::TypeInt:
    {
        int int_param = (int)value;
        out = std::to_string(int_param);
    }   break;
    case XmlRpc::XmlRpcValue::TypeDouble:
    {
        double double_param = (double)value;
        out = std::to_string(double_param);
    }   break;
    case XmlRpc::XmlRpcValue::TypeArray:
    {
        std::stringstream ss;
        for (int i = 0; i < value.size(); ++i) {
            XmlRpc::XmlRpcValue& arr_value = value[i];
            switch (arr_value.getType()) {
            case XmlRpc::XmlRpcValue::TypeBoolean:
                ss << (bool)arr_value;
                break;
            case XmlRpc::XmlRpcValue::TypeInt:
                ss << (int)arr_value;
                break;
            case XmlRpc::XmlRpcValue::TypeDouble:
                ss << (double)arr_value;
                break;
            case XmlRpc::XmlRpcValue::TypeString:
                ss << (std::string)arr_value;
                break;
            default:
                ROS_ERROR_NAMED(PP_LOGGER, "Unsupported array member type (%s)", xmlTypeToString(arr_value.getType()));
                return false;
            }

            if (i != value.size() - 1) {
                ss << ' ';
            }
        }
        out = ss.str();
        return true;
    }   break;
    case XmlRpc::XmlRpcValue::TypeStruct:
    {
        std::stringstream ss;
        int i = 0;
        for (auto it = value.begin(); it != value.end(); ++it) {
            ss << it->first << ' ';
            XmlRpc::XmlRpcValue& struct_value = it->second;
            switch (struct_value.getType()) {
            case XmlRpc::XmlRpcValue::TypeBoolean:
                ss << (bool)struct_value;
                break;
            case XmlRpc::XmlRpcValue::TypeInt:
                ss << (int)struct_value;
                break;
            case XmlRpc::XmlRpcValue::TypeDouble:
                ss << (double)struct_value;
                break;
            case XmlRpc::XmlRpcValue::TypeString:
                ss << (std::string)struct_value;
                break;
            default:
                ROS_ERROR_NAMED(PP_LOGGER, "Unsupported struct member type (%s)", xmlTypeToString(struct_value.getType()));
                return false;
            }

            if (i != value.size() - 1) {
                ss << ' ';
            }

            ++i;
        }

        out = ss.str();
        return true;
    }   break;
    default:
        return false;
    }

    return true;
}

MoveItRobotModel* SBPLPlannerManager::getModelForGroup(
    const std::string& group_name)
{
    auto it = m_sbpl_models.find(group_name);
    if (it == m_sbpl_models.end()) {
        auto ent = m_sbpl_models.insert(
                std::make_pair(group_name, std::make_shared<MoveItRobotModel>()));
        assert(ent.second);
        MoveItRobotModel* sbpl_model = ent.first->second.get();
        if (!sbpl_model->init(m_robot_model, group_name)) {
            m_sbpl_models.erase(ent.first);
            ROS_WARN_NAMED(PP_LOGGER, "Failed to initialize SBPL Robot Model");
            return nullptr;
        }

        ROS_INFO_NAMED(PP_LOGGER, "Created SBPL Robot Model for group '%s'", group_name.c_str());
        return sbpl_model;
    }
    else {
        ROS_DEBUG_NAMED(PP_LOGGER, "Using existing SBPL Robot Model for group '%s'", group_name.c_str());
        return it->second.get();
    }
}

} // namespace sbpl_interface

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(
        sbpl_interface::SBPLPlannerManager,
        planning_interface::PlannerManager);

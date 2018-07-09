#ifndef SMPL_URDF_ROBOT_MODEL_URDF_ROBOT_MODEL_H
#define SMPL_URDF_ROBOT_MODEL_URDF_ROBOT_MODEL_H

// system includes
#include <smpl/robot_model.h>

// project includes
#include <smpl_urdf_robot_model/robot_model.h>
#include <smpl_urdf_robot_model/robot_state.h>

namespace smpl {

struct URDFRobotModel :
    public virtual sbpl::motion::RobotModel,
    public virtual sbpl::motion::ForwardKinematicsInterface
{
    struct VariableProperties
    {
        double min_position;
        double max_position;
        double vel_limit;
        double acc_limit;
        bool continuous;
        bool bounded;
    };

    const ::smpl::RobotModel* robot_model = NULL;
    RobotState robot_state;
    std::vector<VariableProperties> vprops;
    std::vector<int> planning_to_state_variable;
    const Link* planning_link = NULL;

#if 0
    const Link* kinematics_frame = NULL;
#endif

    auto computeFK(const sbpl::motion::RobotState& state) -> Eigen::Affine3d override;

    double minPosLimit(int jidx) const override;
    double maxPosLimit(int jidx) const override;
    bool hasPosLimit(int jidx) const override;
    bool isContinuous(int jidx) const override;
    double velLimit(int jidx) const override;
    double accLimit(int jidx) const override;
    bool checkJointLimits(
        const sbpl::motion::RobotState& state,
        bool verbose = false) override;

    auto getExtension(size_t class_code) -> sbpl::motion::Extension* override;
};

bool Init(
    URDFRobotModel* urdf_model,
    const RobotModel* robot_model,
    const std::vector<std::string>* planning_joint_names);
bool Init(
    URDFRobotModel* urdf_model,
    const RobotModel* robot_model,
    const std::vector<const Joint*>* planning_joints);

bool SetPlanningLink(URDFRobotModel* urdf_model, const std::string* link_name);
bool SetPlanningLink(URDFRobotModel* urdf_model, const Link* link);

} // namespace smpl

#endif


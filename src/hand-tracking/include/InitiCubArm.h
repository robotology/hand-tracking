#ifndef INITICUBARM_H
#define INITICUBARM_H

#include <InitPoseParticles.h>

#include <iCub/iKin/iKinFwd.h>
#include <yarp/os/Bottle.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/os/ConstString.h>
#include <yarp/sig/Vector.h>


class InitiCubArm : public InitPoseParticles
{
public:
    InitiCubArm(const yarp::os::ConstString& cam_sel, const yarp::os::ConstString& laterality,
                const yarp::os::ConstString& port_prefix) noexcept;

    InitiCubArm(const yarp::os::ConstString& cam_sel, const yarp::os::ConstString& laterality) noexcept;

    ~InitiCubArm() noexcept;

protected:
    Eigen::VectorXd readPose() override;

private:
    const yarp::os::ConstString  log_ID_ = "[InitiCubArm]";

    yarp::os::ConstString port_prefix_;

    iCub::iKin::iCubArm                      icub_kin_arm_;

    yarp::os::BufferedPort<yarp::os::Bottle> port_torso_enc_;

    yarp::os::BufferedPort<yarp::os::Bottle> port_arm_enc_;

    yarp::sig::Vector                        readTorso();

    yarp::sig::Vector                        readRootToEE();
};

#endif /* INITICUBARM_H */

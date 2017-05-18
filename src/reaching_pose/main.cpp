#include <cmath>
#include <iostream>

#include <iCub/ctrl/minJerkCtrl.h>
#include <iCub/iKin/iKinFwd.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <yarp/dev/CartesianControl.h>
#include <yarp/dev/GazeControl.h>
#include <yarp/dev/PolyDriver.h>
#include <yarp/math/Math.h>
#include <yarp/math/SVD.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/os/ConstString.h>
#include <yarp/os/LogStream.h>
#include <yarp/os/Network.h>
#include <yarp/os/Property.h>
#include <yarp/os/RFModule.h>
#include <yarp/os/RpcClient.h>
#include <yarp/os/Time.h>
#include <yarp/sig/Image.h>
#include <yarp/sig/Matrix.h>
#include <yarp/sig/Vector.h>

using namespace yarp::dev;
using namespace yarp::math;
using namespace yarp::os;
using namespace yarp::sig;
using namespace iCub::ctrl;
using namespace iCub::iKin;


class RFMReaching : public RFModule
{
public:
    bool configure(ResourceFinder &rf)
    {
        robot_name_ = rf.find("robot").asString();
        if (robot_name_.empty())
        {
            yError() << "Robot name not provided! Closing.";
            return false;
        }

        if (!port_estimates_left_in_.open("/reaching_pose/estimates/left:i"))
        {
            yError() << "Could not open /reaching_pose/estimates/left:i port! Closing.";
            return false;
        }

        if (!port_estimates_right_in_.open("/reaching_pose/estimates/right:i"))
        {
            yError() << "Could not open /reaching_pose/estimates/right:i port! Closing.";
            return false;
        }

        if (!port_image_left_in_.open("/reaching_pose/cam_left/img:i"))
        {
            yError() << "Could not open /reaching_pose/cam_left/img:i port! Closing.";
            return false;
        }

        if (!port_image_left_out_.open("/reaching_pose/cam_left/img:o"))
        {
            yError() << "Could not open /reaching_pose/cam_left/img:o port! Closing.";
            return false;
        }

        if (!port_click_left_.open("/reaching_pose/cam_left/click:i"))
        {
            yError() << "Could not open /reaching_pose/cam_left/click:in port! Closing.";
            return false;
        }

        if (!port_image_right_in_.open("/reaching_pose/cam_right/img:i"))
        {
            yError() << "Could not open /reaching_pose/cam_right/img:i port! Closing.";
            return false;
        }

        if (!port_image_right_out_.open("/reaching_pose/cam_right/img:o"))
        {
            yError() << "Could not open /reaching_pose/cam_right/img:o port! Closing.";
            return false;
        }

        if (!port_click_right_.open("/reaching_pose/cam_right/click:i"))
        {
            yError() << "Could not open /reaching_pose/cam_right/click:i port! Closing.";
            return false;
        }

        if (!setGazeController()) return false;

        if (!setTorsoRemoteControlboard()) return false;

        if (!setRightArmRemoteControlboard()) return false;

        if (!setRightArmCartesianController()) return false;

        Bottle btl_cam_info;
        itf_gaze_->getInfo(btl_cam_info);
        yInfo() << "[CAM INFO]" << btl_cam_info.toString();
        Bottle* cam_left_info = btl_cam_info.findGroup("camera_intrinsics_left").get(1).asList();
        Bottle* cam_right_info = btl_cam_info.findGroup("camera_intrinsics_right").get(1).asList();

        float left_fx  = static_cast<float>(cam_left_info->get(0).asDouble());
        float left_cx  = static_cast<float>(cam_left_info->get(2).asDouble());
        float left_fy  = static_cast<float>(cam_left_info->get(5).asDouble());
        float left_cy  = static_cast<float>(cam_left_info->get(6).asDouble());

        yInfo() << "[CAM]" << "Left camera:";
        yInfo() << "[CAM]" << " - fx:"     << left_fx;
        yInfo() << "[CAM]" << " - fy:"     << left_fy;
        yInfo() << "[CAM]" << " - cx:"     << left_cx;
        yInfo() << "[CAM]" << " - cy:"     << left_cy;

        l_proj_ = zeros(3, 4);
        l_proj_(0, 0)  = left_fx;
        l_proj_(0, 2)  = left_cx;
        l_proj_(1, 1)  = left_fy;
        l_proj_(1, 2)  = left_cy;
        l_proj_(2, 2)  = 1.0;

        yInfo() << "l_proj_ =\n" << l_proj_.toString();

        float right_fx = static_cast<float>(cam_right_info->get(0).asDouble());
        float right_cx = static_cast<float>(cam_right_info->get(2).asDouble());
        float right_fy = static_cast<float>(cam_right_info->get(5).asDouble());
        float right_cy = static_cast<float>(cam_right_info->get(6).asDouble());

        yInfo() << "[CAM]" << "Right camera:";
        yInfo() << "[CAM]" << " - fx:"     << right_fx;
        yInfo() << "[CAM]" << " - fy:"     << right_fy;
        yInfo() << "[CAM]" << " - cx:"     << right_cx;
        yInfo() << "[CAM]" << " - cy:"     << right_cy;

        r_proj_ = zeros(3, 4);
        r_proj_(0, 0)  = right_fx;
        r_proj_(0, 2)  = right_cx;
        r_proj_(1, 1)  = right_fy;
        r_proj_(1, 2)  = right_cy;
        r_proj_(2, 2)  = 1.0;

        yInfo() << "r_proj_ =\n" << r_proj_.toString();


        Vector left_eye_x;
        Vector left_eye_o;
        itf_gaze_->getLeftEyePose(left_eye_x, left_eye_o);

        Vector right_eye_x;
        Vector right_eye_o;
        itf_gaze_->getRightEyePose(right_eye_x, right_eye_o);

        yInfo() << "left_eye_o =" << left_eye_o.toString();
        yInfo() << "left_eye_x =" << left_eye_x.toString();
        yInfo() << "right_eye_o =" << right_eye_o.toString();
        yInfo() << "right_eye_x =" << right_eye_x.toString();


        l_H_eye_to_r_ = axis2dcm(left_eye_o);
        left_eye_x.push_back(1.0);
        l_H_eye_to_r_.setCol(3, left_eye_x);
        l_H_r_to_eye_ = SE3inv(l_H_eye_to_r_);

        r_H_eye_to_r_ = axis2dcm(right_eye_o);
        right_eye_x.push_back(1.0);
        r_H_eye_to_r_.setCol(3, right_eye_x);
        r_H_r_to_eye_ = SE3inv(r_H_eye_to_r_);

        yInfo() << "l_H_r_to_eye_ =\n" << l_H_r_to_eye_.toString();
        yInfo() << "r_H_r_to_eye_ =\n" << r_H_r_to_eye_.toString();

        l_H_r_to_cam_ = l_proj_ * l_H_r_to_eye_;
        r_H_r_to_cam_ = r_proj_ * r_H_r_to_eye_;

        yInfo() << "l_H_r_to_cam_ =\n" << l_H_r_to_cam_.toString();
        yInfo() << "r_H_r_to_cam_ =\n" << r_H_r_to_cam_.toString();


        icub_index_ = iCubFinger("right_index");
        std::deque<IControlLimits*> temp_lim;
        temp_lim.push_front(itf_fingers_lim_);
        if (!icub_index_.alignJointsBounds(temp_lim))
        {
            yError() << "Cannot set joint bound for index finger.";
            return false;
        }

        handler_port_.open("/reaching_pose/cmd:i");
        attach(handler_port_);

        return true;
    }


    double getPeriod() { return 0; }


    bool updateModule()
    {
        while (!take_estimates_);

        if (should_stop_) return false;


        Vector est_copy_left(6);
        Vector est_copy_right(6);

        /* Get the initial end-effector pose from left eye particle filter */
        Vector* estimates = port_estimates_left_in_.read(true);
        yInfo() << "Got [" << estimates->toString() << "] from left eye particle filter.";
        if (estimates->length() == 7)
        {
            est_copy_left = estimates->subVector(0, 5);
            float ang     = (*estimates)[6];

            est_copy_left[3] *= ang;
            est_copy_left[4] *= ang;
            est_copy_left[5] *= ang;
        }
        else
            est_copy_left = *estimates;

        /* Get the initial end-effector pose from right eye particle filter */
        estimates = port_estimates_right_in_.read(true);
        yInfo() << "Got [" << estimates->toString() << "] from right eye particle filter.";
        if (estimates->length() == 7)
        {
            est_copy_right = estimates->subVector(0, 5);
            float ang      = (*estimates)[6];

            est_copy_right[3] *= ang;
            est_copy_right[4] *= ang;
            est_copy_right[5] *= ang;
        }
        else
            est_copy_left = *estimates;


        yInfo() << "RUNNING!\n";

        yInfo() << "EE estimates left = ["  << est_copy_left.toString() << "]";
        yInfo() << "EE estimates right = ["  << est_copy_right.toString() << "]";
        yInfo() << "l_px_goal_ = [" << l_px_goal_.toString() << "]";
        yInfo() << "r_px_goal_ = [" << r_px_goal_.toString() << "]";

        Vector px_des;
        px_des.push_back(l_px_goal_[0]);    /* u_ee_l */
        px_des.push_back(r_px_goal_[0]);    /* u_ee_r */
        px_des.push_back(l_px_goal_[1]);    /* v_ee_l */

        px_des.push_back(l_px_goal_[2]);    /* u_x1_l */
        px_des.push_back(r_px_goal_[2]);    /* u_x1_r */
        px_des.push_back(l_px_goal_[3]);    /* v_x1_l */

        px_des.push_back(l_px_goal_[4]);    /* u_x2_l */
        px_des.push_back(r_px_goal_[4]);    /* u_x2_r */
        px_des.push_back(l_px_goal_[5]);    /* v_x2_l */

        px_des.push_back(l_px_goal_[6]);    /* u_x3_l */
        px_des.push_back(r_px_goal_[6]);    /* u_x3_r */
        px_des.push_back(l_px_goal_[7]);    /* v_x3_l */

        yInfo() << "px_des = ["  << px_des.toString() << "]";


        /* LEFT: ORIENTATION = GOAL */
        Vector l_est_position = est_copy_left;
        l_est_position.setSubvector(3, goal_pose_.subVector(3, 5));

        Vector l_ee_position_x0 = zeros(4);
        Vector l_ee_position_x1 = zeros(4);
        Vector l_ee_position_x2 = zeros(4);
        Vector l_ee_position_x3 = zeros(4);
        getPalmPoints(l_est_position, l_ee_position_x0, l_ee_position_x1, l_ee_position_x2, l_ee_position_x3);

        /* LEFT: POSITION = GOAL */
        Vector l_est_orientation = est_copy_left;
        l_est_orientation.setSubvector(0, goal_pose_.subVector(0, 2));

        Vector l_ee_orientation_x0 = zeros(4);
        Vector l_ee_orientation_x1 = zeros(4);
        Vector l_ee_orientation_x2 = zeros(4);
        Vector l_ee_orientation_x3 = zeros(4);
        getPalmPoints(l_est_orientation, l_ee_orientation_x0, l_ee_orientation_x1, l_ee_orientation_x2, l_ee_orientation_x3);


        /* LEFT: EVALUATE CORRESPONDING ORIENTATION = GOAL PIXELS */
        Vector l_px0_position = l_H_r_to_cam_ * l_ee_position_x0;
        l_px0_position[0] /= l_px0_position[2];
        l_px0_position[1] /= l_px0_position[2];
        Vector l_px1_position = l_H_r_to_cam_ * l_ee_position_x1;
        l_px1_position[0] /= l_px1_position[2];
        l_px1_position[1] /= l_px1_position[2];
        Vector l_px2_position = l_H_r_to_cam_ * l_ee_position_x2;
        l_px2_position[0] /= l_px2_position[2];
        l_px2_position[1] /= l_px2_position[2];
        Vector l_px3_position = l_H_r_to_cam_ * l_ee_position_x3;
        l_px3_position[0] /= l_px3_position[2];
        l_px3_position[1] /= l_px3_position[2];
        yInfo() << "Left POSITION ee    = [" << l_px0_position.subVector(0, 1).toString() << "]";
        yInfo() << "Left POSITION ee x1 = [" << l_px1_position.subVector(0, 1).toString() << "]";
        yInfo() << "Left POSITION ee x2 = [" << l_px2_position.subVector(0, 1).toString() << "]";
        yInfo() << "Left POSITION ee x3 = [" << l_px3_position.subVector(0, 1).toString() << "]";

        /* LEFT: EVALUATE CORRESPONDING POSITION = GOAL PIXELS */
        Vector l_px0_orientation = l_H_r_to_cam_ * l_ee_orientation_x0;
        l_px0_orientation[0] /= l_px0_orientation[2];
        l_px0_orientation[1] /= l_px0_orientation[2];
        Vector l_px1_orientation = l_H_r_to_cam_ * l_ee_orientation_x1;
        l_px1_orientation[0] /= l_px1_orientation[2];
        l_px1_orientation[1] /= l_px1_orientation[2];
        Vector l_px2_orientation = l_H_r_to_cam_ * l_ee_orientation_x2;
        l_px2_orientation[0] /= l_px2_orientation[2];
        l_px2_orientation[1] /= l_px2_orientation[2];
        Vector l_px3_orientation = l_H_r_to_cam_ * l_ee_orientation_x3;
        l_px3_orientation[0] /= l_px3_orientation[2];
        l_px3_orientation[1] /= l_px3_orientation[2];
        yInfo() << "Left ORIENTATION ee    = [" << l_px0_orientation.subVector(0, 1).toString() << "]";
        yInfo() << "Left ORIENTATION ee x1 = [" << l_px1_orientation.subVector(0, 1).toString() << "]";
        yInfo() << "Left ORIENTATION ee x2 = [" << l_px2_orientation.subVector(0, 1).toString() << "]";
        yInfo() << "Left ORIENTATION ee x3 = [" << l_px3_orientation.subVector(0, 1).toString() << "]";


        /* RIGHT: ORIENTATION = GOAL */
        Vector r_est_position = est_copy_right;
        r_est_position.setSubvector(3, goal_pose_.subVector(3, 5));

        Vector r_ee_position_x0 = zeros(4);
        Vector r_ee_position_x1 = zeros(4);
        Vector r_ee_position_x2 = zeros(4);
        Vector r_ee_position_x3 = zeros(4);
        getPalmPoints(r_est_position, r_ee_position_x0, r_ee_position_x1, r_ee_position_x2, r_ee_position_x3);

        /* RIGHT: POSITION = GOAL */
        Vector r_est_orientation = est_copy_right;
        r_est_orientation.setSubvector(0, goal_pose_.subVector(0, 2));

        Vector r_ee_orientation_x0 = zeros(4);
        Vector r_ee_orientation_x1 = zeros(4);
        Vector r_ee_orientation_x2 = zeros(4);
        Vector r_ee_orientation_x3 = zeros(4);
        getPalmPoints(r_est_orientation, r_ee_orientation_x0, r_ee_orientation_x1, r_ee_orientation_x2, r_ee_orientation_x3);


        /* RIGHT: EVALUATE CORRESPONDING ORIENTATION = GOAL PIXELS */
        Vector r_px0_position = r_H_r_to_cam_ * r_ee_position_x0;
        r_px0_position[0] /= r_px0_position[2];
        r_px0_position[1] /= r_px0_position[2];
        Vector r_px1_position = r_H_r_to_cam_ * r_ee_position_x1;
        r_px1_position[0] /= r_px1_position[2];
        r_px1_position[1] /= r_px1_position[2];
        Vector r_px2_position = r_H_r_to_cam_ * r_ee_position_x2;
        r_px2_position[0] /= r_px2_position[2];
        r_px2_position[1] /= r_px2_position[2];
        Vector r_px3_position = r_H_r_to_cam_ * r_ee_position_x3;
        r_px3_position[0] /= r_px3_position[2];
        r_px3_position[1] /= r_px3_position[2];
        yInfo() << "Right POSITION ee    = [" << r_px0_position.subVector(0, 1).toString() << "]";
        yInfo() << "Right POSITION ee x1 = [" << r_px1_position.subVector(0, 1).toString() << "]";
        yInfo() << "Right POSITION ee x2 = [" << r_px2_position.subVector(0, 1).toString() << "]";
        yInfo() << "Right POSITION ee x3 = [" << r_px3_position.subVector(0, 1).toString() << "]";

        /* RIGHT: EVALUATE CORRESPONDING POSITION = GOAL PIXELS */
        Vector r_px0_orientation = r_H_r_to_cam_ * r_ee_orientation_x0;
        r_px0_orientation[0] /= r_px0_orientation[2];
        r_px0_orientation[1] /= r_px0_orientation[2];
        Vector r_px1_orientation = r_H_r_to_cam_ * r_ee_orientation_x1;
        r_px1_orientation[0] /= r_px1_orientation[2];
        r_px1_orientation[1] /= r_px1_orientation[2];
        Vector r_px2_orientation = r_H_r_to_cam_ * r_ee_orientation_x2;
        r_px2_orientation[0] /= r_px2_orientation[2];
        r_px2_orientation[1] /= r_px2_orientation[2];
        Vector r_px3_orientation = r_H_r_to_cam_ * r_ee_orientation_x3;
        r_px3_orientation[0] /= r_px3_orientation[2];
        r_px3_orientation[1] /= r_px3_orientation[2];
        yInfo() << "Right ORIENTATION ee    = [" << r_px0_orientation.subVector(0, 1).toString() << "]";
        yInfo() << "Right ORIENTATION ee x1 = [" << r_px1_orientation.subVector(0, 1).toString() << "]";
        yInfo() << "Right ORIENTATION ee x2 = [" << r_px2_orientation.subVector(0, 1).toString() << "]";
        yInfo() << "Right ORIENTATION ee x3 = [" << r_px3_orientation.subVector(0, 1).toString() << "]";


        /* JACOBIAN: ORIENTATION = GOAL */
        Vector px_ee_cur_position;

        px_ee_cur_position.push_back(l_px0_position[0]);  /* u_ee_l */
        px_ee_cur_position.push_back(r_px0_position[0]);  /* u_ee_r */
        px_ee_cur_position.push_back(l_px0_position[1]);  /* v_ee_l */

        px_ee_cur_position.push_back(l_px1_position[0]);  /* u_x1_l */
        px_ee_cur_position.push_back(r_px1_position[0]);  /* u_x1_r */
        px_ee_cur_position.push_back(l_px1_position[1]);  /* v_x1_l */

        px_ee_cur_position.push_back(l_px2_position[0]);  /* u_x2_l */
        px_ee_cur_position.push_back(r_px2_position[0]);  /* u_x2_r */
        px_ee_cur_position.push_back(l_px2_position[1]);  /* v_x2_l */

        px_ee_cur_position.push_back(l_px3_position[0]);  /* u_x3_l */
        px_ee_cur_position.push_back(r_px3_position[0]);  /* u_x3_r */
        px_ee_cur_position.push_back(l_px3_position[1]);  /* v_x3_l */

        yInfo() << "px_ee_cur_position = [" << px_ee_cur_position.toString() << "]";

        Matrix jacobian_position = zeros(12, 6);

        /* Point 0 */
        jacobian_position.setRow(0,  setJacobianU(LEFT,  l_px0_position));
        jacobian_position.setRow(1,  setJacobianU(RIGHT, r_px0_position));
        jacobian_position.setRow(2,  setJacobianV(LEFT,  l_px0_position));

        /* Point 1 */
        jacobian_position.setRow(3,  setJacobianU(LEFT,  l_px1_position));
        jacobian_position.setRow(4,  setJacobianU(RIGHT, r_px1_position));
        jacobian_position.setRow(5,  setJacobianV(LEFT,  l_px1_position));

        /* Point 2 */
        jacobian_position.setRow(6,  setJacobianU(LEFT,  l_px2_position));
        jacobian_position.setRow(7,  setJacobianU(RIGHT, r_px2_position));
        jacobian_position.setRow(8,  setJacobianV(LEFT,  l_px2_position));

        /* Point 3 */
        jacobian_position.setRow(9,  setJacobianU(LEFT,  l_px3_position));
        jacobian_position.setRow(10, setJacobianU(RIGHT, r_px3_position));
        jacobian_position.setRow(11, setJacobianV(LEFT,  l_px3_position));
        /* ******** */


        /* JACOBIAN: POSITION = GOAL */
        Vector px_ee_cur_orientation;

        px_ee_cur_orientation.push_back(l_px0_orientation[0]);  /* u_ee_l */
        px_ee_cur_orientation.push_back(r_px0_orientation[0]);  /* u_ee_r */
        px_ee_cur_orientation.push_back(l_px0_orientation[1]);  /* v_ee_l */

        px_ee_cur_orientation.push_back(l_px1_orientation[0]);  /* u_x1_l */
        px_ee_cur_orientation.push_back(r_px1_orientation[0]);  /* u_x1_r */
        px_ee_cur_orientation.push_back(l_px1_orientation[1]);  /* v_x1_l */

        px_ee_cur_orientation.push_back(l_px2_orientation[0]);  /* u_x2_l */
        px_ee_cur_orientation.push_back(r_px2_orientation[0]);  /* u_x2_r */
        px_ee_cur_orientation.push_back(l_px2_orientation[1]);  /* v_x2_l */

        px_ee_cur_orientation.push_back(l_px3_orientation[0]);  /* u_x3_l */
        px_ee_cur_orientation.push_back(r_px3_orientation[0]);  /* u_x3_r */
        px_ee_cur_orientation.push_back(l_px3_orientation[1]);  /* v_x3_l */

        yInfo() << "px_ee_cur_orientation = [" << px_ee_cur_orientation.toString() << "]";

        Matrix jacobian_orientation = zeros(12, 6);

        /* Point 0 */
        jacobian_orientation.setRow(0,  setJacobianU(LEFT,  l_px0_orientation));
        jacobian_orientation.setRow(1,  setJacobianU(RIGHT, r_px0_orientation));
        jacobian_orientation.setRow(2,  setJacobianV(LEFT,  l_px0_orientation));

        /* Point 1 */
        jacobian_orientation.setRow(3,  setJacobianU(LEFT,  l_px1_orientation));
        jacobian_orientation.setRow(4,  setJacobianU(RIGHT, r_px1_orientation));
        jacobian_orientation.setRow(5,  setJacobianV(LEFT,  l_px1_orientation));

        /* Point 2 */
        jacobian_orientation.setRow(6,  setJacobianU(LEFT,  l_px2_orientation));
        jacobian_orientation.setRow(7,  setJacobianU(RIGHT, r_px2_orientation));
        jacobian_orientation.setRow(8,  setJacobianV(LEFT,  l_px2_orientation));

        /* Point 3 */
        jacobian_orientation.setRow(9,  setJacobianU(LEFT,  l_px3_orientation));
        jacobian_orientation.setRow(10, setJacobianU(RIGHT, r_px3_orientation));
        jacobian_orientation.setRow(11, setJacobianV(LEFT,  l_px3_orientation));
        /* ******** */


        /* Restoring cartesian and gaze context */
        itf_rightarm_cart_->restoreContext(ctx_cart_);
        itf_gaze_->restoreContext(ctx_gaze_);


        double Ts    = 0.1;   // controller's sample time [s]
        double K_x   = 0.5;  // visual servoing proportional gain
        double K_o   = 0.5;  // visual servoing proportional gain
//        double v_max = 0.0005; // max cartesian velocity [m/s]

        bool done = false;
        while (!should_stop_ && !done)
        {
            Vector e_position               = px_des - px_ee_cur_position;
            Matrix inv_jacobian_position    = pinv(jacobian_position);

            Vector e_orientation            = px_des - px_ee_cur_orientation;
            Matrix inv_jacobian_orientation = pinv(jacobian_orientation);


            Vector vel_x = zeros(3);
            Vector vel_o = zeros(3);
            for (int i = 0; i < inv_jacobian_position.cols(); ++i)
            {
                Vector delta_vel_position    = inv_jacobian_position.getCol(i)    * e_position(i);
                Vector delta_vel_orientation = inv_jacobian_orientation.getCol(i) * e_orientation(i);

                if (i == 1 || i == 4 || i == 7 || i == 10)
                {
                    vel_x += r_H_eye_to_r_.submatrix(0, 2, 0, 2) * delta_vel_position.subVector(0, 2);
                    vel_o += r_H_eye_to_r_.submatrix(0, 2, 0, 2) * delta_vel_orientation.subVector(3, 5);
                }
                else
                {
                    vel_x += l_H_eye_to_r_.submatrix(0, 2, 0, 2) * delta_vel_position.subVector(0, 2);
                    vel_o += l_H_eye_to_r_.submatrix(0, 2, 0, 2) * delta_vel_orientation.subVector(3, 5);
                }
            }


            yInfo() << "px_des = ["             << px_des.toString()             << "]";
            yInfo() << "px_ee_cur_position = [" << px_ee_cur_position.toString() << "]";
            yInfo() << "e_position = ["         << e_position.toString()         << "]";
            yInfo() << "e_orientation = ["      << e_orientation.toString()      << "]";
            yInfo() << "vel_x = ["              << vel_x.toString()              << "]";
            yInfo() << "vel_o = ["              << vel_o.toString()              << "]";

            /* Enforce velocity bounds */
//            for (size_t i = 0; i < vel_x.length(); ++i)
//            {
//                vel_x[i] = sign(vel_x[i]) * std::min(v_max, std::fabs(vel_x[i]));
//            }

            yInfo() << "bounded vel_x = [" << vel_x.toString() << "]";

            double ang = norm(vel_o);
            vel_o /= ang;
            vel_o.push_back(ang);
            yInfo() << "axis-angle vel_o = [" << vel_o.toString() << "]";

            /* Visual control law */
            /* SIM */
            vel_x    *= K_x;
            vel_o(3) *= K_o;
            /* Real robot - Pose */
            itf_rightarm_cart_->setTaskVelocities(vel_x, vel_o);
            /* Real robot - Orientation */
//            itf_rightarm_cart_->setTaskVelocities(Vector(3, 0.0), vel_o);
            /* Real robot - Translation */
//            itf_rightarm_cart_->setTaskVelocities(vel_x, Vector(4, 0.0));

            yInfo() << "Pixel errors: " << std::abs(px_des(0) - px_ee_cur_position(0)) << std::abs(px_des(1)  - px_ee_cur_position(1))  << std::abs(px_des(2)  - px_ee_cur_position(2))
                                        << std::abs(px_des(3) - px_ee_cur_position(3)) << std::abs(px_des(4)  - px_ee_cur_position(4))  << std::abs(px_des(5)  - px_ee_cur_position(5))
                                        << std::abs(px_des(6) - px_ee_cur_position(6)) << std::abs(px_des(7)  - px_ee_cur_position(7))  << std::abs(px_des(8)  - px_ee_cur_position(8))
                                        << std::abs(px_des(9) - px_ee_cur_position(9)) << std::abs(px_des(10) - px_ee_cur_position(10)) << std::abs(px_des(11) - px_ee_cur_position(11));

            Time::delay(Ts);

            done = ((std::abs(px_des(0) - px_ee_cur_position(0)) < 5.0)    && (std::abs(px_des(1)  - px_ee_cur_position(1))  < 5.0)    && (std::abs(px_des(2)  - px_ee_cur_position(2))  < 5.0)    &&
                    (std::abs(px_des(3) - px_ee_cur_position(3)) < 5.0)    && (std::abs(px_des(4)  - px_ee_cur_position(4))  < 5.0)    && (std::abs(px_des(5)  - px_ee_cur_position(5))  < 5.0)    &&
                    (std::abs(px_des(6) - px_ee_cur_position(6)) < 5.0)    && (std::abs(px_des(7)  - px_ee_cur_position(7))  < 5.0)    && (std::abs(px_des(8)  - px_ee_cur_position(8))  < 5.0)    &&
                    (std::abs(px_des(9) - px_ee_cur_position(9)) < 5.0)    && (std::abs(px_des(10) - px_ee_cur_position(10)) < 5.0)    && (std::abs(px_des(11) - px_ee_cur_position(11)) < 5.0)    &&
                    (std::abs(px_des(0) - px_ee_cur_orientation(0)) < 5.0) && (std::abs(px_des(1)  - px_ee_cur_orientation(1))  < 5.0) && (std::abs(px_des(2)  - px_ee_cur_orientation(2))  < 5.0) &&
                    (std::abs(px_des(3) - px_ee_cur_orientation(3)) < 5.0) && (std::abs(px_des(4)  - px_ee_cur_orientation(4))  < 5.0) && (std::abs(px_des(5)  - px_ee_cur_orientation(5))  < 5.0) &&
                    (std::abs(px_des(6) - px_ee_cur_orientation(6)) < 5.0) && (std::abs(px_des(7)  - px_ee_cur_orientation(7))  < 5.0) && (std::abs(px_des(8)  - px_ee_cur_orientation(8))  < 5.0) &&
                    (std::abs(px_des(9) - px_ee_cur_orientation(9)) < 5.0) && (std::abs(px_des(10) - px_ee_cur_orientation(10)) < 5.0) && (std::abs(px_des(11) - px_ee_cur_orientation(11)) < 5.0));
            if (done)
            {
                yInfo() << "\npx_des ="              << px_des.toString();
                yInfo() << "px_ee_cur_position ="    << px_ee_cur_position.toString();
                yInfo() << "px_ee_cur_orientation =" << px_ee_cur_orientation.toString();
                yInfo() << "\nTERMINATING!\n";
            }
            else
            {
                /* Get the new end-effector pose from left eye particle filter */
                estimates = port_estimates_left_in_.read(true);
                yInfo() << "Got [" << estimates->toString() << "] from left eye particle filter.";
                if (estimates->length() == 7)
                {
                    est_copy_left = estimates->subVector(0, 5);
                    float ang     = (*estimates)[6];

                    est_copy_left[3] *= ang;
                    est_copy_left[4] *= ang;
                    est_copy_left[5] *= ang;
                }
                else
                    est_copy_left = *estimates;

                /* Get the new end-effector pose from right eye particle filter */
                yInfo() << "Got [" << estimates->toString() << "] from right eye particle filter.";
                estimates = port_estimates_right_in_.read(true);
                if (estimates->length() == 7)
                {
                    est_copy_right = estimates->subVector(0, 5);
                    float ang      = (*estimates)[6];

                    est_copy_right[3] *= ang;
                    est_copy_right[4] *= ang;
                    est_copy_right[5] *= ang;
                }
                else
                    est_copy_left = *estimates;

                yInfo() << "EE estimates left = [" << est_copy_left.toString() << "]";
                yInfo() << "EE estimates right = [" << est_copy_right.toString() << "]\n";

                /* SIM */
//                /* Simulate reaching starting from the initial position */
//                /* Comment any previous write on variable 'estimates' */
//
//                /* Evaluate the new orientation vector from axis-angle representation */
//                /* The following code is a copy of the setTaskVelocities() code */
//                Vector l_o = getAxisAngle(est_copy_left.subVector(3, 5));
//                Matrix l_R = axis2dcm(l_o);
//                Vector r_o = getAxisAngle(est_copy_right.subVector(3, 5));
//                Matrix r_R = axis2dcm(r_o);
//
//                vel_o[3] *= Ts;
//                l_R = axis2dcm(vel_o) * l_R;
//                r_R = axis2dcm(vel_o) * r_R;
//
//                Vector l_new_o = dcm2axis(l_R);
//                double l_ang = l_new_o(3);
//                l_new_o.pop_back();
//                l_new_o *= l_ang;
//
//                Vector r_new_o = dcm2axis(r_R);
//                double r_ang = r_new_o(3);
//                r_new_o.pop_back();
//                r_new_o *= r_ang;
//
//                est_copy_left.setSubvector(0, est_copy_left.subVector(0, 2)  + vel_x * Ts);
//                est_copy_left.setSubvector(3, l_new_o);
//                est_copy_right.setSubvector(0, est_copy_right.subVector(0, 2)  + vel_x * Ts);
//                est_copy_right.setSubvector(3, r_new_o);
                /* **************************************************** */


                /* LEFT: ORIENTATION = GOAL */
                l_est_position = est_copy_left;
                l_est_position.setSubvector(3, goal_pose_.subVector(3, 5));

                l_ee_position_x0 = zeros(4);
                l_ee_position_x1 = zeros(4);
                l_ee_position_x2 = zeros(4);
                l_ee_position_x3 = zeros(4);
                getPalmPoints(l_est_position, l_ee_position_x0, l_ee_position_x1, l_ee_position_x2, l_ee_position_x3);

                /* LEFT: POSITION = GOAL */
                l_est_orientation = est_copy_left;
                l_est_orientation.setSubvector(0, goal_pose_.subVector(0, 2));

                l_ee_orientation_x0 = zeros(4);
                l_ee_orientation_x1 = zeros(4);
                l_ee_orientation_x2 = zeros(4);
                l_ee_orientation_x3 = zeros(4);
                getPalmPoints(l_est_orientation, l_ee_orientation_x0, l_ee_orientation_x1, l_ee_orientation_x2, l_ee_orientation_x3);


                /* LEFT: EVALUATE CORRESPONDING ORIENTATION = GOAL PIXELS */
                l_px0_position = l_H_r_to_cam_ * l_ee_position_x0;
                l_px0_position[0] /= l_px0_position[2];
                l_px0_position[1] /= l_px0_position[2];
                l_px1_position = l_H_r_to_cam_ * l_ee_position_x1;
                l_px1_position[0] /= l_px1_position[2];
                l_px1_position[1] /= l_px1_position[2];
                l_px2_position = l_H_r_to_cam_ * l_ee_position_x2;
                l_px2_position[0] /= l_px2_position[2];
                l_px2_position[1] /= l_px2_position[2];
                l_px3_position = l_H_r_to_cam_ * l_ee_position_x3;
                l_px3_position[0] /= l_px3_position[2];
                l_px3_position[1] /= l_px3_position[2];
                yInfo() << "Left POSITION ee    = [" << l_px0_position.subVector(0, 1).toString() << "]";
                yInfo() << "Left POSITION ee x1 = [" << l_px1_position.subVector(0, 1).toString() << "]";
                yInfo() << "Left POSITION ee x2 = [" << l_px2_position.subVector(0, 1).toString() << "]";
                yInfo() << "Left POSITION ee x3 = [" << l_px3_position.subVector(0, 1).toString() << "]";

                /* LEFT: EVALUATE CORRESPONDING POSITION = GOAL PIXELS */
                l_px0_orientation = l_H_r_to_cam_ * l_ee_orientation_x0;
                l_px0_orientation[0] /= l_px0_orientation[2];
                l_px0_orientation[1] /= l_px0_orientation[2];
                l_px1_orientation = l_H_r_to_cam_ * l_ee_orientation_x1;
                l_px1_orientation[0] /= l_px1_orientation[2];
                l_px1_orientation[1] /= l_px1_orientation[2];
                l_px2_orientation = l_H_r_to_cam_ * l_ee_orientation_x2;
                l_px2_orientation[0] /= l_px2_orientation[2];
                l_px2_orientation[1] /= l_px2_orientation[2];
                l_px3_orientation = l_H_r_to_cam_ * l_ee_orientation_x3;
                l_px3_orientation[0] /= l_px3_orientation[2];
                l_px3_orientation[1] /= l_px3_orientation[2];
                yInfo() << "Left ORIENTATION ee    = [" << l_px0_orientation.subVector(0, 1).toString() << "]";
                yInfo() << "Left ORIENTATION ee x1 = [" << l_px1_orientation.subVector(0, 1).toString() << "]";
                yInfo() << "Left ORIENTATION ee x2 = [" << l_px2_orientation.subVector(0, 1).toString() << "]";
                yInfo() << "Left ORIENTATION ee x3 = [" << l_px3_orientation.subVector(0, 1).toString() << "]";


                /* RIGHT: ORIENTATION = GOAL */
                r_est_position = est_copy_right;
                r_est_position.setSubvector(3, goal_pose_.subVector(3, 5));

                r_ee_position_x0 = zeros(4);
                r_ee_position_x1 = zeros(4);
                r_ee_position_x2 = zeros(4);
                r_ee_position_x3 = zeros(4);
                getPalmPoints(r_est_position, r_ee_position_x0, r_ee_position_x1, r_ee_position_x2, r_ee_position_x3);

                /* RIGHT: POSITION = GOAL */
                r_est_orientation = est_copy_right;
                r_est_orientation.setSubvector(0, goal_pose_.subVector(0, 2));

                r_ee_orientation_x0 = zeros(4);
                r_ee_orientation_x1 = zeros(4);
                r_ee_orientation_x2 = zeros(4);
                r_ee_orientation_x3 = zeros(4);
                getPalmPoints(r_est_orientation, r_ee_orientation_x0, r_ee_orientation_x1, r_ee_orientation_x2, r_ee_orientation_x3);


                /* RIGHT: EVALUATE CORRESPONDING ORIENTATION = GOAL PIXELS */
                r_px0_position = r_H_r_to_cam_ * r_ee_position_x0;
                r_px0_position[0] /= r_px0_position[2];
                r_px0_position[1] /= r_px0_position[2];
                r_px1_position = r_H_r_to_cam_ * r_ee_position_x1;
                r_px1_position[0] /= r_px1_position[2];
                r_px1_position[1] /= r_px1_position[2];
                r_px2_position = r_H_r_to_cam_ * r_ee_position_x2;
                r_px2_position[0] /= r_px2_position[2];
                r_px2_position[1] /= r_px2_position[2];
                r_px3_position = r_H_r_to_cam_ * r_ee_position_x3;
                r_px3_position[0] /= r_px3_position[2];
                r_px3_position[1] /= r_px3_position[2];
                yInfo() << "Right POSITION ee    = [" << r_px0_position.subVector(0, 1).toString() << "]";
                yInfo() << "Right POSITION ee x1 = [" << r_px1_position.subVector(0, 1).toString() << "]";
                yInfo() << "Right POSITION ee x2 = [" << r_px2_position.subVector(0, 1).toString() << "]";
                yInfo() << "Right POSITION ee x3 = [" << r_px3_position.subVector(0, 1).toString() << "]";

                /* RIGHT: EVALUATE CORRESPONDING POSITION = GOAL PIXELS */
                r_px0_orientation = r_H_r_to_cam_ * r_ee_orientation_x0;
                r_px0_orientation[0] /= r_px0_orientation[2];
                r_px0_orientation[1] /= r_px0_orientation[2];
                r_px1_orientation = r_H_r_to_cam_ * r_ee_orientation_x1;
                r_px1_orientation[0] /= r_px1_orientation[2];
                r_px1_orientation[1] /= r_px1_orientation[2];
                r_px2_orientation = r_H_r_to_cam_ * r_ee_orientation_x2;
                r_px2_orientation[0] /= r_px2_orientation[2];
                r_px2_orientation[1] /= r_px2_orientation[2];
                r_px3_orientation = r_H_r_to_cam_ * r_ee_orientation_x3;
                r_px3_orientation[0] /= r_px3_orientation[2];
                r_px3_orientation[1] /= r_px3_orientation[2];
                yInfo() << "Right ORIENTATION ee    = [" << r_px0_orientation.subVector(0, 1).toString() << "]";
                yInfo() << "Right ORIENTATION ee x1 = [" << r_px1_orientation.subVector(0, 1).toString() << "]";
                yInfo() << "Right ORIENTATION ee x2 = [" << r_px2_orientation.subVector(0, 1).toString() << "]";
                yInfo() << "Right ORIENTATION ee x3 = [" << r_px3_orientation.subVector(0, 1).toString() << "]";


                /* JACOBIAN: ORIENTATION = GOAL */
                px_ee_cur_position[0]  = l_px0_position[0];   /* u_ee_l */
                px_ee_cur_position[1]  = r_px0_position[0];   /* u_ee_r */
                px_ee_cur_position[2]  = l_px0_position[1];   /* v_ee_l */

                px_ee_cur_position[3]  = l_px1_position[0];   /* u_x1_l */
                px_ee_cur_position[4]  = r_px1_position[0];   /* u_x1_r */
                px_ee_cur_position[5]  = l_px1_position[1];   /* v_x1_l */

                px_ee_cur_position[6]  = l_px2_position[0];   /* u_x2_l */
                px_ee_cur_position[7]  = r_px2_position[0];   /* u_x2_r */
                px_ee_cur_position[8]  = l_px2_position[1];   /* v_x2_l */

                px_ee_cur_position[9]  = l_px3_position[0];   /* u_x3_l */
                px_ee_cur_position[10] = r_px3_position[0];   /* u_x3_r */
                px_ee_cur_position[11] = l_px3_position[1];   /* v_x3_l */

                yInfo() << "px_ee_cur_position = [" << px_ee_cur_position.toString() << "]";

                jacobian_position = zeros(12, 6);

                /* Point 0 */
                jacobian_position.setRow(0,  setJacobianU(LEFT,  l_px0_position));
                jacobian_position.setRow(1,  setJacobianU(RIGHT, r_px0_position));
                jacobian_position.setRow(2,  setJacobianV(LEFT,  l_px0_position));

                /* Point 1 */
                jacobian_position.setRow(3,  setJacobianU(LEFT,  l_px1_position));
                jacobian_position.setRow(4,  setJacobianU(RIGHT, r_px1_position));
                jacobian_position.setRow(5,  setJacobianV(LEFT,  l_px1_position));

                /* Point 2 */
                jacobian_position.setRow(6,  setJacobianU(LEFT,  l_px2_position));
                jacobian_position.setRow(7,  setJacobianU(RIGHT, r_px2_position));
                jacobian_position.setRow(8,  setJacobianV(LEFT,  l_px2_position));

                /* Point 3 */
                jacobian_position.setRow(9,  setJacobianU(LEFT,  l_px3_position));
                jacobian_position.setRow(10, setJacobianU(RIGHT, r_px3_position));
                jacobian_position.setRow(11, setJacobianV(LEFT,  l_px3_position));
                /* ******** */


                /* JACOBIAN: POSITION = GOAL */
                px_ee_cur_orientation[0]  = l_px0_orientation[0];   /* u_ee_l */
                px_ee_cur_orientation[1]  = r_px0_orientation[0];   /* u_ee_r */
                px_ee_cur_orientation[2]  = l_px0_orientation[1];   /* v_ee_l */

                px_ee_cur_orientation[3]  = l_px1_orientation[0];   /* u_x1_l */
                px_ee_cur_orientation[4]  = r_px1_orientation[0];   /* u_x1_r */
                px_ee_cur_orientation[5]  = l_px1_orientation[1];   /* v_x1_l */

                px_ee_cur_orientation[6]  = l_px2_orientation[0];   /* u_x2_l */
                px_ee_cur_orientation[7]  = r_px2_orientation[0];   /* u_x2_r */
                px_ee_cur_orientation[8]  = l_px2_orientation[1];   /* v_x2_l */

                px_ee_cur_orientation[9]  = l_px3_orientation[0];   /* u_x3_l */
                px_ee_cur_orientation[10] = r_px3_orientation[0];   /* u_x3_r */
                px_ee_cur_orientation[11] = l_px3_orientation[1];   /* v_x3_l */
                
                yInfo() << "px_ee_cur_orientation = [" << px_ee_cur_orientation.toString() << "]";
                
                jacobian_orientation = zeros(12, 6);
                
                /* Point 0 */
                jacobian_orientation.setRow(0,  setJacobianU(LEFT,  l_px0_orientation));
                jacobian_orientation.setRow(1,  setJacobianU(RIGHT, r_px0_orientation));
                jacobian_orientation.setRow(2,  setJacobianV(LEFT,  l_px0_orientation));
                
                /* Point 1 */
                jacobian_orientation.setRow(3,  setJacobianU(LEFT,  l_px1_orientation));
                jacobian_orientation.setRow(4,  setJacobianU(RIGHT, r_px1_orientation));
                jacobian_orientation.setRow(5,  setJacobianV(LEFT,  l_px1_orientation));
                
                /* Point 2 */
                jacobian_orientation.setRow(6,  setJacobianU(LEFT,  l_px2_orientation));
                jacobian_orientation.setRow(7,  setJacobianU(RIGHT, r_px2_orientation));
                jacobian_orientation.setRow(8,  setJacobianV(LEFT,  l_px2_orientation));
                
                /* Point 3 */
                jacobian_orientation.setRow(9,  setJacobianU(LEFT,  l_px3_orientation));
                jacobian_orientation.setRow(10, setJacobianU(RIGHT, r_px3_orientation));
                jacobian_orientation.setRow(11, setJacobianV(LEFT,  l_px3_orientation));
                /* ******** */


                /* DEBUG OUTPUT */
                cv::Scalar red   (255,   0,   0);
                cv::Scalar green (  0, 255,   0);
                cv::Scalar blue  (  0,   0, 255);
                cv::Scalar yellow(255, 255,   0);
                

                /* Left eye end-effector superimposition */
                ImageOf<PixelRgb>* l_imgin  = port_image_left_in_.read(true);
                ImageOf<PixelRgb>& l_imgout = port_image_left_out_.prepare();
                l_imgout = *l_imgin;
                cv::Mat l_img = cv::cvarrToMat(l_imgout.getIplImage());

                cv::circle(l_img, cv::Point(l_px0_position[0],    l_px0_position[1]),    4, red   , 4);
                cv::circle(l_img, cv::Point(l_px1_position[0],    l_px1_position[1]),    4, green , 4);
                cv::circle(l_img, cv::Point(l_px2_position[0],    l_px2_position[1]),    4, blue  , 4);
                cv::circle(l_img, cv::Point(l_px3_position[0],    l_px3_position[1]),    4, yellow, 4);
                cv::circle(l_img, cv::Point(l_px0_orientation[0], l_px0_orientation[1]), 4, red   , 4);
                cv::circle(l_img, cv::Point(l_px1_orientation[0], l_px1_orientation[1]), 4, green , 4);
                cv::circle(l_img, cv::Point(l_px2_orientation[0], l_px2_orientation[1]), 4, blue  , 4);
                cv::circle(l_img, cv::Point(l_px3_orientation[0], l_px3_orientation[1]), 4, yellow, 4);
                cv::circle(l_img, cv::Point(l_px_goal_[0], l_px_goal_[1]),               4, red   , 4);
                cv::circle(l_img, cv::Point(l_px_goal_[2], l_px_goal_[3]),               4, green , 4);
                cv::circle(l_img, cv::Point(l_px_goal_[4], l_px_goal_[5]),               4, blue  , 4);
                cv::circle(l_img, cv::Point(l_px_goal_[6], l_px_goal_[7]),               4, yellow, 4);

                port_image_left_out_.write();

                /* Right eye end-effector superimposition */
                ImageOf<PixelRgb>* r_imgin  = port_image_right_in_.read(true);
                ImageOf<PixelRgb>& r_imgout = port_image_right_out_.prepare();
                r_imgout = *r_imgin;
                cv::Mat r_img = cv::cvarrToMat(r_imgout.getIplImage());

                cv::circle(r_img, cv::Point(r_px0_position[0],    r_px0_position[1]),    4, red   , 4);
                cv::circle(r_img, cv::Point(r_px1_position[0],    r_px1_position[1]),    4, green , 4);
                cv::circle(r_img, cv::Point(r_px2_position[0],    r_px2_position[1]),    4, blue  , 4);
                cv::circle(r_img, cv::Point(r_px3_position[0],    r_px3_position[1]),    4, yellow, 4);
                cv::circle(r_img, cv::Point(r_px0_orientation[0], r_px0_orientation[1]), 4, red   , 4);
                cv::circle(r_img, cv::Point(r_px1_orientation[0], r_px1_orientation[1]), 4, green , 4);
                cv::circle(r_img, cv::Point(r_px2_orientation[0], r_px2_orientation[1]), 4, blue  , 4);
                cv::circle(r_img, cv::Point(r_px3_orientation[0], r_px3_orientation[1]), 4, yellow, 4);
                cv::circle(r_img, cv::Point(r_px_goal_[0], r_px_goal_[1]),               4, red   , 4);
                cv::circle(r_img, cv::Point(r_px_goal_[2], r_px_goal_[3]),               4, green , 4);
                cv::circle(r_img, cv::Point(r_px_goal_[4], r_px_goal_[5]),               4, blue  , 4);
                cv::circle(r_img, cv::Point(r_px_goal_[6], r_px_goal_[7]),               4, yellow, 4);

                port_image_right_out_.write();
                /* ************ */
            }
        }

        itf_rightarm_cart_->stopControl();

        Time::delay(0.5);

        return false;
    }


    bool respond(const Bottle& command, Bottle& reply)
    {
        int cmd = command.get(0).asVocab();
        switch (cmd)
        {
            /* Go to initial position (open-loop) */
            case VOCAB4('i', 'n', 'i', 't'):
            {
                /* FINGERTIP */
//                Matrix Od(3, 3);
//                Od(0, 0) = -1.0;
//                Od(1, 1) =  1.0;
//                Od(2, 2) = -1.0;
//                Vector od = dcm2axis(Od);

                /* Trial 27/04/17 */
                // -0.346 0.133 0.162 0.140 -0.989 0.026 2.693
//                Vector od(4);
//                od[0] =  0.140;
//                od[1] = -0.989;
//                od[2] =  0.026;
//                od[3] =  2.693;

                /* Trial 17/05/17 */
                // -0.300 0.088 0.080 -0.245 0.845 -0.473 2.896
                Vector od(4);
                od[0] = -0.245;
                od[1] =  0.845;
                od[2] = -0.473;
                od[3] =  2.896;

                /* KARATE */
//                // -0.319711 0.128912 0.075052 0.03846 -0.732046 0.680169 2.979943
//                Matrix Od = zeros(3, 3);
//                Od(0, 0) = -1.0;
//                Od(2, 1) = -1.0;
//                Od(1, 2) = -1.0;
//                Vector od = dcm2axis(Od);

                /* GRASPING */
//                Vector od = zeros(4);
//                od(0) = -0.141;
//                od(1) =  0.612;
//                od(2) = -0.777;
//                od(4) =  3.012;

                /* SIM */
//                Matrix Od(3, 3);
//                Od(0, 0) = -1.0;
//                Od(1, 1) = -1.0;
//                Od(2, 2) =  1.0;
//                Vector od = dcm2axis(Od);


                double traj_time = 0.0;
                itf_rightarm_cart_->getTrajTime(&traj_time);

                if (traj_time == traj_time_)
                {
                    Vector init_pos = zeros(3);

                    /* FINGERTIP init */
//                    Vector chain_joints;
//                    icub_index_.getChainJoints(readRootToFingers().subVector(3, 18), chain_joints);
//
//                    Matrix tip_pose_index = icub_index_.getH((M_PI/180.0) * chain_joints);
//                    Vector tip_x = tip_pose_index.getCol(3);
//                    Vector tip_o = dcm2axis(tip_pose_index);
//                    itf_rightarm_cart_->attachTipFrame(tip_x, tip_o);
//
                    //FIXME: to implement
//                    init_pos[0] = -0.345;
//                    init_pos[1] =  0.139;
//                    init_pos[2] =  0.089;

                    /* Trial 27/04/17 */
                    // -0.346 0.133 0.162 0.140 -0.989 0.026 2.693
//                    init_pos[0] = -0.346;
//                    init_pos[1] =  0.133;
//                    init_pos[2] =  0.162;

                    /* Trial 17/05/17 */
                    // -0.300 0.088 0.080 -0.245 0.845 -0.473 2.896
                    init_pos[0] = -0.300;
                    init_pos[1] =  0.088;
                    init_pos[2] =  0.080;

//                    /* KARATE init */
//                    // -0.319711 0.128912 0.075052 0.03846 -0.732046 0.680169 2.979943
//                    init_pos[0] = -0.319;
//                    init_pos[1] =  0.128;
//                    init_pos[2] =  0.075;

                    /* GRASPING init */
//                    init_pos[0] = -0.370;
//                    init_pos[1] =  0.103;
//                    init_pos[2] =  0.064;

                    /* SIM init 1 */
//                    init_pos[0] = -0.416;
//                    init_pos[1] =  0.024 + 0.1;
//                    init_pos[2] =  0.055;

                    /* SIM init 2 */
//                    init_pos[0] = -0.35;
//                    init_pos[1] =  0.025 + 0.05;
//                    init_pos[2] =  0.10;

                    yInfo() << "Init: " << init_pos.toString() << " " << od.toString();

                    unsetTorsoDOF();

//                    itf_rightarm_cart_->setLimits(0,  15.0,  15.0);
//                    itf_rightarm_cart_->setLimits(2, -23.0, -23.0);
//                    itf_rightarm_cart_->setLimits(3, -16.0, -16.0);
//                    itf_rightarm_cart_->setLimits(4,  53.0,  53.0);
//                    itf_rightarm_cart_->setLimits(5,   0.0,   0.0);
//                    itf_rightarm_cart_->setLimits(7, -58.0, -58.0);

                    itf_rightarm_cart_->goToPoseSync(init_pos, od);
                    itf_rightarm_cart_->waitMotionDone(0.1, 10.0);
                    itf_rightarm_cart_->stopControl();

                    itf_rightarm_cart_->removeTipFrame();

                    itf_rightarm_cart_->storeContext(&ctx_cart_);


                    /* Normal trials */
//                    Vector gaze_loc(3);
//                    gaze_loc[0] = init_pos[0];
//                    gaze_loc[1] = init_pos[1];
//                    gaze_loc[2] = init_pos[2];

                    /* Trial 27/04/17 */
                    // -6.706 1.394 -3.618
//                    Vector gaze_loc(3);
//                    gaze_loc[0] = -6.706;
//                    gaze_loc[1] =  1.394;
//                    gaze_loc[2] = -3.618;

                    /* Trial 17/05/17 */
                    // -0.681 0.112 -0.240
                    Vector gaze_loc(3);
                    gaze_loc[0] = -0.681;
                    gaze_loc[1] =  0.112;
                    gaze_loc[2] = -0.240;

                    yInfo() << "Fixation point: " << gaze_loc.toString();

                    itf_gaze_->lookAtFixationPointSync(gaze_loc);
                    itf_gaze_->waitMotionDone(0.1, 10.0);
                    itf_gaze_->stopControl();

                    itf_gaze_->storeContext(&ctx_gaze_);


                    reply.addString("ack");
                }
                else
                {
                    reply.addString("nack");
                }

                break;
            }
            /* Get 3D point from Structure From Motion clicking on the left camera image */
            /* PLUS: Compute again the roto-translation and projection matrices from root to left and right camera planes */
            case VOCAB3('s', 'f', 'm'):
            {
                Vector left_eye_x;
                Vector left_eye_o;
                itf_gaze_->getLeftEyePose(left_eye_x, left_eye_o);

                Vector right_eye_x;
                Vector right_eye_o;
                itf_gaze_->getRightEyePose(right_eye_x, right_eye_o);

                yInfo() << "left_eye_o =" << left_eye_o.toString();
                yInfo() << "right_eye_o =" << right_eye_o.toString();


                l_H_eye_to_r_ = axis2dcm(left_eye_o);
                left_eye_x.push_back(1.0);
                l_H_eye_to_r_.setCol(3, left_eye_x);
                l_H_r_to_eye_ = SE3inv(l_H_eye_to_r_);

                r_H_eye_to_r_ = axis2dcm(right_eye_o);
                right_eye_x.push_back(1.0);
                r_H_eye_to_r_.setCol(3, right_eye_x);
                r_H_r_to_eye_ = SE3inv(r_H_eye_to_r_);

                yInfo() << "l_H_r_to_eye_ =\n" << l_H_r_to_eye_.toString();
                yInfo() << "r_H_r_to_eye_ =\n" << r_H_r_to_eye_.toString();

                l_H_r_to_cam_ = l_proj_ * l_H_r_to_eye_;
                r_H_r_to_cam_ = r_proj_ * r_H_r_to_eye_;


                Network yarp;
                Bottle  cmd;
                Bottle  rep;

                Bottle* click_left = port_click_left_.read(true);
                Vector l_click = zeros(2);
                l_click[0] = click_left->get(0).asDouble();
                l_click[1] = click_left->get(1).asDouble();

                RpcClient port_sfm;
                port_sfm.open("/reaching_pose/tosfm");
                yarp.connect("/reaching_pose/tosfm", "/SFM/rpc");

                cmd.clear();

                cmd.addInt(l_click[0]);
                cmd.addInt(l_click[1]);

                Bottle reply_pos;
                port_sfm.write(cmd, reply_pos);
                if (reply_pos.size() == 5)
                {
                    Matrix R_ee = zeros(3, 3);
                    R_ee(0, 0) = -1.0;
                    R_ee(1, 1) =  1.0;
                    R_ee(2, 2) = -1.0;
                    Vector ee_o = dcm2axis(R_ee);

                    Vector sfm_pos = zeros(3);
                    sfm_pos[0] = reply_pos.get(0).asDouble();
                    sfm_pos[1] = reply_pos.get(1).asDouble();
                    sfm_pos[2] = reply_pos.get(2).asDouble();

                    Vector p = zeros(7);
                    p.setSubvector(0, sfm_pos.subVector(0, 2));
                    p.setSubvector(3, ee_o.subVector(0, 2) * ee_o(3));

                    goal_pose_ = p;
                    yInfo() << "Goal: " << goal_pose_.toString();

                    Vector p0 = zeros(4);
                    Vector p1 = zeros(4);
                    Vector p2 = zeros(4);
                    Vector p3 = zeros(4);
                    getPalmPoints(p, p0, p1, p2, p3);

                    yInfo() << "goal px: [" << p0.toString() << ";" << p1.toString() << ";" << p2.toString() << ";" << p3.toString() << "];";


                    Vector l_px0_goal = l_H_r_to_cam_ * p0;
                    l_px0_goal[0] /= l_px0_goal[2];
                    l_px0_goal[1] /= l_px0_goal[2];
                    Vector l_px1_goal = l_H_r_to_cam_ * p1;
                    l_px1_goal[0] /= l_px1_goal[2];
                    l_px1_goal[1] /= l_px1_goal[2];
                    Vector l_px2_goal = l_H_r_to_cam_ * p2;
                    l_px2_goal[0] /= l_px2_goal[2];
                    l_px2_goal[1] /= l_px2_goal[2];
                    Vector l_px3_goal = l_H_r_to_cam_ * p3;
                    l_px3_goal[0] /= l_px3_goal[2];
                    l_px3_goal[1] /= l_px3_goal[2];

                    l_px_goal_.resize(8);
                    l_px_goal_[0] = l_px0_goal[0];
                    l_px_goal_[1] = l_px0_goal[1];
                    l_px_goal_[2] = l_px1_goal[0];
                    l_px_goal_[3] = l_px1_goal[1];
                    l_px_goal_[4] = l_px2_goal[0];
                    l_px_goal_[5] = l_px2_goal[1];
                    l_px_goal_[6] = l_px3_goal[0];
                    l_px_goal_[7] = l_px3_goal[1];


                    Vector r_px0_goal = r_H_r_to_cam_ * p0;
                    r_px0_goal[0] /= r_px0_goal[2];
                    r_px0_goal[1] /= r_px0_goal[2];
                    Vector r_px1_goal = r_H_r_to_cam_ * p1;
                    r_px1_goal[0] /= r_px1_goal[2];
                    r_px1_goal[1] /= r_px1_goal[2];
                    Vector r_px2_goal = r_H_r_to_cam_ * p2;
                    r_px2_goal[0] /= r_px2_goal[2];
                    r_px2_goal[1] /= r_px2_goal[2];
                    Vector r_px3_goal = r_H_r_to_cam_ * p3;
                    r_px3_goal[0] /= r_px3_goal[2];
                    r_px3_goal[1] /= r_px3_goal[2];

                    r_px_goal_.resize(8);
                    r_px_goal_[0] = r_px0_goal[0];
                    r_px_goal_[1] = r_px0_goal[1];
                    r_px_goal_[2] = r_px1_goal[0];
                    r_px_goal_[3] = r_px1_goal[1];
                    r_px_goal_[4] = r_px2_goal[0];
                    r_px_goal_[5] = r_px2_goal[1];
                    r_px_goal_[6] = r_px3_goal[0];
                    r_px_goal_[7] = r_px3_goal[1];
                }
                else
                {
                    reply.addString("nack");
                }

                yarp.disconnect("/reaching_pose/tosfm", "/SFM/rpc");
                port_sfm.close();

                reply = command;

                break;
            }
            /* Set a fixed goal in pixel coordinates */
            /* PLUS: Compute again the roto-translation and projection matrices from root to left and right camera planes */
            case VOCAB4('g', 'o', 'a', 'l'):
            {
                Vector left_eye_x;
                Vector left_eye_o;
                itf_gaze_->getLeftEyePose(left_eye_x, left_eye_o);

                Vector right_eye_x;
                Vector right_eye_o;
                itf_gaze_->getRightEyePose(right_eye_x, right_eye_o);

                yInfo() << "left_eye_o = ["  << left_eye_o.toString()  << "]";
                yInfo() << "right_eye_o = [" << right_eye_o.toString() << "]";


                l_H_eye_to_r_ = axis2dcm(left_eye_o);
                left_eye_x.push_back(1.0);
                l_H_eye_to_r_.setCol(3, left_eye_x);
                l_H_r_to_eye_ = SE3inv(l_H_eye_to_r_);

                r_H_eye_to_r_ = axis2dcm(right_eye_o);
                right_eye_x.push_back(1.0);
                r_H_eye_to_r_.setCol(3, right_eye_x);
                r_H_r_to_eye_ = SE3inv(r_H_eye_to_r_);

                yInfo() << "l_H_r_to_eye_ = [\n" << l_H_r_to_eye_.toString() << "]";
                yInfo() << "r_H_r_to_eye_ = [\n" << r_H_r_to_eye_.toString() << "]";

                l_H_r_to_cam_ = l_proj_ * l_H_r_to_eye_;
                r_H_r_to_cam_ = r_proj_ * r_H_r_to_eye_;


//                /* Hand pointing forward, palm looking down */
//                Matrix R_ee = zeros(3, 3);
//                R_ee(0, 0) = -1.0;
//                R_ee(1, 1) =  1.0;
//                R_ee(2, 2) = -1.0;
//                Vector ee_o = dcm2axis(R_ee);

                /* Trial 27/04/17 */
                // -0.323 0.018 0.121 0.310 -0.873 0.374 3.008
//                Vector p = zeros(6);
//                p[0] = -0.323;
//                p[1] =  0.018;
//                p[2] =  0.121;
//                p[3] =  0.310 * 3.008;
//                p[4] = -0.873 * 3.008;
//                p[5] =  0.374 * 3.008;

                /* Trial 17/05/17 */
                // -0.284 0.013 0.104 -0.370 0.799 -0.471 2.781
                Vector p = zeros(6);
                p[0] = -0.284;
                p[1] =  0.013;
                p[2] =  0.104;
                p[3] = -0.370 * 2.781;
                p[4] =  0.799 * 2.781;
                p[5] = -0.471 * 2.781;

                /* KARATE */
//                Vector p = zeros(6);
//                p[0] = -0.319;
//                p[1] =  0.128;
//                p[2] =  0.075;
//                p.setSubvector(3, ee_o.subVector(0, 2) * ee_o(3));

                /* SIM init 1 */
                // -0.416311	-0.026632	 0.055334	-0.381311	-0.036632	 0.055334	-0.381311	-0.016632	 0.055334
//                Vector p = zeros(6);
//                p[0] = -0.416;
//                p[1] = -0.024;
//                p[2] =  0.055;
//                p.setSubvector(3, ee_o.subVector(0, 2) * ee_o(3));

                /* SIM init 2 */
//                Vector p = zeros(6);
//                p[0] = -0.35;
//                p[1] =  0.025;
//                p[2] =  0.10;
//                p.setSubvector(3, ee_o.subVector(0, 2) * ee_o(3));

                goal_pose_ = p;
                yInfo() << "Goal: " << goal_pose_.toString();

                Vector p0 = zeros(4);
                Vector p1 = zeros(4);
                Vector p2 = zeros(4);
                Vector p3 = zeros(4);
                getPalmPoints(p, p0, p1, p2, p3);

                yInfo() << "Goal px: [" << p0.toString() << ";" << p1.toString() << ";" << p2.toString() << ";" << p3.toString() << "];";


                Vector l_px0_goal = l_H_r_to_cam_ * p0;
                l_px0_goal[0] /= l_px0_goal[2];
                l_px0_goal[1] /= l_px0_goal[2];
                Vector l_px1_goal = l_H_r_to_cam_ * p1;
                l_px1_goal[0] /= l_px1_goal[2];
                l_px1_goal[1] /= l_px1_goal[2];
                Vector l_px2_goal = l_H_r_to_cam_ * p2;
                l_px2_goal[0] /= l_px2_goal[2];
                l_px2_goal[1] /= l_px2_goal[2];
                Vector l_px3_goal = l_H_r_to_cam_ * p3;
                l_px3_goal[0] /= l_px3_goal[2];
                l_px3_goal[1] /= l_px3_goal[2];

                l_px_goal_.resize(8);
                l_px_goal_[0] = l_px0_goal[0];
                l_px_goal_[1] = l_px0_goal[1];
                l_px_goal_[2] = l_px1_goal[0];
                l_px_goal_[3] = l_px1_goal[1];
                l_px_goal_[4] = l_px2_goal[0];
                l_px_goal_[5] = l_px2_goal[1];
                l_px_goal_[6] = l_px3_goal[0];
                l_px_goal_[7] = l_px3_goal[1];


                Vector r_px0_goal = r_H_r_to_cam_ * p0;
                r_px0_goal[0] /= r_px0_goal[2];
                r_px0_goal[1] /= r_px0_goal[2];
                Vector r_px1_goal = r_H_r_to_cam_ * p1;
                r_px1_goal[0] /= r_px1_goal[2];
                r_px1_goal[1] /= r_px1_goal[2];
                Vector r_px2_goal = r_H_r_to_cam_ * p2;
                r_px2_goal[0] /= r_px2_goal[2];
                r_px2_goal[1] /= r_px2_goal[2];
                Vector r_px3_goal = r_H_r_to_cam_ * p3;
                r_px3_goal[0] /= r_px3_goal[2];
                r_px3_goal[1] /= r_px3_goal[2];

                r_px_goal_.resize(8);
                r_px_goal_[0] = r_px0_goal[0];
                r_px_goal_[1] = r_px0_goal[1];
                r_px_goal_[2] = r_px1_goal[0];
                r_px_goal_[3] = r_px1_goal[1];
                r_px_goal_[4] = r_px2_goal[0];
                r_px_goal_[5] = r_px2_goal[1];
                r_px_goal_[6] = r_px3_goal[0];
                r_px_goal_[7] = r_px3_goal[1];


                reply = command;

                break;
            }
            /* Start reaching phase */
            case VOCAB2('g','o'):
            {
                reply = command;
                take_estimates_ = true;

                break;
            }
            /* Safely close the application */
            case VOCAB4('q','u','i','t'):
            {
                itf_rightarm_cart_->stopControl();
                itf_gaze_->stopControl();

                should_stop_    = true;
                take_estimates_ = true;

                reply = command;

                break;
            }
            default:
            {
                reply.addString("nack");
            }
        }

        return true;
    }

    bool interruptModule()
    {
        yInfo() << "Interrupting module...";

        yInfo() << "...blocking controllers...";
        itf_rightarm_cart_->stopControl();
        itf_gaze_->stopControl();

        Time::delay(3.0);

        yInfo() << "...port cleanup...";
        port_estimates_left_in_.interrupt();
        port_estimates_right_in_.interrupt();
        port_image_left_in_.interrupt();
        port_image_left_out_.interrupt();
        port_click_left_.interrupt();
        port_image_right_in_.interrupt();
        port_image_right_out_.interrupt();
        port_click_right_.interrupt();
        handler_port_.interrupt();

        yInfo() << "...done!";
        return true;
    }

    bool close()
    {
        yInfo() << "Calling close functions...";

        port_estimates_left_in_.close();
        port_estimates_right_in_.close();
        port_image_left_in_.close();
        port_image_left_out_.close();
        port_click_left_.close();
        port_image_right_in_.close();
        port_image_right_out_.close();
        port_click_right_.close();

        itf_rightarm_cart_->removeTipFrame();

        if (rightarm_cartesian_driver_.isValid()) rightarm_cartesian_driver_.close();
        if (gaze_driver_.isValid())               gaze_driver_.close();

        handler_port_.close();

        yInfo() << "...done!";
        return true;
    }

private:
    ConstString                      robot_name_;

    Port                             handler_port_;
    bool                             should_stop_ = false;

    BufferedPort<Vector>             port_estimates_left_in_;
    BufferedPort<Vector>             port_estimates_right_in_;

    BufferedPort<ImageOf<PixelRgb>>  port_image_left_in_;
    BufferedPort<ImageOf<PixelRgb>>  port_image_left_out_;
    BufferedPort<Bottle>             port_click_left_;

    BufferedPort<ImageOf<PixelRgb>>  port_image_right_in_;
    BufferedPort<ImageOf<PixelRgb>>  port_image_right_out_;
    BufferedPort<Bottle>             port_click_right_;

    PolyDriver                       rightarm_cartesian_driver_;
    ICartesianControl              * itf_rightarm_cart_;

    PolyDriver                       gaze_driver_;
    IGazeControl                   * itf_gaze_;

    PolyDriver                       rightarm_remote_driver_;
    IEncoders                      * itf_rightarm_enc_;
    IControlLimits                 * itf_fingers_lim_;

    PolyDriver                       torso_remote_driver_;
    IEncoders                      * itf_torso_enc_;

    iCubFinger                       icub_index_;

    Vector                           goal_pose_;
    Matrix                           l_proj_;
    Matrix                           r_proj_;
    Matrix                           l_H_r_to_eye_;
    Matrix                           r_H_r_to_eye_;
    Matrix                           l_H_eye_to_r_;
    Matrix                           r_H_eye_to_r_;
    Matrix                           l_H_r_to_cam_;
    Matrix                           r_H_r_to_cam_;
    Matrix                           px_to_cartesian_;

    double                           traj_time_ = 3.0;
    Vector                           l_px_goal_;
    Vector                           r_px_goal_;
    bool                             take_estimates_ = false;

    int                              ctx_cart_;
    int                              ctx_gaze_;

    enum camsel
    {
        LEFT = 0,
        RIGHT = 1
    };

    bool setRightArmCartesianController()
    {
        Property rightarm_cartesian_options;
        rightarm_cartesian_options.put("device", "cartesiancontrollerclient");
        rightarm_cartesian_options.put("local",  "/reaching_pose/cart_right_arm");
        rightarm_cartesian_options.put("remote", "/"+robot_name_+"/cartesianController/right_arm");

        rightarm_cartesian_driver_.open(rightarm_cartesian_options);
        if (rightarm_cartesian_driver_.isValid())
        {
            rightarm_cartesian_driver_.view(itf_rightarm_cart_);
            if (!itf_rightarm_cart_)
            {
                yError() << "Error getting ICartesianControl interface.";
                return false;
            }
            yInfo() << "cartesiancontrollerclient succefully opened.";
        }
        else
        {
            yError() << "Error opening cartesiancontrollerclient device.";
            return false;
        }

        if (!itf_rightarm_cart_->setTrajTime(traj_time_))
        {
            yError() << "Error setting ICartesianControl trajectory time.";
            return false;
        }
        yInfo() << "Succesfully set ICartesianControl trajectory time!";

        if (!itf_rightarm_cart_->setInTargetTol(0.01))
        {
            yError() << "Error setting ICartesianControl target tolerance.";
            return false;
        }
        yInfo() << "Succesfully set ICartesianControl target tolerance!";

        return true;
    }

    bool setGazeController()
    {
        Property gaze_option;
        gaze_option.put("device", "gazecontrollerclient");
        gaze_option.put("local",  "/reaching_pose/gaze");
        gaze_option.put("remote", "/iKinGazeCtrl");

        gaze_driver_.open(gaze_option);
        if (gaze_driver_.isValid())
        {
            gaze_driver_.view(itf_gaze_);
            if (!itf_gaze_)
            {
                yError() << "Error getting IGazeControl interface.";
                return false;
            }
        }
        else
        {
            yError() << "Gaze control device not available.";
            return false;
        }

        return true;
    }

    bool setRightArmRemoteControlboard()
    {
        Property rightarm_remote_options;
        rightarm_remote_options.put("device", "remote_controlboard");
        rightarm_remote_options.put("local",  "/reaching_pose/control_right_arm");
        rightarm_remote_options.put("remote", "/"+robot_name_+"/right_arm");

        rightarm_remote_driver_.open(rightarm_remote_options);
        if (rightarm_remote_driver_.isValid())
        {
            yInfo() << "Right arm remote_controlboard succefully opened.";

            rightarm_remote_driver_.view(itf_rightarm_enc_);
            if (!itf_rightarm_enc_)
            {
                yError() << "Error getting right arm IEncoders interface.";
                return false;
            }

            rightarm_remote_driver_.view(itf_fingers_lim_);
            if (!itf_fingers_lim_)
            {
                yError() << "Error getting IControlLimits interface.";
                return false;
            }
        }
        else
        {
            yError() << "Error opening right arm remote_controlboard device.";
            return false;
        }

        return true;
    }

    bool setTorsoRemoteControlboard()
    {
        Property torso_remote_options;
        torso_remote_options.put("device", "remote_controlboard");
        torso_remote_options.put("local",  "/reaching_pose/control_torso");
        torso_remote_options.put("remote", "/"+robot_name_+"/torso");

        torso_remote_driver_.open(torso_remote_options);
        if (torso_remote_driver_.isValid())
        {
            yInfo() << "Torso remote_controlboard succefully opened.";

            torso_remote_driver_.view(itf_torso_enc_);
            if (!itf_torso_enc_)
            {
                yError() << "Error getting torso IEncoders interface.";
                return false;
            }

            return true;
        }
        else
        {
            yError() << "Error opening Torso remote_controlboard device.";
            return false;
        }
    }

    bool setTorsoDOF()
    {
        Vector curDOF;
        itf_rightarm_cart_->getDOF(curDOF);
        yInfo() << "Old DOF: [" + curDOF.toString(0) + "].";
        yInfo() << "Setting iCub to use the DOF from the torso.";
        Vector newDOF(curDOF);
        newDOF[0] = 1;
        newDOF[1] = 1;
        newDOF[2] = 1;
        if (!itf_rightarm_cart_->setDOF(newDOF, curDOF))
        {
            yError() << "Cannot set torso DOF.";
            return false;
        }
        yInfo() << "Setting the DOF done.";
        yInfo() << "New DOF: [" + curDOF.toString(0) + "]";

        return true;
    }

    bool unsetTorsoDOF()
    {
        Vector curDOF;
        itf_rightarm_cart_->getDOF(curDOF);
        yInfo() << "Old DOF: [" + curDOF.toString(0) + "].";
        yInfo() << "Setting iCub to not use the DOF from the torso.";
        Vector newDOF(curDOF);
        newDOF[0] = 0;
        newDOF[1] = 0;
        newDOF[2] = 0;
        if (!itf_rightarm_cart_->setDOF(newDOF, curDOF))
        {
            yError() << "Cannot set torso DOF.";
            return false;
        }
        yInfo() << "Setting the DOF done.";
        yInfo() << "New DOF: [" + curDOF.toString(0) + "]";

        return true;
    }

    Vector readTorso()
    {
        Vector torso_enc(3);
        itf_torso_enc_->getEncoders(torso_enc.data());

        std::swap(torso_enc(0), torso_enc(2));

        return torso_enc;
    }

    Vector readRootToFingers()
    {
        Vector rightarm_encoder(16);
        itf_rightarm_enc_->getEncoders(rightarm_encoder.data());

        Vector root_fingers_enc(19);
        root_fingers_enc.setSubvector(0, readTorso());

        root_fingers_enc.setSubvector(3, rightarm_encoder);

        return root_fingers_enc;
    }

    void getPalmPoints(const Vector& endeffector, Vector& p0, Vector& p1, Vector& p2, Vector& p3)
    {
        Vector ee_x = endeffector.subVector(0, 2);
        ee_x.push_back(1.0);
        double ang  = norm(endeffector.subVector(3, 5));
        Vector ee_o = endeffector.subVector(3, 5) / ang;
        ee_o.push_back(ang);

        Matrix H_ee_to_root = axis2dcm(ee_o);
        H_ee_to_root.setCol(3, ee_x);


        Vector p = zeros(4);

        p(0) =  0;
        p(1) = -0.015;
        p(2) =  0;
        p(3) =  1.0;

        p0 = zeros(4);
        p0 = H_ee_to_root * p;

        p(0) = 0;
        p(1) = 0.015;
        p(2) = 0;
        p(3) = 1.0;

        p1 = zeros(4);
        p1 = H_ee_to_root * p;

        p(0) = -0.035;
        p(1) =  0.015;
        p(2) =  0;
        p(3) =  1.0;

        p2 = zeros(4);
        p2 = H_ee_to_root * p;

        p(0) = -0.035;
        p(1) = -0.015;
        p(2) =  0;
        p(3) =  1.0;

        p3 = zeros(4);
        p3 = H_ee_to_root * p;
    }

    Vector setJacobianU(const int cam, const Vector& px)
    {
        Vector jacobian = zeros(6);

        if (cam == LEFT)
        {
            jacobian(0) = l_proj_(0, 0) / px(2);
            jacobian(2) = - (px(0) - l_proj_(0, 2)) / px(2);
            jacobian(3) = - ((px(0) - l_proj_(0, 2)) * (px(1) - l_proj_(1, 2))) / l_proj_(1, 1);
            jacobian(4) = (pow(l_proj_(0, 0), 2.0) + pow(px(0) - l_proj_(0, 2), 2.0)) / l_proj_(0, 0);
            jacobian(5) = - l_proj_(0, 0) / l_proj_(1, 1) * (px(1) - l_proj_(1, 2));
        }
        else if (cam == RIGHT)
        {
            jacobian(0) = r_proj_(0, 0) / px(2);
            jacobian(2) = - (px(0) - r_proj_(0, 2)) / px(2);
            jacobian(3) = - ((px(0) - r_proj_(0, 2)) * (px(1) - r_proj_(1, 2))) / r_proj_(1, 1);
            jacobian(4) = (pow(r_proj_(0, 0), 2.0) + pow(px(0) - r_proj_(0, 2), 2.0)) / r_proj_(0, 0);
            jacobian(5) = - r_proj_(0, 0) / r_proj_(1, 1) * (px(1) - r_proj_(1, 2));
        }

        return jacobian;
    }

    Vector setJacobianV(const int cam, const Vector& px)
    {
        Vector jacobian = zeros(6);

        if (cam == LEFT)
        {
            jacobian(1) = l_proj_(1, 1) / px(2);
            jacobian(2) = - (px(1) - l_proj_(1, 2)) / px(2);
            jacobian(3) = - (pow(l_proj_(1, 1), 2.0) + pow(px(1) - l_proj_(1, 2), 2.0)) / l_proj_(1, 1);
            jacobian(4) = ((px(0) - l_proj_(0, 2)) * (px(1) - l_proj_(1, 2))) / l_proj_(0, 0);
            jacobian(5) = l_proj_(1, 1) / l_proj_(0, 0) * (px(0) - l_proj_(0, 2));
        }
        else if (cam == RIGHT)
        {
            jacobian(1) = r_proj_(1, 1) / px(2);
            jacobian(2) = - (px(1) - r_proj_(1, 2)) / px(2);
            jacobian(3) = - (pow(r_proj_(1, 1), 2.0) + pow(px(1) - r_proj_(1, 2), 2.0)) / r_proj_(1, 1);
            jacobian(4) = ((px(0) - r_proj_(0, 2)) * (px(1) - r_proj_(1, 2))) / r_proj_(0, 0);
            jacobian(5) = r_proj_(1, 1) / r_proj_(0, 0) * (px(0) - r_proj_(0, 2));
        }

        return jacobian;
    }

    Matrix getSkew(const Vector& v)
    {
        Matrix skew = zeros(3, 3);

        skew(0, 1) = -v(2);
        skew(0, 2) =  v(1);

        skew(1, 0) =  v(2);
        skew(1, 2) = -v(0);

        skew(2, 0) = -v(1);
        skew(2, 1) =  v(0);

        return skew;
    }

    Matrix getGamma(const Vector& p)
    {
        Matrix G = zeros(6, 6);

        G.setSubmatrix(-1.0 * eye(3, 3), 0, 0);
        G.setSubmatrix(getSkew(p)      , 0, 3);
        G.setSubmatrix(-1.0 * eye(3, 3), 3, 3);

        return G;
    }

    Vector getAxisAngle(const Vector& v)
    {
        double ang  = norm(v);
        Vector aa   = v / ang;
        aa.push_back(ang);

        return aa;
    }
};


int main(int argc, char **argv)
{
    Network yarp;
    if (!yarp.checkNetwork(3.0))
    {
        yError() << "YARP seems unavailable!";
        return EXIT_FAILURE;
    }

    ResourceFinder rf;
    rf.configure(argc, argv);
    RFMReaching reaching;
    reaching.runModule(rf);

    return EXIT_SUCCESS;
}

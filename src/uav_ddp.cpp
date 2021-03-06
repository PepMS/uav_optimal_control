
#include "uav_ddp.hpp"

UavDDPNode::UavDDPNode()
{
    // Initialize specific classes needed in the DDP solver
    initializeDDP();

    tau_     = Eigen::VectorXd::Zero(uav_model_.nv);
    tau_max_ = Eigen::VectorXd::Zero(uav_model_.nv);
    optiuavm::computeGeneralizedTorqueMax(*uav_params_, uav_model_.nv, nav_problem_->actuation_->get_nu(), tau_max_);
    tau_min_ = -tau_max_;
    tau_min_(2) = 0.0;
    
    px4_tau_ = Eigen::VectorXd::Zero(4);
    px4_tau_min_ = Eigen::VectorXd::Zero(4);
    px4_tau_max_ = Eigen::VectorXd::Zero(4);
    px4_tau_min_ << 0.0, -1.0, -1.0, -1.0; // T, Mx, My, Mz
    px4_tau_max_ << 1.0,  1.0,  1.0,  1.0; // T, Mx, My, Mz
    std::cout << "Here!" << std::endl;
    // Multithread
    // sb_pose_opt_ = ros::SubscribeOptions::create<geometry_msgs::PoseStamped>("/mavros/local_position/pose", 1, boost::bind(&UavDDPNode::callbackPose, this, _1), ros::VoidPtr(), &sb_pose_queue_);
    // sb_pose_ = nh_.subscribe(sb_pose_opt_);

    // sb_twist_opt_ = ros::SubscribeOptions::create<geometry_msgs::TwistStamped>("/mavros/local_position/velocity_body", 1, boost::bind(&UavDDPNode::callbackTwist, this, _1), ros::VoidPtr(), &sb_twist_queue_);
    // sb_twist_ = nh_.subscribe(sb_twist_opt_);

    // Publishers and subscribers
    sb_pose_ = nh_.subscribe("/mavros/local_position/pose", 1, &UavDDPNode::callbackPose, this);
    sb_twist_ = nh_.subscribe("/mavros/local_position/velocity_body", 1, &UavDDPNode::callbackTwist, this);
    pub_policy_ = nh_.advertise<uav_oc_msgs::UAVOptCtlPolicy>("/optctl/actuator_control", 10);    
}

UavDDPNode::~UavDDPNode(){}

void UavDDPNode::initializeDDP()
{
    // Load URDF Model
    pinocchio::urdf::buildModel(EXAMPLE_ROBOT_DATA_MODEL_DIR "/iris_description/robots/iris_simple.urdf",  pinocchio::JointModelFreeFlyer(), uav_model_);

    // Load UAV params
    ParserYAML yaml_file("/home/pepms/ros-ws/uav-ws/src/optiuavm_ros/config/iris.yaml", "", true);
    optiuavm::ParamsServer server(yaml_file.getParams());

    uav_params_ = boost::make_shared<crocoddyl::UavUamParams>();
    optiuavm::fillUavUamParams(*uav_params_, server);

    // Frame we want in the cost function
    bl_frameid_ = uav_model_.getFrameId("iris__base_link");

    // x0_:Set an initial pose (should be read from topic)
    x0_ = Eigen::VectorXd::Zero(uav_model_.nv*2 + 1);
    x0_(2) = 2.5;
    x0_(6) = 1.0;
    
    // This may be problematic as tpos and tquat are passed by reference so when the constructor ends, it may destruct both variables.
    Eigen::Vector3d tpos;
    tpos << 0,0,1;
    Eigen::Quaterniond tquat(1,0,0,0);
    Eigen::Vector3d zero_vel = Eigen::Vector3d::Zero();
    wp_ = boost::make_shared<optiuavm::WayPoint>(15, tpos, tquat, zero_vel, zero_vel);

    // Navigation problem
    nav_problem_ = boost::make_shared<optiuavm::SimpleGotoProblem>(uav_model_, *uav_params_, true);

    // Shooting problem
    double dt = 6.25e-3; // (160 Hz)
    ddp_problem_ = nav_problem_->createProblem(x0_, *wp_, dt, bl_frameid_);

    // Solver callbacks
    fddp_cbs_.push_back(boost::make_shared<crocoddyl::CallbackVerbose>());

    // Setting the solver
    fddp_ = boost::make_shared<crocoddyl::SolverFDDP>(ddp_problem_);      
    fddp_->setCallbacks(fddp_cbs_);
    fddp_->solve();
    fddp_->solve(fddp_->get_xs(), fddp_->get_us());
    
}

// void UavDDPNode::fillUavParams()
// {
//     if (! nh_.hasParam("multirotor"))
//     {
//         ROS_ERROR("Fill UAV params: multirotor parameter not found. Please, load the YAML file with the multirotor description.");
//     }     
    
//     if (!nh_.getParam("multirotor/cf", uav_params_->cf_))
//         ROS_ERROR("Fill UAV params: please, specify the thrust coefficient for the propellers (cf).");
    
//     if (!nh_.getParam("multirotor/cm", uav_params_->cm_))
//         ROS_ERROR("Fill UAV params: please, specify the torque coefficient for the propellers (cm).");
    
//     if (!nh_.getParam("multirotor/max_thrust", uav_params_->max_thrust_))
//         ROS_ERROR("Fill UAV params: please, specify the max lift force (max_thrust) that a rotor can produce.");
    
//     if (!nh_.getParam("multirotor/min_thrust", uav_params_->min_thrust_))
//         ROS_ERROR("Fill UAV params: please, specify the min lift force (max_thrust) that a rotor can produce.");

//     XmlRpc::XmlRpcValue rotors;
//     if (!nh_.getParam("multirotor/rotors", rotors))
//         ROS_ERROR("Fill UAV params: please, specify the position of the rotors.");
    
//     if (rotors.size() < 4)
//         ROS_ERROR("Please, specify more than 3 rotors. This node is intended for planar multirotors.");

//     uav_params_->n_rotors_ = rotors.size();
//     Eigen::MatrixXd S = Eigen::MatrixXd::Zero(6, rotors.size());

//     for (int32_t i = 0; i < rotors.size(); ++i)
//     {
//         XmlRpc::XmlRpcValue rotor = rotors[i];
//         double x = rotor["x"]; 
//         double y = rotor["y"]; 
//         double z = rotor["z"]; 

//         S(2, i) = 1.0; // Thrust
//         S(3, i) = y;   // Mx 
//         S(4, i) = -x;  // My 
//         S(5, i) = z*uav_params_->cm_/uav_params_->cf_; // Mz
//     }
//     uav_params_->tau_f_ = S;
// }

void UavDDPNode::callbackPose(const geometry_msgs::PoseStamped::ConstPtr& msg_pose)
{
    q0_.x() =  msg_pose->pose.orientation.x;
    q0_.y() =  msg_pose->pose.orientation.y;
    q0_.z() =  msg_pose->pose.orientation.z;
    q0_.w() =  msg_pose->pose.orientation.w;
    q0_.normalize();

    x0_.head(3) << msg_pose->pose.position.x, msg_pose->pose.position.y, msg_pose->pose.position.z;
    x0_.segment(3,4) << q0_.x(), q0_.y(), q0_.z(), q0_.w();
    // std::cout << "Callback pose!" << std::endl;
}

void UavDDPNode::callbackTwist(const geometry_msgs::TwistStamped::ConstPtr& msg_twist)
{
    x0_.tail(6) << msg_twist->twist.linear.x, msg_twist->twist.linear.y, msg_twist->twist.linear.z, msg_twist->twist.angular.x, msg_twist->twist.angular.y, msg_twist->twist.angular.z;

    // std::cout << "Callback twist!" << std::endl;
    // std::cout << x0_ << std::endl;
}

void UavDDPNode::publishControls()
{   
    // Header
    policy_msg_.header.stamp = ros::Time::now();

    // U desired for the current node 
    optiuavm::getGeneralizedTorque(ddp_problem_->get_runningDatas()[0], tau_);
    optiuavm::mapVector(tau_.tail(4), tau_min_.tail(4), tau_max_.tail(4), px4_tau_min_, px4_tau_max_, px4_tau_);

    std::cout << "Squash in: " << std::endl << fddp_->get_us()[0] << std::endl;
    std::cout << std::endl << "Squash out: " << std::endl << tau_<< std::endl;
    std::cout << "Tau PX4: " << std::endl << px4_tau_ << std::endl;

    policy_msg_.u_desired = std::vector<float>(uav_params_->n_rotors);
    policy_msg_.u_desired[0] = px4_tau_[1];
    policy_msg_.u_desired[1] = -px4_tau_[2];
    policy_msg_.u_desired[2] = -px4_tau_[3];
    policy_msg_.u_desired[3] = px4_tau_[0];    
    
    // State desired for the current node
    int ndx = fddp_->get_xs()[0].size();
    policy_msg_.x_desired = std::vector<float>(ndx);
    for (int ii = 0; ii < ndx; ++ii)
    {
        policy_msg_.x_desired[ii] = fddp_->get_xs()[0][ii];
    }

    policy_msg_.ffterm.mx = fddp_->get_k()[0][0];
    policy_msg_.ffterm.my = fddp_->get_k()[0][1];
    policy_msg_.ffterm.mz = fddp_->get_k()[0][2];
    policy_msg_.ffterm.th = fddp_->get_k()[0][3];

    pub_policy_.publish(policy_msg_);
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, ros::this_node::getName());
    UavDDPNode croco_node;
    ros::Rate loop_rate(500);

    // Multithread
    // ros::AsyncSpinner async_spinner_1(1, &croco_node.sb_pose_queue_);
    // ros::AsyncSpinner async_spinner_2(1, &croco_node.sb_twist_queue_);
    // async_spinner_1.start();
    // async_spinner_2.start();
    

    while (ros::ok())
    {    
        // croco_node.sb_pose_queue_.callOne();
        // croco_node.sb_twist_queue_.callOne();
        // croco_node.mut_twist0_.lock();
        // croco_node.mut_pose0_.lock();
        // croco_node.mut_twist0_.unlock();
        // croco_node.mut_pose0_.unlock();
        ros::spinOnce();
        croco_node.ddp_problem_->set_x0(croco_node.x0_);
        std::cout << "Initial state: " << std::endl << croco_node.x0_ << std::endl;
        croco_node.fddp_->solve(croco_node.fddp_->get_xs(), croco_node.fddp_->get_us(), 1);

        // if (croco_node.fddp_->get_stop() > 1e-5)
        // {
        //     ROS_WARN("Stop condition too big");
        // }

        croco_node.publishControls();

        loop_rate.sleep();
    }
    return 0;
}
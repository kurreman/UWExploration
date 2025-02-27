#include <rbpf_multiagent/rbpf_par_slam_multiagent_extension.hpp>

RbpfSlamMultiExtension::RbpfSlamMultiExtension(ros::NodeHandle &nh, ros::NodeHandle &nh_mb, string &base_link_custom_): RbpfSlam(nh, nh_mb){
    ROS_INFO("Inside RbpfSlamMultiExtension constructor");
    path_sub_.shutdown(); //shutdown the path subscriber to allow the survey area define the first inducing points.
    // base_frame_ = base_link_custom_ + "/base_link";
    // // nh_->param<string>(("mbes_link"), mbes_frame_, "mbes_link");
    // // nh_->param<string>(("base_link"), base_frame_, "base_link");
    // // nh_->param<string>(("odom_frame"), odom_frame_, "odom");
    // mbes_frame_ = base_link_custom_ + "/mbes_link";
    // odom_frame_ = base_link_custom_ + "/odom";
    // ROS_INFO("base_frame_ = %s", base_frame_.c_str());
    // The mission waypoints as a path
    // std::string fls_meas_topic;
    bool rbpf_sensor_FLS;
    bool rbpf_sensor_MBES;

    nh_->param<bool>(("rbpf_sensor_FLS"), rbpf_sensor_FLS, true);
    nh_->param<bool>(("rbpf_sensor_MBES"), rbpf_sensor_MBES, false);
    nh_->param<string>(("fls_meas_topic"), fls_meas_topic, "/sim/hugin_0/fls_measurement");
    nh_->param<string>(("namespace"), namespace_, "hugin_0");
    nh_->param("resampling_noise_covariance", res_noise_cov_, vector<float>());


    nh_->param<int>(("particle_count_neighbours"), pcn_, 5);
    nh_->param<float>(("measurement_std"), meas_std_, 0.01);
    nh_->param("init_covariance", init_cov_, vector<float>());
    nh_->param("motion_covariance", motion_cov_, vector<float>());
    nh_->param<int>(("n_beams_mbes"), beams_real_, 512);
    nh_->param<string>(("map_frame"), map_frame_, "map");
    nh_->param<string>(("vehicle_model"), vehicle_model_, "hugin");
    // nh_->param("fls_measurement_std", fls_measurement_std_, vector<float>());
    nh_->param<double>(("fls_range_std"), fls_measurement_std_range_, 0.01);
    nh_->param<double>(("fls_angle_std"), fls_measurement_std_angle_, 0.01);
    nh_->param<double>(("max_throttle"), max_throttle_,3);
    nh_->param<double>(("particle_spread_std_factor"), particle_spread_std_factor_, 1.0);
    nh_->param<string>(("comms_type"), comms_type_, "disabled");
    nh_->param<string>(("weight_slicing"), weight_slicing_, "top");
    nh_->param<string>(("pmp"), pmp_, "poly");

    if (pmp_ == "mono" && weight_slicing_ == "top")
    {
        ROS_ERROR("pmp_ = mono and weight_slicing_ = top is not a valid combination!");
        throw std::invalid_argument("pmp_ = mono and weight_slicing_ = top is not a valid combination!");
    }
    
    if (pmp_ == "mono" && pc_ != pcn_)
    {
        ROS_ERROR("pmp_ = mono and pc_ != pcn_ is not a valid combination!");
        throw std::invalid_argument("pmp_ = mono and pc_ != pcn_ is not a valid combination!");
    }
    // nh_mb_->param<float>(("rbpf_period"), rbpf_period_, 0.3);

    if(!rbpf_sensor_MBES){
        // cout << "rbpf_sensor_MBES is off" << endl;
        timer_rbpf_.stop();
    }

    // pcdebug
    if(rbpf_sensor_FLS){
        sub_fls_meas_ = nh_->subscribe(fls_meas_topic, rbpf_period_, &RbpfSlamMultiExtension::rbpf_update_fls_cb, this);
    }
    inducing_pts_sent = false;
    nh_->param<string>(("survey_area_topic"), survey_area_topic, "/multi_agent/survey_area");
    survey_area_sub_ = nh_->subscribe(survey_area_topic, 1, &RbpfSlamMultiExtension::survey_area_cb, this);
    
    //Rviz visualization
    std::string rbpf_markers_left_top;
    std::string rbpf_markers_right_top;
    nh_->param<string>(("markers_left_top"), rbpf_markers_left_top, "/markers_left");
    nh_->param<string>(("markers_right_top"), rbpf_markers_right_top, "/markers_right");
    vis_pub_left_ = nh_->advertise<visualization_msgs::MarkerArray>(rbpf_markers_left_top, 0);
    vis_pub_right_ = nh_->advertise<visualization_msgs::MarkerArray>(rbpf_markers_right_top, 0);
    // Timer for updating RVIZ
    nh_->param<float>(("rviz_period"), rviz_period_, 0.3);
    if(rviz_period_ != 0.){
        timer_neighbours_rviz_ = nh_->createTimer(ros::Duration(rviz_period_), &RbpfSlamMultiExtension::update_rviz_cb, this, false);
    }

    std::string wp_counter_topic;
    nh_->param<string>(("wp_counter_topic"), wp_counter_topic, "/wp_counter");
    wp_counter_sub_ = nh_->subscribe(wp_counter_topic, 1, &RbpfSlamMultiExtension::wp_counter_cb, this);


    //Setup neighbours
    RbpfSlamMultiExtension::setup_neighbours();

    //Neighbour prediction
    odom_sub_.shutdown(); //shutdown the odom subscriber to handle the ego predction within this script instead. Allows for more flexibility regarding "zero" odom.
    nh_->param<string>(("odometry_topic"), odom_top_, "odom");
    odom_sub_neigh_ = nh_->subscribe(odom_top_, 100, &RbpfSlamMultiExtension::odom_callback, this);
     // Start timing now
    time_neigh_ = ros::Time::now().toSec();
    old_time_neigh_ = ros::Time::now().toSec();

    nh_->param<string>(("z_hat_viz_top"), z_hat_viz_top_, "/z_hat_marker");
    z_hat_pub_ = nh_->advertise<visualization_msgs::Marker>(z_hat_viz_top_, 0);

    client_plots_ = nh_->serviceClient<plot_generator::PlotGenerator>("/plot_generator");
    // Timer for updating plots
    nh_->param<float>(("plot_period"), plot_period_, 1.0);
    //pcdebug
    if(plot_period_ != 0.){
        timer_generate_plots = nh_->createTimer(ros::Duration(plot_period_), &RbpfSlamMultiExtension::update_plots, this, false);
    }
    t_plot_old_ = ros::Time::now().toSec();

    //Setup particle sharing over comms
    if (comms_type_ != "disabled")
    {
        string ego_particles_topic = "/multi_agent/particles_" + namespace_;
        timer_pub_particles_ = nh_->createTimer(ros::Duration(0.5), &RbpfSlamMultiExtension::pub_particles_cb, this, false);
        pub_particles_ = nh_->advertise<geometry_msgs::PoseArray>(ego_particles_topic, 0); //TODO: fix message type

        if (auv_id_left_)
        {
            string neighbour_left_particles_topic = "/multi_agent/particles_" + vehicle_model_ + "_" + std::to_string(*auv_id_left_);
            sub_particles_left_ = nh_->subscribe(neighbour_left_particles_topic, 1, &RbpfSlamMultiExtension::sub_particles_left_cb, this);
        }
        if (auv_id_right_)
        {   
            string neighbour_right_particles_topic = "/multi_agent/particles_" + vehicle_model_ + "_" + std::to_string(*auv_id_right_);
            sub_particles_right_ = nh_->subscribe(neighbour_right_particles_topic, 1, &RbpfSlamMultiExtension::sub_particles_right_cb, this);
        }
    }
}   

void RbpfSlamMultiExtension::pub_particles_cb(const ros::TimerEvent& event)
{
    if(wp_counter_ % 2 != 0 || comms_type_ == "unlimited") //AUVs not facing each other, instead travelling "next to each other"
    {
        geometry_msgs::PoseArray pose_array_msg;
        pose_array_msg.header.frame_id = odom_frame_;
        pose_array_msg.header.stamp = ros::Time::now();
        for (const RbpfParticle& particle_m : particles_) //particle m
        {  
            geometry_msgs::Pose pose_msg;
            pose_msg.position.x = particle_m.p_pose_(0);
            pose_msg.position.y = particle_m.p_pose_(1);
            pose_msg.position.z = particle_m.p_pose_(2);
            float roll = particle_m.p_pose_(3);
            float pitch = particle_m.p_pose_(4);
            float yaw = particle_m.p_pose_(5);
            tf2::Quaternion q;
            q.setRPY(roll, pitch, yaw);
            pose_msg.orientation.x = q.x();
            pose_msg.orientation.y = q.y();
            pose_msg.orientation.z = q.z();
            pose_msg.orientation.w = q.w();
            pose_array_msg.poses.push_back(pose_msg);
        }
        pub_particles_.publish(pose_array_msg);
    }
}

void RbpfSlamMultiExtension::sub_particles_left_cb(const geometry_msgs::PoseArray& pose_array_msg)
{
    if(frontal_direction_ == -1 || comms_type_ == "unlimited") //Latest frontal AUV was the one on the left, now weŕe travelling along side the left AUV
    {
        ROS_INFO("namespace_ = %s replacing particles_left_", namespace_.c_str());
        RbpfSlamMultiExtension::update_particles_poses_from_pose_array(pose_array_msg, particles_left_);
    }
}

void RbpfSlamMultiExtension::sub_particles_right_cb(const geometry_msgs::PoseArray& pose_array_msg)
{
    if(frontal_direction_ == 1 || comms_type_ == "unlimited") //Latest frontal AUV was the one on the right, now weŕe travelling along side the right AUV
    {
        ROS_INFO("namespace_ = %s replacing particles_right_", namespace_.c_str());
        RbpfSlamMultiExtension::update_particles_poses_from_pose_array(pose_array_msg, particles_right_);
    }
}

void RbpfSlamMultiExtension::update_particles_poses_from_pose_array(const geometry_msgs::PoseArray& pose_array_msg, std::vector<RbpfParticle>& particles)
{
    
    if (pose_array_msg.poses.size() == particles.size())
    {
        for (int i = 0; i < pose_array_msg.poses.size(); i++)
        {
            particles[i].p_pose_(0) = pose_array_msg.poses[i].position.x;
            particles[i].p_pose_(1) = pose_array_msg.poses[i].position.y;
            particles[i].p_pose_(2) = pose_array_msg.poses[i].position.z;
            tf2::Quaternion q(pose_array_msg.poses[i].orientation.x, pose_array_msg.poses[i].orientation.y, pose_array_msg.poses[i].orientation.z, pose_array_msg.poses[i].orientation.w);
            tf2::Matrix3x3 m(q);
            double roll, pitch, yaw;
            m.getRPY(roll, pitch, yaw);
            // m.getRPY(particles[i].p_pose_(3), particles[i].p_pose_(4), particles[i].p_pose_(5));
            particles[i].p_pose_(3) = roll;
            particles[i].p_pose_(4) = pitch;
            particles[i].p_pose_(5) = yaw;
        }
    }
    else
    {
        ROS_WARN("pose_array_msg.poses.size() != particles.size()");
    }
}

// TODO: connect to topic with survey area boundaries
void RbpfSlamMultiExtension::survey_area_cb(const visualization_msgs::MarkerArray& marker_array)
{
    // ROS_INFO("IN MULTI EXTENSIONS!!!!");
    if (!inducing_pts_sent) //only send inducing points once, when receiving the survey area the first time.
    {
        inducing_pts_sent = true;
        ROS_INFO("Inside survey_area_cb");
        
        if (marker_array.markers[0].points.size() > 0) 
        {
            // // This service will start the auv simulation or auv_2_ros nodes to start the mission
            // nh_->param<string>(("synch_topic"), synch_top_, "/pf_synch");
            // // srv_server_ = nh_->advertiseService(synch_top_, &RbpfMultiagent::empty_srv, this);
            // srv_server_ = nh_->advertiseService(synch_top_, &RbpfSlam::empty_srv, (RbpfSlam*) this);
            if (!start_training_){
                ROS_INFO("Sending inducing points");
                std::vector<Eigen::RowVector3f> i_points;
                int ma_size = marker_array.markers[0].points.size() > 0;
                sensor_msgs::PointCloud2 ip_pcloud;
                Eigen::RowVector3f ip;

                auto points = marker_array.markers[0].points;

                for (int i = 0; i < ma_size; i++)
                {
                    ip << points[i].x, points[i].y, points[i].z; //<< is element-wise assignment of the three values to the right into the row vector ip
                    i_points.push_back(ip); //add ip to the end of the vector i_points
                }

                ip_pcloud = pack_cloud(map_frame_, i_points);
                start_training_ = true; // We can start to check for loop closures
                ip_pub_.publish(ip_pcloud);
                // This service will start the auv simulation or auv_2_ros nodes to start the mission
                nh_->param<string>(("synch_topic"), synch_top_, "/pf_synch");
                srv_server_multi_ = nh_->advertiseService(synch_top_, &RbpfSlamMultiExtension::empty_srv_multi, this);
            }
        }
        else{
            ROS_WARN("Received empty mission");
        }
    }
    // else{
    //     ROS_INFO("Survey area already received");
    // }

}

void RbpfSlamMultiExtension::wp_counter_cb(const std_msgs::Int32& wp_counter_msg)
{   
    wp_counter_ = wp_counter_msg.data;
    // ROS_INFO("Updating wp_counter_ to value %d", wp_counter_);
    RbpfSlamMultiExtension::update_frontal_neighbour_id();
    // ROS_INFO("namespace_ = %s", namespace_.c_str());
    // if (frontal_neighbour_id_)
    // {
    //     ROS_INFO("frontal_neighbour_id_ = %d", *frontal_neighbour_id_);
    // }
    // else
    // {
    //     ROS_INFO("frontal_neighbour_id_ = nullptr");
    // }
    // ROS_INFO("frontal_direction_ = %d", frontal_direction_);

}

void RbpfSlamMultiExtension::update_frontal_neighbour_id()
{   
    if (wp_counter_ % 2 == 0) //even wp and not the first
    {
    if (wp_counter_ != 0)
    {
        frontal_direction_ = -frontal_direction_;
    }
    if (frontal_direction_ == 1)
    {
        if (auv_id_right_)
        {
            frontal_neighbour_id_ = new int(*auv_id_right_);
        }
        else
        {
            // ROS_WARN("No frontal neighbour id available");
            frontal_neighbour_id_ = nullptr;
        }
        // frontal_neighbour_id_ = new int(*auv_id_right_);
    }
    else
    {
        if (auv_id_left_)
        {
            frontal_neighbour_id_ = new int(*auv_id_left_);
        }
        else
        {
            // ROS_WARN("No frontal neighbour id available");
            frontal_neighbour_id_ = nullptr;
        }
        // frontal_neighbour_id_ = new int(*auv_id_left_);
    }   
    }
    else if(wp_counter_ % 2 != 0)
    {
        // ROS_INFO("nullptr");
        frontal_neighbour_id_ = nullptr;
    }

}

bool RbpfSlamMultiExtension::empty_srv_multi(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res)
{
    ROS_DEBUG("RBPF ready");
    return true;
}

void RbpfSlamMultiExtension::rbpf_update_fls_cb(const auv_2_ros::FlsReading& fls_reading)
{
    // ROS_WARN("time diff = %f", ros::Time::now().toSec() - fls_reading.header.stamp.toSec());
    ros::Time timestamp = fls_reading.header.stamp;
    // particles_busy_ = true;
    if (wp_counter_ % 2 == 0) //Only if facing anoher auv, not when travelling in the same direction
    {
        // fls_meas_(0) = fls_reading.range;
        // fls_meas_(1) = fls_reading.angle;
        // fls_meas_(2) = frontal_neighbour_id_;
        // RbpfSlamMultiExtension::update_particles_weights(fls_reading.range, fls_reading.angle, frontal_neighbour_id_);
        // ROS_INFO("namespace_ = %s inside rbpf_update_fls_cb", namespace_.c_str());
        if (frontal_neighbour_id_)
        {
        // log current time
        auto start = std::chrono::high_resolution_clock::now();
        // ROS_INFO("before update_particles_weights, namespace_ = %s", namespace_.c_str());
        std::vector<Weight> weights = RbpfSlamMultiExtension::update_particles_weights(fls_reading.range.data, fls_reading.angle.data, frontal_neighbour_id_, timestamp);
        // ROS_INFO("before resample, namespace_ = %s", namespace_.c_str());

        double ESS = RbpfSlamMultiExtension::calc_ESS(weights);
        if (ESS == 0.0)
        {
            ROS_WARN("ESS is zero, is only allowed to be within [1,N]. Check your weights.");
            return;
        }
    
        if (ESS < std::round(pc_*pcn_/2)) //https://www.stats.ox.ac.uk/~doucet/doucet_johansen_tutorialPF2011.pdf
        {
            RbpfSlamMultiExtension::resample(weights);
        }
        else 
        {
            ROS_INFO(" Not resampling: ESS = %f !< %f", ESS, std::round(pc_*pcn_/2));
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> duration = end - start;
        // ROS_INFO("Time for resampling: %d", duration.count());
        // std::cout << "Duration of resampling step: " << duration.count() << " seconds" << std::endl;
        }
        else
        {
            ROS_WARN("No frontal neighbour id available");
        }
    // particles_busy_ = false;
    
    }
    // int neighbour_id = RbpfSlamMultiExtension::identify_frontal_neighbour_id();
    // cout << "Received FLS reading" << endl;
    // cout << fls_reading << endl;

    //Here implement measurement model and update weights, similar as RbpfSlam::rbpf_update

}

std::vector<Weight> RbpfSlamMultiExtension::update_particles_weights(const float &range, const float &angle, const int *fls_neighbour_id, const ros::Time timestamp)
{
    // ROS_INFO("namespace_ = %s", namespace_.c_str());
    // ROS_INFO("Updating particle weights using FLS measurement");
    // ROS_INFO("fls_neighbour_id = %d", *fls_neighbour_id);
    std::vector<Weight> weights;

    const std::vector<RbpfParticle>* particles_neighbour = nullptr;
    const Eigen::Matrix4f* oN2o_mat_ptr = nullptr; //Transformation matrix odom neighbour to odom self
    string neighbour_location;
    // ROS_INFO("0");
    if (auv_id_left_)
    {   
        // ROS_INFO("auv_id_left_ = %d", *auv_id_left_);
        if (*fls_neighbour_id == *auv_id_left_)
        {
            // ROS_INFO("1");
            particles_neighbour = &particles_left_;
            oN2o_mat_ptr = &oL2o_mat_;
            neighbour_location = "left";
        }
    }
    if (auv_id_right_)
    {
        // ROS_INFO("auv_id_right_ = %d", *auv_id_right_);
        if (*fls_neighbour_id == *auv_id_right_)
        {
            // ROS_INFO("2");
            particles_neighbour = &particles_right_;
            oN2o_mat_ptr = &oR2o_mat_;
            neighbour_location = "right";
        }
    }
    // ROS_INFO("mid");
    if (particles_neighbour && oN2o_mat_ptr) // Check if particles_neighbour is not nullptr
    {
        // ROS_INFO("3");
        // std::vector<Weight> weights;
        for (const RbpfParticle& particle_m : particles_) //particle m
        {
            // ROS_INFO("4");
            // std::lock_guard<std::mutex> lock(*particle_m.pc_mutex_);

            for (const RbpfParticle& n_particle_phi : *particles_neighbour) //neighbour particle phi
            {
                // std::lock_guard<std::mutex> lock2(*n_particle_phi.pc_mutex_);

                // ROS_INFO("5");
                Eigen::Vector4f n_point_Nodom;//HOMOGENOUS neighbour point in neighbour odom frame
                Eigen::Vector3f n_point; //neighbour point in self odom frame
                Eigen::Vector3f s_point; //self point in self odom frame
                const float PI = std::acos(-1.0f);
                n_point_Nodom.head(3) = n_particle_phi.p_pose_.head(3); 
                n_point_Nodom(3) = 1;
                //*oN2o_mat_ptr has been checked manually and is correct.
                n_point = ((*oN2o_mat_ptr) * n_point_Nodom).head(3); 
                s_point = particle_m.p_pose_.head(3); 
                float heading = particle_m.p_pose_(5); // in self odom frame
                double time_diff = ros::Time::now().toSec() - timestamp.toSec();
                float range_hat = (n_point.head(2) - s_point.head(2)).norm();//+5*time_diff*2;
                // float phi = std::atan2(n_point(1) - s_point(1), n_point(0) - s_point(0)) - PI/2;
                float phi = std::atan2(n_point(1) - s_point(1), n_point(0) - s_point(0));
                // float angle_hat = (-heading - phi - PI/2);
                // float angle_hat = fmod((-heading - phi - PI/2), (2 * PI));
                float angle_hat = phi-heading;
                int sgn = (angle_hat > 0) - (angle_hat < 0);
                // ROS_INFO("namespace_ = %s", namespace_.c_str());

                // ROS_INFO("sgn = %d", sgn);
                // ROS_INFO("angle_hat = %f", angle_hat);
                float angle_hat_alt = -sgn*(2*PI-sgn*angle_hat);
                //set angle_hat as the one of the smallest absolute value
                if (std::abs(angle_hat) > std::abs(angle_hat_alt))
                {
                    angle_hat = angle_hat_alt;
                }
                RbpfSlamMultiExtension::pub_estimated_measurement_to_rviz(s_point, n_point, odom_frame_);
                // ROS_INFO("namespace_ = %s", namespace_.c_str());
                // ROS_INFO("range = %f", range);
                // ROS_INFO("range_hat = %f", range_hat);
                // ROS_INFO("angle = %f", angle);
                // ROS_INFO("angle_hat = %f", angle_hat);
                // ROS_INFO("heading = %f", heading);
                // ROS_INFO("oN2o_mat_ptr contents:");

                // const Eigen::Matrix4f& oN2o_mat = *oN2o_mat_ptr; // Dereference the pointer to get the matrix
                // for (int i = 0; i < 4; ++i) {
                //     ROS_INFO_STREAM(" | " << oN2o_mat(i, 0) << " " << oN2o_mat(i, 1) << " " << oN2o_mat(i, 2) << " " << oN2o_mat(i, 3));
                // }
                // ROS_INFO("s_point = %f, %f, %f", s_point(0), s_point(1), s_point(2));
                // ROS_INFO("n_point = %f, %f, %f", n_point(0), n_point(1), n_point(2));

                //TODO(Koray): Include transformation to/from fls_frame. Right now real mesaurement is from fls_frame, while hat is from base_link. This is okay now sincethey're fused. To futureproof, this should be fixed (in case fls_frame is move from bein identical to base_link)
                Weight w;
                w.self_index = particle_m.index_;
                w.neighbour_index = n_particle_phi.index_;
                w.neighbour_location = neighbour_location;
                // std::vector<double> ego_cov_array = RbpfSlamMultiExtension::average_pose_with_cov(particles_).covariance;
                // std::vector<double> neigh_cov_array = RbpfSlamMultiExtension::average_pose_with_cov(*particles_neighbour).covariance;

                // Assuming covariance is of type boost::array<double, 36>
                // ROS_INFO("namespace_ = %s, 1", namespace_.c_str());
                // code_stage_ = 1;
                geometry_msgs::PoseWithCovariance pose_with_cov = RbpfSlamMultiExtension::average_pose_with_cov(particles_);
                // Convert boost::array<double, 36> to std::vector<double>
                // geometry_msgs::PoseWithCovariance pose_with_cov;
                std::vector<double> ego_cov_array(pose_with_cov.covariance.begin(), pose_with_cov.covariance.end());
                // ROS_INFO("namespace_ = %s, 2", namespace_.c_str());
                // code_stage_ = 2;
                
                geometry_msgs::PoseWithCovariance pose_with_cov_neigh = RbpfSlamMultiExtension::average_pose_with_cov(*particles_neighbour);
                // geometry_msgs::PoseWithCovariance pose_with_cov_neigh;
                std::vector<double> neigh_cov_array(pose_with_cov_neigh.covariance.begin(), pose_with_cov_neigh.covariance.end());
                // ROS_INFO("ego variance x,y,z = %f, %f, %f", ego_cov_array[0], ego_cov_array[7], ego_cov_array[14]);

                w.value = RbpfSlamMultiExtension::compute_weight(Eigen::Vector2f(range, angle).cast<double>(), Eigen::Vector2f(range_hat, angle_hat).cast<double>(), ego_cov_array, neigh_cov_array,time_diff);
                weights.push_back(w);
                //print the weight in the terminal
                // ROS_INFO("w.value = %f", w.value);
                // ROS_INFO("w.self_index = %d", w.self_index);
                // ROS_INFO("w.neighbour_index = %d", w.neighbour_index);
                // ROS_INFO("w.neighbour_location = %s", w.neighbour_location.c_str());

                // Use particle_m and particle_phi here
                
                
            }
        }
    }
    else
    {
        ROS_WARN("No neighbour particles or transformation matrix available");
    }
    
    return weights;
}

double RbpfSlamMultiExtension::compute_weight(const Eigen::VectorXd &z, const Eigen::VectorXd &z_hat, const std::vector<double> &ego_cov_array, const std::vector<double> &neigh_cov_array, const double &time_diff)
{
    //see log_pdf_uncorrelated in rbpf_particle.cpp adn combine with whiteboard notes.
    //Determine the covariance matrices gp_var and fls_sigma. 
    Eigen::VectorXd z_hat_exact = z_hat;//z.array()+0.001;
    double n = double(z.cols());
    double PI = std::acos(-1.0);
    // Eigen::VectorXd var_diag = gp_var.array() + std::pow(mbes_sigma, 2);
    //Vector of ones
    // Eigen::VectorXd var_diag = Eigen::Vector2d(1e-9,1e-9); // Add spread in x and y converted to range and angle of self particle set + neighbour particle set + FLS sensor noise 
    // ROS_INFO("fls_measurement_std_range_ == 1e-9 = %d", fls_measurement_std_range_ == 1e-9); 
    // ROS_INFO("fls_measurement_std_angle_ == 1e-9 = %d", fls_measurement_std_angle_ == 1e-9);
    //pointer for x_cov
    double ego_x_cov = ego_cov_array[0];
    double ego_y_cov = ego_cov_array[7];
    double neigh_x_cov = neigh_cov_array[0];
    double neigh_y_cov = neigh_cov_array[7];
    std::pair<double,double> p_ego = RbpfSlamMultiExtension::convert_cartesian_covariance_2_polar(ego_x_cov, ego_y_cov);
    std::pair<double,double> p_neigh = RbpfSlamMultiExtension::convert_cartesian_covariance_2_polar(neigh_x_cov, neigh_y_cov); //TODO(): Check if this is correct and make sense);
    // ROS_INFO("r_cov_ego = %f theta_cov_ego = %f", p_ego.first, p_ego.second);
    // ROS_INFO("x_cov_ego = %f y_cov_ego = %f", ego_x_cov, ego_y_cov);
    // ROS_INFO("r_cov_neigh = %f theta_cov_neigh = %f", p_neigh.first, p_neigh.second);
    // ROS_INFO("x_cov_neigh = %f y_cov_neigh = %f", neigh_x_cov, neigh_y_cov);
    Eigen::VectorXd var_diag = Eigen::Vector2d(std::pow(fls_measurement_std_range_,2),std::pow(fls_measurement_std_angle_,2)) + Eigen::Vector2d(std::pow(max_throttle_*time_diff*2,2),0) + particle_spread_std_factor_ * Eigen::Vector2d(p_ego.first, p_ego.second) + particle_spread_std_factor_ * Eigen::Vector2d(p_neigh.first, p_neigh.second); // Add spread in x and y converted to range and angle of self particle set + neighbour particle set + FLS sensor noise
    // ROS_WARN("ego_x_cov = %f, ego_y_cov = %f, neigh_x_cov = %f, neigh_y_cov = %f, namespace = %s", ego_x_cov, ego_y_cov, neigh_x_cov, neigh_y_cov,namespace_.c_str());
    // ROS_WARN("ego_r_cov = %f, ego_theta_cov = %f, neigh_r_cov = %f, neigh_theta_cov = %f, namespace = %s", p_ego.first, p_ego.second, p_neigh.first, p_neigh.second,namespace_.c_str());
    // ROS_ERROR("var_diag = %f, %f. namespace = %s", var_diag(0), var_diag(1),namespace_.c_str());
    // Eigen::VectorXd var_diag = Eigen::Vector2d(fls_measurement_std_range_,fls_measurement_std_angle_) + Eigen::Vector2d(std::pow(max_throttle_*time_diff*2,2),0);// + Eigen::Vector2d(p_ego.first, p_ego.second) + Eigen::Vector2d(p_neigh.first, p_neigh.second); // Add spread in x and y converted to range and angle of self particle set + neighbour particle set + FLS sensor noise
    
    Eigen::MatrixXd var_inv = var_diag.cwiseInverse().asDiagonal();
    Eigen::MatrixXd var_mat = var_diag.asDiagonal();
    Eigen::VectorXd diff = (z - z_hat_exact).array().transpose() * var_inv.array() * 
                            (z - z_hat_exact).array();
    double logl = -(n / 2.) * std::log(2*PI*std::pow(var_mat.determinant(),(1/n))) 
                  -(1 / 2.0) * diff.array().sum();
    double exp_logl = exp(logl);
    // ROS_ERROR("z-z_hat_exact = %f, %f", z(0)-z_hat_exact(0), z(1)-z_hat_exact(1));

    if (std::isnan(logl))
    {
        ROS_WARN("logl is nan. Check your covariance matrices, probably not invertible...");
    }
    // if (exp_logl < 1e-12)
    // {
    //     ROS_WARN("exp_logl is zero.");
    //     ROS_ERROR("logl = %f", logl);
    //     ROS_ERROR("exp(logl) = %f", exp(logl));
    //     ROS_ERROR("n = %f", n);
    //     ROS_ERROR("z = %f, %f", z(0), z(1));
    //     ROS_ERROR("z_hat_exact = %f, %f", z_hat_exact(0), z_hat_exact(1));
    //     ROS_ERROR("z-z_hat_exact = %f, %f", z(0)-z_hat_exact(0), z(1)-z_hat_exact(1));
    //     ROS_ERROR("var_diag = %f, %f", var_diag(0), var_diag(1));
    //     ROS_ERROR("var_inv = %f, %f", var_inv(0), var_inv(1));
    //     ROS_ERROR("var_inv = %f, %f", var_inv(2), var_inv(3));
    //     ROS_ERROR("var_det = %f", var_mat.determinant());
    //     ROS_ERROR("added time noise = %f", std::pow(max_throttle_*time_diff*2,2));
    //     ROS_ERROR("max_throttle_ = %f", max_throttle_);
    //     ROS_ERROR("time_diff = %f", time_diff);
    // }
    // // ROS_ERROR("term 1 = %f", -(n / 2.) * std::log(2*PI*std::pow(var_mat.determinant(),(1/n))));
    // ROS_ERROR("term 2 = %f", -(1 / 2.0) * diff.array().sum());
    // ROS_ERROR("logl = %f", logl);
    // ROS_ERROR("exp(logl) = %f", exp(logl));
    return exp_logl;
    // return 0;
}

std::pair<double,double> RbpfSlamMultiExtension::convert_cartesian_covariance_2_polar(const double x_cov, const double y_cov)
{
    double x_std = std::sqrt(x_cov);
    double y_std = std::sqrt(y_cov);
    if (x_cov <= 1e-6 && y_cov <= 1e-6)
    {
        // ROS_WARN("Standard deviation is zero");
        return std::make_pair(0,0);
    }
    double r_cov = x_cov + y_cov;
    double theta_cov = std::pow(std::atan2(y_std, x_std),2);
    // ROS_INFO("r = %f", r);
    // ROS_INFO("theta = %f", theta);
    // ROS_INFO("r_cov = %f", r_cov);
    // ROS_INFO("theta_cov = %f", theta_cov);
    return std::make_pair(r_cov, theta_cov);
}

double RbpfSlamMultiExtension::calc_ESS(const std::vector<Weight> &weights)
{
    //Normalize weights
    double sum = 0;
    std::vector<double> weights_values;
    for (const Weight& w : weights)
    {
        sum += w.value;
        // ROS_WARN("w.value = %f", w.value);
        // ROS_WARN("nampespace_ = %s w.value = %f, w.self_index = %d, w.neighbour_index = %d, w.neighbour_location = %s", namespace_.c_str(), w.value, w.self_index, w.neighbour_index, w.neighbour_location.c_str());
    }

    if (sum == 0)
    {
        ROS_WARN("Sum of weights is zero");
        for (const Weight& w : weights)
        {
            ROS_WARN("w.value = %f", w.value);
        }
        return 0.0;
    }

    for (const Weight& w : weights)
    {
        weights_values.push_back(w.value/sum);
    }

    // ROS_WARN("weights_values:");
    // for (int i = 0; i < weights_values.size(); i++)
    // {
    //     ROS_WARN("weights_values[%d] = %f", i, weights_values[i]);
    // }

    double ESS = 1/inner_product(begin(weights_values), end(weights_values), begin(weights_values), 0.0);
    return ESS;
}

void RbpfSlamMultiExtension::resample(std::vector<Weight> &weights)
{
   
    
    if (weight_slicing_ == "top")
    {
    //Shrink weights array to contain the maximum weights needed for computational reasons
    int max_pc = std::max(pc_, pcn_);
    //Refactor weights such that it consist of the top max_pc weights in terms of weight value. We still want weight to be an array of weights, but with size max_pc.
    std::vector<Weight> weights_refactored;
    std::sort(weights.begin(), weights.end(), [](const Weight& lhs, const Weight& rhs){return lhs.value > rhs.value;}); //sort weights in descending order
    for (int i = 0; i < max_pc; i++)
    {
        weights_refactored.push_back(weights[i]);
    }
    weights = weights_refactored;
    }

    //Normalize weights
    double sum = 0;
    std::vector<double> weights_values;
    for (const Weight& w : weights)
    {
        sum += w.value;
        // ROS_WARN("w.value = %f", w.value);
        // ROS_WARN("nampespace_ = %s w.value = %f, w.self_index = %d, w.neighbour_index = %d, w.neighbour_location = %s", namespace_.c_str(), w.value, w.self_index, w.neighbour_index, w.neighbour_location.c_str());
    }

    if (sum == 0)
    {
        ROS_WARN("Sum of weights is zero");
        for (Weight& w : weights)
        {
            ROS_WARN("w.value = %f", w.value);
        }
        return;
    }

    for (Weight& w : weights)
    {
        w.value = w.value / sum;
        weights_values.push_back(w.value);
    }

    // //print out weights_values 
    // ROS_WARN("weights_values:");
    // for (int i = 0; i < weights_values.size(); i++)
    // {
    //     ROS_WARN("weights_values[%d] = %f", i, weights_values[i]);
    // }

    // if (pc_ > pcn_)
    // {
    //     ESS_ego_ = 1/inner_product(begin(weights_values), end(weights_values), begin(weights_values), 0.0); //https://www.stats.ox.ac.uk/~doucet/doucet_johansen_tutorialPF2011.pdf
    //     ESS_neigh_ = 1/inner_product(begin(weights_values), begin(weights_values)+pcn_, begin(weights_values), 0.0);
    // }
    // else if (pcn_ > pc_)
    // {
    //     ESS_ego_ = 1/inner_product(begin(weights_values), begin(weights_values)+pc_, begin(weights_values), 0.0);
    //     ESS_neigh_ = 1/inner_product(begin(weights_values), end(weights_values), begin(weights_values), 0.0);
    // }
    // else
    // {
    //     ESS_ego_ = 1/inner_product(begin(weights_values), end(weights_values), begin(weights_values), 0.0);
    //     ESS_neigh_ = 1/inner_product(begin(weights_values), end(weights_values), begin(weights_values), 0.0);
    // }

    // //Resample
    int N = weights.size();
    if (N <=0)
    {
        ROS_WARN("N <= 0");
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dis(0, 1);
    double rand_n = dis(gen);

    vector<int> range = RbpfSlam::arange(0, N, 1);
    vector<double> positions(N); //vector of 0.0 (double) of size N
    vector<int> indexes(N, 0); //vector of 0 (int) of size N
    vector<double> cum_sum(N); //vector of 0.0 (double) of size N

    // make N subdivisions, and choose positions with a consistent random offset
    for(int i = 0; i < N; i++)
        positions[i] = (range[i] + rand_n) / double(N);

    partial_sum(weights_values.begin(), weights_values.end(), cum_sum.begin()); //cumulative sum of weights starting from beginning to end, storing results in cum_sum - starting at beginning of cum_sum

    for(int i = 0; i < N; i++)
    {
        // ROS_WARN("cum_sum[%d] = %f", i, cum_sum[i]);
        // ROS_WARN("rand_n = %f", rand_n);
        // ROS_WARN("positions[%d] = %f", i, positions[i]);
        // ROS_WARN("range[%d] = %d", i, range[i]);
        // ROS_WARN("weights_values[%d] = %f", i, weights_values[i]);
    }
    
    int i = 0;
    int j = 0;
    while(i < N)
    {   
        // ROS_WARN("j = %d, N = %d, i = %d", j, N, i);
        // ROS_WARN("positions[i] = %f, cum_sum[j] = %f", positions[i], cum_sum[j]);
        // ROS_WARN("indexes[i] = %d", indexes[i]);
        if (j >= N)
        {
            ROS_ERROR("j >= N. j = %d, N = %d, i = %d", j, N, i);
        }
        if(positions[i] < cum_sum[j])
        {
            indexes[i] = j;
            i++;
        }
        else
            j++;
    }
    // return indexes;
    // ------------------------------

    // //TEMPORARY: TODO(): FIX RESAMPLING ABOVE, the j index grows out of bounds...
    // for (int i = 0; i < N; i++)
    // {
    //     // if (indexes[i] >= N)
    //     // {
    //     //     ROS_WARN("Index out of bounds. indexes[%d] = %d, N = %d", i, indexes[i], N);
    //     // }
    //     indexes[i] = i;
    // }
    // // ------------------------------
    RbpfSlamMultiExtension::regenerate_particle_sets(indexes, weights); 
}

void RbpfSlamMultiExtension::regenerate_particle_sets(const vector<int> &indexes,const std::vector<Weight> &weights)
{
    //continue in here, comment out stuff and find out what causes the spread
    std::vector<RbpfParticle> particles_self_new;
    std::vector<RbpfParticle> particles_neighbour_new;

    std::vector<RbpfParticle>* particles_neighbour_ptr = nullptr;

    std::vector<int> particle_votes(pc_,0); //a vector of size pc_ filled with zeros
    std::vector<int> particle_votes_neighbour(pcn_,0);

    if (weight_slicing_ == "all" && weights.size() != pc_*pcn_)
    {
        ROS_ERROR("weights.size() != pc_*pcn_");
        return;
    }
    if (weight_slicing_ == "top" && weights.size() != std::max(pc_,pcn_))
    {
        ROS_ERROR("weights.size() != std::max(pc_,pcn_)");
        ROS_ERROR("weights.size() = %ld", weights.size());
        return;
    }
    if (weights.size() != indexes.size())
    {
        ROS_ERROR("weights.size() != indexes.size()");
        return;
    }

    if (indexes.size() == 0)
    {
        ROS_WARN("Indexes array for resampling is empty");
        return;
    }

    if (weights[0].neighbour_location == "left") {
        particles_neighbour_ptr = &particles_left_;
    } else if (weights[0].neighbour_location == "right") {
        particles_neighbour_ptr = &particles_right_;
    } else {
        ROS_WARN("No neighbour location available. Neighbour pointer is nullptr");
    }

    // std::vector<RbpfParticle> particles_copy_self = particles_;
    // std::vector<RbpfParticle> particles_copy_neighbour = *particles_neighbour_ptr;

    std::vector<int> dupes_s; //self
    std::vector<int> dupes_n; //neighbour
    // std::vector<int> married_ego_particles;
    // std::vector<int> married_neighbour_particles;
    // std::unordered_set<int> married_ego_particles;
    // std::unordered_set<int> married_neighbour_particles;
    std::vector<int> married_ego_particles(pc_,0);
    std::vector<int> married_neighbour_particles(pcn_,0);

    if (pmp_ == "poly")
    {
        for (int k=0; k < indexes.size(); k++)
        {   
            //generate a random integer r between 0 and indexes.size()-1
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, indexes.size()-1);
            int r = dis(gen);
            if (r >= indexes.size())
            {
                ROS_WARN("r >= indexes.size()");//. r = %d, indexes.size() = %d", r, indexes.size());
            }
            int i = indexes[r];
            if (i >= weights.size())
            {
                ROS_WARN("i >= weights.size()");//. i = %d, weights.size() = %d", i, weights.size());
            }
            const Weight w = weights[i];
            const int i_self = w.self_index;
            const int i_neighbour = w.neighbour_index;


            if (i_self >= particles_.size() || i_neighbour >= particles_neighbour_ptr->size()) {
                ROS_WARN("Index out of bounds");//. i_self = %d, i_neighbour = %d, r = %d, i = %d, indexes.size() = %d", i_self, i_neighbour, r, i, indexes.size());
                continue;
            }
            if (dupes_s.size() < pc_)
            {
                // std::lock_guard<std::mutex> lock(*particles_.at(i_self).pc_mutex_); //lock guard doesnt work the way I want/need it to
                // particles_self_new.emplace_back(particles_[i_self]);
                // particles_self_new.emplace_back(particles_copy_self[i_self]);
                if (pmp_ == "mono")
                {
                    // std::vector<int>::iterator it = std::find(married_neighbour_particles.begin(), married_neighbour_particles.end(), i_neighbour);
                    // bool already_married = it != married_neighbour_particles.end(); //find returns end if i is not found

                    // bool already_married = married_neighbour_particles.find(i_neighbour) != married_neighbour_particles.end();'
                    bool already_married = married_neighbour_particles[i_neighbour] != 0;
                    if (already_married)
                    {
                        ROS_INFO("Neighbour already married");
                        continue;
                    }
                }
                ROS_INFO("Adding ego particle %d to dupes_s", i_self);
                ROS_INFO("Neighbour particle adding is %d", i_neighbour);
                dupes_s.push_back(i_self);
                // married_neighbour_particles.push_back(i_neighbour);
                // married_neighbour_particles.insert(i_neighbour);
                married_neighbour_particles[i_neighbour]++;
                
            }
            if (dupes_n.size() < pcn_)//(k<pcn_)
            {
                // std::lock_guard<std::mutex> lock(*particles_neighbour_ptr->at(i_neighbour).pc_mutex_);
                // particles_neighbour_new.emplace_back((*particles_neighbour_ptr)[i_neighbour]);
                // particles_neighbour_new.emplace_back((particles_copy_neighbour)[i_neighbour]);
        
                if (pmp_ == "mono")
                {
                    // std::vector<int>::iterator it = std::find(married_ego_particles.begin(), married_ego_particles.end(), i_self);
                    // bool already_married = it != married_ego_particles.end(); //find returns end if i is not found

                    // bool already_married = married_ego_particles.find(i_self) != married_ego_particles.end();
                    bool already_married = married_ego_particles[i_self] != 0;
                    if (already_married)
                    {
                        ROS_INFO("Ego already married");
                        continue;
                    }
                }
                ROS_INFO("Adding neighbour particle %d to dupes_n", i_neighbour);
                ROS_INFO("Ego particle adding is %d", i_self);
                dupes_n.push_back(i_neighbour);
                // married_ego_particles.push_back(i_self);
                // married_ego_particles.insert(i_self);
                married_ego_particles[i_self]++;
            }
            // particle_votes[i_self] += 1;
            // particle_votes_neighbour[i_neighbour] += 1;
            // if (w.neighbour_location == "left")
            // {
            //     particles_neighbour_new.emplace_back(particles_left_[w.neighbour_index]);
            // }
            // else if (w.neighbour_location == "right")
            // {
            //     particles_neighbour_new.emplace_back(particles_right_[w.neighbour_index]);
            // }
            // else
            // {
            //     ROS_WARN("No neighbour location available");
            // }
        }
    }
    else if (pmp_ == "mono")
    {
        if (pc_ != pcn_)
        {
            ROS_ERROR("pc_ != pcn_");
            ROS_ERROR("Resampling skipped, for mono marital policy the number of particles in self and neighbour particle sets must be equal");
            return;
        }
        std::vector<int> ego_particles_best_neighbour_votes(pc_,-1); //filled with -1 to throw error if we havent changed value
        std::vector<int> neighbour_particles_best_ego_votes(pcn_,-1);
        std::vector<float> best_ego_w_values(pc_,0);
        std::vector<float> best_neighbour_w_values(pcn_,0);
        for(int k=0; k < weights.size(); k++)
        {
            const Weight w = weights[k];
            const float w_value = w.value;
            const int i_self = w.self_index;
            const int i_neighbour = w.neighbour_index;
            if (w_value > best_ego_w_values[i_self])
            
            {
                best_ego_w_values[i_self] = w_value;
                ego_particles_best_neighbour_votes[i_self] = i_neighbour;
            }
            if (w_value > best_neighbour_w_values[i_neighbour])
            {
                best_neighbour_w_values[i_neighbour] = w_value;
                neighbour_particles_best_ego_votes[i_neighbour] = i_self;
                }
        }

        dupes_n = ego_particles_best_neighbour_votes;
        dupes_s = neighbour_particles_best_ego_votes;
    }
        
    // if (ESS_ego_ < std::round(pc_/2))
    // {

    //Here check sizes of dupes 
    if (dupes_s.size() != pc_)
    {
        ROS_WARN("dupes_s.size() != pc_");
        ROS_WARN("dupes_s.size() = %ld, pc_ = %d", dupes_s.size(), pc_);
        return;
    }
    if (dupes_n.size() != pcn_)
    {
        ROS_WARN("dupes_n.size() != pcn_");
        ROS_WARN("dupes_n.size() = %ld, pcn_ = %d", dupes_n.size(), pcn_);
        return;
    }

    RbpfSlamMultiExtension::replace_lost_particles(dupes_s, &particles_);
    // Add noise to particles to avoid loss of variance. TODO() To the noise addition of the copy particle sets before assigning to global sets
    for(int i = 0; i < pc_; i++)
    {
        particles_[i].add_noise(res_noise_cov_);
    }
    // }
    // else
    // {
    //     ROS_INFO(" Not resampling ego particles: ESS_ego_ = %f !< %f", ESS_ego_, std::round(pc_/2));
    // }

    // if (ESS_neigh_ < std::round(pcn_/2))
    // {
    RbpfSlamMultiExtension::replace_lost_particles(dupes_n, particles_neighbour_ptr);
    for(int i = 0; i < pcn_; i++)
    {
        //Use the neighbour pointer
        if (particles_neighbour_ptr != nullptr) {
            particles_neighbour_ptr->at(i).add_noise(res_noise_cov_);
        }
    }
    // }
    // else
    // {
    //     ROS_INFO("Not resampling neighbour particles: ESS_neigh_ = %f !< %f", ESS_neigh_, std::round(pcn_/2));
    // }

    //normalize votes
    // int self_sum = std::accumulate(particle_votes.begin(), particle_votes.end(), 0);
    // int neighbour_sum = std::accumulate(particle_votes_neighbour.begin(), particle_votes_neighbour.end(), 0);
    // if (self_sum != 0 && neighbour_sum != 0)
    // {
    //     transform(particle_votes.begin(), particle_votes.end(), particle_votes.begin(), [&self_sum](int& c){return c/self_sum;});
    //     transform(particle_votes_neighbour.begin(), particle_votes_neighbour.end(), particle_votes_neighbour.begin(), [&neighbour_sum](int& c){return c/neighbour_sum;});
    // }
    // else
    // {
    //     ROS_WARN("Sum of votes is zero");
    //     return;
    // }
    // std::vector<int> particle_indexes_self = RbpfSlamMultiExtension::resample_particle_votes(particle_votes);
    // std::vector<int> particle_indexes_neighbour = RbpfSlamMultiExtension::resample_particle_votes(particle_votes_neighbour);

    // for (const int& i : particle_indexes_self)
    // {
    //     particles_self_new.emplace_back(particles_[i]);
    // }

    // for (const int& i : particle_indexes_neighbour)
    // {
    //     particles_neighbour_new.emplace_back((*particles_neighbour_ptr)[i]);
    // }
    
    
    
    

    // //Reassign particles
    // if (particles_self_new.size() == pc_)
    // {
    //     particles_ = particles_self_new;
    // }
    // else
    // {
    //     ROS_WARN("Resampling failed, too many or too few SELF particles regenerated!");
    // }
    // if (particles_neighbour_ptr != nullptr) {
    //     if (particles_neighbour_new.size() == pcn_)
    //     {
    //         *particles_neighbour_ptr = particles_neighbour_new;
    //     }
    //     else
    //     {
    //         ROS_WARN("Resampling failed, too many or too few NEIGHBOUR particles regenerated!");
    //     }
    // }
    // ROS_INFO("Particles resampled!");


    if (particles_.size() != pc_)
    {
        ROS_ERROR("Resampling failed, too many or too few SELF particles regenerated!");
    }
    if (auv_id_left_ && particles_left_.size() != pcn_)
    {
        ROS_ERROR("Resampling failed, too many or too few LEFT neighbour particles regenerated!");
    }
    if (!auv_id_left_ && particles_left_.size() != 0)
    {
        ROS_ERROR("LEFT neighbour particles generated, they should NOT exist!");
    }
    if (auv_id_right_ && particles_right_.size() != pcn_)
    {
        ROS_ERROR("Resampling failed, too many or too few RIGHT neighbour particles regenerated!");
    }
    if (!auv_id_right_ && particles_right_.size() != 0)
    {
        ROS_ERROR("RIGHT neighbour particles generated, they should NOT exist!");
    }

    // //Add noise to particles to avoid loss of variance. TODO() To the noise addition of the copy particle sets before assigning to global sets
    // for(int i = 0; i < pc_; i++)
    // {
    //     particles_[i].add_noise(res_noise_cov_);
    // }
    
    // for(int i = 0; i < pcn_; i++)
    // {
    //     //Use the neighbour pointer
    //     if (particles_neighbour_ptr != nullptr) {
    //         (*particles_neighbour_ptr)[i].add_noise(res_noise_cov_);
    //     }
    // }


}

void RbpfSlamMultiExtension::replace_lost_particles(std::vector<int>& dupes, std::vector<RbpfParticle>* particles_ptr)
{
    // ROS_INFO("inside replace_lost_particles");
    std::vector<int> dupe_stack = dupes;
    for(int i=0; i < dupes.size(); i++)
    {
        // if i is in dupes delete one instance of the value i inside dupe_stack
        std::vector<int>::iterator it = std::find(dupe_stack.begin(), dupe_stack.end(), i);
        bool found = it != dupe_stack.end(); //find returns end if i is not found
        if (found) 
        {
            // int index_i = it - dupe_stack.begin();
            // dupe_stack.erase(dupe_stack.begin() + index_i);
            dupe_stack.erase(it);
        } 
    }

    for(int i=0; i < dupes.size(); i++)
    {
        // if i is in dupes delete one instance of the value i inside dupe_stack
        std::vector<int>::iterator it = std::find(dupes.begin(), dupes.end(), i);
        bool found = it != dupes.end(); //find returns end if i is not found
        if (!found) //find returns end if i is not found
        {
            if (dupe_stack.empty())
            {
                ROS_WARN("dupe_stack is empty");
            }
            else
            {
                int p = dupe_stack.back();
                dupe_stack.pop_back();
                // particles_ptr->at(i) = particles_ptr->at(p);
                // particles_ptr->at(i).index_ = i;
                particles_ptr->at(i).p_pose_ = particles_ptr->at(p).p_pose_;
                //TODO(): Include below to allow SVGP to work when enabled.
                // particles_ptr->at(i).pos_history_ = particles_ptr->at(p).pos_history_;
                // particles_ptr->at(i).rot_history_ = particles_ptr->at(p).rot_history_;
            }
        } 
    }
}

std::vector<int> RbpfSlamMultiExtension::resample_particle_votes(std::vector<int> votes)
{
    
    int N = votes.size(); 
    // std::vector<int> indexes(N,0);
    std::vector<double> votes_normalized(N);
    //Normalize votes
    int sum = std::accumulate(votes.begin(), votes.end(), 0);
    if (sum != 0)
    {
        transform(votes.begin(), votes.end(), votes_normalized.begin(), [&sum](int& c){return c/sum;});
    }
    else
    {
        ROS_WARN("Sum of votes is zero");
        return std::vector<int>(); //return empty vector
    }

    // int N = votes.size(); 
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dis(0, 1);
    double rand_n = dis(gen);

    vector<int> range = RbpfSlam::arange(0, N, 1);
    vector<double> positions(N); //vector of 0.0 (double) of size N
    vector<int> indexes(N, 0); //vector of 0 (int) of size N
    vector<double> cum_sum(N); //vector of 0.0 (double) of size N

    // make N subdivisions, and choose positions with a consistent random offset
    for(int i = 0; i < N; i++)
        positions[i] = (range[i] + rand_n) / double(N);

    partial_sum(votes_normalized.begin(), votes_normalized.end(), cum_sum.begin()); //cumulative sum of weights starting from beginning to end, storing results in cum_sum - starting at beginning of cum_sum

    int i = 0;
    int j = 0;

    while(i < N)
    {
        if(positions[i] < cum_sum[j])
        {
            indexes[i] = j;
            i++;
        }
        else
            j++;
    }

    return indexes;
}

void RbpfSlamMultiExtension::pub_estimated_measurement_to_rviz(const Eigen::Vector3f& start, const Eigen::Vector3f& end, const std::string frame_id)
{
   
    visualization_msgs::Marker marker;
    marker.header.frame_id = frame_id;
    marker.header.stamp = ros::Time::now();
    
    marker.id = 0;
    marker.type = visualization_msgs::Marker::LINE_STRIP;
    marker.pose.orientation.w = 1.0;


    marker.scale.x = 0.5;
    
    marker.color.a = 1.0; 
    marker.color.r = 1.0;
    marker.color.g = 0.0;
    marker.color.b = 0.0;

    // markers.markers.push_back(marker);

    geometry_msgs::Point p_s;
    geometry_msgs::Point p_e;
    p_s.x = start(0);
    p_s.y = start(1);
    p_s.z = start(2);
    p_e.x = end(0);
    p_e.y = end(1);
    p_e.z = end(2);
    marker.points.push_back(p_s);
    marker.points.push_back(p_e);
        

    z_hat_pub_.publish(marker);
}


void RbpfSlamMultiExtension::setup_neighbours()
{
    // std::string namespace_;
    // int num_auvs_;
    // nh_->param<string>(("namespace"), namespace_, "hugin_0");
    nh_->param<int>(("num_auvs"), num_auvs_, 1);
    int auv_id = namespace_.back() - '0'; // ASCII code for 0 is 48, 1 is 49, etc. https://sentry.io/answers/char-to-int-in-c-and-cpp/#:~:text=C%20and%20C%2B%2B%20store%20characters,the%20value%20of%20'0'%20.
    auv_id_ = new int(auv_id);
    if (auv_id == 0 && num_auvs_ > 1)
    {
        // ROS_INFO("Inside RbpfMultiagent constructor: auv_id == 0");
        particles_right_ = RbpfSlamMultiExtension::init_particles_of(auv_id+1);
        auv_id_right_ = new int(auv_id+1);
        
    }
    else if (auv_id == num_auvs_-1 && num_auvs_ > 1)
    {
        // ROS_INFO("Inside RbpfMultiagent constructor: auv_id == num_auvs_-1");
        particles_left_ = RbpfSlamMultiExtension::init_particles_of(auv_id-1);
        auv_id_left_ = new int(auv_id-1);
    }
    else
    {
        // ROS_INFO("Inside RbpfMultiagent constructor: auv_id is neither 0 nor num_auvs_-1");
        particles_left_ = RbpfSlamMultiExtension::init_particles_of(auv_id-1);
        particles_right_ = RbpfSlamMultiExtension::init_particles_of(auv_id+1);
        auv_id_left_ = new int(auv_id-1);
        auv_id_right_ = new int(auv_id+1);
    }
    particle_sets_instantiated_ = true;

    //Initiate first FLS direction
    if (num_auvs_ % 2 == 0)
    {
        if (*auv_id_ % 2 == 0)
        {
            frontal_direction_ = 1;
        }
        else
        {
            frontal_direction_ = -1;
        }
    }
    else
    {
        if (*auv_id_ % 2 == 0)
        {
            frontal_direction_ = -1;
        }
        else
        {
            frontal_direction_ = 1;
        }
    }
    // Store transforms between auv odom and its two neighbours (if they exist)
    if (auv_id_left_)
    {
        try 
        {
            string odom_frame_left = vehicle_model_ + "_" + std::to_string(*auv_id_left_) + "/odom";
            tf::StampedTransform oL2o_tf; //tf from odom left to odom self

            tfListener_.waitForTransform(odom_frame_, odom_frame_left, ros::Time(0), ros::Duration(300.0));
            tfListener_.lookupTransform(odom_frame_, odom_frame_left, ros::Time(0), oL2o_tf);
            pcl_ros::transformAsMatrix(oL2o_tf, oL2o_mat_);
            
        }
        catch (const std::exception &e)
        {
            ROS_ERROR("ERROR: Could not lookup transform from odom_left to odom");
        }
    }
    if (auv_id_right_)
    {
        try
        {
            string odom_frame_right = vehicle_model_ + "_" + std::to_string(*auv_id_right_) + "/odom";
            tf::StampedTransform oR2o_tf; //tf from odom right to odom self
            tfListener_.waitForTransform(odom_frame_, odom_frame_right, ros::Time(0), ros::Duration(300.0));
            tfListener_.lookupTransform(odom_frame_, odom_frame_right, ros::Time(0), oR2o_tf);
            pcl_ros::transformAsMatrix(oR2o_tf, oR2o_mat_);
        }
        catch (const std::exception &e)
        {
            ROS_ERROR("ERROR: Could not lookup transform from odom_right to odom");
        }
    }
}

std::vector<RbpfParticle> RbpfSlamMultiExtension::init_particles_of(int agent_id)
{
    std::vector<RbpfParticle> particles;
    // string base_frame = "hugin_" + std::to_string(agent_id) + "/base_link";
    // string mbes_frame = "hugin_" + std::to_string(agent_id) + "/mbes_link";
    // string odom_frame = "hugin_" + std::to_string(agent_id) + "/odom";

    // nh_->param<int>(("particle_count_neighbours"), pcn_, 5);
    // nh_->param<float>(("measurement_std"), meas_std_, 0.01);
    // nh_->param("init_covariance", init_cov_, vector<float>());
    // nh_->param("motion_covariance", motion_cov_, vector<float>());
    // nh_->param<int>(("n_beams_mbes"), beams_real_, 512);
    // nh_->param<string>(("map_frame"), map_frame_, "map");
    // nh_->param<string>(("vehicle_model"), vehicle_model_, "hugin");

    string base_frame = vehicle_model_ + "_" + std::to_string(agent_id) + "/base_link";
    string mbes_frame = vehicle_model_ + "_" + std::to_string(agent_id) + "/mbes_link";
    string odom_frame = vehicle_model_ + "_" + std::to_string(agent_id) + "/odom";


    tf2_ros::TransformListener tf_listener(tf_buffer_);
    try
    {
        ROS_DEBUG("Waiting for transforms");
        auto asynch_1 = std::async(std::launch::async, [this,base_frame, mbes_frame]
                                       { return tf_buffer_.lookupTransform(base_frame, mbes_frame,
                                                                           ros::Time(0), ros::Duration(60.)); });

        auto asynch_2 = std::async(std::launch::async, [this,odom_frame]
                                       { return tf_buffer_.lookupTransform(map_frame_, odom_frame,
                                                                           ros::Time(0), ros::Duration(60.)); });

        tf::StampedTransform mbes_tf;
        geometry_msgs::TransformStamped tfmsg_mbes_base = asynch_1.get();
        tf::transformMsgToTF(tfmsg_mbes_base.transform, mbes_tf);
        pcl_ros::transformAsMatrix(mbes_tf, base2mbes_mat_);

        tf::StampedTransform m2o_tf;
        geometry_msgs::TransformStamped tfmsg_map_odom = asynch_2.get();
        tf::transformMsgToTF(tfmsg_map_odom.transform, m2o_tf);
        pcl_ros::transformAsMatrix(m2o_tf, m2o_mat_);

        ROS_INFO("Transforms locked - RBPF node");
    }
    catch (const std::exception &e)
    {
        ROS_ERROR("ERROR: Could not lookup transform from base_link to mbes_link");
    }

    // Initialize the particles on top of vehicle 
    tf::StampedTransform o2b_tf;
    tfListener_.waitForTransform(odom_frame, base_frame, ros::Time(0), ros::Duration(300.0));
    tfListener_.lookupTransform(odom_frame, base_frame, ros::Time(0), o2b_tf);
    double x, y, z, roll_o2b, pitch_o2b, yaw_o2b;
    x = o2b_tf.getOrigin().x();
    y = o2b_tf.getOrigin().y();
    z = o2b_tf.getOrigin().z();
    o2b_tf.getBasis().getRPY(roll_o2b, pitch_o2b, yaw_o2b);
    init_p_pose_(0)= x;
    init_p_pose_(1)= y;
    init_p_pose_(2)= z;
    init_p_pose_(3)= roll_o2b;
    init_p_pose_(4)= pitch_o2b;
    init_p_pose_(5)= yaw_o2b;

    // Create particles
    for (int i=0; i<pcn_; i++){
        particles.emplace_back(RbpfParticle(beams_real_, pcn_, i, base2mbes_mat_, m2o_mat_, init_p_pose_,
                                            init_cov_, meas_std_, motion_cov_));
    }
    return particles;
}

void RbpfSlamMultiExtension::update_rviz_cb(const ros::TimerEvent &)
{
    if (auv_id_left_) {
        geometry_msgs::PoseArray array_msg;
        array_msg = particles_2_pose_array(*auv_id_left_, particles_left_);
        pub_markers(array_msg, vis_pub_left_);
    }
    if (auv_id_right_) {
        geometry_msgs::PoseArray array_msg;
        array_msg = particles_2_pose_array(*auv_id_right_, particles_right_);
        pub_markers(array_msg, vis_pub_right_);
    }

}

geometry_msgs::PoseArray RbpfSlamMultiExtension::particles_2_pose_array(const int& id, const std::vector<RbpfParticle>& particles)
{
    geometry_msgs::PoseArray array_msg;
    array_msg.header.frame_id = vehicle_model_ + "_" + std::to_string(id) + "/odom";
    // array_msg.header.frame_id = vehicle_model_ + "_" + std::to_string(*auv_id_) + "/odom";

    array_msg.header.stamp = ros::Time::now();

    for (int i=0; i<pcn_; i++){
        geometry_msgs::Pose pose_i;
        pose_i.position.x = particles.at(i).p_pose_(0);
        pose_i.position.y = particles.at(i).p_pose_(1);
        pose_i.position.z = particles.at(i).p_pose_(2);

        Eigen::AngleAxisf rollAngle(particles.at(i).p_pose_(3), Eigen::Vector3f::UnitX());
        Eigen::AngleAxisf pitchAngle(particles.at(i).p_pose_(4), Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf yawAngle(particles.at(i).p_pose_(5), Eigen::Vector3f::UnitZ());
        Eigen::Quaternion<float> q = rollAngle * pitchAngle * yawAngle;
        pose_i.orientation.x = q.x();
        pose_i.orientation.y = q.y();
        pose_i.orientation.z = q.z();
        pose_i.orientation.w = q.w();

        array_msg.poses.push_back(pose_i);
    }
    // average_pose(array_msg);
    
    // if(false){
    //     pf_pub_.publish(array_msg);
    // }
    // else{
    //     pub_markers(array_msg);
    // }
    return array_msg;

}

void RbpfSlamMultiExtension::update_plots(const ros::TimerEvent &)
{
    if (!particles_busy_)
    {
        plot_generator::PlotGenerator srv;
        
        srv.request.ego.header.frame_id = vehicle_model_ + "_" + std::to_string(*auv_id_) + "/odom";
        srv.request.ego.header.stamp = latest_odom_stamp_;//ros::Time::now();
        if (latest_odom_stamp_ == ros::Time(0))
        {
            // ROS_WARN("latest_odom_stamp_ is ros::Time(0)");
            return;
        }
        // ROS_INFO("before ego average_pose_with_cov");
        // ROS_INFO("namespace_ = %s, 3", namespace_.c_str());
        // code_stage_ = 3;
        srv.request.ego.pose = RbpfSlamMultiExtension::average_pose_with_cov(particles_);
        
        // ROS_INFO("after ego average_pose_with_cov");

        if (auv_id_left_)
        {
            srv.request.left.header.frame_id = vehicle_model_ + "_" + std::to_string(*auv_id_left_) + "/odom";
            srv.request.left.header.stamp = latest_odom_stamp_;//ros::Time::now();
            // ROS_INFO("before left average_pose_with_cov");
            // ROS_INFO("namespace_ = %s, 4", namespace_.c_str());
            // code_stage_ = 4;
            srv.request.left.pose = RbpfSlamMultiExtension::average_pose_with_cov(particles_left_);
            srv.request.left_mean_dist = RbpfSlamMultiExtension::average_distance_to_ego_particles(particles_left_);
            // ROS_INFO("after left average_pose_with_cov");

        }
        else
        {
            srv.request.left.header.frame_id = "-1"; // -1 means no neighbour
        }
        if (auv_id_right_)
        {
            srv.request.right.header.frame_id = vehicle_model_ + "_" + std::to_string(*auv_id_right_) + "/odom";
            srv.request.right.header.stamp = latest_odom_stamp_;//ros::Time::now();
            // ROS_INFO("before right average_pose_with_cov");
            // ROS_INFO("namespace_ = %s, 5", namespace_.c_str());
            // code_stage_ = 5;
            srv.request.right.pose = RbpfSlamMultiExtension::average_pose_with_cov(particles_right_);
            srv.request.right_mean_dist = RbpfSlamMultiExtension::average_distance_to_ego_particles(particles_right_);
            // ROS_INFO("after right average_pose_with_cov");
        }
        else
        {
            srv.request.right.header.frame_id = "-1"; // -1 means no neighbour
        }

        if (!client_plots_.call(srv))
        {
            ROS_ERROR("Failed to call plot generator service");
        }
    }
    else
    {
        ROS_WARN("Particles are busy, not updating plots");
    }
}

std_msgs::Float32 RbpfSlamMultiExtension::average_distance_to_ego_particles(const std::vector<RbpfParticle>& particles)
{
    // return std_msgs::Float32();
    std_msgs::Float32 mean_distance_msg;
    const Eigen::Matrix4f* oN2o_mat_ptr = nullptr;

    
    if (&particles == &particles_left_)
    {
    
        oN2o_mat_ptr = &oL2o_mat_;
    }
    else if (&particles == &particles_right_)
    {
    
        oN2o_mat_ptr = &oR2o_mat_;
    }
    else
    {
        //CONTINUE HERE, all particles end upp here for some reason
        ROS_ERROR("particles is neither particles_left_ nor particles_right_");
        mean_distance_msg.data = -1;
        return mean_distance_msg;
    }

    

    int total_num_data_pts = 0;

    for (const RbpfParticle& particle : particles)
    {   
    
        Eigen::Vector4f n_point_Nodom;//HOMOGENOUS neighbour point in neighbour odom frame
    
        Eigen::Vector3f n_point; //neighbour point in self odom frame
    
        Eigen::Vector3f s_point; //self point in self odom frame
        
        n_point_Nodom.head(3) = particle.p_pose_.head(3); 
        
        n_point_Nodom(3) = 1;
        
        n_point = ((*oN2o_mat_ptr) * n_point_Nodom).head(3);

        for (const RbpfParticle& particle_self : particles_)
        {
            
            s_point = particle_self.p_pose_.head(3);
            
            mean_distance_msg.data += (s_point - n_point).norm();
            
            total_num_data_pts++; 
        }
    }
    
    mean_distance_msg.data = mean_distance_msg.data / total_num_data_pts; //CONTINUE HERE; continue in plot service and test
    
    return mean_distance_msg;
}

geometry_msgs::PoseWithCovariance RbpfSlamMultiExtension::average_pose_with_cov(const std::vector<RbpfParticle> particles)
{
    geometry_msgs::PoseWithCovariance pose;
    Eigen::Vector3f mean_pose = Eigen::Vector3f::Zero();
    Eigen::Quaternionf mean_quat = Eigen::Quaternionf::Identity();
    std::vector<float> covariances(36, 0.0);

    // Compute mean pose
    for (const RbpfParticle& particle : particles)
    {
        // std::lock_guard<std::mutex> lock(*particle.pc_mutex_);
        mean_pose += particle.p_pose_.head(3);
        Eigen::AngleAxisf rollAngle(particle.p_pose_(3), Eigen::Vector3f::UnitX());
        Eigen::AngleAxisf pitchAngle(particle.p_pose_(4), Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf yawAngle(particle.p_pose_(5), Eigen::Vector3f::UnitZ());
        Eigen::Quaternion<float> q = rollAngle * pitchAngle * yawAngle;
        mean_quat = mean_quat * q;
    }
    mean_pose = mean_pose / particles.size();
    mean_quat.x() = mean_quat.x() / particles.size();
    mean_quat.y() = mean_quat.y() / particles.size();
    mean_quat.z() = mean_quat.z() / particles.size();
    mean_quat.w() = mean_quat.w() / particles.size();
    pose.pose.position.x = mean_pose(0);
    pose.pose.position.y = mean_pose(1);
    pose.pose.position.z = mean_pose(2);
    mean_quat.normalize();
    pose.pose.orientation.x = mean_quat.x();
    pose.pose.orientation.y = mean_quat.y();
    pose.pose.orientation.z = mean_quat.z();
    pose.pose.orientation.w = mean_quat.w();

    double mean_roll, mean_pitch, mean_yaw;
    tf::Quaternion q;
    tf::quaternionMsgToTF(pose.pose.orientation, q);
    tf::Matrix3x3(q).getRPY(mean_roll, mean_pitch, mean_yaw);
    // Compute covariance in translation
    for (const RbpfParticle& particle : particles)
    {
        covariances[0] += std::pow(particle.p_pose_(0) - mean_pose(0), 2);
        covariances[7] += std::pow(particle.p_pose_(1) - mean_pose(1), 2);
        covariances[14] += std::pow(particle.p_pose_(2) - mean_pose(2), 2);
        
        covariances[21] += std::pow(particle.p_pose_(3) - mean_roll, 2);
        covariances[28] += std::pow(particle.p_pose_(4) - mean_pitch, 2);
        covariances[35] += std::pow(particle.p_pose_(5) - mean_yaw, 2);
    }
    covariances[0] = covariances[0] / particles.size();
    covariances[7] = covariances[7] / particles.size();
    covariances[14] = covariances[14] / particles.size();
    covariances[21] = covariances[21] / particles.size();
    covariances[28] = covariances[28] / particles.size();
    covariances[35] = covariances[35] / particles.size();
    pose.covariance[0] = covariances[0];
    pose.covariance[7] = covariances[7];
    pose.covariance[14] = covariances[14];
    pose.covariance[21] = covariances[21];
    pose.covariance[28] = covariances[28];
    pose.covariance[35] = covariances[35];
    // if (covariances[0] > 1e-6)
    // {   
    //     // ROS_INFO("namespace_ = %s", namespace_.c_str());
    //     ROS_ERROR("covariance x = %f namespace_ = %s code_stage_ = %d", covariances[0], namespace_.c_str(), code_stage_);
    //     // ros::Duration(3).sleep();
    //     // sleep(3);
    // }
    // if (covariances[7] > 1e-6)
    // {
    //     // ROS_INFO("namespace_ = %s", namespace_.c_str());
    //     // ROS_ERROR("covariance y = %f", covariances[7]);
    //     ROS_ERROR("covariance y = %f namespace_ = %s code_stage_ = %d", covariances[7], namespace_.c_str(), code_stage_);
    //     // ros::Duration(3).sleep();
    //     // sleep(3);
    // }
    return pose;
}

void RbpfSlamMultiExtension::pub_markers(const geometry_msgs::PoseArray& array_msg, const ros::Publisher& publisher)
{
    visualization_msgs::MarkerArray markers;
    int i = 0;
    string frame_id = array_msg.header.frame_id;
    for (geometry_msgs::Pose pose_i : array_msg.poses)
    {
        visualization_msgs::Marker marker;
        marker.header.frame_id = frame_id;
        marker.header.stamp = ros::Time();
        marker.ns = "markers";
        marker.id = i;
        marker.type = visualization_msgs::Marker::MESH_RESOURCE;
        marker.action = visualization_msgs::Marker::ADD;
        marker.pose.position.x = pose_i.position.x;
        marker.pose.position.y = pose_i.position.y;
        marker.pose.position.z = pose_i.position.z;
        marker.pose.orientation.x = pose_i.orientation.x;
        marker.pose.orientation.y = pose_i.orientation.y;
        marker.pose.orientation.z = pose_i.orientation.z;
        marker.pose.orientation.w = pose_i.orientation.w;
        marker.scale.x = 0.001;
        marker.scale.y = 0.001;
        marker.scale.z = 0.001;
        marker.color.a = 1.0; 
        marker.color.r = 0.0;
        marker.color.g = 1.0;
        marker.color.b = 0.0;
        marker.mesh_resource = "package://hugin_description/mesh/Hugin_big_meter.dae";
        markers.markers.push_back(marker);
        i++;
    }

    publisher.publish(markers);
}



void RbpfSlamMultiExtension::odom_callback(const nav_msgs::OdometryConstPtr& odom_msg)
{
    // ROS_INFO("namespace_ = %s", namespace_.c_str());
    // ROS_INFO("particles_.size() = %zu", particles_.size());
    // ROS_INFO("particles_left_.size() = %zu", particles_left_.size());
    // ROS_INFO("particles_right_.size() = %zu", particles_right_.size());
    // ROS_INFO("pc_ = %d", pc_);
    // ROS_INFO("pcn_ = %d", pcn_);
    // ROS_INFO("namespace_ = %s", namespace_.c_str());
    // if (frontal_neighbour_id_)
    // {
    //     ROS_INFO("frontal_neighbour_id_ = %d", *frontal_neighbour_id_);
    // }
    // else
    // {
    //     ROS_INFO("frontal_neighbour_id_ = nullptr");
    // }
    // ROS_INFO("frontal_direction_ = %d", frontal_direction_);
    // ROS_INFO("odom_callback");

    // if (ros::Time::now().toSec() - t_plot_old_ > plot_period_)
    // {
    //     t_plot_old_ = ros::Time::now().toSec();
    //     RbpfSlamMultiExtension::update_plots(ros::TimerEvent());
    // }

    // ROS_INFO("init_covariance = %f, %f, %f, %f, %f, %f", init_cov_[0], init_cov_[1], init_cov_[2], init_cov_[3], init_cov_[4], init_cov_[5]);
    // ROS_INFO("motion_covariance = %f, %f, %f, %f, %f, %f", motion_cov_[0], motion_cov_[1], motion_cov_[2], motion_cov_[3], motion_cov_[4], motion_cov_[5]);
    // ROS_INFO("resampling_noise_covariance = %f, %f, %f, %f, %f, %f", res_noise_cov_[0], res_noise_cov_[1], res_noise_cov_[2], res_noise_cov_[3], res_noise_cov_[4], res_noise_cov_[5]);
    // ROS_INFO("fls_range_std = %f", fls_measurement_std_range_);
    // ROS_INFO("fls_angle_std = %f", fls_measurement_std_angle_);
    // ROS_INFO("particle_spread_std_factor = %f", particle_spread_std_factor_);
    // ROS_INFO("weight_slicing_ = %s", weight_slicing_.c_str());
    // ROS_INFO("pmp_ = %s", pmp_.c_str());

    particles_busy_ = true;

    float tol = 0.001;
    bool zero_odom = abs(odom_msg->twist.twist.linear.x)  < tol && abs(odom_msg->twist.twist.linear.y) < tol && abs(odom_msg->twist.twist.linear.z) < tol &&
                    abs(odom_msg->twist.twist.angular.x) < tol && abs(odom_msg->twist.twist.angular.y) < tol && abs(odom_msg->twist.twist.angular.z) < tol;

    // ROS_INFO("zero_odom = %d", zero_odom);
    
    if (particle_sets_instantiated_ && !zero_odom)
    {
        latest_odom_stamp_ = odom_msg->header.stamp;
        // RbpfSlam::odom_callback(odom_msg); //Prediction of ego particles
        
        time_neigh_ = odom_msg->header.stamp.toSec();

        if(mission_finished_ != true)
        {
            // Motion prediction
            if (time_neigh_ > old_time_neigh_)
            {
                // Added in the MBES CB to synch the DR steps with the pings log
                nav_msgs::Odometry odom_cp = *odom_msg; // local copy
                // float dt = float(time_ - old_time_); //HERE LOOK YAMANIAC
                float dt = float(time_neigh_ - old_time_neigh_);
                // ROS_INFO("namespace_ = %s", namespace_.c_str());

                RbpfSlamMultiExtension::predict(odom_cp, dt, particles_, pred_threads_vec_);

                if (auv_id_left_)
                {
                    // ROS_INFO("Predicting left neighbour");
                    RbpfSlamMultiExtension::predict(odom_cp, dt,particles_left_, pred_threads_vec_neigh_left_);
                }
                if (auv_id_right_)
                {
                    // ROS_INFO("Predicting right neighbour");
                    RbpfSlamMultiExtension::predict(odom_cp, dt,particles_right_, pred_threads_vec_neigh_right_);
                }

            }
        }
        old_time_neigh_ = time_neigh_;
    }
    else if(zero_odom) //We still need to have correct delta_t when we start moving again
    {
        time_neigh_ = odom_msg->header.stamp.toSec();
        old_time_neigh_ = time_neigh_;
        
        time_ = odom_msg->header.stamp.toSec();
        old_time_ = time_;

    }
    particles_busy_ = false;
}

void RbpfSlamMultiExtension::predict(nav_msgs::Odometry odom_t, float dt, std::vector<RbpfParticle>& particles, std::vector<std::thread>& pred_threads_vec)
{
    // ROS_INFO("Inside predict");
    // Multithreading
    // auto t1 = high_resolution_clock::now();
    // Eigen::VectorXf noise_vec(6, 1);
    // ROS_INFO("frame_id = %s", odom_t.header.frame_id.c_str());
    // ROS_INFO("auv_id_ = %d", *auv_id_);
    
    std::vector<RbpfParticle> particles_copy = particles;
    Eigen::Vector3f vel_rot, vel_p;

    int pc;

    if (&particles == &particles_) //Compare variable adresses
    {
        pc = pc_;
        vel_rot = Eigen::Vector3f(odom_t.twist.twist.angular.x,
                                                odom_t.twist.twist.angular.y,
                                                odom_t.twist.twist.angular.z);

        // Linear vel
        vel_p = Eigen::Vector3f(odom_t.twist.twist.linear.x,
                                                odom_t.twist.twist.linear.y,
                                                odom_t.twist.twist.linear.z);
    }
    else
    {
        pc = pcn_;
        vel_rot = Eigen::Vector3f(odom_t.twist.twist.angular.x,
                                                odom_t.twist.twist.angular.y,
                                                -odom_t.twist.twist.angular.z);

        // Linear vel
        vel_p = Eigen::Vector3f(odom_t.twist.twist.linear.x,
                                                -odom_t.twist.twist.linear.y,
                                                odom_t.twist.twist.linear.z);
    }
    // Depth (read directly)
    float depth = odom_t.pose.pose.position.z;
    // ROS_INFO("size of particles = %d", particles.size());
    // ROS_INFO("vel_rot = %f, %f, %f", vel_rot(0), vel_rot(1), vel_rot(2));
    // ROS_INFO("vel_p = %f, %f, %f", vel_p(0), vel_p(1), vel_p(2));
    for(int i = 0; i < pc; i++)
    {
        // ROS_INFO("dt = %f", dt);
        // ROS_INFO("BEFORE pose of particle %d = %f, %f, %f", i, particles.at(i).p_pose_(0), particles.at(i).p_pose_(1), particles.at(i).p_pose_(2));
        // std::lock_guard<std::mutex> lock(*particles.at(i).pc_mutex_);
        pred_threads_vec.emplace_back(std::thread(&RbpfParticle::motion_prediction, 
                                    std::ref(particles_copy.at(i)), std::ref(vel_rot), std::ref(vel_p),
                                    depth, dt, std::ref(rng_)));
        // ROS_INFO("AFTER pose of particle %d = %f, %f, %f", i, particles.at(i).p_pose_(0), particles.at(i).p_pose_(1), particles.at(i).p_pose_(2));

    }

    for (int i = 0; i < pc; i++)
    {
        // ROS_INFO("Joining thread %d", i);
        if (pred_threads_vec[i].joinable())
        {
            pred_threads_vec[i].join();
        }
        // ROS_INFO("AFTER2 pose of particle %d = %f, %f, %f", i, particles.at(i).p_pose_(0), particles.at(i).p_pose_(1), particles.at(i).p_pose_(2));

    }
    pred_threads_vec.clear();

    particles = particles_copy;

    // // Particle to compute DR without filtering
    // dr_particle_.at(0).motion_prediction(vel_rot, vel_p, depth, dt, rng_);

}
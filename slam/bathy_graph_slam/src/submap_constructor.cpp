#include <bathy_graph_slam/submap_constructor.hpp>
#include <iomanip>      // std::setprecision

submapConstructor::submapConstructor(std::string node_name, ros::NodeHandle &nh) : node_name_(node_name), nh_(&nh)
{
    std::string pings_top, debug_pings_top, odom_top, enable_top, submap_top, sift_map_top;
    std::string lm_idx_top;
    nh_->param<std::string>("mbes_pings", pings_top, "/gt/mbes_pings");
    nh_->param<std::string>("odom_topic", odom_top, "/gt/odom");
    nh_->param<std::string>("map_frame", map_frame_, "map");
    nh_->param<std::string>("odom_frame", odom_frame_, "odom");
    nh_->param<std::string>("base_link", base_frame_, "base_frame");
    nh_->param<std::string>("mbes_link", mbes_frame_, "mbes_frame");
    nh_->param<std::string>("submaps_topic", submap_top, "/submaps");
    nh_->param<std::string>("landmarks_idx_topic", lm_idx_top, "/lm_idx");
    nh_->param<std::string>("survey_finished_top", enable_top, "enable");
    nh_->param<std::string>("sift_map_top", sift_map_top, "map_sift");

    // Synchronizer for MBES and odom msgs
    mbes_subs_.subscribe(*nh_, pings_top, 30);
    odom_subs_.subscribe(*nh_, odom_top, 30);
    synch_ = new message_filters::Synchronizer<MySyncPolicy>(MySyncPolicy(30), mbes_subs_, odom_subs_);
    synch_->registerCallback(&submapConstructor::pingCB, this);

    // Submaps publisher
    submaps_pub_ = nh_->advertise<sensor_msgs::PointCloud2>(submap_top, 10, false);
    lm_idx_pub_ = nh_->advertise<bathy_graph_slam::LandmarksIdx>(lm_idx_top, 10, false);

    enable_subs_ = nh_->subscribe(enable_top, 1, &submapConstructor::enableCB, this);
    // SIFT map subscriber. For known data association
    sift_map_subs_ = nh_->subscribe(sift_map_top, 1, &submapConstructor::siftMapCB, this);

    try
    {
        tflistener_.waitForTransform(base_frame_, mbes_frame_, ros::Time(0), ros::Duration(20.0));
        tflistener_.lookupTransform(base_frame_, mbes_frame_, ros::Time(0), tf_base_mbes_);
        ROS_INFO_NAMED(node_name_, " locked transform map --> odom");

        tflistener_.waitForTransform(map_frame_, odom_frame_, ros::Time(0), ros::Duration(20.0));
        tflistener_.lookupTransform(map_frame_, odom_frame_, ros::Time(0), tf_map_odom_);
        ROS_INFO_NAMED(node_name_, " locked transform base --> sensor");
    }
    catch (tf::TransformException &exception)
    {
        ROS_ERROR("%s", exception.what());
        ros::Duration(1.0).sleep();
    }

    // Initialize survey params
    submaps_cnt_ = 0;
    first_sift_map_ = true;

    // SLAM problem
    known_association_ = false;

    // GICP registration class
    gicp_reg_.reset(new SubmapRegistration());

    ROS_INFO_NAMED(node_name_, " initialized");
}

void submapConstructor::siftMapCB(const sensor_msgs::PointCloud2Ptr &sift_map_msg){
    if(first_sift_map_){
        sift_map_.reset(new PointCloudT);
        ROS_INFO("SIFT map received");
        pcl::fromROSMsg(*sift_map_msg, *sift_map_);
        first_sift_map_ = false;
    }
}


// Callback to finish mission
void submapConstructor::enableCB(const std_msgs::BoolPtr &enable_msg)
{
    if (enable_msg->data == true)
    {
        ROS_INFO_STREAM("Creating benchmark");
        benchmark::track_error_benchmark benchmark("real_data");
        PointsT gt_map = pclToMatrixSubmap(submaps_vec_);
        PointsT gt_track = trackToMatrixSubmap(submaps_vec_);
        ROS_INFO_STREAM("About to... " << gt_track.size());
        benchmark.add_ground_truth(gt_map, gt_track);
    }
}

void submapConstructor::pingCB(const sensor_msgs::PointCloud2Ptr &mbes_ping,
                            const nav_msgs::OdometryPtr &odom_msg)
{
    tf::Transform odom_base_tf;
    tf::poseMsgToTF(odom_msg->pose.pose, odom_base_tf);
    // Storing point cloud of mbes pings in mbes frame and current tf_odom_mbes_
    submap_raw_.emplace_back(mbes_ping, tf_map_odom_ * odom_base_tf * tf_base_mbes_);
    // submap_raw_.emplace_back(mbes_ping, odom_base_tf * tf_base_mbes_);

    if (submap_raw_.size() > 100)
    {
        this->addSubmap(submap_raw_);
        submap_raw_.clear();
    }
}

void submapConstructor::addSubmap(std::vector<ping_raw> submap_pings)
{
    // Store submap tf
    tf::Transform tf_submap_i = std::get<1>(submap_pings.at(submap_pings.size() / 2));
    ros::Time submap_time = std::get<0>(submap_pings.at(submap_pings.size() / 2))->header.stamp;

    // Create submap object
    // TODO: this should be done with the function in submaps.hpp
    SubmapObj submap_i;
    Eigen::Affine3d tf_affine;
    tf::transformTFToEigen(tf_submap_i, tf_affine);
    submap_i.submap_tf_ = tf_affine.matrix().cast<float>();

    for (ping_raw &ping_j : submap_pings)
    {
        PointCloudT pcl_ping;
        pcl::fromROSMsg(*std::get<0>(ping_j).get(), pcl_ping);
        pcl_ros::transformPointCloud(pcl_ping, pcl_ping, tf_submap_i.inverse() * std::get<1>(ping_j));
        submap_i.submap_pcl_ += pcl_ping;
    }

    submap_i.submap_pcl_.sensor_origin_ << submap_i.submap_tf_.translation();
    submap_i.submap_pcl_.sensor_orientation_ = submap_i.submap_tf_.linear();
    submap_i.submap_id_ = submaps_cnt_;

    submap_i.submap_pcl_.width = submap_i.submap_pcl_.size();
    submap_i.submap_pcl_.height = 1;
    submap_i.submap_pcl_.is_dense = true;
    submap_i.auv_tracks_ = Eigen::MatrixXd(3, 3);

    // Broadcast submap tf
    geometry_msgs::TransformStamped msg_map_submap;
    tf::StampedTransform tf_map_submap_stp(tf_submap_i,
                                           submap_time,
                                           map_frame_,
                                           "submap_" + std::to_string(submaps_cnt_));

    std::cout << "submap_" + std::to_string(submaps_cnt_) << std::endl;
    std::cout << "Position in map frame " << submap_i.submap_tf_.translation().transpose() << std::endl;
    tf::transformStampedTFToMsg(tf_map_submap_stp, msg_map_submap);
    static_broadcaster_.sendTransform(msg_map_submap);

    // // If we want to see the full pointcloud
    // sensor_msgs::PointCloud2 submap_msg;
    // pcl::toROSMsg(submap_i.submap_pcl_, submap_msg);
    // submap_msg.header.frame_id = "submap_" + std::to_string(submaps_cnt_);
    // submap_msg.header.stamp = ros::Time::now();
    // submaps_pub_.publish(submap_msg);

    // auto a1 = std::async(std::launch::async, [this, &msg] {
    //     return tf_buffer_.lookupTransform(frame_id_, msg->header.frame_id,
    //     msg->header.stamp, tr ansform_timeout_);

    // Save for testing
    // pcl::io::savePCDFileASCII ("/home/torroba/Downloads/submaps/submap_"+ std::to_string(submaps_cnt_)+".pcd",
    //                            submap_i.submap_pcl_);
    
    if (!known_association_){
        // Extract landmarks from submap point cloud
        PointCloudT::Ptr cloud_lm = this->extractLandmarksUnknown(submap_i);


        // Check for loop closures
        if(submaps_cnt_ > 1){
            this->checkForLoopClosures(submap_i);

            // Register overlapping submaps
            if(!submap_i.overlaps_idx_.empty()){
                SubmapObj submap_i_copy = submap_i;
                SubmapObj submap_trg = gicp_reg_->constructTrgSubmap(submaps_vec_, submap_i.overlaps_idx_);
                if(gicp_reg_->gicpSubmapRegistrationSimple(submap_trg, submap_i_copy)){
                    // submap_i_copy comes out already registered

                }
                submap_trg.submap_pcl_.clear();
            }
        }

        // Publish SIFT landmarks for the graph
        sensor_msgs::PointCloud2 submap_msg;
        pcl::toROSMsg(*cloud_lm, submap_msg);
        submap_msg.header.frame_id = "submap_" + std::to_string(submaps_cnt_);
        submap_msg.header.stamp = submap_time;
        // std::cout << setprecision (11) << "Submap time " << submap_time.toSec() << std::endl;
        submaps_pub_.publish(submap_msg);
    }
    else{
        // See which known landmarks are contained in the submap area
        PointCloudT::Ptr cloud_lm;
        std::vector<int> lm_idx;
        this->extractLandmarksKnown(submap_i, cloud_lm, lm_idx);

        // Check for loop closures 
        // This is unnecessary when using known landmarks. The LC will be detected
        // when adding the landmarks to the graph if they already exist
        // if(submaps_cnt_ > 1){
        //     this->checkForLoopClosures(submap_i);
        // }

        // Publish SIFT landmarks for the graph
        sensor_msgs::PointCloud2 submap_msg;
        pcl::toROSMsg(*cloud_lm, submap_msg);
        submap_msg.header.frame_id = map_frame_;
        submap_msg.header.stamp = submap_time;
        submaps_pub_.publish(submap_msg);
        
        // Publish their landmarks indexes as well
        bathy_graph_slam::LandmarksIdx lm_idx_msg;
        lm_idx_msg.header.stamp = submap_time;
        lm_idx_msg.idx.data = lm_idx;
        lm_idx_pub_.publish(lm_idx_msg);
    }

    submaps_vec_.push_back(submap_i);
    submaps_cnt_++;
}

PointCloudT::Ptr submapConstructor::extractLandmarksUnknown(SubmapObj& submap_i)
{
    // Parameters for sift computation
    const float min_scale = 0.01f;
    const int n_octaves = 6;
    const int n_scales_per_octave = 4;
    const float min_contrast = 0.005f;

    PointCloudT::Ptr cloud_xyz(new PointCloudT(submap_i.submap_pcl_));

    // Estimate the sift interest points using z values from xyz as the Intensity variants
    pcl::SIFTKeypoint<pcl::PointXYZ, pcl::PointWithScale> sift;
    pcl::PointCloud<pcl::PointWithScale> result;
    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>());
    sift.setSearchMethod(tree);
    sift.setScales(min_scale, n_octaves, n_scales_per_octave);
    sift.setMinimumContrast(min_contrast);
    sift.setInputCloud(cloud_xyz);
    sift.compute(result);

    std::cout << "No of points in original submap " << submap_i.submap_pcl_.size() << std::endl;
    std::cout << "No of SIFT points in the result are " << result.size() << std::endl;

    PointCloudT::Ptr cloud_temp(new PointCloudT);
    copyPointCloud(result, *cloud_temp);

    return cloud_temp;
}

void submapConstructor::extractLandmarksKnown(SubmapObj& submap_i, 
                                              PointCloudT::Ptr& landmarks, 
                                              std::vector<int>& lm_idx)
{    
    // Get submap corners
    bool submaps_in_map_tf = false;
    std::pair<int, corners> corners_i = getSubmapCorners(submaps_in_map_tf, submap_i);
    // Find map SIFT landmarks contained within the corners of submap_i 
    bool overlap_flag;
    int i = 0;
    landmarks.reset(new PointCloudT);

    for(PointT& point: sift_map_->points){
        overlap_flag = false;
        Vector3d point_v = Vector3f(point.x, point.y, point.z).cast<double>();
        // Check each SIFT point against the four edges of submap_i
        overlap_flag = checkSiftOverlap(std::get<1>(corners_i), point_v);
        if(overlap_flag == true){
            landmarks->points.push_back(point);
            lm_idx.push_back(i);
        }
        i++;
    }
}

void submapConstructor::checkForLoopClosures(SubmapObj& submap_i)
{
    // Look for submaps overlapping the latest one
    SubmapsVec submaps_prev;
    for (SubmapObj &submap_k : submaps_vec_)
    {
        // Don't look for overlaps between consecutive submaps
        if (submap_k.submap_id_ != submap_i.submap_id_ - 1)
        {
            submaps_prev.push_back(submap_k);
        }
    }

    // Submaps in map frame?
    bool submaps_in_map_tf = false;
    submap_i.findOverlaps(submaps_in_map_tf, submaps_prev);
    submaps_prev.clear();
    std::cout << "Overlap with submaps: ";
    for (unsigned int j = 0; j < submap_i.overlaps_idx_.size(); j++)
    {
        std::cout << submap_i.overlaps_idx_.at(j) << " " ;
    }
    std::cout << std::endl; 
}


int main(int argc, char **argv)
{

    ros::init(argc, argv, "submap_constructor_node");
    ros::NodeHandle nh("~");

    boost::shared_ptr<submapConstructor> submapper;
    submapper.reset(new submapConstructor("submap_constructor_node", nh));

    ros::spin();

    ROS_INFO_NAMED(ros::this_node::getName(), " finished");

    return 0;
}

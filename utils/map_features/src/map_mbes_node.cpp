#include <ros/ros.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/keypoints/sift_keypoint.h>
#include <pcl/point_types.h>
#include <pcl/features/normal_3d.h>
#include <pcl/io/pcd_io.h>

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <iostream>

#include <sensor_msgs/PointCloud2.h>
#include <nav_msgs/Odometry.h>
#include <std_srvs/Empty.h>
#include <std_srvs/EmptyRequest.h>
#include <std_srvs/EmptyResponse.h>

#include <eigen_conversions/eigen_msg.h>
#include <Eigen/Core>

typedef pcl::PointCloud<pcl::PointXYZ> PointCloudT;
typedef pcl::PointXYZ PointT;


class MapConstructor{

public:

    MapConstructor(std::string node_name, ros::NodeHandle &nh, std::string auv_name, ros::Publisher map_pub__,PointCloudT &mbes_map__ ) : node_name_(node_name), nh_(&nh),mbes_map_(mbes_map__)
    {
        // ROS_INFO("Initializing map constructor");
        std::string pings_top, odom_top, save_map_srv_name, map_topic;
        // nh_->param<std::string>("mbes_pings", pings_top, "/gt/mbes_pings");
        // nh_->param<std::string>("odom_topic", odom_top, "/gt/odom");
        nh_->param<std::string>("map_frame", map_frame_, "map");
        // nh_->param<std::string>("odom_frame", odom_frame_, "odom");
        // nh_->param<std::string>("base_link", base_frame_, "base_frame");
        // nh_->param<std::string>("mbes_link", mbes_frame_, "mbes_frame");
        // nh_->param<std::string>("map_topic", map_topic, "/map_mbes");
        nh_->param<bool>("publish_mbes_cloud", publish_mbes_, true);
        nh_->param<std::string>("mode", mode_, "sim");

        pings_top = "/" + mode_ + "/" + auv_name + "/mbes_pings";
        odom_top = "/" + mode_ + "/" + auv_name + "/odom";
        odom_frame_ = auv_name + "/odom";
        base_frame_ = auv_name + "/base_link";
        mbes_frame_ = auv_name + "/mbes_link";

        // Synchronizer for MBES and odom msgs
        mbes_subs_.subscribe(*nh_, pings_top, 30);
        odom_subs_.subscribe(*nh_, odom_top, 30);
        synch_ = new message_filters::Synchronizer<MySyncPolicy>(MySyncPolicy(30), mbes_subs_, odom_subs_);
        synch_->registerCallback(&MapConstructor::addPingCB, this);

        // Save map service
        // nh_->param<std::string>(("save_map_srv"), save_map_srv_name, "/pf_synch");
        save_map_srv_name = mode_ + "/" + auv_name + "/save_map";
        nh_->param<std::string>(("save_map_path"), save_map_path_, "~/.ros/");
        save_map_srv_ = nh_->advertiseService(save_map_srv_name, &MapConstructor::saveMap, this);

        // To publish map live
        // map_pub_ = nh_->advertise<sensor_msgs::PointCloud2>(map_topic, 2, false);
        map_pub_ = map_pub__;
        mbes_map_ = mbes_map__;

        try
        {
            tflistener_.waitForTransform(base_frame_, mbes_frame_, ros::Time(0), ros::Duration(30.0));
            tflistener_.lookupTransform(base_frame_, mbes_frame_, ros::Time(0), tf_base_mbes_);
            ROS_INFO_NAMED(node_name_, " locked transform base --> sensor");

            tflistener_.waitForTransform(map_frame_, odom_frame_, ros::Time(0), ros::Duration(30.0));
            tflistener_.lookupTransform(map_frame_, odom_frame_, ros::Time(0), tf_map_odom_);
            ROS_INFO_NAMED(node_name_, " locked transform map --> odom");
        }
        catch (tf::TransformException &exception)
        {
            ROS_ERROR("%s", exception.what());
            ros::Duration(1.0).sleep();
        }

    }

    bool saveMap(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res)
    {
        // Save
        ROS_INFO("Saving map");
        mbes_map_.width = mbes_map_.size();
        mbes_map_.height = 1;
        mbes_map_.is_dense = true;
        pcl::io::savePCDFileASCII(save_map_path_, mbes_map_);
        std::cout << "Size of map " << mbes_map_.size() << std::endl; 

        return true;
    }

    void addPingCB(const sensor_msgs::PointCloud2Ptr &mbes_ping,
                   const nav_msgs::OdometryPtr &odom_msg)
    {
        // ROS_INFO("Adding ping to map");
        tf::Transform odom_base_tf;
        tf::poseMsgToTF(odom_msg->pose.pose, odom_base_tf);
        
        PointCloudT pcl_ping;
        pcl::fromROSMsg(*mbes_ping, pcl_ping);
        pcl_ros::transformPointCloud(pcl_ping, pcl_ping, tf_map_odom_ * odom_base_tf * tf_base_mbes_);
        mbes_map_ += pcl_ping;

        if(publish_mbes_){
            // For live visualization in rviz
            pcl::toROSMsg(mbes_map_, *mbes_ping);
            mbes_ping->header.frame_id = map_frame_;
            map_pub_.publish(*mbes_ping);
        }
    }

    std::string node_name_;
    ros::NodeHandle *nh_;
    bool publish_mbes_;

    message_filters::Subscriber<sensor_msgs::PointCloud2> mbes_subs_;
    message_filters::Subscriber<nav_msgs::Odometry> odom_subs_;
    typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::PointCloud2, nav_msgs::Odometry> MySyncPolicy;
    message_filters::Synchronizer<MySyncPolicy> *synch_;

    ros::ServiceServer save_map_srv_;
    ros::Publisher map_pub_;

    std::string map_frame_, odom_frame_, base_frame_, mbes_frame_, save_map_path_,mode_;
    tf::TransformListener tflistener_;
    tf::StampedTransform tf_base_mbes_, tf_map_odom_;
    PointCloudT &mbes_map_;

// private:
//     PointCloudT &mbes_map_;
};


int main(int argc, char **argv)
{
    ros::init(argc, argv, "map_mbes_node");
    ros::NodeHandle nh("~");
    // tf_map_odom_
    // tf_base_mbes_
    // mbes_map_
    // map_frame_
    // map_pub_
    PointCloudT mbes_map_;
    ros::Publisher map_pub_;

    map_pub_ = nh.advertise<sensor_msgs::PointCloud2>("/map/fused_mbes_pings", 2, false);

    int num_auvs = nh.param<int>("num_auvs", 1);
    std::vector<boost::shared_ptr<MapConstructor>> map_constructors(num_auvs);
    for (int i = 0; i < num_auvs; i++)
    {
        std::string auv_name = "hugin_" + std::to_string(i);
        boost::shared_ptr<MapConstructor> map_constructor(new MapConstructor(auv_name+"_map_mbes_node", nh, auv_name, map_pub_, mbes_map_));    
        map_constructors[i] = map_constructor;
    }
    // boost::shared_ptr<MapConstructor> map_constructor(new MapConstructor("map_mbes_node", nh));

    // boost::shared_ptr<MapConstructor> map_constructor0(new MapConstructor("map_mbes_node", nh, "hugin_0", map_pub_, mbes_map_));
    // boost::shared_ptr<MapConstructor> map_constructor1(new MapConstructor("map_mbes_node", nh, "hugin_1", map_pub_, mbes_map_));
    // boost::shared_ptr<MapConstructor> map_constructor2(new MapConstructor("map_mbes_node", nh, "hugin_2", map_pub_, mbes_map_));
    // boost::shared_ptr<MapConstructor> map_constructor3(new MapConstructor("map_mbes_node", nh, "hugin_3", map_pub_, mbes_map_));


    ros::spin();

    return 0;
}
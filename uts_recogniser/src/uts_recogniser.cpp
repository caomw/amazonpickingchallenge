#include "include/uts_recogniser.h"

#include <boost/bind.hpp>
#include <unistd.h>

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/sync_policies/exact_time.h>

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>

#include <pcl/console/parse.h>

// publish poses topic name
const string g_object_topic_name = "/object_poses";
const string g_object_srv_name   = "/recog_publish_srv";
// service server name
const string g_target_srv_name  = "/data_publish_srv";

// json configuration file
//const string g_json_filename    = "/home/kanzhi/hydro_workspace/amazon_picking_challenge/uts_recogniser/data/amazon.json";

// object models, xml
//const string g_models_dir       = "/home/kanzhi/hydro_workspace/amazon_picking_challenge/uts_recogniser/data/amazon_models";

// method configuration
//const string g_method_filename  = "/home/kanzhi/hydro_workspace/amazon_picking_challenge/uts_recogniser/data/method.txt";

const string g_rgb_win_name     = "rgb_image";
const string g_mask_win_name    = "mask_image";



// constructor
UTSRecogniser::UTSRecogniser(ros::NodeHandle &nh, string json_file, string method_file, string kd_dir, string mask_dir, bool use_cloud)
    : exit_flag_(false)
    , sensor_empty_(true)
    , target_received_(false)
    , target_count_(0)
    , recogniser_done_(true)
    , image_captured_(true)
    , debug_(true){
    nh_ = & nh;
    json_filename_ = json_file;
    use_cloud_ = use_cloud;
    kd_dir_ = kd_dir;
    mask_dir_ = mask_dir;

    // cindex = 0;
    // imshow_data_ptr_ = 0;
    sensor_data_ptr_ = &sensor_data_;

    // load methods for all candidate objects, for amazon picking challenge
    // totally 27 objects, for uts dataset, totally 9 objects
    load_method_config( method_file );


    // resolve topic name
    xtion_rgb_topic_ = nh.resolveName( "/camera/lowres_rgb/image" );
    ROS_INFO( "subscribing to topic %s", xtion_rgb_topic_.c_str( ));
    xtion_rgb_info_topic_ = nh.resolveName("/camera/lowres_rgb/camera_info");
    ROS_INFO( "subscribing to topic %s", xtion_rgb_info_topic_.c_str( ));
    xtion_depth_topic_ = nh.resolveName("/camera/depth/image");
    ROS_INFO( "subscribing to topic %s", xtion_depth_topic_.c_str() );
    xtion_depth_info_topic_ = nh.resolveName( "/camera/depth/camera_info" );
    ROS_INFO( "subscribing to topic %s", xtion_depth_info_topic_.c_str() );

    camera_rgb_topic_ = nh.resolveName( "/camera/highres_rgb/image" );
    ROS_INFO( "subscribing to topic %s", camera_rgb_topic_.c_str());
    camera_rgb_info_topic_ = nh.resolveName( "/camera/highres_rgb/camera_info" );
    ROS_INFO( "subscribing to topic %s", camera_rgb_info_topic_.c_str());

    if ( use_cloud_ == true ) {
        xtion_cloud_topic_ = nh.resolveName( "/camera/points" );
        ROS_INFO( "subscribing to topic %s", xtion_cloud_topic_.c_str());
    }

    recog_pub_ = nh.advertise<apc_msgs::BinObjects>(g_object_topic_name, 100);
    recog_client_ = nh.serviceClient<apc_msgs::RecogStatus>( g_object_srv_name );
    //Open a window to display the image
//    cv::namedWindow(g_rgb_win_name);
//    cv::moveWindow( g_rgb_win_name, 0, 0 );
//    cv::namedWindow(g_mask_win_name);
//    cv::moveWindow( g_mask_win_name, 1280, 0 );

    string svm_model_name = "modelrgbkdes";
    string kdes_model_name = "kpcaRGBDes";
    string model_folder = string(kd_dir_);
    string model = "RGB Kernel Descriptor";
    unsigned int model_type = 2;
    kdr_.init_libkdes( svm_model_name, kdes_model_name, model_folder, model, model_type );
    ROS_INFO( "Initializing kernel descriptor recogniser" );

}

// desctructor
UTSRecogniser::~UTSRecogniser() {
    exit_flag_ = true;
    sensor_cond_.notify_all();
    process_thread_.interrupt();
    process_thread_.join();

    //Remove the window to display the image
//    cv::destroyWindow(g_rgb_win_name);
//    cv::destroyWindow( g_mask_win_name );
}


// sensor callback with point cloud
void UTSRecogniser::sensor_callback( const sensor_msgs::ImageConstPtr & rgb_image_msg,
                                         const sensor_msgs::CameraInfoConstPtr & rgb_image_info,
                                         const sensor_msgs::ImageConstPtr & depth_image_msg,
                                         const sensor_msgs::CameraInfoConstPtr & depth_image_info,
                                         const sensor_msgs::ImageConstPtr & camera_image_msg,
                                         const sensor_msgs::CameraInfoConstPtr & camera_image_info,
                                         const sensor_msgs::PointCloud2ConstPtr & cloud_ptr_msg ) {
    ROS_INFO_ONCE( "[sensor_callback] Sensor information available" );

    bool lrecg, icap;

    srvc_mutex_.lock();
    lrecg = recogniser_done_;
    icap = image_captured_;
    srvc_mutex_.unlock();

    if (!lrecg && !icap) {
        SensorData* data = sensor_data_ptr_;
        xtion_rgb_model_.fromCameraInfo( rgb_image_info );
        xtion_depth_model_.fromCameraInfo( depth_image_info );
        camera_rgb_model_.fromCameraInfo( camera_image_info );

        // set buffer info
        data->xtion_rgb_ptr     = cv_bridge::toCvCopy( rgb_image_msg, sensor_msgs::image_encodings::BGR8 );
        data->xtion_depth_ptr   = cv_bridge::toCvCopy( depth_image_msg, sensor_msgs::image_encodings::TYPE_32FC1 );
        data->use_cloud = true;
        data->xtion_cloud_ptr   = pcl::PointCloud<pcl::PointXYZRGB>::Ptr( new pcl::PointCloud<pcl::PointXYZRGB>( ));
        pcl::fromROSMsg( *cloud_ptr_msg, *(data->xtion_cloud_ptr) );

        data->camera_rgb_ptr    = cv_bridge::toCvCopy( camera_image_msg, sensor_msgs::image_encodings::BGR8 );
//        camera_rgb_model_.rectifyImage( data->camera_rgb_ptr->image, data->camera_rgb_ptr->image );
        {
            boost::mutex::scoped_lock lock(sensor_mutex_);
            sensor_empty_ = false;
        }
        sensor_cond_.notify_one();

        srvc_mutex_.lock();
        image_captured_ = true;
        srvc_mutex_.unlock();
    }
}


// sensor callback without point cloud
void UTSRecogniser::sensor_callback_no_cloud(const sensor_msgs::ImageConstPtr &rgb_image_msg,
                                                const sensor_msgs::CameraInfoConstPtr &rgb_image_info,
                                                const sensor_msgs::ImageConstPtr &depth_image_msg,
                                                const sensor_msgs::CameraInfoConstPtr &depth_image_info,
                                                const sensor_msgs::ImageConstPtr &camera_image_msg,
                                                const sensor_msgs::CameraInfoConstPtr &camera_image_info) {
    ROS_INFO_ONCE( "[sensor_callback] Sensor information available" );

    bool lrecg, icap;

    srvc_mutex_.lock();
    lrecg = recogniser_done_;
    icap = image_captured_;
    srvc_mutex_.unlock();
    if (!lrecg && !icap) {
        SensorData* data = sensor_data_ptr_;
        xtion_rgb_model_.fromCameraInfo( rgb_image_info );
        xtion_depth_model_.fromCameraInfo( depth_image_info );
        camera_rgb_model_.fromCameraInfo( camera_image_info );

        // set buffer info
        data->xtion_rgb_ptr     = cv_bridge::toCvCopy( rgb_image_msg, sensor_msgs::image_encodings::BGR8 );
        data->xtion_depth_ptr   = cv_bridge::toCvCopy( depth_image_msg, sensor_msgs::image_encodings::TYPE_32FC1 );
        data->use_cloud = false;

        data->camera_rgb_ptr    = cv_bridge::toCvCopy( camera_image_msg, sensor_msgs::image_encodings::BGR8 );
//        camera_rgb_model_.rectifyImage( data->camera_rgb_ptr->image, data->camera_rgb_ptr->image );
        {
            boost::mutex::scoped_lock lock(sensor_mutex_);
            sensor_empty_ = false;
        }
        sensor_cond_.notify_one();

        srvc_mutex_.lock();
        image_captured_ = true;
        srvc_mutex_.unlock();
    }
}



/** callback for target object request */

bool UTSRecogniser::target_srv_callback(apc_msgs::TargetRequest::Request &req,
                                            apc_msgs::TargetRequest::Response &resp) {
    srvc_mutex_.lock();

    if ( recogniser_done_ ) {
        ROS_INFO_ONCE("[target_srv_callback] target item request received");
        // srv index and data on disk
//        cout << "lrecg and icap: " << recogniser_done_ << ", " << image_captured_ << endl;


        recogniser_done_ = false;
        image_captured_  = false;

        // set data index value
        srv_bin_id_ = req.BinID;
        srv_object_index_ = req.ObjectIndex;
        srv_rm_object_indices_.clear();
        for ( int i = 0; i < (int)req.RemovedObjectIndices.size(); ++ i )
            srv_rm_object_indices_.push_back( (int)req.RemovedObjectIndices[i] );
        srv_object_name_ = req.ObjectName;
        srv_bin_contents_.clear();
        for ( int i = 0; i < (int)req.BinContents.size(); ++ i )
            srv_bin_contents_.push_back( req.BinContents[i] );

        ROS_INFO( "Target item bin index %s", srv_bin_id_.c_str() );

        resp.Found = true;
        target_received_ = true;
        srvc_mutex_.unlock();
    }
    else {
        resp.Found = false;
        srvc_mutex_.unlock();
    }
    return true;
}



/** main processing function */
void UTSRecogniser::process() {
    while ( !exit_flag_ ) {
        ROS_INFO_ONCE( "[process] Process function" );
        {
            boost::mutex::scoped_lock lock(sensor_mutex_);
            while(sensor_empty_) {
                sensor_cond_.wait( lock );
            }
        }


        {
            SensorData * data = sensor_data_ptr_;


            if ( data->xtion_rgb_ptr->image.rows != 0 && data->xtion_rgb_ptr->image.cols != 0 &&
                 data->xtion_depth_ptr->image.rows != 0 && data->xtion_depth_ptr->image.cols != 0 &&
                 data->camera_rgb_ptr->image.rows != 0 && data->camera_rgb_ptr->image.cols != 0 ) {

//                // convert depth image scale
//                double min;
//                double max;
//                cv::minMaxIdx(data->xtion_depth_ptr->image, &min, &max);
//                cv::convertScaleAbs(data->xtion_depth_ptr->image, data->xtion_depth_ptr->image, 255/max);

                apc_msgs::BinObjects bin_objs;

                ROS_INFO("Load mask images with index %s", srv_bin_id_.c_str());
                string xtion_rgb_mask_path  = mask_dir_ + "/mask_xtion_rgb_" + srv_bin_id_ + ".png";
                // load mask image
                cv::Mat xtion_rgb_mask = cv::imread( xtion_rgb_mask_path, CV_LOAD_IMAGE_GRAYSCALE );

                // kernel descriptor recogniser
                cv::Mat rgb_image = data->xtion_rgb_ptr->image.clone();
                cv::Mat depth_image = data->xtion_depth_ptr->image.clone();
                cv::Mat empty_image = cv::imread( string(mask_dir_+"/"+srv_bin_id_+"_empty.png"), CV_LOAD_IMAGE_COLOR );
                cv::Mat empty_depth_image = cv::imread( string(mask_dir_+"/"+srv_bin_id_+"_depth_empty.png"), CV_LOAD_IMAGE_ANYDEPTH );



                kdr_.load_sensor_data( rgb_image, depth_image );
                kdr_.load_info( empty_image, xtion_rgb_mask, empty_depth_image );
                kdr_.set_env_configuration( srv_object_name_, srv_bin_contents_ );
                vector<pair<string, vector<cv::Point> > > kd_results = kdr_.process();

                for ( int i = 0; i < (int)kd_results.size(); ++ i ) {
                    cv::putText( rgb_image, kd_results[i].first, kd_results[i].second[0], CV_FONT_HERSHEY_SIMPLEX|CV_FONT_ITALIC, 0.5, cv::Scalar(0,0,255));
                    for ( int j = 0; j < (int)kd_results[i].second.size(); ++ j )
                        cv::line( rgb_image, kd_results[i].second[j], kd_results[i].second[(j+1)%kd_results[i].second.size()], cv::Scalar(255, 0, 255), 2 );
                }
                cv::imshow( "bbox_image", rgb_image );
                cv::waitKey(0);
                kdr_.clear();

                // check the item class
                for ( int i = 0; i < (int)srv_bin_contents_.size(); ++ i ) {
                    apc_msgs::Object obj;
                    ROS_INFO( "Start recognising item %s in bin %s", srv_bin_contents_[i].c_str(), srv_bin_id_.c_str() );
                    if ( methods_.find( srv_bin_contents_[i] ) == methods_.end() ) {
                        obj.name = srv_bin_contents_[i];
                        obj.use_pose = false;
                        for ( int j = 0; j < kd_results[i].second.size(); ++ j ) {
                            obj.convex_hull_x.push_back( kd_results[i].second[j].x );
                            obj.convex_hull_y.push_back( kd_results[i].second[j].y );
                        }
                    }
                }


                // client to call recogniser notification
                apc_msgs::RecogStatus recog_status;
                recog_status.request.recog = true;
                if( recog_client_.call(recog_status) ) {
                    ROS_INFO( "return status: %s", recog_status.response.pub? "true" : "false" );
                }
                else {
                    ROS_ERROR( "Failed to call service %s", g_object_srv_name.c_str() );
                }

                recog_pub_.publish( bin_objs );



                srvc_mutex_.lock();
                recogniser_done_ = true;
                srvc_mutex_.unlock();
//                srvc_cond_.notify_one();
                sensor_mutex_.lock();
                sensor_empty_ = true;
                sensor_mutex_.unlock();
            }
        }

        if ( !exit_flag_ ) {
            boost::unique_lock<boost::mutex> sensor_lock( sensor_mutex_ );
            sensor_empty_ = true;
        }
        // call the robotic service to receive the pose/item

    }
}


/** load method configuration method */
void UTSRecogniser::load_method_config( string filename ) {
    ROS_INFO( "Read method configuration file from %s", filename.c_str() );
    ifstream in( filename.c_str() );
    string line;
    getline( in, line );
    vector<string> seg_line;
    while ( getline(in, line) ) {
        boost::algorithm::split( seg_line, line, boost::algorithm::is_any_of(" ") );
        if ( seg_line[1] == "rgb" )
            methods_.insert( make_pair(seg_line[0], RGB_RECOG) );
        else if ( seg_line[1] == "rgbd" )
            methods_.insert( make_pair(seg_line[0], RGBD_RECOG) );
        ROS_INFO( "Object name %s and method %s ", seg_line[0].c_str(), seg_line[1].c_str());
        seg_line.clear();
    }
}


void UTSRecogniser::start_monitor( void ) {
    // service target object
    ros::ServiceServer target_srv = nh_->advertiseService( g_target_srv_name, &UTSRecogniser::target_srv_callback, this );

    // subscribe to sensors
    xtion_rgb_sub_.subscribe(*nh_, xtion_rgb_topic_, 1 );
    xtion_rgb_info_sub_.subscribe( *nh_, xtion_rgb_info_topic_, 1 );
    xtion_depth_sub_.subscribe( *nh_, xtion_depth_topic_, 1 );
    xtion_depth_info_sub_.subscribe( *nh_, xtion_depth_info_topic_, 1 );
    camera_rgb_sub_.subscribe( *nh_, camera_rgb_topic_, 1 );
    camera_rgb_info_sub_.subscribe( *nh_, camera_rgb_info_topic_, 1 );

    if ( use_cloud_ == true ) {
        xtion_cloud_sub_.subscribe( *nh_, xtion_cloud_topic_, 1 );
        m_sensor_sync_.reset( new message_filters::Synchronizer<sensor_sync_policy>( sensor_sync_policy(100), xtion_rgb_sub_, xtion_rgb_info_sub_, xtion_depth_sub_, xtion_depth_info_sub_, camera_rgb_sub_, camera_rgb_info_sub_, xtion_cloud_sub_) );
        m_sensor_sync_->registerCallback( boost::bind( &UTSRecogniser::sensor_callback, this, _1, _2, _3, _4, _5, _6, _7 ) );
    }
    else {
        m_no_cloud_sensor_sync_.reset( new message_filters::Synchronizer<no_cloud_sensor_sync_policy>( no_cloud_sensor_sync_policy(10), xtion_rgb_sub_, xtion_rgb_info_sub_, xtion_depth_sub_, xtion_depth_info_sub_, camera_rgb_sub_, camera_rgb_info_sub_  ) );
        m_no_cloud_sensor_sync_->registerCallback( boost::bind( &UTSRecogniser::sensor_callback_no_cloud, this, _1, _2, _3, _4, _5, _6 ) );
    }
    process_thread_ = boost::thread(boost::bind(&UTSRecogniser::process, this));
    ros::MultiThreadedSpinner spinner(6);
    ros::Rate loop(10);
    while ( ros::ok() ) {
        spinner.spin();
        loop.sleep();
    }
}

void print_usage( char * prog_name ) {
    std::cout << "\nUsage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "-------------------------------------------\n"
              << "\t-j <string>                 json file configuration\n"
              << "\t-mask <string>              mask image directory, empty shelf and mask image\n"
              << "\t-method <string>            method file\n"
              << "\t-kd <string>                kernel descriptor folder\n"
              << "\t-h                          this help\n"
              << "\n\n";
}


int main( int argc, char ** argv ) {
    string json_file, method_file, kd_dir, mask_dir;
    if ( pcl::console::parse(argc, argv, "-mask", mask_dir) >= 0 &&
         pcl::console::parse(argc, argv, "-method", method_file) >= 0 &&
         pcl::console::parse(argc, argv, "-kd", kd_dir) >= 0 &&
         pcl::console::parse(argc, argv, "-j", json_file) >= 0) {
        ros::init( argc, argv, "offline_recogniser" );
        ros::NodeHandle nh("~");
        UTSRecogniser reco( nh, json_file, method_file, kd_dir, mask_dir, false );
        reco.start_monitor();
        return 0;
    }
    else {
        print_usage( argv[0] );
        return -1;
    }
}






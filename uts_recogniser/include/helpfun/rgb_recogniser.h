#ifndef RGB_RECOGNISER_H
#define RGB_RECOGNISER_H

#include "include/helpfun/utils.h"
#include "include/helpfun/json_parser.hpp"

#include "include/helpfun/feature_detector.h"
#include "include/helpfun/feature_matcher.h"
#include "include/helpfun/feature_cluster.h"
#include "include/helpfun/levmar_pose_estimator.h"
#include "include/helpfun/projection_filter.h"

#include <iostream>
#include <stdio.h>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <dirent.h>
#include <stdlib.h>
#include <algorithm>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/bind.hpp>

#include <eigen3/Eigen/Core>
#include <eigen3/Eigen/Dense>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/opencv.hpp>
#include <pcl/io/pcd_io.h>
#include <pcl/io/ply_io.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/console/time.h>
#include <pcl/common/common_headers.h>
#include <pcl_conversions/pcl_conversions.h>


using namespace std;


class RGBRecogniser{
private:
    string models_dir_;

    cv::Mat rgb_image_;
    // mask image
    cv::Mat mask_image_;

    int target_idx_;

    vector<string> target_bin_content_;
    string target_object_;
    int target_in_bin_;

    vector<SP_Model> models_;

    vector<DetectedFeatureRGB> detected_features_;

    typedef vector<MatchRGB> Matches;
    Matches matches_;

    typedef list<int> Cluster;
    vector<Cluster> clusters_;

    list<SP_Object> objects_;

private:
    void filter_objects(list<SP_Object> & objects);

public:
    /** constructor */
    RGBRecogniser( cv::Mat rgb_image );

    RGBRecogniser( cv::Mat rgb_image, cv::Mat mask_image );

    void load_models(string models_dir);

    /** set target item related variables */
    void set_env_configuration( int idx, vector<pair<string, string> > work_order, map<string, vector<string> > bin_contents );

    /** set target item and neighboured items */
    void set_env_configuration( string target_item, vector<string> items );

    //! set all items in the environment
    void set_bin_contents( vector<string> items );

    //! set target item
    //! target item has to be in the items list
    void set_target_item( string target );


    bool run( list<SP_Object> & objects, RGBParam param, int min_matches, int min_filtered_matches, bool visualise );

    //! get convex hull of matched features
    vector<cv::Point> get_convex_matches(int idx);


};

#endif // RGB_RECOGNISER_H

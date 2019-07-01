// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2019 Intel Corporation. All Rights Reserved.
#include <librealsense2/rs.hpp>
#include <librealsense2/rsutil.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <future>
#include <math.h>

#include "apriltag.h"
#include "apriltag_pose.h"
#include "common/homography.h"
#include "tag36h11.h"

#define FORMAT_VALUE     std::fixed << std::right << std::setprecision(3) << std::setw(6)

matd_t* homography_compute2(double c[4][4]);

typedef rs2_extrinsics transformation;
static transformation to_transform(const double R[9], const double t[3]) {
    transformation tf;
    for(int r=0; r<9; ++r){ tf.rotation[r] = static_cast<float>(R[r]); }
    for(int i=0; i<3; ++i){ tf.translation[i] = static_cast<float>(t[i]); }
    return tf;
}

static transformation to_transform(const rs2_quaternion& q, const rs2_vector& t) {
    transformation tf;
    tf.rotation[0] = q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z;
    tf.rotation[1] = 2 * (q.x * q.y - q.w * q.z);
    tf.rotation[2] = 2 * (q.x * q.z + q.w * q.y);
    tf.rotation[3] = 2 * (q.x * q.y + q.w * q.z);
    tf.rotation[4] = q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z;
    tf.rotation[5] = 2 * (q.y * q.z - q.w * q.x);
    tf.rotation[6] = 2 * (q.x * q.z - q.w * q.y);
    tf.rotation[7] = 2 * (q.y * q.z + q.w * q.x);
    tf.rotation[8] = q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z;
    tf.translation[0] = t.x;
    tf.translation[1] = t.y;
    tf.translation[2] = t.z;
    return tf;
}

static transformation operator*(const transformation& a, const transformation& b) {
    transformation tf;
    tf.rotation[0] = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[3] + a.rotation[2] * b.rotation[6];
    tf.rotation[1] = a.rotation[0] * b.rotation[1] + a.rotation[1] * b.rotation[4] + a.rotation[2] * b.rotation[7];
    tf.rotation[2] = a.rotation[0] * b.rotation[2] + a.rotation[1] * b.rotation[5] + a.rotation[2] * b.rotation[8];
    tf.rotation[3] = a.rotation[3] * b.rotation[0] + a.rotation[4] * b.rotation[3] + a.rotation[5] * b.rotation[6];
    tf.rotation[4] = a.rotation[3] * b.rotation[1] + a.rotation[4] * b.rotation[4] + a.rotation[5] * b.rotation[7];
    tf.rotation[5] = a.rotation[3] * b.rotation[2] + a.rotation[4] * b.rotation[5] + a.rotation[5] * b.rotation[8];
    tf.rotation[6] = a.rotation[6] * b.rotation[0] + a.rotation[7] * b.rotation[3] + a.rotation[8] * b.rotation[6];
    tf.rotation[7] = a.rotation[6] * b.rotation[1] + a.rotation[7] * b.rotation[4] + a.rotation[8] * b.rotation[7];
    tf.rotation[8] = a.rotation[6] * b.rotation[2] + a.rotation[7] * b.rotation[5] + a.rotation[8] * b.rotation[8];
    
    tf.translation[0] = a.rotation[0] * b.translation[0] + a.rotation[1] * b.translation[1] + a.rotation[2] * b.translation[2] + a.translation[0];
    tf.translation[1] = a.rotation[3] * b.translation[0] + a.rotation[4] * b.translation[1] + a.rotation[5] * b.translation[2] + a.translation[1];
    tf.translation[2] = a.rotation[6] * b.translation[0] + a.rotation[7] * b.translation[1] + a.rotation[8] * b.translation[2] + a.translation[2];
    return tf;
}

static std::string print(const transformation& tf) {
    std::stringstream ss; ss << "R:";
    for(const auto& r : tf.rotation){ ss << FORMAT_VALUE << r << ","; }
    ss << "|t:";
    for(const auto& t : tf.translation){ ss << FORMAT_VALUE << t << ","; }
    return ss.str();
}

class apriltag_manager {
public:
    apriltag_manager(const rs2_intrinsics& _intr,  const rs2_extrinsics _extr_b2f, double tagsize = 0.144)
    : intr(_intr), tf_body_to_fisheye(_extr_b2f) {
        tf = tag36h11_create();
        td = apriltag_detector_create();
        apriltag_detector_add_family(td, tf);
        
        td->quad_decimate = 1.0;
        td->quad_sigma = 0.0;
        td->nthreads = 1;
        td->debug = 0;
        td->refine_edges = 1;
        
        info.tagsize = tagsize;
        info.fx = info.fy = 1;
        info.cx = info.cy = 0;
    }
    ~apriltag_manager() {
        apriltag_detector_destroy(td);
        tag36h11_destroy(tf);
    }
    
    struct apriltag_array_t {
        std::shared_ptr<zarray_t>                     det;
        std::vector<std::shared_ptr<apriltag_pose_t>> pose_raw;       //tag pose from library
        std::vector<transformation>                   pose_in_camera; //tag pose in camera coordinate
        std::vector<transformation>                   pose_in_world;  //tag pose in world coordinate
        
        apriltag_detection_t* get(int t) const { apriltag_detection_t* ptr; zarray_get(det.get(), t, &ptr); return ptr; }
        int get_id(int t) const { return get(t)->id; }
        int size() const { return pose_in_camera.size(); }
    };
    
    static void apriltag_pose_destory(apriltag_pose_t* p){ matd_destroy(p->R); matd_destroy(p->t); delete p;}
    
    apriltag_array_t detect(unsigned char* gray, const rs2_pose* camera_pose) const {
        image_u8_t img{ intr.width, intr.height, intr.width, gray};
        
        apriltag_array_t tags;
        tags.det = std::shared_ptr<zarray_t>(apriltag_detector_detect(td, &img),apriltag_detections_destroy);
        tags.pose_in_camera.resize(zarray_size(tags.det.get()));
        tags.pose_raw.resize(tags.size());


        auto info_ = info;
        for(int t=0, num_of_tags=(int)tags.size(); t<num_of_tags; ++t)
        {
            tags.pose_raw[t] = std::shared_ptr<apriltag_pose_t>(new apriltag_pose_t(),apriltag_pose_destory);

            undistort(*(info_.det = tags.get(t)), intr);
            estimate_pose_for_tag_homography(&info_, tags.pose_raw[t].get());
            for(auto c : {1,2,4,5,7,8}){ tags.pose_raw[t]->R->data[c] *= -1; }
            
            tags.pose_in_camera[t] = to_transform(tags.pose_raw[t]->R->data, tags.pose_raw[t]->t->data);
        }
        
        if(camera_pose){ compute_world_to_apriltag(tags, *camera_pose); }
        return tags;
    }
    
protected:
    apriltag_family_t *tf;
    apriltag_detector_t *td;
    apriltag_detection_info_t info;
    rs2_intrinsics intr;
    transformation tf_body_to_fisheye;
    
    void compute_world_to_apriltag(apriltag_array_t& tags, const rs2_pose& camera_world_pose) const
    {
        tags.pose_in_world.resize(tags.size());
        for(int t=0, num_of_tags=tags.size(); t<num_of_tags; ++t){
            auto tf_fisheye_to_tag = tags.pose_in_camera[t];
            auto tf_world_to_body = to_transform(camera_world_pose.rotation, camera_world_pose.translation);
            tags.pose_in_world[t] = tf_world_to_body * tf_body_to_fisheye * tf_fisheye_to_tag;
        }
    }
    
    static void undistort(apriltag_detection_t& src, const rs2_intrinsics& intr)
    {
        deproject(src.c, intr, src.c);
        
        double corr_arr[4][4];
        for(int c=0; c<4; ++c){
            deproject(src.p[c], intr, src.p[c]);
            
            corr_arr[c][0] = (c==0 || c==3) ? -1 : 1;
            corr_arr[c][1] = (c==0 || c==1) ? -1 : 1;
            corr_arr[c][2] = src.p[c][0];
            corr_arr[c][3] = src.p[c][1];
        }
        matd_destroy(src.H);
        src.H = homography_compute2(corr_arr);
    }
    static void deproject(double pt[2], const rs2_intrinsics& intr, const double px[2])
    {
        float fpt[3], fpx[2] = { (float)px[0], (float)px[1] };
        rs2_deproject_pixel_to_point(fpt, &intr, fpx, 1.0f);
        pt[0] = fpt[0];
        pt[1] = fpt[1];
    }
};

int main(int argc, char * argv[]) try
{
    // Declare RealSense pipeline, encapsulating the actual device and sensors
    rs2::pipeline pipe;
    // Create a configuration for configuring the pipeline with a non default profile
    rs2::config cfg;
    // Add pose stream
    cfg.enable_stream(RS2_STREAM_POSE, RS2_FORMAT_6DOF);
    // Enable both image streams
    // Note: It is not currently possible to enable only one
    cfg.enable_stream(RS2_STREAM_FISHEYE, 1, RS2_FORMAT_Y8);
    cfg.enable_stream(RS2_STREAM_FISHEYE, 2, RS2_FORMAT_Y8);
    
    // replay
    if(argc > 1){ cfg.enable_device_from_file(argv[1]); }
    
    // Start pipe
    const int fisheye_sensor_idx = 1;
    auto pipe_profile       = pipe.start(cfg);
    auto fisheye_stream     = pipe_profile.get_stream(RS2_STREAM_FISHEYE, fisheye_sensor_idx);
    auto fisheye_intrinsics = fisheye_stream.as<rs2::video_stream_profile>().get_intrinsics();
    auto body_fisheye_extr  = fisheye_stream.get_extrinsics_to(pipe_profile.get_stream(RS2_STREAM_POSE));

    apriltag_manager tag_manager(fisheye_intrinsics, body_fisheye_extr);
    // Main loop
    while (true)
    {
        // Wait for the next set of frames from the camera
        auto frames        = pipe.wait_for_frames();
        auto fisheye_frame = frames.get_fisheye_frame(fisheye_sensor_idx);
        auto frame_number  = fisheye_frame.get_frame_number();
        auto camera_pose   = frames.get_pose_frame().get_pose_data();
        
        if(frame_number % 4 == 0)
        {
            fisheye_frame.keep();
            std::async(std::launch::async, [img=fisheye_frame,fn=frame_number,pose=camera_pose,&tag_manager](){
                auto tags = tag_manager.detect((unsigned char*)img.get_data(), &pose);

                for(int t=0; t<tags.pose_in_camera.size(); ++t){
                    std::stringstream ss; ss << "frame " << fn << "|tag id: " << tags.get_id(t) << "|";
                    std::cout << ss.str() << "camera " << print(tags.pose_in_camera[t]) << std::endl;
                    std::cout << std::setw(ss.str().size()) << " " << "world  "<< print(tags.pose_in_world[t]) << std::endl << std::endl;
                }
            });
        }
    }
    
    return EXIT_SUCCESS;
}
catch (const rs2::error & e)
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args() << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}

    
matd_t* homography_compute2(double c[4][4]) {
    double A[] =  {
        c[0][0], c[0][1], 1,       0,       0, 0, -c[0][0]*c[0][2], -c[0][1]*c[0][2], c[0][2],
        0,       0, 0, c[0][0], c[0][1], 1, -c[0][0]*c[0][3], -c[0][1]*c[0][3], c[0][3],
        c[1][0], c[1][1], 1,       0,       0, 0, -c[1][0]*c[1][2], -c[1][1]*c[1][2], c[1][2],
        0,       0, 0, c[1][0], c[1][1], 1, -c[1][0]*c[1][3], -c[1][1]*c[1][3], c[1][3],
        c[2][0], c[2][1], 1,       0,       0, 0, -c[2][0]*c[2][2], -c[2][1]*c[2][2], c[2][2],
        0,       0, 0, c[2][0], c[2][1], 1, -c[2][0]*c[2][3], -c[2][1]*c[2][3], c[2][3],
        c[3][0], c[3][1], 1,       0,       0, 0, -c[3][0]*c[3][2], -c[3][1]*c[3][2], c[3][2],
        0,       0, 0, c[3][0], c[3][1], 1, -c[3][0]*c[3][3], -c[3][1]*c[3][3], c[3][3],
    };
    
    double epsilon = 1e-10;
    
    // Eliminate.
    for (int col = 0; col < 8; col++) {
        // Find best row to swap with.
        double max_val = 0;
        int max_val_idx = -1;
        for (int row = col; row < 8; row++) {
            double val = fabs(A[row*9 + col]);
            if (val > max_val) {
                max_val = val;
                max_val_idx = row;
            }
        }
        
        if (max_val < epsilon) {
            fprintf(stderr, "WRN: Matrix is singular.\n");
        }
        
        // Swap to get best row.
        if (max_val_idx != col) {
            for (int i = col; i < 9; i++) {
                double tmp = A[col*9 + i];
                A[col*9 + i] = A[max_val_idx*9 + i];
                A[max_val_idx*9 + i] = tmp;
            }
        }
        
        // Do eliminate.
        for (int i = col + 1; i < 8; i++) {
            double f = A[i*9 + col]/A[col*9 + col];
            A[i*9 + col] = 0;
            for (int j = col + 1; j < 9; j++) {
                A[i*9 + j] -= f*A[col*9 + j];
            }
        }
    }
    
    // Back solve.
    for (int col = 7; col >=0; col--) {
        double sum = 0;
        for (int i = col + 1; i < 8; i++) {
            sum += A[col*9 + i]*A[i*9 + 8];
        }
        A[col*9 + 8] = (A[col*9 + 8] - sum)/A[col*9 + col];
    }
   return matd_create_data(3, 3, (double[]) { A[8], A[17], A[26], A[35], A[44], A[53], A[62], A[71], 1 });
}

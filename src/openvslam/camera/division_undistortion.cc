// Created by Steffen Urban June 2019, urbste@googlemail.com, github.com/urbste

#include "openvslam/camera/division_undistortion.h"

#include <spdlog/spdlog.h>

namespace openvslam {
namespace camera {

division_undistortion::division_undistortion(const std::string& name, const setup_type_t& setup_type, const color_order_t& color_order,
                         const unsigned int cols, const unsigned int rows, const double fps,
                         const double fx, const double fy, const double cx, const double cy,
                         const double distortion, const double focal_x_baseline)
        : base(name, setup_type, model_type_t::DivisionUndistortion, color_order, cols, rows, fps, focal_x_baseline, focal_x_baseline / fx),
          fx_(fx), fy_(fy), cx_(cx), cy_(cy), fx_inv_(1.0 / fx), fy_inv_(1.0 / fy),
          distortion_(distortion) {
    spdlog::debug("CONSTRUCT: camera::division_undistortion");

    cv_cam_matrix_ = (cv::Mat_<float>(3, 3) << fx_, 0, cx_, 0, fy_, cy_, 0, 0, 1);

    eigen_cam_matrix_ << fx_, 0, cx_, 0, fy_, cy_, 0, 0, 1;

    // 画像の最大範囲を計算
    img_bounds_ = compute_image_bounds();

    // セルサイズの逆数を計算しておく
    inv_cell_width_ = static_cast<double>(num_grid_cols_) / (img_bounds_.max_x_ - img_bounds_.min_x_);
    inv_cell_height_ = static_cast<double>(num_grid_rows_) / (img_bounds_.max_y_ - img_bounds_.min_y_);
}

division_undistortion::division_undistortion(const YAML::Node& yaml_node)
        : division_undistortion(yaml_node["Camera.name"].as<std::string>(),
                      load_setup_type(yaml_node),
                      load_color_order(yaml_node),
                      yaml_node["Camera.cols"].as<unsigned int>(),
                      yaml_node["Camera.rows"].as<unsigned int>(),
                      yaml_node["Camera.fps"].as<double>(),
                      yaml_node["Camera.fx"].as<double>(),
                      yaml_node["Camera.fy"].as<double>(),
                      yaml_node["Camera.cx"].as<double>(),
                      yaml_node["Camera.cy"].as<double>(),
                      yaml_node["Camera.distortion"].as<double>(),
                      yaml_node["Camera.focal_x_baseline"].as<double>(0.0)) {}

division_undistortion::~division_undistortion() {
    spdlog::debug("DESTRUCT: camera::division_undistortion");
}

void division_undistortion::show_parameters() const {
    show_common_parameters();
    std::cout << "  - fx: " << fx_ << std::endl;
    std::cout << "  - fy: " << fy_ << std::endl;
    std::cout << "  - cx: " << cx_ << std::endl;
    std::cout << "  - cy: " << cy_ << std::endl;
    std::cout << "  - distortion: " << distortion_ << std::endl;
    std::cout << "  - min x: " << img_bounds_.min_x_ << std::endl;
    std::cout << "  - max x: " << img_bounds_.max_x_ << std::endl;
    std::cout << "  - min y: " << img_bounds_.min_y_ << std::endl;
    std::cout << "  - max y: " << img_bounds_.max_y_ << std::endl;
}

image_bounds division_undistortion::compute_image_bounds() const {
    spdlog::debug("compute image bounds");

    if (distortion_ == 0.0) {
        return image_bounds{0.0, cols_, 0.0, rows_};
    }
    else {
        const std::vector<cv::KeyPoint> corners{cv::KeyPoint(0.0, 0.0, 1.0), // 左上
                                                cv::KeyPoint(cols_, 0.0, 1.0), // 右上
                                                cv::KeyPoint(0.0, rows_, 1.0), // 左下
                                                cv::KeyPoint(cols_, rows_, 1.0)}; // 右下

        std::vector<cv::KeyPoint> undist_corners;
        undistort_keypoints(corners, undist_corners);

        return image_bounds{std::min(undist_corners.at(0).pt.x, undist_corners.at(2).pt.x),
                            std::max(undist_corners.at(1).pt.x, undist_corners.at(3).pt.x),
                            std::min(undist_corners.at(0).pt.y, undist_corners.at(1).pt.y),
                            std::max(undist_corners.at(2).pt.y, undist_corners.at(3).pt.y)};
    }
}

void division_undistortion::undistort_keypoints(const std::vector<cv::KeyPoint>& dist_keypts, std::vector<cv::KeyPoint>& undist_keypts) const {
    // fill cv::Mat with distorted keypoints
    undist_keypts.resize(dist_keypts.size());
    for (unsigned long idx = 0; idx < dist_keypts.size(); ++idx) {
        undist_keypts.at(idx) = this->undistort_keypoint(dist_keypts.at(idx));
        undist_keypts.at(idx).angle = dist_keypts.at(idx).angle;
        undist_keypts.at(idx).size = dist_keypts.at(idx).size;
        undist_keypts.at(idx).octave = dist_keypts.at(idx).octave;
    }
}

void division_undistortion::convert_keypoints_to_bearings(const std::vector<cv::KeyPoint>& undist_keypts, eigen_alloc_vector<Vec3_t>& bearings) const {
    bearings.resize(undist_keypts.size());
#ifdef USE_OPENMP
#pragma omp parallel for
#endif
    for (unsigned long idx = 0; idx < undist_keypts.size(); ++idx) {
        bearings.at(idx) = this->convert_keypoint_to_bearing(undist_keypts.at(idx));
    }
}

void division_undistortion::convert_bearings_to_keypoints(const eigen_alloc_vector<Vec3_t>& bearings, std::vector<cv::KeyPoint>& undist_keypts) const {
    undist_keypts.resize(bearings.size());
#ifdef USE_OPENMP
#pragma omp parallel for
#endif
    for (unsigned long idx = 0; idx < bearings.size(); ++idx) {
        undist_keypts.at(idx) = convert_bearing_to_keypoint(bearings.at(idx));
    }
}

bool division_undistortion::reproject_to_image(const Mat33_t& rot_cw, const Vec3_t& trans_cw, const Vec3_t& pos_w, Vec2_t& reproj, float& x_right) const {
    // カメラ基準の座標に変換
    const Vec3_t pos_c = rot_cw * pos_w + trans_cw;

    // カメラの正面になければ不可視
    if (pos_c(2) <= 0.0) {
        return false;
    }

    // 画像上に投影
    const auto z_inv = 1.0 / pos_c(2);
    reproj(0) = fx_ * pos_c(0) * z_inv + cx_;
    reproj(1) = fy_ * pos_c(1) * z_inv + cy_;
    x_right = reproj(0) - focal_x_baseline_ * z_inv;

    // 画像内であることを確認
    if (reproj(0) < img_bounds_.min_x_ || reproj(0) > img_bounds_.max_x_) {
        return false;
    }
    if (reproj(1) < img_bounds_.min_y_ || reproj(1) > img_bounds_.max_y_) {
        return false;
    }

    return true;
}

bool division_undistortion::reproject_to_bearing(const Mat33_t& rot_cw, const Vec3_t& trans_cw, const Vec3_t& pos_w, Vec3_t& reproj) const {
    // カメラ基準の座標に変換
    reproj = rot_cw * pos_w + trans_cw;

    // カメラの正面になければ不可視
    if (reproj(2) <= 0.0) {
        return false;
    }

    // 画像上に投影
    const auto z_inv = 1.0 / reproj(2);
    const auto x = fx_ * reproj(0) * z_inv + cx_;
    const auto y = fy_ * reproj(1) * z_inv + cy_;

    // 画像内であることを確認
    if (x < img_bounds_.min_x_ || x > img_bounds_.max_x_) {
        return false;
    }
    if (y < img_bounds_.min_y_ || y > img_bounds_.max_y_) {
        return false;
    }

    // bearingにする
    reproj.normalize();

    return true;
}

nlohmann::json division_undistortion::to_json() const {
    return {{"model_type", get_model_type_string()},
            {"setup_type", get_setup_type_string()},
            {"color_order", get_color_order_string()},
            {"cols", cols_},
            {"rows", rows_},
            {"fps", fps_},
            {"focal_x_baseline", focal_x_baseline_},
            {"num_grid_cols", num_grid_cols_},
            {"num_grid_rows", num_grid_rows_},
            {"fx", fx_},
            {"fy", fy_},
            {"cx", cx_},
            {"cy", cy_},
            {"distortion", distortion_},};
}

} // namespace camera
} // namespace openvslam

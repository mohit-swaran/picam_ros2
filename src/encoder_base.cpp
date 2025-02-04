#include "picam_ros2/encoder_base.hpp"

Encoder::Encoder(CameraInterface *interface, std::shared_ptr<libcamera::Camera> camera) {
    this->interface = interface;
    this->camera = camera;
}

Encoder::~Encoder() {
    this->interface = NULL;
    this->camera = NULL;
}
#include <sys/mman.h>
#include <fmt/core.h>
#include <sys/ioctl.h>

#include "picam_ros2/const.hpp"
#include "picam_ros2/camera_interface.hpp"

using namespace libcamera;

void freeBuffer(void* opaque, uint8_t* data) {
    munmap(data, static_cast<size_t>(reinterpret_cast<uintptr_t>(opaque)));
}

uint32_t roundUp4096(uint32_t x) {
    constexpr uint32_t mask = 4096 - 1; // 0xFFF
    return (x + mask) & ~mask;
}

CameraInterface::CameraInterface(std::shared_ptr<Camera> camera, std::shared_ptr<PicamROS2> node) {
    this->camera = camera;
    this->node = node;
}


void CameraInterface::start() {
    if (this->running)
        return;

    this->running = true;

    std::cout << GREEN << "Initializing " << camera->id() << CLR << std::endl; 

    // inspect camera
    this->camera->acquire();
    const ControlList &props = camera->properties();
    if (props.contains(properties::Location.id())) {
        this->location = props.get(properties::Location).value();
    }
    if (props.contains(properties::Model.id())) {
        this->model = props.get(properties::Model)->c_str();
    }
    if (props.contains(properties::Rotation.id())) {
        this->rotation = props.get(properties::Rotation).value();
    }  

    // declare params & read configs
    this->readConfig();
    

    // configure camera
    std::unique_ptr<CameraConfiguration> config = this->camera->generateConfiguration( { StreamRole::VideoRecording } );
    
    this->streamConfig = &config->at(0);
    // this->streamConfig->controls.set(libcamera::controls::AeEnable, false);
    this->streamConfig->size.width = this->width;
    this->streamConfig->size.height = this->height;
    this->streamConfig->bufferCount = this->buffer_count;
    // this->streamConfig.stride = (uint)this->width;

    if (config->validate() == CameraConfiguration::Invalid)
	    throw std::runtime_error("Failed to validate stream configurations");

    this->stride = this->streamConfig->stride;

    std::cout << YELLOW << "Camera model: " << this->model << " Location: " << this->location << " Rotation: " << this->rotation << CLR << std::endl; 
    std::cout << YELLOW << "Camera orinetation: " << config->orientation << CLR << std::endl; 
    std::cout << YELLOW << "Stream config: " << this->streamConfig->toString() << CLR << std::endl; 
    std::cout << YELLOW << "Stride: " << this->streamConfig->stride << CLR << std::endl; 
    std::cout << YELLOW << "Bit rate: " << this->bit_rate << CLR << std::endl; 
    std::cout << YELLOW << "Compression rate: " << this->compression << CLR << std::endl; 
    std::cout << YELLOW << "Buffer count: " << this->streamConfig->bufferCount << CLR << std::endl; 
    std::cout << YELLOW << "Auto exposure enabled: " << this->ae_enable << CLR << std::endl; 
    std::cout << YELLOW << "Exposure time: " << this->exposure_time << " ns"<< CLR << std::endl; 
    std::cout << YELLOW << "Analogue gain: " << this->analog_gain << CLR << std::endl; 
    std::cout << YELLOW << "Auto white balance enabled: " << this->awb_enable << CLR << std::endl; 
    std::cout << YELLOW << "Color gains: {" << this->color_gains[0] << ", " << this->color_gains[1] << "}"<< CLR << std::endl; 
    std::cout << YELLOW << "Brightness: " << this->brightness << CLR << std::endl; 
    std::cout << YELLOW << "Contrast: " << this->contrast << CLR << std::endl; 
    this->camera->configure(config.get());

    // FrameBufferAllocator *allocator = new FrameBufferAllocator(this->camera);

    std::cout << "Allocating..." << std::endl;
    for (StreamConfiguration &cfg : *config) {
        auto stream = cfg.stream();

        std::vector<std::unique_ptr<FrameBuffer>> buffers;
        this->buffer_size = roundUp4096(cfg.frameSize);

		for (uint i = 0; i < cfg.bufferCount; i++)
		{
			std::string name("pica-ros2-" + std::to_string(i));
			libcamera::UniqueFD fd = this->dma_heap.alloc(name.c_str(), this->buffer_size);

			if (!fd.isValid())
				throw std::runtime_error("Failed to allocate capture buffers for stream");

			std::vector<FrameBuffer::Plane> plane(1);
			plane[0].fd = libcamera::SharedFD(std::move(fd));
			plane[0].offset = 0;
			plane[0].length = this->buffer_size;

			buffers.push_back(std::make_unique<FrameBuffer>(plane));
			void *memory = mmap(NULL, this->buffer_size, PROT_READ , MAP_SHARED, plane[0].fd.get(), 0);

            uint plane_offset = 0;
            for (uint j = 0; j < 3; j++) {
                
                uint plane_stride = (j == 0) ? this->stride : this->stride / 2;
                uint plane_length = plane_stride * (j == 0 ? this->height : this->height / 2);
                
                this->mapped_capture_buffers[buffers.back().get()].push_back(av_buffer_create(
                    static_cast<uint8_t*>(memory) + plane_offset,
                    plane_length,
                    freeBuffer,
                    reinterpret_cast<void*>(this->buffer_size),
                    0
                ));
                this->mapped_capture_buffer_strides[buffers.back().get()].push_back(plane_stride);
                plane_offset += plane_length;
            }
            
			// libcamera::Span<uint8_t>(static_cast<uint8_t *>(memory), cfg.frameSize));
		}

        this->capture_frame_buffers[stream] = std::move(buffers);

        std::cout << "Allocated " << this->capture_frame_buffers[stream].size() << " capture dma buffers for stream pixel format: " << cfg.pixelFormat.toString() << std::endl;

        // int ret = allocator->allocate(stream);
        // if (ret < 0) {
        //     std::cerr << "Can't allocate buffers" << std::endl;
        //     return;
        // }
        // const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
        // size_t allocated = buffers.size();
        // std::cout << "Allocated " << allocated << " buffers for stream pixel format: " << cfg.pixelFormat.toString() << std::endl;

        // make requests
        for (unsigned int i = 0; i < this->capture_frame_buffers[stream].size(); ++i) {
            std::unique_ptr<Request> request = camera->createRequest();
            if (!request)
            {
                std::cerr << "Can't create request" << std::endl;
                return;
            }

            request->controls().set(libcamera::controls::AeEnable, this->ae_enable);
            if (this->ae_enable) {
                request->controls().set(libcamera::controls::AeMeteringMode, this->ae_metering_mode);
                request->controls().set(libcamera::controls::AeConstraintMode, this->ae_constraint_mode);
                request->controls().set(libcamera::controls::AeExposureMode, this->ae_exposure_mode);
                
            } else {
                request->controls().set(libcamera::controls::ExposureTime, this->exposure_time);
            }
                
            request->controls().set(libcamera::controls::AnalogueGain, this->analog_gain);
            request->controls().set(libcamera::controls::AwbEnable, this->awb_enable);
            
            Span<const float, 2> color_gains({(float)this->color_gains[0], (float)this->color_gains[1]});
            request->controls().set(libcamera::controls::ColourGains, color_gains);
            request->controls().set(libcamera::controls::Brightness, this->brightness);
            request->controls().set(libcamera::controls::Contrast, this->contrast);

            const std::unique_ptr<FrameBuffer> &buffer = this->capture_frame_buffers[stream][i];
            int ret = request->addBuffer(stream, buffer.get());
            if (ret < 0)
            {
                std::cerr << "Can't set buffer for request" << std::endl;
                return;
            }
            this->capture_requests.push_back(std::move(request));
        }
    }

    // init ros frame publisher
    std::string topic = fmt::format(this->node->get_parameter("topic_prefix").as_string() + "{}/{}", this->location, this->model);
    std::cout << "Creatinng publisher for " << topic << std::endl;
    auto qos = rclcpp::QoS(1);
    this->publisher = this->node->create_publisher<ffmpeg_image_transport_msgs::msg::FFMPEGPacket>(topic, qos);
    
    // init output msg
    std_msgs::msg::Header header;
    header.frame_id = this->frame_id;
    header.stamp = builtin_interfaces::msg::Time();
    this->outFrameMsg.header = header;
    this->outFrameMsg.width = this->width;
    this->outFrameMsg.height = this->height;
    this->outFrameMsg.encoding = "h.264";
    this->outFrameMsg.is_bigendian = false;

    // std::shared_ptr<CameraInterface> sharedPtr(this, [](CameraInterface* ptr) {
    //     // Custom deleter that does nothing
    // });

    // create the encoder
    if (this->hw_encoder) {
        this->encoder = (Encoder *) new EncoderHW(this, this->camera);
    } else {
        this->encoder = (Encoder *) new EncoderLibAV(this, this->camera);
    }

    // init encoder
    // if (this->hw_encoder) {
    //     if (!this->initializeHWEncoder()) {
    //         std::cerr << "Error initializing HW encoder" << std::endl; 
    //         return;
    //     }
    // } else {
    //     if (!this->initializeSWEncoder()) {
    //         std::cerr << "Error initializing SW encoder" << std::endl; 
    //         return;
    //     }
    // }
    
    
    // if (this->hw_encoder) { // wait a bit to allow encoder init
    //     std::this_thread::sleep_for(std::chrono::seconds(2));
    // }

    this->camera->requestCompleted.connect(this, &CameraInterface::captureRequestComplete);
    this->camera->start();

    for (std::unique_ptr<Request> &request : this->capture_requests) {
        this->camera->queueRequest(request.get());
    }

}

void CameraInterface::captureRequestComplete(Request *request) {
    if (request->status() == Request::RequestCancelled) {
        return;
    }

    bool log = false;
    auto now = std::chrono::high_resolution_clock::now();
    auto ns_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();

    if (this->log_message_every_ns > -1) {        
        if (ns_since_epoch - this->last_log >= this->log_message_every_ns) {
            this->last_log = ns_since_epoch;
            log = true;
        }
    }

    const std::map<const Stream *, FrameBuffer *> &request_buffers = request->buffers();

    struct dma_buf_sync dma_sync_start {};
    dma_sync_start.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    struct dma_buf_sync dma_sync_end {};
    dma_sync_end.flags = DMA_BUF_SYNC_END| DMA_BUF_SYNC_READ;

    for (auto p : request_buffers) {
        FrameBuffer *request_buffer = p.second;
        const FrameMetadata &metadata = request_buffer->metadata();

        auto base_fd = p.second->planes()[0].fd.get(); //base plane

        int ret_start = ::ioctl(base_fd, DMA_BUF_IOCTL_SYNC, &dma_sync_start);
		if (ret_start)
		    throw std::runtime_error("Failed to sync/start dma buf on queue request");

        std::time_t current_time = std::time(nullptr);
        if (current_time - this->last_fps_time >= 1.0) {
            this->last_fps = this->frame_count;
            this->last_fps_time = current_time;
            this->frame_count = 0;
        }
        this->frame_count++;

        if (log) {
            if (!this->log_scrolls && this->lines_printed > 0) {
                for (int i = 0; i < this->lines_printed; i++) {
                    std::cout << std::string(this->lines_printed, '\033') << "[A\033[K";
                }
            }
            this->lines_printed = 0;
        }

        if (log) {
            std::cout << this->last_fps << " FPS" << std::endl;
            this->lines_printed++;
            std::cout << std::setw(6) << std::setfill('0') << metadata.sequence << ": ";
        }

        // int plane_offset = 0;
        auto &plane_buffers = this->mapped_capture_buffers[p.second];
        auto &plane_strides = this->mapped_capture_buffer_strides[p.second];

        int ret_end = ::ioctl(base_fd, DMA_BUF_IOCTL_SYNC, &dma_sync_end);
        if (ret_end)
            throw std::runtime_error("Failed to sync/end dma buf on queue request");
        
        // long timestamp_ns = metadata.timestamp;
        long timestamp_ns = ns_since_epoch;
        if (this->timestamp_ns_base == 0) {
            this->timestamp_ns_base = timestamp_ns;
        }
        timestamp_ns -= this->timestamp_ns_base;

        if (log) {
            std::cout << std::endl;
            this->lines_printed++;
         }

        this->encoder->encode(plane_buffers, plane_strides, base_fd, this->buffer_size, &this->frameIdx, timestamp_ns, log);
    }

    if (!this->running)
        return;

    request->reuse(Request::ReuseBuffers);
    this->camera->queueRequest(request);
}

void getCurrentStamp(builtin_interfaces::msg::Time *stamp, uint64_t timestamp_ns) {
    // Split into seconds and nanoseconds
    stamp->sec = static_cast<int32_t>(timestamp_ns / NS_TO_SEC);
    stamp->nanosec = static_cast<uint32_t>(timestamp_ns % NS_TO_SEC);;
}

void CameraInterface::publish(unsigned char *data, int size, bool keyframe, uint64_t pts, long timestamp_ns, bool log) {

    getCurrentStamp(&this->outFrameMsg.header.stamp, timestamp_ns);
    this->outFrameMsg.pts = pts;

    this->outFrameMsg.flags = keyframe ? 1 : 0;
    this->outFrameMsg.data.assign(data, data + size);

    if (log) {
        std::cout << GREEN << " >> Sending " << this->outFrameMsg.data.size() << "B" << CLR << " sec: " << this->outFrameMsg.header.stamp.sec << " nsec: " << outFrameMsg.header.stamp.nanosec << std::endl;
        this->lines_printed++;
    }

    this->publisher->publish(this->outFrameMsg);

}

void CameraInterface::stop() {
    if (!this->running)
        return;
    this->running = false;

}

// void CameraInterface::frameRequestComplete(Request *request) {
//     if (request->status() == Request::RequestCancelled) {
//         return;
//     }
        
//     const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

//     bool log = false;
//     auto now = std::chrono::high_resolution_clock::now();
//     auto ns_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
//         now.time_since_epoch()
//     ).count();

//     if (this->log_message_every_ns > -1) {        
//         if (ns_since_epoch - this->last_log >= this->log_message_every_ns) {
//             this->last_log = ns_since_epoch;
//             log = true;
//         }
//     }

//     struct dma_buf_sync dma_sync_start {};
//     dma_sync_start.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
//     struct dma_buf_sync dma_sync_end {};
//     dma_sync_end.flags = DMA_BUF_SYNC_END| DMA_BUF_SYNC_READ;
//     for (auto bufferPair : buffers) {
//         FrameBuffer *buffer = bufferPair.second;
//         const FrameMetadata &metadata = buffer->metadata();

// 		// auto &list = this->frame_buffers[const_cast<Stream *>(bufferPair.first)];
// 		// auto it = std::find_if(list.begin(), list.end(),
// 		// 					[&bufferPair] (auto &b) { return b.get() == bufferPair.second;} );
// 		// if (it == list.end())
// 		//     throw std::runtime_error("Failed to identify request buffer");
        
//         int ret_start = ::ioctl(bufferPair.second->planes()[0].fd.get(), DMA_BUF_IOCTL_SYNC, &dma_sync_start);
// 		if (ret_start)
// 		    throw std::runtime_error("Failed to sync/start dma buf on queue request");

// 		// if (request->addBuffer(bufferPair.first, bufferPair.second) < 0)
// 		// 	throw std::runtime_error("Failed to add buffer to request in QueueRequest");

//         // const std::vector<libcamera::FrameBuffer::Plane>& planes = buffer->planes();

//         // if (!planes[0].fd.isValid()) {
//         //     std::cerr << "Frame plane[0] fd invalid" << std::endl;
//         //     continue;
//         // }

//         // size_t total_size = 0;
//         // for (size_t i = 0; i < planes.size(); ++i) {
//         //     size_t plane_end = planes[i].offset + planes[i].length;
//         //     if (plane_end > total_size) {
//         //         total_size = plane_end;
//         //     }
//         // }

//         std::time_t current_time = std::time(nullptr);
//         if (current_time - this->last_fps_time >= 1.0) {
//             //std::time_t d = current_time - this->last_fps;
//             this->last_fps = this->frame_count;
//             this->last_fps_time = current_time;
//             this->frame_count = 0;
//         }
//         this->frame_count++;

//         // unsigned int nplane = 0;
//         // std::cout << " seq: " << std::setw(6) << std::setfill('0') << metadata.sequence << " bytesused: ";
//         // for (const FrameMetadata::Plane &plane : metadata.planes())  {
//             // std::cout << plane.bytesused;
//             // if (++nplane < metadata.planes().size()) std::cout << "/";
//         // }
//         // std::cout << std::endl;

//         // void* buf_base = mmap(nullptr, total_size, PROT_READ, MAP_SHARED, planes[0].fd.get(), 0);
//         // if (buf_base == MAP_FAILED) {
//         //     std::cerr << "Failed to mmap entire buffer: " << strerror(errno) << std::endl;
//         //     return;
//         // }

//         if (log) {
//             if (!this->log_scrolls && this->lines_printed > 0) {
//                 for (int i = 0; i < this->lines_printed; i++) {
//                     std::cout << std::string(this->lines_printed, '\033') << "[A\033[K";
//                 }
//             }
//             this->lines_printed = 0;
//         }

//         if (log) {
//             std::cout << this->last_fps << " FPS" << std::endl;
//             this->lines_printed++;

//             // std::cout << "Buff " << total_size << " total B" << std::endl;
//             // this->lines_printed++;

//             std::cout << std::setw(6) << std::setfill('0') << metadata.sequence << ": ";
//         }

    
//         // int plane_offset = 0;
//         auto &list = this->mapped_buffers[bufferPair.second];
//         auto &plane_strides = this->mapped_buffer_strides[bufferPair.second];

//         for (size_t i = 0; i < 3; ++i) {

//             // int plane_stride = (i == 0) ? this->stride : this->stride / 2;
//             // int plane_length = plane_stride * (i == 0 ? this->height : this->height / 2);

//             // Map the memory
//             // if (log) {
//             //     if (i == 0)
//             //         std::cout << "fd=" << planes[i].fd.get() << "; ";
//             //     std::cout << "pl#" << i <<" (" << plane_length << ") " ;
//             //     std::cout << plane_offset << "-" << (plane_offset+plane_length) << " ";
//             //     std::cout << "|" << plane_stride << "|";
//             //     if (i < 2)
//             //         std::cout << "; ";
//             // }
            
//             this->frame->buf[i] = list[i];
//             // plane_offset += plane_length;

//             // Assign plane data using offsets within the mapped buffer
//             this->frame->data[i] = this->frame->buf[i]->data;
//             this->frame->linesize[i] = plane_strides[i];
//         }
//         if (log) {
//             std::cout << std::endl;
//             this->lines_printed++;
//         }

//         // continue;

//         /// Set frame index in range: [1, fps]
//         this->frame->pts = this->frameIdx;

//         /// Set frame type
//         bool isKeyFrame = false; // this->frameIdx == 0;
//         if (isKeyFrame){
//             this->frame->key_frame = 1;
//             this->frame->pict_type = AVPictureType::AV_PICTURE_TYPE_I;
//         }
//         // std::cout << "Sending..." << std::endl;

//         bool frame_ok = false;
//         switch (avcodec_send_frame(this->codec_context, this->frame)){
//             case 0:
//                 frame_ok = true;
//                 this->frameIdx = (this->frameIdx % this->codec_context->framerate.num) + 1;
//                 break;
//             case AVERROR(EAGAIN):
//                 std::cerr << RED << "Error sending frame to encoder: AVERROR(EAGAIN)" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//             case AVERROR_EOF:
//                 std::cerr << RED << "Error sending frame to encoder: AVERROR_EOF" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//             case AVERROR(EINVAL):
//                 std::cerr << RED << "Error sending frame to encoder: AVERROR(EINVAL)" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//             case AVERROR(ENOMEM):
//                 std::cerr << RED << "Error sending frame to encoder: AVERROR(ENOMEM)" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//             default:
//                 std::cerr << RED << "Error sending frame to encoder: Other error" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//         }

//         // if (munmap(buf_base, total_size) == -1) {
//         //     std::cerr << "munmap failed: " << strerror(errno) << std::endl;
//         // }
//         // buf_base = nullptr;  // Optional: prevent accidental reuse

//         int ret_end = ::ioctl(bufferPair.second->planes()[0].fd.get(), DMA_BUF_IOCTL_SYNC, &dma_sync_end);
// 		if (ret_end)
// 		    throw std::runtime_error("Failed to sync/end dma buf on queue request");

//         if (!frame_ok) {
//             continue;
//         }

//         bool packet_ok = false;
//         switch (avcodec_receive_packet(this->codec_context, this->packet)) {
//             case 0:
//                 /// use packet, copy/send it's data, or whatever
//                 packet_ok = true;
//                 if (log) {
//                     std::cout << (this->packet->flags == 1 ? MAGENTA : YELLOW);
                    
//                     std::cout << "PACKET " << this->packet->size
//                             << " / " << this->packet->buf->size
//                             << " pts=" << this->packet->pts
//                             << " flags=" << this->packet->flags;
//                     std::cout << CLR;
//                     std::cout << std::endl;
//                     this->lines_printed++;
//                 }
//                 break;
//             case AVERROR(EAGAIN):
//                 std::cerr << RED << "Error receiving packet AVERROR(EAGAIN)" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//             case AVERROR_EOF:
//                 std::cerr << RED << "Error receiving packet AVERROR_EOF" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//             case AVERROR(EINVAL):
//                 std::cerr << RED << "Error receiving packet AVERROR(EINVAL)" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//             default:
//                 std::cerr << RED << "Error receiving packet" << CLR << std::endl;
//                 this->lines_printed = -1;
//                 break;
//         }

//         if (this->packet->flags & AV_PKT_FLAG_CORRUPT) {
//             std::cerr << RED << "Packed flagged as corrupt" << CLR << std::endl;
//             this->lines_printed = -1;
//             continue;
//         }

//         if (this->packet->flags != 0 && this->packet->flags != AV_PKT_FLAG_KEY) {
//             std::cerr << MAGENTA << "Packed flags:" << std::bitset<4>(this->packet->flags) << CLR << std::endl;
//             this->lines_printed = -1;
//         }
        
//         if (packet_ok) {
            
//             // long timestamp_ns = metadata.timestamp;
//             long timestamp_ns = ns_since_epoch;
//             if (this->timestamp_ns_base == 0) {
//                 this->timestamp_ns_base = timestamp_ns;
//             }
//             timestamp_ns -= this->timestamp_ns_base;
//             get_current_stamp(&this->outFrameMsg.header.stamp, timestamp_ns);
//             this->outFrameMsg.pts = av_rescale_q(this->packet->pts, //this->outFrameMsg.header.stamp.sec * NS_TO_SEC + this->outFrameMsg.header.stamp.nanosec, //this->packet->pts
//                                                  codec_context->time_base,
//                                                  AVRational{1, 90000});

//             this->outFrameMsg.flags = this->packet->flags & AV_PKT_FLAG_KEY;
//             // outFrameMsg.data = &packet->data; // uint8[] out
//             // outFrameMsg.data = std::vector<uint8_t>(packet->size);
//             this->outFrameMsg.data.assign(this->packet->data, this->packet->data + this->packet->size);

//             if (log) {
//                 std::cout << GREEN << " >> Sending " << this->outFrameMsg.data.size() << "B" << CLR << " sec: " << this->outFrameMsg.header.stamp.sec << " nsec: " << outFrameMsg.header.stamp.nanosec << std::endl;
//                 this->lines_printed++;
//             }
        
//             this->publisher->publish(this->outFrameMsg);
//         }
//     }

//     if (!this->running)
//         return;

//     request->reuse(Request::ReuseBuffers);
//     this->camera->queueRequest(request);
// }

void CameraInterface::readConfig() {
    std::string config_prefix = fmt::format("/camera_{}.", this->location);
    
    this->node->declare_parameter(config_prefix + "hflip", false);
    this->node->declare_parameter(config_prefix + "vflip", false);

    this->node->declare_parameter(config_prefix + "hw_encoder", true);
    this->hw_encoder = this->node->get_parameter(config_prefix + "hw_encoder").as_bool();

    this->node->declare_parameter(config_prefix + "width", 1920);
    this->width = (uint) this->node->get_parameter(config_prefix + "width").as_int();
    this->node->declare_parameter(config_prefix + "height", 1080);
    this->height = (uint) this->node->get_parameter(config_prefix + "height").as_int();

    this->node->declare_parameter(config_prefix + "bitrate", 4000000);
    this->bit_rate = this->node->get_parameter(config_prefix + "bitrate").as_int();

    this->node->declare_parameter(config_prefix + "ae_enable", false);
    this->ae_enable = this->node->get_parameter(config_prefix + "ae_enable").as_bool();
    this->node->declare_parameter(config_prefix + "exposure_time_ns", 10000); // 10 ms
    this->exposure_time = (uint) this->node->get_parameter(config_prefix + "exposure_time_ns").as_int();
    this->node->declare_parameter(config_prefix + "ae_metering_mode", 0);
    this->ae_metering_mode = (uint) this->node->get_parameter(config_prefix + "ae_metering_mode").as_int();
    // MeteringCentreWeighted = 0,
	// MeteringSpot = 1,
	// MeteringMatrix = 2,
	// MeteringCustom = 3,

    this->node->declare_parameter(config_prefix + "ae_exposure_mode", 0);
    this->ae_exposure_mode = (uint) this->node->get_parameter(config_prefix + "ae_exposure_mode").as_int();
    // ExposureNormal = 0,
	// ExposureShort = 1,
	// ExposureLong = 2,
	// ExposureCustom = 3,

    this->node->declare_parameter(config_prefix + "ae_constraint_mode", 0);
    this->ae_constraint_mode = (uint) this->node->get_parameter(config_prefix + "ae_constraint_mode").as_int();
    // ConstraintNormal = 0,
	// ConstraintHighlight = 1,
	// ConstraintShadows = 2,
	// ConstraintCustom = 3,

    // this->node->declare_parameter(config_prefix + "ae_constraint_mode_values", std::vector<double>{ 2.0f, 1.8f });
    // this->ae_constraint_mode_values = this->node->get_parameter(config_prefix + "ae_constraint_mode_values").as_double_array();

    this->node->declare_parameter(config_prefix + "analog_gain", 1.5f); // sensor gain
    this->analog_gain = this->node->get_parameter(config_prefix + "analog_gain").as_double();
    this->node->declare_parameter(config_prefix + "awb_enable", true);
    this->awb_enable = this->node->get_parameter(config_prefix + "awb_enable").as_bool();
    this->node->declare_parameter(config_prefix + "color_gains", std::vector<double>{ 2.0f, 1.8f });
    this->color_gains = this->node->get_parameter(config_prefix + "color_gains").as_double_array();
    
    this->node->declare_parameter(config_prefix + "brightness", 0.2f);
    this->brightness = this->node->get_parameter(config_prefix + "brightness").as_double();
    this->node->declare_parameter(config_prefix + "contrast", 1.2f);
    this->contrast = this->node->get_parameter(config_prefix + "contrast").as_double();

    this->node->declare_parameter(config_prefix + "compression", 35);
    this->compression = (uint) this->node->get_parameter(config_prefix + "compression").as_int();

    this->node->declare_parameter(config_prefix + "framerate", 30);
    this->fps = (uint) this->node->get_parameter(config_prefix + "framerate").as_int();

    this->node->declare_parameter(config_prefix + "buffer_count", 4);
    this->buffer_count = (uint) this->node->get_parameter(config_prefix + "buffer_count").as_int();

    this->node->declare_parameter(config_prefix + "frame_id", "picam");
    this->frame_id = this->node->get_parameter(config_prefix + "frame_id").as_string();

    this->log_scrolls = this->node->get_parameter("log_scroll").as_bool();
    this->log_message_every_ns = (long) (this->node->get_parameter("log_message_every_sec").as_double() * NS_TO_SEC);
}

// int CameraInterface::resetEncoder(const char* device_path) {
//     int fd = open(device_path, O_RDWR);
//     if (fd < 0) {
//         perror("Failed to open V4L2 device");
//         return -1;
//     }

//     // Stop streaming
//     enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
//     if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
//         perror("Failed to stop streaming");
//     }

//     // Reset the device (optional, driver-dependent)
//     // Some drivers support VIDIOC_RESET, but it's not universal
//     if (ioctl(fd, _IOW('V', 99, int), 0) < 0) {
//         // Fallback: Close and reopen the device
//         close(fd);
//         fd = open(device_path, O_RDWR);
//     }

//     close(fd);
//     return 0;
// }

CameraInterface::~CameraInterface() {
    std::cout << BLUE << "Cleaning up " << this->model << " interface" << CLR << std::endl;

    this->camera->stop();
    this->camera->requestCompleted.disconnect(this, &CameraInterface::captureRequestComplete);
    this->camera->release();
    this->camera.reset();

    // for (auto &iter : this->mapped_capture_buffers)
	// {
	// 	assert(iter.first->planes().size() == iter.second.size());
	// 	for (unsigned i = 0; i < iter.first->planes().size(); i++)
	// 	for (auto &span : iter.second)
	// 		munmap(span.data(), span.size());
	// }
	this->mapped_capture_buffers.clear();
	this->capture_frame_buffers.clear();
    
    this->camera = NULL;
    this->node = NULL;
}
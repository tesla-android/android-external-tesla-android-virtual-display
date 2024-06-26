#include <errno.h>

#include <unistd.h>

#include <stdio.h>

#include <fcntl.h>

#include <stdlib.h>

#include <string.h>

#include <linux/fb.h>

#include <sys/ioctl.h>

#include <sys/mman.h>

#include <sys/wait.h>

#include <android/bitmap.h>

#include <binder/ProcessState.h>

#include <gui/ISurfaceComposer.h>

#include <gui/SurfaceComposerClient.h>

#include <gui/SyncScreenCaptureListener.h>

#include <ui/GraphicTypes.h>

#include <ui/PixelFormat.h>

#include <system/graphics.h>

#include <chrono>

#include <thread>

#include "encode/m2m.h"

#include "utils/thread_safe_queue.h"

#include <cutils/properties.h>

#include "capture/frame_waiter.h"

#include "capture/minicap_impl.hpp"

#include "stream/mjpeg_streamer.hpp"

#include <atomic>

#include <ws.h>

using MJPEGStreamer = nadjieb::MJPEGStreamer;

using namespace android;

// Having more than 1 encoder makes sense only for mjpeg
const int encoder_pool_size = 1;

static FrameWaiter frameWaiter;

ThreadSafeQueue < us_frame_s > capture_queue;

us_encoder_set encoders;

int isH264 = 0;
int encoderQuality = 70;

std::mutex last_encoded_frame_mutex;
us_frame_s last_encoded_frame;

std::atomic<bool> new_frame_captured(false);

MJPEGStreamer streamer;

int get_system_property_int(const char * prop_name) {
  char prop_value[PROPERTY_VALUE_MAX];
  if (property_get(prop_name, prop_value, nullptr) > 0) {
    return atoi(prop_value);
  } else {
    return -1;
  }
}

void createEncoders() {
  if (isH264) {
    std::string encoder_name_h264 = "encoder_h264";
    encoders.h264_encoder = us_m2m_h264_encoder_init(encoder_name_h264.c_str(), "/dev/video11", 20000, 30);
  } else {
    std::string encoder_name_jpeg = "encoder_jpeg";
    encoders.jpeg_encoder = us_m2m_mjpeg_encoder_init(encoder_name_jpeg.c_str(), "/dev/video11", encoderQuality);
  }
}

void capture_thread() {
  Minicap::DisplayInfo displayInfo;

  if (minicap_try_get_display_info(0, & displayInfo) != 0) {
    fprintf(stderr, "Failed to get info from internal display \n");
    exit(1);
  }

  Minicap::Frame capturedFrame;
  bool haveFrame = false;

  Minicap * minicap = minicap_create(0);
  if (minicap == NULL) {
    fprintf(stderr, "Failed to start display capture \n");
    exit(1);
  }

  if (minicap -> setRealInfo(displayInfo) != 0) {
    fprintf(stderr, "Minicap did not accept real display info \n");
    exit(1);
  }

  if (minicap -> setDesiredInfo(displayInfo) != 0) {
    fprintf(stderr, "Minicap did not accept desired display info \n");
    exit(1);
  }

  minicap -> setFrameAvailableListener( & frameWaiter);

  if (minicap -> applyConfigChanges() != 0) {
    fprintf(stderr, "Unable to start minicap with current config \n");
    exit(1);
  }

  int err;
  while (true) {
    if (!frameWaiter.waitForFrame()) {
      fprintf(stderr, "Unable to wait for frame \n");
      exit(1);
    }
    if ((err = minicap -> consumePendingFrame( & capturedFrame)) != 0) {
      if (err == -EINTR) {
        fprintf(stderr, "Frame consumption interrupted by EINTR \n");
        exit(1);
      } else {
        fprintf(stderr, "Unable to consume pending frame \n");
        exit(1);
      }
    }

    us_frame_s encoderFrame;
    encoderFrame.width = capturedFrame.width;
    encoderFrame.height = capturedFrame.height;
    encoderFrame.format = V4L2_PIX_FMT_BGR32;
    encoderFrame.stride = capturedFrame.stride;
    encoderFrame.used = capturedFrame.size;
    encoderFrame.force_key_on_encode = false;
    encoderFrame.dma_fd = capturedFrame.dma_fd;

    new_frame_captured.store(true);
    capture_queue.push(encoderFrame);

    minicap -> releaseConsumedFrame( & capturedFrame);
  }
}

void encode_frame(us_m2m_encoder_s * encoder,
  const us_frame_s & input_frame, us_frame_s & output_frame, unsigned format) {
  output_frame.width = input_frame.width;
  output_frame.height = input_frame.height;
  output_frame.format = format;
  output_frame.stride = 0;
  output_frame.used = 0;
  output_frame.data = NULL;
  output_frame.force_key_on_encode = false;

  int compression_result = us_m2m_encoder_compress(encoder, & input_frame, & output_frame, input_frame.force_key_on_encode);

  if (compression_result != 0) {
    fprintf(stderr, "Failed to compress frame (error code: %d)\n", compression_result);
  }
}

void encode_thread() {
  while (true) {
    us_frame_s input_frame = capture_queue.pop();
    us_frame_s encoded_frame_h264;
    us_frame_s encoded_frame_jpeg;
    us_encoded_frame_set encoded_frames;

    if (isH264) {
      encode_frame(encoders.h264_encoder, input_frame, encoded_frame_h264, V4L2_PIX_FMT_H264);
      encoded_frames.h264_frame = encoded_frame_h264;
    } else {
      encode_frame(encoders.jpeg_encoder, input_frame, encoded_frame_jpeg, V4L2_PIX_FMT_JPEG);
      encoded_frames.jpeg_frame = encoded_frame_jpeg;
    }

    if (encoded_frame_jpeg.data != nullptr || encoded_frame_h264.data != nullptr) {
      if (isH264) {
        free(encoded_frames.h264_frame.data);
      } else {
        last_encoded_frame_mutex.lock();
        if (last_encoded_frame.data != nullptr) {
        	free(last_encoded_frame.data);
        }
        last_encoded_frame = encoded_frames.jpeg_frame;
        last_encoded_frame.data = static_cast < uint8_t * > (malloc(encoded_frames.jpeg_frame.used));
        memcpy(last_encoded_frame.data, encoded_frames.jpeg_frame.data, last_encoded_frame.used);
        last_encoded_frame_mutex.unlock();

        std::string frameData(reinterpret_cast < char * > (encoded_frames.jpeg_frame.data), encoded_frames.jpeg_frame.used);
        streamer.publish("/stream", frameData);
	ws_sendframe_bin(NULL, reinterpret_cast < char * > (encoded_frames.jpeg_frame.data), encoded_frames.jpeg_frame.used);

        free(encoded_frames.jpeg_frame.data);
      }
    } else {
      std::cout << "encode_thread(): Encoded frame data is null" << std::endl;
    }
    free(input_frame.data);
  }
}

void broadcast_thread() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!new_frame_captured.load()) {
        last_encoded_frame_mutex.lock();
        if (last_encoded_frame.data != nullptr) {
            std::string frameData(reinterpret_cast<char*>(last_encoded_frame.data), last_encoded_frame.used);
            streamer.publish("/stream", frameData);
        }
        last_encoded_frame_mutex.unlock();
    }
    new_frame_captured.store(false);
    }
}

void ws_on_connection_opened(ws_cli_conn_t *client) {
  char *cli;
  cli = ws_getaddress(client);
  printf("Connection opened, addr: %s\n", cli);

  if (!new_frame_captured.load()) {
     last_encoded_frame_mutex.lock();
     if (last_encoded_frame.data != nullptr) {
         std::string frameData(reinterpret_cast<char*>(last_encoded_frame.data), last_encoded_frame.used);
         ws_sendframe_bin(NULL, reinterpret_cast<char*>(last_encoded_frame.data), last_encoded_frame.used);
     }
     last_encoded_frame_mutex.unlock();
  }
  new_frame_captured.store(false);
}

void ws_on_connection_closed(ws_cli_conn_t *client) {
  char *cli;
  cli = ws_getaddress(client);
  printf("Connection closed, addr: %s\n", cli);
}

void ws_on_message(__attribute__ ((unused)) ws_cli_conn_t *client,
       __attribute__ ((unused)) const unsigned char *msg,
       __attribute__ ((unused)) uint64_t size,
       __attribute__ ((unused)) int type) {
  ws_ping(NULL, 5);
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char ** argv) {
  minicap_start_thread_pool();
  streamer.start(9090, 4);

  struct ws_events evs;
  evs.onopen    = &ws_on_connection_opened;
  evs.onclose   = &ws_on_connection_closed;
  evs.onmessage = &ws_on_message;
  ws_socket(&evs, 9091, 1, 1000);

  isH264 = get_system_property_int("persist.tesla-android.virtual-display.is_h264");
  encoderQuality = get_system_property_int("persist.tesla-android.virtual-display.quality");

  last_encoded_frame.data = nullptr;

  createEncoders();

  std::thread captureT(capture_thread);
  std::thread encodeT(encode_thread);
  std::thread broadcastT(broadcast_thread);

  captureT.join();
  encodeT.join();
  broadcastT.join();

  return 0;
}

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

#include <cutils/properties.h>

#include "stream/mjpeg_streamer.hpp"

#include "encode/m2m.h"

#include "utils/thread_safe_queue.h"

using MJPEGStreamer = nadjieb::MJPEGStreamer;

using namespace android;

const int encoder_pool_size = 3;

ThreadSafeQueue < us_frame_s > capture_queue;
ThreadSafeQueue < us_frame_s > encoded_queue;
ThreadSafeQueue < us_m2m_encoder_s * > encoder_pool;

int isVariableRefresh = 0;

int get_system_property_int(const char *prop_name)
{
  char prop_value[PROPERTY_VALUE_MAX];
  if (property_get(prop_name, prop_value, nullptr) > 0)
  {
    return atoi(prop_value);
  }
  else
  {
    return -1;
  }
}

void init_encoder_pool(int pool_size) {
  for (int i = 0; i < pool_size; i++) {
    std::string encoder_name = "encoder" + std::to_string(i);
    us_m2m_encoder_s * encoder = us_m2m_jpeg_encoder_init(encoder_name.c_str(), "/dev/video31", 80);
    encoder_pool.push(encoder);
  }
}

void take_screenshot(PhysicalDisplayId & displayId, us_frame_s & frame) {
  void * base = NULL;

  sp < SyncScreenCaptureListener > captureListener = new SyncScreenCaptureListener();
  status_t result = ScreenshotClient::captureDisplay(displayId, captureListener);
  if (result != NO_ERROR) {
    return;
  }

  ScreenCaptureResults captureResults = captureListener -> waitForResults();
  if (captureResults.result != NO_ERROR) {
    return;
  }
  ui::Dataspace dataspace = captureResults.capturedDataspace;
  sp < GraphicBuffer > buffer = captureResults.buffer;

  result = buffer -> lock(GraphicBuffer::USAGE_SW_READ_OFTEN, & base);

  if (base == nullptr || result != NO_ERROR) {
    String8 reason;
    if (result != NO_ERROR) {
      reason.appendFormat(" Error Code: %d", result);
    } else {
      reason = "Failed to write to buffer";
    }
    fprintf(stderr, "Failed to take screenshot (%s)\n", reason.c_str());
    return;
  }

  frame.width = buffer -> getWidth();
  frame.height = buffer -> getHeight();
  frame.format = V4L2_PIX_FMT_RGBA32;
  frame.stride = buffer -> getStride() * bytesPerPixel(buffer -> getPixelFormat());
  frame.used = static_cast < size_t > (frame.width) * static_cast < size_t > (frame.height) * bytesPerPixel(buffer -> getPixelFormat());
  frame.data = static_cast < uint8_t * > (malloc(frame.used));
  memcpy(frame.data, base, frame.used);

  buffer -> unlock();
}

void screenshot_thread() {
  std::optional < PhysicalDisplayId > displayId = SurfaceComposerClient::getInternalDisplayId();
  if (!displayId) {
    fprintf(stderr, "Failed to get token for internal display\n");
    exit(1);
  }

  while (true) {
    us_frame_s frame;
    take_screenshot( * displayId, frame);
    if (frame.data != nullptr) {
      capture_queue.push(frame);
    }
  }
}

void encode_frame(us_m2m_encoder_s * encoder, const us_frame_s & input_frame, us_frame_s & output_frame) {
  output_frame.width = input_frame.width;
  output_frame.height = input_frame.height;
  output_frame.format = V4L2_PIX_FMT_JPEG;
  output_frame.stride = 0;
  output_frame.used = 0;
  output_frame.data = NULL;

  bool force_key = false;
  int compressionResult = us_m2m_encoder_compress(encoder, & input_frame, & output_frame, force_key);

  if (compressionResult != 0) {
    fprintf(stderr, "Failed to compress JPEG (error code: %d)\n", compressionResult);
  }
}

void encode_thread(us_m2m_encoder_s * encoder) {
  while (true) {
    us_frame_s input_frame = capture_queue.pop();
    if (input_frame.data != nullptr) {
      us_frame_s encoded_frame;
      encode_frame(encoder, input_frame, encoded_frame);

      if (encoded_frame.data != nullptr) {
        encoded_queue.push(encoded_frame);
      } else {
        std::cout << "encode_thread(): Encoded frame data is null" << std::endl;
      }
      free(input_frame.data);
    } else {
      std::cout << "encode_thread(): Input frame data is null" << std::endl; 
    }
  }
}

void stream_frame(MJPEGStreamer & streamer, const us_frame_s & encoded_frame) {
  std::string frameData(reinterpret_cast < char * > (encoded_frame.data), encoded_frame.used);
  streamer.publish("/stream", frameData);
  free(encoded_frame.data);
}

void stream_thread() {
  MJPEGStreamer streamer;
  streamer.start(9090, 4);

  while (true) {
    us_frame_s encoded_frame = encoded_queue.pop();
    if (encoded_frame.data != nullptr) {
      stream_frame(streamer, encoded_frame);
    } else {
      std::cout << "stream_thread(): Encoded frame data is null" << std::endl;
    }
  }
}

void stream_thread_variable_refresh() {
  MJPEGStreamer streamer;
  streamer.start(9090, 4);

  us_frame_s last_frame_jpeg;
  bool has_last_frame = false;
  int drop_counter = 0;
  const int drop_limit = 15;   

  while (true) {
    us_frame_s encoded_frame = encoded_queue.pop();
    if (has_last_frame &&
       (us_frame_compare(&last_frame_jpeg, &encoded_frame)) &&
       drop_counter < drop_limit) {
         drop_counter++;
         free(encoded_frame.data);
         continue;
    }
    drop_counter = 0;
    stream_frame(streamer, encoded_frame);
    us_frame_copy(&encoded_frame, &last_frame_jpeg);
    has_last_frame = true;
  }
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char ** argv) {
  ProcessState::self() -> setThreadPoolMaxThreadCount(4);
  ProcessState::self() -> startThreadPool();

  init_encoder_pool(encoder_pool_size);

  isVariableRefresh = get_system_property_int("persist.tesla-android.virtual-display.is_variable_refresh");

  std::thread t_screenshot(screenshot_thread);
  std::vector < std::thread > encode_threads(encoder_pool_size);
  for (int i = 0; i < encoder_pool_size; i++) {
    us_m2m_encoder_s * encoder = encoder_pool.pop();
    encode_threads[i] = std::thread(encode_thread, encoder);
  }
  if (isVariableRefresh == 1){
    std::thread t_stream(stream_thread_variable_refresh);
    t_stream.join();
  } else {
    std::thread t_stream(stream_thread);
    t_stream.join();
  }

  t_screenshot.join();
  for (auto & t: encode_threads) {
    t.join();
  }

  return 0;
}

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

#include <arm_neon.h>

#include <mjpeg_streamer.hpp>

#include <m2m.h>

#include "thread_safe_queue.h"

const int encoder_pool_size = 1;
const int target_fps = 30;
const std::chrono::milliseconds target_frame_duration(1000 / target_fps);

using namespace android;

using MJPEGStreamer = nadjieb::MJPEGStreamer;

void setResolution() {
  char
  const * binaryPath = "/system/bin/wm";
  char
  const * arg1 = "size";
  char
  const * arg2 = "1056x768";

  pid_t pid;
  pid = fork();
  if (pid == -1) {
    perror("fork failed");
    exit(-1);
  } else if (pid == 0) {
    execlp(binaryPath, binaryPath, arg1, arg2, NULL);
    perror("execlp failed");
    exit(-1);
  }
  int status;
  wait( & status);
  printf("child exit status: %d\n", WEXITSTATUS(status));
}

ThreadSafeQueue < us_frame_s > capture_queue;
ThreadSafeQueue < us_frame_s > encoded_queue;
ThreadSafeQueue < us_m2m_encoder_s * > encoder_pool;

void capture_frame(PhysicalDisplayId & displayId, us_frame_s & frame) {
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

void init_encoder_pool(int pool_size) {
  for (int i = 0; i < pool_size; i++) {
    std::string encoder_name = "encoder" + std::to_string(i);
    us_m2m_encoder_s * encoder = us_m2m_jpeg_encoder_init(encoder_name.c_str(), "/dev/video31", 80);
    encoder_pool.push(encoder);
  }
}

void encode_frame(us_m2m_encoder_s * encoder,
  const us_frame_s & input_frame, us_frame_s & output_frame) {

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

void stream_frame(MJPEGStreamer & streamer,
  const us_frame_s & encoded_frame) {
  std::string frameData(reinterpret_cast < char * > (encoded_frame.data), encoded_frame.used);
  streamer.publish("/stream", frameData);
  free(encoded_frame.data);
}

void capture_thread() {
  std::optional < PhysicalDisplayId > displayId = SurfaceComposerClient::getInternalDisplayId();
  if (!displayId) {
    fprintf(stderr, "Failed to get token for internal display\n");
    exit(1);
  }

  using namespace std::chrono;
  steady_clock::time_point last_fps_update_time = steady_clock::now();
  int frame_count = 0;

  while (true) {
    auto loop_start_time = std::chrono::steady_clock::now();

    us_frame_s frame;
    capture_frame( * displayId, frame);
    if (frame.data != nullptr) {
      capture_queue.push(frame);
      frame_count++;
    }

    auto loop_end_time = std::chrono::steady_clock::now();
    auto loop_duration = std::chrono::duration_cast < std::chrono::milliseconds > (loop_end_time - loop_start_time);

    if (loop_duration < target_frame_duration) {
      std::this_thread::sleep_for(target_frame_duration - loop_duration);
    }

    steady_clock::time_point current_time = steady_clock::now();
    duration < double > elapsed_time = current_time - last_fps_update_time;

    if (elapsed_time >= std::chrono::seconds(1)) {
      double fps = frame_count / elapsed_time.count();
      std::cout << "Capture thread FPS: " << fps << std::endl;
      frame_count = 0;
      last_fps_update_time = current_time;
    }
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

void stream_thread() {
  MJPEGStreamer streamer;
  streamer.start(9090, 4);

  using namespace std::chrono;
  steady_clock::time_point last_fps_update_time = steady_clock::now();
  int frame_count = 0;

  while (true) {
    us_frame_s encoded_frame = encoded_queue.pop();
    if (encoded_frame.data != nullptr) {
      stream_frame(streamer, encoded_frame);
      frame_count++; // Increment the frame count
    } else {
      std::cout << "stream_thread(): Encoded frame data is null" << std::endl;
      free(encoded_frame.data);
    }

    steady_clock::time_point current_time = steady_clock::now();
    duration < double > elapsed_time = current_time - last_fps_update_time;

    if (elapsed_time >= std::chrono::seconds(1)) {
      double fps = frame_count / elapsed_time.count();
      std::cout << "Stream thread FPS: " << fps << std::endl;
      frame_count = 0;
      last_fps_update_time = current_time;
    }
  }
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char ** argv) {
  setResolution();

  ProcessState::self() -> setThreadPoolMaxThreadCount(4);
  ProcessState::self() -> startThreadPool();

  init_encoder_pool(encoder_pool_size);

  std::thread t_capture(capture_thread);
  std::vector < std::thread > encode_threads(encoder_pool_size);
  for (int i = 0; i < encoder_pool_size; i++) {
    us_m2m_encoder_s * encoder = encoder_pool.pop();
    encode_threads[i] = std::thread(encode_thread, encoder);
  }
  std::thread t_stream(stream_thread);

  t_capture.join();
  for (auto & t: encode_threads) {
    t.join();
  }
  t_stream.join();

  return 0;
}
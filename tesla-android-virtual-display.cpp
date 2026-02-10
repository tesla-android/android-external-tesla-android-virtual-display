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

#include <gui/ISurfaceComposer.h>            // ScreenshotClient
#include <gui/SurfaceComposerClient.h>
#include <gui/SyncScreenCaptureListener.h>

#include <ui/GraphicTypes.h>
#include <ui/PixelFormat.h>
#include <ui/DisplayId.h>
#include <system/graphics.h>

#include <chrono>
#include <thread>
#include <atomic>
#include <sstream>
#include <iostream>
#include <optional>
#include <vector>

#include "encode/m2m.h"
#include "utils/thread_safe_queue.h"
#include <cutils/properties.h>
#include "capture/frame_waiter.h"
#include "stream/mjpeg_streamer.hpp"
#include "capture/minicap_impl.hpp"
#include <ws.h>

using MJPEGStreamer = nadjieb::MJPEGStreamer;
using namespace android;

static FrameWaiter frameWaiter;

ThreadSafeQueue<us_frame_s> capture_queue;

us_encoder_set encoders;

int isH264 = 0;
int encoderQuality = 70;

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
    encoders.h264_encoder = us_m2m_h264_encoder_init(encoder_name_h264.c_str(), "/dev/video11", 32000 * (encoderQuality / 100), 30);
  } else {
    std::string encoder_name_jpeg = "encoder_jpeg";
    encoders.jpeg_encoder = us_m2m_mjpeg_encoder_init(encoder_name_jpeg.c_str(), "/dev/video11", encoderQuality);
  }
}

/**
 * Minimal screencap-like helper for your headers:
 * - 2-arg ScreenshotClient::captureDisplay(displayId, listener)
 * - ScreenCaptureResults has .buffer only (no fences/results)
 */
static status_t capture_one_simple(const android::DisplayId displayId,
                                   android::ScreenCaptureResults& outResult)
{
  sp<SyncScreenCaptureListener> listener = new SyncScreenCaptureListener();

  status_t st = ScreenshotClient::captureDisplay(displayId, listener);
  if (st != NO_ERROR) {
    fprintf(stderr, "capture_one_simple(): captureDisplay failed: %d\n", st);
    return st;
  }

  ScreenCaptureResults results = listener->waitForResults();
  if (results.buffer == nullptr) {
    fprintf(stderr, "capture_one_simple(): null buffer\n");
    return UNKNOWN_ERROR;
  }

  outResult = results;
  return NO_ERROR;
}

/**
 * Capture ONE screenshot (no display config changes), and push it **as DMABUF**
 * to avoid switching the encoder away from DMA.
 * We DUP the buffer FD so its lifetime is independent of the GraphicBuffer.
 */
static bool push_one_screenshot_frame_dma()
{
  // Choose a physical display (first available)
  const auto ids = SurfaceComposerClient::getPhysicalDisplayIds();
  if (ids.empty()) {
    fprintf(stderr, "push_one_screenshot_frame_dma(): no physical displays\n");
    return false;
  }
  const DisplayId displayId = ids.front(); // implicit to DisplayId in your tree

  ScreenCaptureResults results;
  status_t st = capture_one_simple(displayId, results);
  if (st != NO_ERROR || results.buffer == nullptr) {
    fprintf(stderr, "push_one_screenshot_frame_dma(): capture failed (%d)\n", st);
    return false;
  }

  sp<GraphicBuffer> gb = results.buffer;

  // Get a DMABUF FD from the native handle (same pattern you use in minicap)
  ANativeWindowBuffer* anb = gb->getNativeBuffer();
  if (!anb || !anb->handle) {
    fprintf(stderr, "push_one_screenshot_frame_dma(): invalid native buffer/handle\n");
    return false;
  }

  // Most grallocs expose the prime fd in handle->data[0]
  const int src_fd = anb->handle->data[0];
  if (src_fd < 0) {
    fprintf(stderr, "push_one_screenshot_frame_dma(): no valid dmabuf fd in handle->data[0]\n");
    return false;
  }

  // DUP so V4L2 can safely use it after this function returns
  const int dup_fd = dup(src_fd);
  if (dup_fd < 0) {
    perror("dup(dmabuf)");
    return false;
  }

  const int width  = gb->getWidth();
  const int height = gb->getHeight();
  const int bpp    = android::bytesPerPixel(gb->getPixelFormat()); // usually 4
  const size_t bytes = (size_t)width * (size_t)height * (size_t)std::max(1, bpp);

  // Enqueue as DMA (data=NULL); keep your existing V4L2_FMT (BGR32) so encoder doesn't re-prepare
  us_frame_s encoderFrame = {};
  encoderFrame.width  = width;
  encoderFrame.height = height;
  encoderFrame.format = V4L2_PIX_FMT_BGR32; // matches your current pipeline
  encoderFrame.stride = 0;                   // unused for DMA
  encoderFrame.used   = bytes;               // consumed as bytesused for DMABUF
  encoderFrame.force_key_on_encode = true;   // ensure IDR
  encoderFrame.data   = NULL;                // NULL for DMA path
  encoderFrame.dma_fd = dup_fd;              // our own ref-counted fd

  capture_queue.push(encoderFrame);
  return true;
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
      fprintf(stderr, "Unable to wait for frame, retrying\n");
      continue;
    }
    if ((err = minicap -> consumePendingFrame( & capturedFrame)) != 0) {
      if (err == -EINTR) {
        fprintf(stderr, "Frame consumption interrupted by EINTR, retrying\n");
        continue;
      } else {
        fprintf(stderr, "Unable to consume pending frame \n");
        exit(1);
      }
    }

    us_frame_s encoderFrame = {};
    encoderFrame.width = capturedFrame.width;
    encoderFrame.height = capturedFrame.height;
    encoderFrame.format = V4L2_PIX_FMT_BGR32;
    encoderFrame.stride = capturedFrame.stride;
    encoderFrame.used = capturedFrame.size;
    encoderFrame.force_key_on_encode = false;
    encoderFrame.dma_fd = capturedFrame.dma_fd;
    capture_queue.push(encoderFrame);

    minicap -> releaseConsumedFrame( & capturedFrame);
  }
}

void encode_frame(us_m2m_encoder_s * encoder,
  const us_frame_s & input_frame, us_frame_s & output_frame, unsigned format) {
  output_frame = {};
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

static void release_input_frame_resources(const us_frame_s& frame) {
  // This heuristically matches our "screenshot-as-DMA" frame.
  if (frame.dma_fd >= 0 && frame.data == NULL && frame.force_key_on_encode) {
    close(frame.dma_fd);
  }

  // Free CPU buffers from normal (non-DMA) capture path (if any).
  if (frame.data) {
    free(frame.data);
  }
}

void encode_thread() {
  while (true) {
    us_frame_s input_frame = capture_queue.pop();
    us_frame_s latest_frame = {};
    while (capture_queue.try_pop(latest_frame)) {
      // Drop stale frames to keep latency low under backpressure.
      release_input_frame_resources(input_frame);
      input_frame = latest_frame;
      latest_frame = {};
    }

    if (isH264) {
      us_frame_s encoded_frame_h264;
      encode_frame(encoders.h264_encoder, input_frame, encoded_frame_h264, V4L2_PIX_FMT_H264);

      if (encoded_frame_h264.data != nullptr) {
        ws_sendframe_bin(NULL, reinterpret_cast<char*>(encoded_frame_h264.data), encoded_frame_h264.used);
        free(encoded_frame_h264.data);
      } else {
        std::cout << "encode_thread(): Encoded frame data is null" << std::endl;
      }
    } else {
      us_frame_s encoded_frame_jpeg;
      encode_frame(encoders.jpeg_encoder, input_frame, encoded_frame_jpeg, V4L2_PIX_FMT_JPEG);
      if (encoded_frame_jpeg.data != nullptr) {
        std::string frameData(reinterpret_cast<char*>(encoded_frame_jpeg.data), encoded_frame_jpeg.used);
        streamer.publish("/stream", frameData);
        ws_sendframe_bin(NULL, reinterpret_cast<char*>(encoded_frame_jpeg.data), encoded_frame_jpeg.used);
        free(encoded_frame_jpeg.data);
      } else {
        std::cout << "encode_thread(): Encoded frame data is null" << std::endl;
      }
    }

    release_input_frame_resources(input_frame);
  }
}

void ws_on_connection_opened(ws_cli_conn_t *client) {
  char *cli;
  cli = ws_getaddress(client);
  printf("Connection opened, addr: %s\n", cli);

  // Capture ONE screenshot as **DMABUF** so the encoder stays in DMA mode.
  if (!push_one_screenshot_frame_dma()) {
    fprintf(stderr, "Warning: screenshot-on-connect failed; will wait for next minicap frame.\n");
  }

  // Force an IDR from the encoder ASAP (if H264) so the first encoded frame after connect is a keyframe.
  if (isH264 && encoders.h264_encoder) {
    int rc = us_m2m_encoder_request_keyframe(encoders.h264_encoder);
    if (rc < 0) {
      fprintf(stderr, "request_keyframe failed: %d\n", rc);
    }
  }
}

void ws_on_connection_closed(ws_cli_conn_t *client) {
  char *cli;
  cli = ws_getaddress(client);
  printf("Connection closed, addr: %s\n", cli);
}

void ws_on_message(__attribute__ ((unused)) ws_cli_conn_t *client,
       __attribute__ ((unused)) const unsigned char *msg,
       __attribute__ ((unused)) uint64_t size,
       __attribute__ ((unused)) int type) {}

void ws_ping_thread() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ws_ping(NULL, 5);
  }
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char ** argv) {
  minicap_start_thread_pool();

  struct ws_events evs;
  evs.onopen    = &ws_on_connection_opened;
  evs.onclose   = &ws_on_connection_closed;
  evs.onmessage = &ws_on_message;
  ws_socket(&evs, 9091, 1, 1000);
  std::thread(ws_ping_thread).detach();

  isH264 = get_system_property_int("persist.tesla-android.virtual-display.is_h264");
  encoderQuality = get_system_property_int("persist.tesla-android.virtual-display.quality");

  createEncoders();

  std::thread captureT(capture_thread);
  std::thread encodeT(encode_thread);

  captureT.join();
  encodeT.join();

  return 0;
}

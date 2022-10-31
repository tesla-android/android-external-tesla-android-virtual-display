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

#include <mjpeg_streamer.hpp>

using namespace android;

using MJPEGStreamer = nadjieb::MJPEGStreamer;

static int32_t flinger2bitmapFormat(PixelFormat f) {
  switch (f) {
  case PIXEL_FORMAT_RGB_565:
    return ANDROID_BITMAP_FORMAT_RGB_565;
  default:
    return ANDROID_BITMAP_FORMAT_RGBA_8888;
  }
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char ** argv) {
  std::optional < PhysicalDisplayId > displayId = SurfaceComposerClient::getInternalDisplayId();
  if (!displayId) {
    fprintf(stderr, "Failed to get token for internal display\n");
    return 1;
  }

  ProcessState::self() -> setThreadPoolMaxThreadCount(4);
  ProcessState::self() -> startThreadPool();

  MJPEGStreamer streamer;
  streamer.start(9090, 4);

  while (true) {
    void * base = NULL;

    sp < SyncScreenCaptureListener > captureListener = new SyncScreenCaptureListener();
    status_t result = ScreenshotClient::captureDisplay(displayId -> value, captureListener);
    if (result != NO_ERROR) {
      continue;
    }

    ScreenCaptureResults captureResults = captureListener -> waitForResults();
    if (captureResults.result != NO_ERROR) {
      continue;
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
      continue;
    }

    AndroidBitmapInfo info;
    info.format = flinger2bitmapFormat(buffer -> getPixelFormat());
    info.flags = ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;
    info.width = buffer -> getWidth();
    info.height = buffer -> getHeight();
    info.stride = buffer -> getStride() * bytesPerPixel(buffer -> getPixelFormat());

    std::string buff = "";

    int compressionResult = AndroidBitmap_compress( & info, static_cast < int32_t > (dataspace), base,
      ANDROID_BITMAP_COMPRESS_FORMAT_JPEG, 75, & buff,
      [](void * fdPtr,
        const void * data, size_t size) -> bool {
        std::string * castedBuffer = (std::string * ) fdPtr;
        castedBuffer -> append((char * ) data, size);
        return true;
      });

    if (compressionResult != ANDROID_BITMAP_RESULT_SUCCESS) {
      fprintf(stderr, "Failed to compress PNG (error code: %d)\n", compressionResult);
      continue;
    }

    streamer.publish("/stream", buff);

  }

  return 0;
}

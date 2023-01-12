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

#include <ws.h>

using namespace android;

static int32_t flinger2bitmapFormat(PixelFormat f) {
  switch (f) {
  case PIXEL_FORMAT_RGB_565:
    return ANDROID_BITMAP_FORMAT_RGB_565;
  default:
    return ANDROID_BITMAP_FORMAT_RGBA_8888;
  }
}

void setResolution() {
  char const *binaryPath = "/system/bin/wm";
  char const *arg1 = "size";
  char const *arg2 = "1034x788";

  pid_t pid;
  pid = fork();
  if (pid == -1) {
      perror("fork failed");
      exit(-1);
  } else if (pid == 0) {
    execlp(binaryPath, binaryPath, arg1, arg2 ,NULL);
    perror("execlp failed");
    exit(-1);
  }
  int status;
  wait(&status);
  printf("child exit status: %d\n", WEXITSTATUS(status));
}

void webSocketOnConnectionOpened(ws_cli_conn_t *client) {
  char *cli;
  cli = ws_getaddress(client);
  printf("Connection opened, addr: %s\n", cli);
}

void webSocketOnConnectionClosed(ws_cli_conn_t *client) {
  char *cli;
  cli = ws_getaddress(client);
  printf("Connection closed, addr: %s\n", cli);
}

void webSocketOnMessage(ws_cli_conn_t *client,
       const unsigned char *msg, uint64_t size, int type) {
  char *cli;
  cli = ws_getaddress(client);
  printf("Message: %s (size: %" PRId64 ", type: %d), from: %s\n", msg, size, type, cli);
}


int main(__attribute__((unused)) int argc, __attribute__((unused)) char ** argv) {
  std::optional < PhysicalDisplayId > displayId = SurfaceComposerClient::getInternalDisplayId();
  if (!displayId) {
    fprintf(stderr, "Failed to get token for internal display\n");
    return 1;
  }

  setResolution();

  ProcessState::self() -> setThreadPoolMaxThreadCount(4);
  ProcessState::self() -> startThreadPool();

  struct ws_events evs;
  evs.onopen    = &webSocketOnConnectionOpened;
  evs.onclose   = &webSocketOnConnectionClosed;
  evs.onmessage = &webSocketOnMessage;
  ws_socket(&evs, 9090, 1, 1000);

  while (true) {
    void * base = NULL;

    sp < SyncScreenCaptureListener > captureListener = new SyncScreenCaptureListener();
    status_t result = ScreenshotClient::captureDisplay(*displayId, captureListener);
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
      ANDROID_BITMAP_COMPRESS_FORMAT_JPEG, 80, & buff,
      [](void * fdPtr,
        const void * data, size_t size) -> bool {
        std::string * castedBuffer = (std::string * ) fdPtr;
        castedBuffer -> append((char * ) data, size);
        return true;
      });

    if (compressionResult != ANDROID_BITMAP_RESULT_SUCCESS) {
      fprintf(stderr, "Failed to compress JPEG (error code: %d)\n", compressionResult);
      continue;
    }

    ws_sendframe_bin(NULL, buff.c_str(), buff.length());
  }

  return 0;
}

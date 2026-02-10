#include "m2m.h"


static us_m2m_encoder_s *_m2m_encoder_init(
	const char *name, const char *path, unsigned output_format,
	unsigned fps, unsigned bitrate, unsigned gop, unsigned quality, bool allow_dma);

static void _m2m_encoder_prepare(us_m2m_encoder_s *enc, const us_frame_s *frame);

static int _m2m_encoder_init_buffers(
	us_m2m_encoder_s *enc, const char *name, enum v4l2_buf_type type,
	us_m2m_buffer_s **bufs_ptr, unsigned *n_bufs_ptr, bool dma);

static void _m2m_encoder_cleanup(us_m2m_encoder_s *enc);

static int _m2m_encoder_compress_raw(us_m2m_encoder_s *enc, const us_frame_s *src, us_frame_s *dest, bool force_key);

static int _m2m_encoder_set_ctrl(
	us_m2m_encoder_s *enc, uint32_t cid, int value, bool optional, const char *cid_name);


#define _E_LOG_ERROR(x_msg, ...)	US_LOG_ERROR("%s: " x_msg, enc->name, ##__VA_ARGS__)
#define _E_LOG_PERROR(x_msg, ...)	US_LOG_PERROR("%s: " x_msg, enc->name, ##__VA_ARGS__)
#define _E_LOG_INFO(x_msg, ...)		US_LOG_INFO("%s: " x_msg, enc->name, ##__VA_ARGS__)
#define _E_LOG_VERBOSE(x_msg, ...)	US_LOG_VERBOSE("%s: " x_msg, enc->name, ##__VA_ARGS__)
#define _E_LOG_DEBUG(x_msg, ...)	US_LOG_DEBUG("%s: " x_msg, enc->name, ##__VA_ARGS__)

#define _RUN(x_next) enc->run->x_next

static bool _g_m2m_diagnostics_enabled = false;

void us_m2m_encoder_set_diagnostics(bool enabled) {
	_g_m2m_diagnostics_enabled = enabled;
}

static void _m2m_encoder_diag_report_ctrl_support(us_m2m_encoder_s *enc, uint32_t cid, const char *cid_name);
static void _m2m_encoder_diag_report_ctrl_value(us_m2m_encoder_s *enc, uint32_t cid, const char *cid_name);
static void _m2m_encoder_diag_report_format(
	us_m2m_encoder_s *enc, const char *stream_name, const struct v4l2_format *fmt);
static void _m2m_encoder_diag_report_recommended_v4l2ctl(us_m2m_encoder_s *enc);
static void _m2m_encoder_diag_report_controls(us_m2m_encoder_s *enc);

int us_m2m_encoder_request_keyframe(us_m2m_encoder_s *enc) {
	if (!enc || !enc->run) return -EINVAL;

	if (!_RUN(ready) || _RUN(fd) < 0) {
		_RUN(force_key_pending) = 1;
		return 0;
	}

	struct v4l2_control ctl = {0};
	ctl.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
	ctl.value = 1;

	if (ioctl(_RUN(fd), VIDIOC_S_CTRL, &ctl) == 0) {
		_RUN(force_key_pending) = 0;
		return 0;
	}

	if (errno == EBUSY || errno == EINVAL) {
		_RUN(force_key_pending) = 1;
		return 0;
	}
	_RUN(force_key_pending) = 1;
	return -errno;
}

us_m2m_encoder_s *us_m2m_h264_encoder_init(const char *name, const char *path, unsigned bitrate, unsigned gop) {
	// FIXME: 30 or 0? https://github.com/6by9/yavta/blob/master/yavta.c#L2100
	// По логике вещей правильно 0, но почему-то на низких разрешениях типа 640x480
	// енкодер через несколько секунд перестает производить корректные фреймы.
	bitrate *= 1000; // From Kbps
	return _m2m_encoder_init(name, path, V4L2_PIX_FMT_H264, 30, bitrate, gop, 0, true);
}

us_m2m_encoder_s *us_m2m_mjpeg_encoder_init(const char *name, const char *path, unsigned quality) {
	const double b_min = 25;
	const double b_max = 20000;
	const double step = 25;
	double bitrate = log10(quality) * (b_max - b_min) / 2 + b_min;
	bitrate = step * round(bitrate / step);
	bitrate *= 1000; // From Kbps
	assert(bitrate > 0);
	// FIXME: То же самое про 30 or 0, но еще даже не проверено на низких разрешениях
	return _m2m_encoder_init(name, path, V4L2_PIX_FMT_MJPEG, 30, bitrate, 0, 0, true);
}

us_m2m_encoder_s *us_m2m_jpeg_encoder_init(const char *name, const char *path, unsigned quality) {
	// FIXME: DMA не работает
	return _m2m_encoder_init(name, path, V4L2_PIX_FMT_JPEG, 30, 0, 0, quality, false);
}

void us_m2m_encoder_destroy(us_m2m_encoder_s *enc) {
	_E_LOG_INFO("Destroying encoder ...");
	_m2m_encoder_cleanup(enc);
	free(enc->path);
	free(enc->name);
	free(enc);
}

int us_m2m_encoder_compress(us_m2m_encoder_s *enc, const us_frame_s *src, us_frame_s *dest, bool force_key) {
	us_frame_encoding_begin(src, dest, (enc->output_format == V4L2_PIX_FMT_MJPEG ? V4L2_PIX_FMT_JPEG : enc->output_format));

	if (
		_RUN(width) != src->width
		|| _RUN(height) != src->height
		|| _RUN(input_format) != src->format
		|| _RUN(stride) != src->stride
		|| _RUN(dma) != (enc->allow_dma && src->dma_fd >= 0)
	) {
		_m2m_encoder_prepare(enc, src);
	}
	if (!_RUN(ready)) { // Already prepared but failed
		return -1;
	}

#if defined(__GNUC__)
	int pending = __atomic_exchange_n(&_RUN(force_key_pending), 0, __ATOMIC_ACQ_REL);
#else
	int pending = _RUN(force_key_pending);
	_RUN(force_key_pending) = 0;
#endif

	force_key = (enc->output_format == V4L2_PIX_FMT_H264) &&
	            (force_key || pending || _RUN(last_online) != src->online);


	if (_m2m_encoder_compress_raw(enc, src, dest, force_key) < 0) {
		_m2m_encoder_cleanup(enc);
		_E_LOG_ERROR("Encoder destroyed due an error (compress)");
		return -1;
	}

	us_frame_encoding_end(dest);

	_E_LOG_VERBOSE("Compressed new frame: size=%zu, time=%0.3Lf, force_key=%d",
		dest->used, dest->encode_end_ts - dest->encode_begin_ts, force_key);

	_RUN(last_online) = src->online;
	return 0;
}

static us_m2m_encoder_s *_m2m_encoder_init(
	const char *name, const char *path, unsigned output_format,
	unsigned fps, unsigned bitrate, unsigned gop, unsigned quality, bool allow_dma) {

	US_LOG_INFO("%s: Initializing encoder ...", name);

	us_m2m_encoder_runtime_s *run = calloc(1, sizeof(us_m2m_encoder_runtime_s));

	run->force_key_pending = 0;
	run->last_online = -1;
	run->fd = -1;

	us_m2m_encoder_s *enc = calloc(1, sizeof(us_m2m_encoder_s));
	enc->name = us_strdup(name);
	if (path == NULL) {
		enc->path = us_strdup(output_format == V4L2_PIX_FMT_JPEG ? "/dev/video31" : "/dev/video11");
	} else {
		enc->path = us_strdup(path);
	}
	enc->output_format = output_format;
	enc->fps = fps;
	enc->bitrate = bitrate;
	enc->gop = gop;
	enc->quality = quality;
	enc->allow_dma = allow_dma;
	enc->run = run;
	return enc;
}

#define _E_XIOCTL(x_request, x_value, x_msg, ...) { \
		if (us_xioctl(_RUN(fd), x_request, x_value) < 0) { \
			_E_LOG_PERROR(x_msg, ##__VA_ARGS__); \
			goto error; \
		} \
	}

static int _m2m_encoder_set_ctrl(
	us_m2m_encoder_s *enc, uint32_t cid, int value, bool optional, const char *cid_name) {
	struct v4l2_control ctl = {0};
	ctl.id = cid;
	ctl.value = value;
	_E_LOG_DEBUG("Configuring option %s=%d ...", cid_name, value);

	if (us_xioctl(_RUN(fd), VIDIOC_S_CTRL, &ctl) == 0) {
		_m2m_encoder_diag_report_ctrl_value(enc, cid, cid_name);
		return 1;
	}

	if (optional && (
		errno == EINVAL
		|| errno == ENOTTY
		|| errno == EOPNOTSUPP
		|| errno == ERANGE
	)) {
		_E_LOG_INFO("Skipping unsupported option %s=%d", cid_name, value);
		return 0;
	}

	_E_LOG_PERROR("Can't set option %s", cid_name);
	return -1;
}

static void _m2m_encoder_prepare(us_m2m_encoder_s *enc, const us_frame_s *frame) {
	const bool dma = (enc->allow_dma && frame->dma_fd >= 0);

	_RUN(force_key_pending) = 0;

	_E_LOG_INFO("Configuring encoder: DMA=%d ...", dma);

	_m2m_encoder_cleanup(enc);

	_RUN(width) = frame->width;
	_RUN(height) = frame->height;
	_RUN(input_format) = frame->format;
	_RUN(stride) = frame->stride;
	_RUN(dma) = dma;

	if ((_RUN(fd) = open(enc->path, O_RDWR)) < 0) {
		_E_LOG_PERROR("Can't open encoder device");
		goto error;
	}
	_E_LOG_DEBUG("Encoder device fd=%d opened", _RUN(fd));
	_m2m_encoder_diag_report_recommended_v4l2ctl(enc);
	_m2m_encoder_diag_report_controls(enc);

#	define SET_OPTION_REQUIRED(x_cid, x_value) { \
			if (_m2m_encoder_set_ctrl(enc, x_cid, x_value, false, #x_cid) < 0) { \
				goto error; \
			} \
		}
#	define SET_OPTION_OPTIONAL(x_cid, x_value) { \
			if (_m2m_encoder_set_ctrl(enc, x_cid, x_value, true, #x_cid) < 0) { \
				goto error; \
			} \
		}

	if (enc->output_format == V4L2_PIX_FMT_H264) {
		int high_profile_set = 0;
		int level_set = 0;

		SET_OPTION_REQUIRED(V4L2_CID_MPEG_VIDEO_BITRATE, enc->bitrate);
#if defined(V4L2_CID_MPEG_VIDEO_BITRATE_MODE) && defined(V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)
		SET_OPTION_OPTIONAL(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
#endif
		SET_OPTION_REQUIRED(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, enc->gop);
#if defined(V4L2_CID_MPEG_VIDEO_H264_PROFILE) && defined(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)
		{
			int status = _m2m_encoder_set_ctrl(
				enc,
				V4L2_CID_MPEG_VIDEO_H264_PROFILE,
				V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
				true,
				"V4L2_CID_MPEG_VIDEO_H264_PROFILE(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)"
			);
			if (status < 0) {
				goto error;
			}
			high_profile_set = (status > 0);
		}
#endif
		if (!high_profile_set) {
			SET_OPTION_REQUIRED(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE);
		}
#if defined(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE) && defined(V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC)
		SET_OPTION_OPTIONAL(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE, V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC);
#endif
		if (_RUN(width) * _RUN(height) <= 1920 * 1080) { // https://forums.raspberrypi.com/viewtopic.php?t=291447#p1762296
			SET_OPTION_REQUIRED(V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_0);
			level_set = 1;
		} else {
#if defined(V4L2_MPEG_VIDEO_H264_LEVEL_4_2)
			{
				int status = _m2m_encoder_set_ctrl(
					enc,
					V4L2_CID_MPEG_VIDEO_H264_LEVEL,
					V4L2_MPEG_VIDEO_H264_LEVEL_4_2,
					true,
					"V4L2_CID_MPEG_VIDEO_H264_LEVEL(V4L2_MPEG_VIDEO_H264_LEVEL_4_2)"
				);
				if (status < 0) {
					goto error;
				}
				level_set = (status > 0);
			}
#endif
			if (!level_set) {
				SET_OPTION_REQUIRED(V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_0);
			}
		}
		SET_OPTION_REQUIRED(V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1);
#if defined(V4L2_CID_MPEG_VIDEO_H264_MIN_QP)
		SET_OPTION_OPTIONAL(V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 12);
#endif
#if defined(V4L2_CID_MPEG_VIDEO_H264_MAX_QP)
		SET_OPTION_OPTIONAL(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 36);
#endif
	} else if (enc->output_format == V4L2_PIX_FMT_MJPEG) {
		SET_OPTION_REQUIRED(V4L2_CID_MPEG_VIDEO_BITRATE, enc->bitrate);
	} else if (enc->output_format == V4L2_PIX_FMT_JPEG) {
		SET_OPTION_REQUIRED(V4L2_CID_JPEG_COMPRESSION_QUALITY, enc->quality);
	}

#	undef SET_OPTION_REQUIRED
#	undef SET_OPTION_OPTIONAL

	{
		struct v4l2_format fmt = {0};
		fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		fmt.fmt.pix_mp.width = _RUN(width);
		fmt.fmt.pix_mp.height = _RUN(height);
		fmt.fmt.pix_mp.pixelformat = _RUN(input_format);
		fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
#ifdef V4L2_COLORSPACE_SRGB
		fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_SRGB;
#else
		fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG; // fallback for older headers
#endif
		fmt.fmt.pix_mp.num_planes = 1;
		if (_RUN(stride) > 0) {
			fmt.fmt.pix_mp.plane_fmt[0].bytesperline = _RUN(stride);
		}
		_E_LOG_DEBUG("Configuring INPUT format ...");
		_E_XIOCTL(VIDIOC_S_FMT, &fmt, "Can't set INPUT format");
		_m2m_encoder_diag_report_format(enc, "INPUT", &fmt);
	}

	{
		struct v4l2_format fmt = {0};
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		fmt.fmt.pix_mp.width = _RUN(width);
		fmt.fmt.pix_mp.height = _RUN(height);
		fmt.fmt.pix_mp.pixelformat = enc->output_format;
		fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
		fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
		fmt.fmt.pix_mp.num_planes = 1;
		// fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
		if (enc->output_format == V4L2_PIX_FMT_H264) {
			// https://github.com/pikvm/ustreamer/issues/169
			// https://github.com/raspberrypi/linux/pull/5232
			const uint64_t min_sizeimage = (uint64_t)((1024 + 512) << 10); // 1.5 MiB
			uint64_t dynamic_sizeimage = ((uint64_t)_RUN(width) * _RUN(height) * 3) / 2;
			if (dynamic_sizeimage < min_sizeimage) {
				dynamic_sizeimage = min_sizeimage;
			}
			if (dynamic_sizeimage > UINT32_MAX) {
				dynamic_sizeimage = UINT32_MAX;
			}
			fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (uint32_t)dynamic_sizeimage;
		}
		_E_LOG_DEBUG("Configuring OUTPUT format ...");
		_E_XIOCTL(VIDIOC_S_FMT, &fmt, "Can't set OUTPUT format");
		_m2m_encoder_diag_report_format(enc, "OUTPUT", &fmt);
		if (fmt.fmt.pix_mp.pixelformat != enc->output_format) {
			char fourcc_str[8];
			_E_LOG_ERROR("The OUTPUT format can't be configured as %s",
				us_fourcc_to_string(enc->output_format, fourcc_str, 8));
			_E_LOG_ERROR("In case of Raspberry Pi, try to append 'start_x=1' to /boot/config.txt");
			goto error;
		}
	}

	if (enc->fps > 0) { // TODO: Check this for MJPEG
		struct v4l2_streamparm setfps = {0};
		setfps.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		setfps.parm.output.timeperframe.numerator = 1;
		setfps.parm.output.timeperframe.denominator = enc->fps;
		_E_LOG_DEBUG("Configuring INPUT FPS ...");
		_E_XIOCTL(VIDIOC_S_PARM, &setfps, "Can't set INPUT FPS");
	}

	if (_m2m_encoder_init_buffers(enc, (dma ? "INPUT-DMA" : "INPUT"), V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		&_RUN(input_bufs), &_RUN(n_input_bufs), dma) < 0) {
		goto error;
	}
	if (_m2m_encoder_init_buffers(enc, "OUTPUT", V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		&_RUN(output_bufs), &_RUN(n_output_bufs), false) < 0) {
		goto error;
	}

	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		_E_LOG_DEBUG("Starting INPUT ...");
		_E_XIOCTL(VIDIOC_STREAMON, &type, "Can't start INPUT");

		type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		_E_LOG_DEBUG("Starting OUTPUT ...");
		_E_XIOCTL(VIDIOC_STREAMON, &type, "Can't start OUTPUT");
	}

	_RUN(ready) = true;
	_E_LOG_DEBUG("Encoder state: *** READY ***");
	return;

	error:
		_m2m_encoder_cleanup(enc);
		_E_LOG_ERROR("Encoder destroyed due an error (prepare)");
}

static void _m2m_encoder_diag_report_ctrl_support(us_m2m_encoder_s *enc, uint32_t cid, const char *cid_name) {
	if (!_g_m2m_diagnostics_enabled || _RUN(fd) < 0) {
		return;
	}

	struct v4l2_queryctrl query = {0};
	query.id = cid;
	if (ioctl(_RUN(fd), VIDIOC_QUERYCTRL, &query) == 0) {
		_E_LOG_INFO(
			"Diag control support: %s (%#x) min=%d max=%d step=%d default=%d flags=%#x",
			cid_name,
			cid,
			query.minimum,
			query.maximum,
			query.step,
			query.default_value,
			query.flags
		);
		return;
	}

	if (errno == EINVAL) {
		_E_LOG_INFO("Diag control support: %s (%#x) not supported", cid_name, cid);
		return;
	}

	_E_LOG_PERROR("Diag control support query failed for %s", cid_name);
}

static void _m2m_encoder_diag_report_ctrl_value(us_m2m_encoder_s *enc, uint32_t cid, const char *cid_name) {
	if (!_g_m2m_diagnostics_enabled || _RUN(fd) < 0) {
		return;
	}

	struct v4l2_control ctrl = {0};
	ctrl.id = cid;
	if (ioctl(_RUN(fd), VIDIOC_G_CTRL, &ctrl) == 0) {
		_E_LOG_INFO("Diag control applied: %s (%#x)=%d", cid_name, cid, ctrl.value);
		return;
	}

	if (errno == EINVAL) {
		_E_LOG_INFO("Diag control readback unavailable: %s (%#x)", cid_name, cid);
		return;
	}

	_E_LOG_PERROR("Diag control readback failed for %s", cid_name);
}

static void _m2m_encoder_diag_report_format(
	us_m2m_encoder_s *enc, const char *stream_name, const struct v4l2_format *fmt) {
	if (!_g_m2m_diagnostics_enabled) {
		return;
	}

	char fourcc_str[8];
	_E_LOG_INFO(
		"Diag %s format: %ux%u %s bytesperline=%u sizeimage=%u colorspace=%u",
		stream_name,
		fmt->fmt.pix_mp.width,
		fmt->fmt.pix_mp.height,
		us_fourcc_to_string(fmt->fmt.pix_mp.pixelformat, fourcc_str, sizeof(fourcc_str)),
		fmt->fmt.pix_mp.plane_fmt[0].bytesperline,
		fmt->fmt.pix_mp.plane_fmt[0].sizeimage,
		fmt->fmt.pix_mp.colorspace
	);
}

static void _m2m_encoder_diag_report_recommended_v4l2ctl(us_m2m_encoder_s *enc) {
	if (!_g_m2m_diagnostics_enabled) {
		return;
	}
	_E_LOG_INFO("Diag run on device: v4l2-ctl -d %s --list-ctrls-menus", enc->path);
	_E_LOG_INFO("Diag run on device: v4l2-ctl -d %s --all", enc->path);
}

static void _m2m_encoder_diag_report_controls(us_m2m_encoder_s *enc) {
	if (!_g_m2m_diagnostics_enabled) {
		return;
	}

	if (enc->output_format == V4L2_PIX_FMT_H264) {
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_BITRATE, "V4L2_CID_MPEG_VIDEO_BITRATE");
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, "V4L2_CID_MPEG_VIDEO_H264_I_PERIOD");
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_H264_PROFILE, "V4L2_CID_MPEG_VIDEO_H264_PROFILE");
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_H264_LEVEL, "V4L2_CID_MPEG_VIDEO_H264_LEVEL");
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, "V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER");
#if defined(V4L2_CID_MPEG_VIDEO_BITRATE_MODE)
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, "V4L2_CID_MPEG_VIDEO_BITRATE_MODE");
#endif
#if defined(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE)
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE, "V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE");
#endif
#if defined(V4L2_CID_MPEG_VIDEO_H264_MIN_QP)
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, "V4L2_CID_MPEG_VIDEO_H264_MIN_QP");
#endif
#if defined(V4L2_CID_MPEG_VIDEO_H264_MAX_QP)
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, "V4L2_CID_MPEG_VIDEO_H264_MAX_QP");
#endif
	} else if (enc->output_format == V4L2_PIX_FMT_MJPEG) {
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_MPEG_VIDEO_BITRATE, "V4L2_CID_MPEG_VIDEO_BITRATE");
	} else if (enc->output_format == V4L2_PIX_FMT_JPEG) {
		_m2m_encoder_diag_report_ctrl_support(enc, V4L2_CID_JPEG_COMPRESSION_QUALITY, "V4L2_CID_JPEG_COMPRESSION_QUALITY");
	}
}

static int _m2m_encoder_init_buffers(
	us_m2m_encoder_s *enc, const char *name, enum v4l2_buf_type type,
	us_m2m_buffer_s **bufs_ptr, unsigned *n_bufs_ptr, bool dma) {

	_E_LOG_DEBUG("Initializing %s buffers ...", name);

	struct v4l2_requestbuffers req = {0};
	req.count = 1;
	req.type = type;
	req.memory = (dma ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP);

	_E_LOG_DEBUG("Requesting %u %s buffers ...", req.count, name);
	_E_XIOCTL(VIDIOC_REQBUFS, &req, "Can't request %s buffers", name);
	if (req.count < 1) {
		_E_LOG_ERROR("Insufficient %s buffer memory: %u", name, req.count);
		goto error;
	}
	_E_LOG_DEBUG("Got %u %s buffers", req.count, name);

	if (dma) {
		*n_bufs_ptr = req.count;
	} else {
		*bufs_ptr = calloc(req.count, sizeof(us_m2m_buffer_s));
		for (*n_bufs_ptr = 0; *n_bufs_ptr < req.count; ++(*n_bufs_ptr)) {
			struct v4l2_buffer buf = {0};
			struct v4l2_plane plane = {0};
			buf.type = type;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = *n_bufs_ptr;
			buf.length = 1;
			buf.m.planes = &plane;

			_E_LOG_DEBUG("Querying %s buffer=%u ...", name, *n_bufs_ptr);
			_E_XIOCTL(VIDIOC_QUERYBUF, &buf, "Can't query %s buffer=%u", name, *n_bufs_ptr);

			_E_LOG_DEBUG("Mapping %s buffer=%u ...", name, *n_bufs_ptr);
			if (((*bufs_ptr)[*n_bufs_ptr].data = mmap(
				NULL,
				plane.length,
				PROT_READ | PROT_WRITE,
				MAP_SHARED,
				_RUN(fd),
				plane.m.mem_offset
			)) == MAP_FAILED) {
				_E_LOG_PERROR("Can't map %s buffer=%u", name, *n_bufs_ptr);
				goto error;
			}
			assert((*bufs_ptr)[*n_bufs_ptr].data != NULL);
			(*bufs_ptr)[*n_bufs_ptr].allocated = plane.length;

			_E_LOG_DEBUG("Queuing %s buffer=%u ...", name, *n_bufs_ptr);
			_E_XIOCTL(VIDIOC_QBUF, &buf, "Can't queue %s buffer=%u", name, *n_bufs_ptr);
		}
	}

	return 0;
	error:
		return -1;
}

static void _m2m_encoder_cleanup(us_m2m_encoder_s *enc) {
	if (_RUN(ready)) {
#		define STOP_STREAM(x_name, x_type) { \
				enum v4l2_buf_type m_type_var = x_type; \
				_E_LOG_DEBUG("Stopping %s ...", x_name); \
				if (us_xioctl(_RUN(fd), VIDIOC_STREAMOFF, &m_type_var) < 0) { \
					_E_LOG_PERROR("Can't stop %s", x_name); \
				} \
			}

		STOP_STREAM("OUTPUT", V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		STOP_STREAM("INPUT", V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

#		undef STOP_STREAM
	}

#	define DESTROY_BUFFERS(x_name, x_target) { \
		if (_RUN(x_target##_bufs) != NULL) { \
			for (unsigned m_index = 0; m_index < _RUN(n_##x_target##_bufs); ++m_index) { \
				if (_RUN(x_target##_bufs[m_index].allocated) > 0 && _RUN(x_target##_bufs[m_index].data) != NULL) { \
					if (munmap(_RUN(x_target##_bufs[m_index].data), _RUN(x_target##_bufs[m_index].allocated)) < 0) { \
						_E_LOG_PERROR("Can't unmap %s buffer=%u", #x_name, m_index); \
					} \
				} \
			} \
			free(_RUN(x_target##_bufs)); \
			_RUN(x_target##_bufs) = NULL; \
		} \
		_RUN(n_##x_target##_bufs) = 0; \
	}

	DESTROY_BUFFERS("OUTPUT", output);
	DESTROY_BUFFERS("INPUT", input);

#	undef DESTROY_BUFFERS

	if (_RUN(fd) >= 0) {
		if (close(_RUN(fd)) < 0) {
			_E_LOG_PERROR("Can't close encoder device");
		}
		_RUN(fd) = -1;
	}

	_RUN(last_online) = -1;
	_RUN(force_key_pending) = 0;
	_RUN(ready) = false;

	_E_LOG_DEBUG("Encoder state: ~~~ NOT READY ~~~");
}

static int _m2m_encoder_compress_raw(us_m2m_encoder_s *enc, const us_frame_s *src, us_frame_s *dest, bool force_key) {
	assert(_RUN(ready));

	_E_LOG_DEBUG("Compressing new frame; force_key=%d ...", force_key);

	if (force_key) {
		struct v4l2_control ctl = {0};
		ctl.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
		ctl.value = 1;
		_E_LOG_DEBUG("Forcing keyframe ...");
		/* be forgiving with transient EBUSY */
		if (ioctl(_RUN(fd), VIDIOC_S_CTRL, &ctl) < 0 && errno != EBUSY) {
			_E_LOG_PERROR("Can't force keyframe");
			goto error;
		}
	}

	struct v4l2_buffer input_buf = {0};
	struct v4l2_plane input_plane = {0};
	input_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	input_buf.length = 1;
	input_buf.m.planes = &input_plane;

	if (_RUN(dma)) {
		input_buf.index = 0;
		input_buf.memory = V4L2_MEMORY_DMABUF;
		input_buf.field = V4L2_FIELD_NONE;
		input_plane.m.fd = src->dma_fd;
		_E_LOG_DEBUG("Using INPUT-DMA buffer=%u", input_buf.index);
	} else {
		input_buf.memory = V4L2_MEMORY_MMAP;
		_E_LOG_DEBUG("Grabbing INPUT buffer ...");
		_E_XIOCTL(VIDIOC_DQBUF, &input_buf, "Can't grab INPUT buffer");
		if (input_buf.index >= _RUN(n_input_bufs)) {
			_E_LOG_ERROR("V4L2 error: grabbed invalid INPUT: buffer=%u, n_bufs=%u",
				input_buf.index, _RUN(n_input_bufs));
			goto error;
		}
		_E_LOG_DEBUG("Grabbed INPUT buffer=%u", input_buf.index);
	}

	const uint64_t now = us_get_now_monotonic_u64();
	struct timeval ts = {
		.tv_sec = now / 1000000,
		.tv_usec = now % 1000000,
	};

	input_buf.timestamp.tv_sec = ts.tv_sec;
	input_buf.timestamp.tv_usec = ts.tv_usec;
	input_plane.bytesused = src->used;
	input_plane.length = src->used;
	if (!_RUN(dma)) {
		memcpy(_RUN(input_bufs[input_buf.index].data), src->data, src->used);
	}

	const char *input_name = (_RUN(dma) ? "INPUT-DMA" : "INPUT");

	_E_LOG_DEBUG("Sending%s %s buffer ...", (!_RUN(dma) ? " (releasing)" : ""), input_name);
	_E_XIOCTL(VIDIOC_QBUF, &input_buf, "Can't send %s buffer", input_name);

	// Для не-DMA отправка буфера по факту являтся освобождением этого буфера
	bool input_released = !_RUN(dma);

	while (true) {
		struct pollfd enc_poll = {_RUN(fd), POLLIN, 0};

		_E_LOG_DEBUG("Polling encoder ...");
		if (poll(&enc_poll, 1, 1000) < 0 && errno != EINTR) {
			_E_LOG_PERROR("Can't poll encoder");
			goto error;
		}

		if (enc_poll.revents & POLLIN) {
			if (!input_released) {
				_E_LOG_DEBUG("Releasing %s buffer=%u ...", input_name, input_buf.index);
				_E_XIOCTL(VIDIOC_DQBUF, &input_buf, "Can't release %s buffer=%u",
					input_name, input_buf.index);
				input_released = true;
			}

			struct v4l2_buffer output_buf = {0};
			struct v4l2_plane output_plane = {0};
			output_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			output_buf.memory = V4L2_MEMORY_MMAP;
			output_buf.length = 1;
			output_buf.m.planes = &output_plane;
			_E_LOG_DEBUG("Fetching OUTPUT buffer ...");
			_E_XIOCTL(VIDIOC_DQBUF, &output_buf, "Can't fetch OUTPUT buffer");

			bool done = false;
			if (ts.tv_sec != output_buf.timestamp.tv_sec || ts.tv_usec != output_buf.timestamp.tv_usec) {
				// Енкодер первый раз может выдать буфер с мусором и нулевым таймстампом,
				// так что нужно убедиться, что мы читаем выходной буфер, соответствующий
				// входному (с тем же таймстампом).
				_E_LOG_DEBUG("Need to retry OUTPUT buffer due timestamp mismatch");
			} else {
				us_frame_set_data(dest, _RUN(output_bufs[output_buf.index].data), output_plane.bytesused);
				dest->key = output_buf.flags & V4L2_BUF_FLAG_KEYFRAME;
				dest->gop = enc->gop;
				done = true;
			}

			_E_LOG_DEBUG("Releasing OUTPUT buffer=%u ...", output_buf.index);
			_E_XIOCTL(VIDIOC_QBUF, &output_buf, "Can't release OUTPUT buffer=%u", output_buf.index);

			if (done) {
				break;
			}
		}
	}

	return 0;
	error:
		return -1;
}

#undef _E_XIOCTL

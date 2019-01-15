#pragma  once
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <string>
#include <memory>
#include <functional>
using namespace std;

#include <windows.h>



extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include "ThreadSafe_Queue.h"
//#include "Shader.h"


#define  QUE_PACKAGEINFO_SIZE 200

enum YMediaCallBackType
{
	MEDIA_ERROR,
};

enum DecodeError
{
	ERROR_NO_ERROR = 0,
	ERROR_NO_QUIT,
	ERROR_FILE_ERROR,
	ERROR_QUE_BLOCK,
};

struct MediaInfo
{
	double dur;
	
};

struct VideoPackageInfo
{
	void *data=nullptr;
	int width;
	int height;
	double pts;
	double clock;
	DecodeError error  = ERROR_NO_ERROR;
};


struct AudioPackageInfo
{
	void *data=nullptr;
	int size = 0;
	double pts;
	double dur;
	int sample_rate;
	int channels;
	DecodeError error = ERROR_NO_ERROR;
};


//flag:0 no error
//flag:1 conduct block queue

enum FLAG_PKG {
	FLAG_DEFAULT=0,
	FLAG_CONDUCT_QUE,
	FLAG_SEEK,
};
struct InnerPacketInfo {
	AVPacket *pkg;
	FLAG_PKG flag= FLAG_DEFAULT;
};


class FormatCtx;
class CodecCtx;
class AVFrameManger;
class AudioConvertManger;
class VideoConvertManger;
class YMediaDecode
{
public:
	YMediaDecode();
	~YMediaDecode();

	bool SetMedia(const std::string & path_file);

	bool Pause();

	bool StopDecode();

	bool StartDecode();

	void EmptyAudioQue();

	void EmptyVideoQue();

	void SeekPos(double pos);

	AudioPackageInfo PopAudioQue();//audio call back by multi-thread

	VideoPackageInfo PopVideoQue(double cur_clock); //video call back by multi-thread

	void FreeAudioPackageInfo(AudioPackageInfo*);

	void ConductAudioBlocking();

	void ConductVideoBlocking();

	void SetErrorFunction(std::function<void(DecodeError)> error_func);

	void SetMediaFunction(std::function<void(MediaInfo)> func);

	void IsDecodeDone();
protected:

	void DecodeThread();

	void DoConvertAudio(AVPacket *pkg);

	void DoConvertVideo(AVPacket *pkg,double cur_clock);

	double synchronize(std::shared_ptr<CodecCtx>,AVFrame *srcFrame, double pts, double cur_clock);

private:

	void NotifyDecodeStatus(DecodeError);

	void NotifyMediaInfo(MediaInfo info);

	std::string path_file_;

	std::thread decode_thread_;

	atomic_bool is_manual_stop_;

	atomic_bool is_seek_;
	double seek_time_;

	ThreadSafe_Queue<AudioPackageInfo> audio_que_;
	ThreadSafe_Queue<VideoPackageInfo> video_que_;

	ThreadSafe_Queue<InnerPacketInfo> audio_inner_que_;
	ThreadSafe_Queue<InnerPacketInfo> video_inner_que_;

	std::weak_ptr<FormatCtx>		format_ctx_;
	std::weak_ptr<CodecCtx>		audio_codec_;
	std::weak_ptr<CodecCtx>		video_codec_;

	std::weak_ptr<AudioConvertManger> audio_convert_;
	std::weak_ptr<VideoConvertManger> video_convert_;
	

	std::weak_ptr<AVFrameManger> audio_frame_;
	std::weak_ptr<AVFrameManger> video_frame_;

	

	std::function<void (DecodeError)> error_func_;

	std::function<void(MediaInfo)> media_func_;
};



class FormatCtx {
public:
	inline FormatCtx()
		: open_input_(false){
		ctx_= avformat_alloc_context();
		pkg_ = av_packet_alloc();
		av_init_packet(pkg_);
	}

	inline ~FormatCtx() {
		if (open_input_)
		{
			avformat_close_input(&ctx_);
		}
		av_packet_free(&pkg_);
		avformat_free_context(ctx_);
	}

	bool InitFormatCtx(const char* filename)
	{
		if (avformat_open_input(&ctx_, filename, 0, 0) != 0)
		{
			return false;
		}
		open_input_ = true;
		if (avformat_find_stream_info(ctx_, 0) < 0)
		{
			return false;
		}
		return true;
	}


	inline bool read() {
		return  av_read_frame(ctx_, pkg_) >= 0;
	}

	inline bool seek()
	{

	}

	void release_package()
	{
		av_packet_unref(pkg_);
	}

	bool open_input_;
	AVFormatContext* ctx_;
	AVPacket *pkg_;
};

class CodecCtx {
public:
	inline CodecCtx(AVFormatContext * format_ctx, AVMediaType type)//AVMEDIA_TYPE_AUDIO
		:format_(format_ctx)
		, codec_ctx_(nullptr)
		, type_(type)
		, stream_index_(-1)
	{

	}

	~CodecCtx()
	{
		if (codec_ctx_)
		{
			avcodec_close(codec_ctx_);
			avcodec_free_context(&codec_ctx_);
		}
	}

	bool InitDecoder()
	{
		if (!format_)
			return false;

		int ret, stream_index;
		AVStream *st;
		AVCodec *dec = NULL;
		AVDictionary *opts = NULL;

		ret = av_find_best_stream(format_, type_, -1, -1, NULL, 0);
		if (ret < 0) {
			/*	fprintf(stderr, "Could not find %s stream in input file '%s'\n",
			av_get_media_type_string(type), src_filename);*/
			return false;
		}
		else {
			stream_index = ret;
			st = format_->streams[stream_index];

			/* find decoder for the stream */
			dec = avcodec_find_decoder(st->codecpar->codec_id);
			if (!dec) {
				fprintf(stderr, "Failed to find %s codec\n",
					av_get_media_type_string(type_));
				return false;
			}

			/* Allocate a codec context for the decoder */
			codec_ctx_ = avcodec_alloc_context3(dec);
			if (!codec_ctx_) {
				fprintf(stderr, "Failed to allocate the %s codec context\n",
					av_get_media_type_string(type_));
				return false;
			}

			/* Copy codec parameters from input stream to output codec context */
			if ((ret = avcodec_parameters_to_context(codec_ctx_, st->codecpar)) < 0) {
				fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
					av_get_media_type_string(type_));
				return false;
			}

			/* Init the decoders, with or without reference counting */
			av_dict_set(&opts, "refcounted_frames", "0", 0);
			if ((ret = avcodec_open2(codec_ctx_, dec, &opts)) < 0) {
				fprintf(stderr, "Failed to open %s codec\n",
					av_get_media_type_string(type_));
				return false;
			}
			stream_index_ = stream_index;
			return true;
		}
	}

	AVStream *GetStream()
	{
		return format_->streams[stream_index_];
	}

	bool IsValid()
	{
		return stream_index_ >= 0;
	}

	AVFormatContext *format_;
	AVCodecContext *codec_ctx_;
	int stream_index_;
	AVMediaType type_;
};

class AVFrameManger
{
public:
	AVFrameManger()
	{
		frame_ = av_frame_alloc();
	}
	~AVFrameManger()
	{
		av_frame_free(&frame_);
	}
	AVFrame *frame_;
};

struct AudioConvertParameter
{
	int des_layout;
	AVSampleFormat des_fmt;
	int des_sample_rate;
	int src_channel;
	AVSampleFormat src_sample_fmt;
	int src_sample_rate;
};

class AudioConvertManger
{
public:
	AudioConvertManger(AudioConvertParameter parameter)
	{
		audio_convert_ctx_ = swr_alloc();
		audio_convert_ctx_ = swr_alloc_set_opts(audio_convert_ctx_,
			parameter.des_layout,
			parameter.des_fmt,
			parameter.des_sample_rate,
			parameter.src_channel,
			parameter.src_sample_fmt,
			parameter.src_sample_rate,
			0,
			NULL);
		swr_init(audio_convert_ctx_);
	}
	~AudioConvertManger()
	{
		swr_free(&audio_convert_ctx_);
	}

	SwrContext* audio_convert_ctx_;
};

class VideoConvertManger
{
public:
	VideoConvertManger(int width,int height,AVPixelFormat fmt)
	{
		int pic_size_ = avpicture_get_size(AV_PIX_FMT_RGB24, width, height);
		pic_buff = (uint8_t*)av_malloc(pic_size_);
		avpicture_fill((AVPicture *)(rgb_frame_.frame_), pic_buff, AV_PIX_FMT_RGB24, width, height);
		video_convert_ctx_ = sws_getContext(width, height, fmt, width, height, AV_PIX_FMT_BGR24, SWS_BICUBIC, NULL, NULL, NULL);

	}

	~VideoConvertManger()
	{
		av_free(pic_buff);
		sws_freeContext(video_convert_ctx_);
	}

	void Convert(const uint8_t *const* src_data,int *src_stride,int height)
	{
		sws_scale(video_convert_ctx_, src_data, src_stride, 0,height, rgb_frame_.frame_->data, rgb_frame_.frame_->linesize);
	}

	SwsContext* video_convert_ctx_;
	uint8_t * pic_buff;
	AVFrameManger rgb_frame_;
};

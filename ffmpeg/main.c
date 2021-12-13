#include "config.h"
#include "inttypes.h""
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

#include "SDL/SDL.h"
#include "SDL/SDL_thread.h"

#include "cmdutils.h"

const char program_name[] = "FFplay_dss";
const int program_birth_year = 2021;

#define MAX_QUEUE_SIZE (15*2014*1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
#define SDL_VOLUME_STEP (0.75)

#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_SYNC_FRAMEUP_THRESHOLD 0.1
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10

#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

#define AUDIO_DIFF_AVG_NB 20
#define REPRESH_RATE 0.01

#define SAMPLE_ARRAY_SIZE (8*65536)
#define CURSOR_HIDE_DELAY 1000000
#define USE_ONEPASS_SUBTITLE_RENDER 1
static unsigned sws_flags = SWS_BICUBIC;
typedef struct MyAVPacketList
{
	AVPacket* pkt;
	int serial;
}MyAVPacketList;

typedef struct PacketQueue
{
	AVFifoBuffer* pkt_list;
	int nb_packets;
	int size;
	int64_t duration;
	int abort_request;
	int serial;
	SDL_mutex* mutex;
	SDL_cond* cond;
}PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams
{
	int freq;
	int channels;
	int64_t channel_layout;
	enum AVSampleFormat fmt;
	int frame_size;
	int bytes_per_sec;
}AudioParams;

typedef struct Clock {
	double pts;
	double pts_drift;
	double last_updated;
	double speed;
	int serial;
	int paused;
	int* queue_serial;
}Clock;

typedef struct Frame
{
	AVFrame* frame;
	AVSubtitle sub;
	int serial;
	double pts;
	double duration;
	int64_t pos;
	int width;
	int height;
	int format;
	AVRational sar;
	int uploaded;
	int flip_v;
}Frame;

typedef struct FrameQueue
{
	Frame queue[FRAME_QUEUE_SIZE];
	int rindex;
	int windex;
	int size;
	int max_size;
	int keep_last;
	int rindex_shown;
	SDL_mutex mutex;
	SDL_cond* cond;
	PacketQueue* pktq;
}FrameQueue;

enum
{
	AV_SYNC_AUDIO_MASTYER,
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_CLOCK
};;

typedef struct Decoder
{
	AVPacket* pkt;
	PacketQueue* queue;
	AVCodecContext* avctx;
	int pkt_serial;
	int finished;
	int packet_pending;
	SDL_cond* empty_queue_cond;
	int64_t start_pts;
	AVRational start_pts_tb;
	int64_t next_pts;
	AVRational next_pts_tb;
	SDL_Thread* decoder_tid;
}Decoder;

typedef struct VideoState
{
	SDL_Thread* read_tid;
	const AVInputFormat* iformat;
	int abort_request;
	int forc_refresh;
	int paused;
	int last_paused;
	int queue_attachments_req;
	int seek_req;
	int seek_flags;
	int64_t seek_pos;
	int64_t seek_rel;
	int read_pause_return;
	AVFormatContext* ic;
	int realtime;
	Clock audclk;
	Clock vidclk;
	Clock extclk;
	FrameQueue pictq;
	FrameQueue subpq;
	FrameQueue sampq;
	Decoder auddec;
	Decoder viddec;
	Decoder subdec;
	int audio_stream;
	int av_sync_type;
	double audio_diff_cum;
	double audio_diff_avg_coef;
	double audio_diff_threshold;
	int audio_diff_avg_count;
	AVStream* audio_st;
	PacketQueue audioq;
	int audio_hw_buf_size;
	uint8_t* audio_buf;
	uint8_t* audio_buf1;
	unsigned int audio_buf_size;
	unsigned int audio_buf1_size;
	int audio_buf_index;
	int audio_write_buf_size;
	int audio_volume;
	int muted;
	struct AudioParams audio_src;
	struct AudioParams audio_tgt;
	struct SwrContext swr_ctx;
	int frame_drops_early;
	int frame_drops_late;
	enum ShowMode {
		SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
	} show_mode;

	int16_t sample_array[SAMPLE_ARRAY_SIZE];
	int sample_array_index;
	int last_i_start;
	RDFTContext* rdft;
	int rdft_bits;
	FFTSample* rdft_data;
	int xpos;
	double last_vis_time;
	SDL_Texture* vis_texture;
	SDL_Texture* sub_texture;
	SDL_Texture* vid_texture;
	int subtitle_stream;
	AVStream* subtitle_st;
	PacketQueue subtitleq;
	double frame_timer;
	double frame_last_returned_time;
	double frame_last_filter_delay;
	int video_stream;
	AVStream* video_st;
	PacketQueue videoq;
	double max_frame_duration;
	struct SwsContext* img_convert_ctx;
	struct SwsContext* sub_convert_ctx;
	int eof;
	char* filename;
	int width, height, xleft, ytop;
	int step;
	int last_video_stream, last_audio_stream, last_subtitle_stream;
	SDL_cond* continue_read_thread;
}VideoState;

/* options specified by the user*/
static const AVInputFormat file_iformat;
static const char* input_filename;
static const char* window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };
static int seek_by_bytes = -1;
static float seek_interval = 10;
static int display_disable;
static int borderless;
static int alwayssontop;
static int startup_volume = 100;
static int show_status = -1;
static int av_sync_type = AV_SYNC_AUDIO_MASTYER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static enum ShowMode show_mode = SHOW_MODE_NONE;
static const char* audio_codec_name;
static const char* subtitle_codec_name;
static const char* video_codec_name;
double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;

static int is_full_screen;
static int64_t audio_callback_time;
#define FF_QUIT_EVENT (SDL_USEREVENT+2)
static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_RendererInfo renderer_info = { 0 };
static SDL_AudioDeviceID audio_dev;
static const struct TextureFormatEntry
{
	enum AVPixelFormat format;
	int texture_fmt;
}sdl_texture_format_map[] = {
	{AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
	{AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444},
	{AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555},
	{AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555},
	{AV_PIX_FMT_RGB565,SDL_PIXELFORMAT_BGR565},
	{AV_PIX_FMT_RGB24,SDL_PIXELFORMAT_RGB24},
	{AV_PIX_FMT_BGR24,SDL_PIXELFORMAT_BGR24},
	{AV_PIX_FMT_0RGB32,SDL_PIXELFORMAT_RGB888},
	{AV_PIX_FMT_0BGR32,SDL_PIXELFORMAT_BGR888},
	{AV_PIX_FMT_NE(RGB0,0BGR),SDL_PIXELFORMAT_RGBX8888},
	{AV_PIX_FMT_NE(BGR0,0RGB),SDL_PIXELFORMAT_BGRX8888},
	{AV_PIX_FMT_RGB32,SDL_PIXELFORMAT_ARGB8888},
	{AV_PIX_FMT_RGB32_1,SDL_PIXELFORMAT_RGBA8888},
	{AV_PIX_FMT_BGR32,SDL_PIXELFORMAT_ABGR8888},
	{AV_PIX_FMT_BGR32_1,SDL_PIXELFORMAT_BGRA8888},
	{AV_PIX_FMT_YUV420P,SDL_PIXELFORMAT_IYUV},
	{AV_PIX_FMT_YUYV422,SDL_PIXELFORMAT_YUY2},
	{AV_PIX_FMT_UYVY422,SDL_PIXELFORMAT_UYVY},
	{AV_PIX_FMT_NONE,SDL_PIXELFORMAT_UNKNOWN}
};

static inline int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1, enum AVSampleFormat fmt2, int64_t channel_count2)
{
	if (channel_count1 == 1 && channel_count2 == 1)
		return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
	else
		return channel_count1 != channel_count2 || fmt1 != fmt2;
}


static inline int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
{
	if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
		return channel_layout;
	else
		return 0;
}

static int packet_queue_put_private(PacketQueue* q, AVPacket* pkt)
{
	MyAVPacketList pkt1;
	if (q->abort_request)
		return -1;
	if (av_fifo_space(q->pkt_list) < sizeof(pkt1))
		if (av_fifo_grow(q->pkt_list, sizeof(pkt1)) < 0)
			return -1;

	pkt1.pkt = pkt;
	pkt1.serial = q->serial;

	av_fifo_generic_write(q->pkt_list, &pkt1, sizeof(pkt1), NULL);
	q->nb_packets++;
	q->size += pkt1.pkt->size + sizeof(pkt1);
	q->duration += pkt1.pkt->duration;
	SDL_CondSignal(q->cond);
	return 0;
}

static int packet_queue_put(PacketQueue* q, AVPacket* pkt)
{
	AVPacket* pkt1;
	int ret;
	pkt1 = av_packet_alloc();
	if (!pkt1)
	{
		av_packet_unref(pkt);
		return -1;
	}
	av_packet_move_ref(pkt1, pkt);

	SDL_LockMutex(q->mutex);
	ret = packet_queue_put_private(q, pkt1);
	SDL_UnlockMutex(q->mutex);
	if (ret < 0)
		av_packet_free(&pkt1);

	return ret;
}

static int packet_queue_put_nullpacket(PacketQueue* q, AVPacket* pkt, int stream_index)
{
	pkt->stream_index = stream_index;
	return packet_queue_put(q, pkt);
}

static int packet_queue_init(PacketQueue* q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->pkt_list = av_fifo_alloc(sizeof(MyAVPacketList));
	if (!q->pkt_list)
		return AVERROR(ENOMEM);
	q->mutex = SDL_CreateMutex();
	if (!q->mutex)
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %d\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->cond = SDL_CreateCond();
	if (!q->cond)
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond():%s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	q->abort_request = 1;
	return 0;
}

static void packet_queue_flush(PacketQueue* q)
{
	MyAVPacketList pkt1;
	SDL_LockMutex(q->mutex);
	while (av_fifo_size(q->pkt_list) >= sizeof(pkt1))
	{
		av_fifo_generic_read(q->pkt_list, &pkt1, sizeof(pkt1), NULL);
		av_packet_free(&pkt1.pkt);
	}
	q->nb_packets = 0;
	q->size = 0;
	q->duration = 0;
	q->serial++;
	SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue* q)
{
	packet_queue_flush(q);
	av_fifo_freep(&q->pkt_list);
	SDL_DestroyMutex(q->mutex);
	SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue* q)
{
	SDL_LockMutex(q->mutex);
	q->abort_request = 1;
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue* q)
{
	SDL_LockMutex(q->mutex);
	q->abort_request = 0;
	q->serial++;
	SDL_UnlockMutex(q->mutex);
}

static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block, int* serial)
{
	MyAVPacketList pkt1;
	int ret;
	SDL_LockMutex(q->mutex);
	for (;;)
	{
		if (q->abort_request)
		{
			ret = -1;
			break;
		}

		if (av_fifo_size(q->pkt_list) >= sizeof(pkt1))
		{
			av_fifo_generic_read(q->pkt_list, &pkt1, sizeof(pkt1), NULL);
			q->nb_packets--;
			q->size -= pkt1.pkt->size + sizeof(pkt1);
			q->duration -= pkt1.pkt->duration;
			av_packet_move_ref(pkt, pkt1.pkt);
			if (serial)
				*serial = pkt1.serial;
			av_packet_free(&pkt1.pkt);
			ret = 1;
			break;
		}
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
		{
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

static int decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, SDL_cond* empty_queue_cond)
{
	memset(d, 0, sizeof(Decoder));
	d->pkt = av_packet_alloc();
	if (!d->pkt)
		return AVERROR(ENOMEM);
	d->avctx = avctx;
	d->queue = queue;
	d->empty_queue_cond = empty_queue_cond;
	d->start_pts = AV_NOPTS_VALUE;
	d->pkt_serial = -1;
	return 0;
}

static int decoder_decode_frame(Decoder* d, AVFrame* frame, AVSubtitle* sub)
{
	int ret = AVERROR(EAGAIN);
	for (;;)
	{
		if (d->queue->serial == d->pkt_serial)
		{
			do {
				if (d->queue->abort_request)
					return -1;
				switch (d->avctx->codec_type)
				{
				case AVMEDIA_TYPE_VIDEO:
					ret = avcodec_receive_frame(d->avctx, frame);
					if (ret >= 0)
					{
						if (decoder_reorder_pts == -1)
							frame->pts = frame->best_effort_timestamp;
						else if (!decoder_reorder_pts)
							frame->pts = frame->pkt_dts;
					}
					break;

				case AVMEDIA_TYPE_AUDIO:
					ret = avcodec_receive_frame(d->avctx, frame);
					if (ret >= 0)
					{
						AVRational tb = (AVRational){ 1, frame->sample_rate };
						if (frame->pts != AV_NOPTS_VALUE)
							frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
						else if (d->next_pts != AV_NOPTS_VALUE)
							frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
						if (frame->pts != AV_NOPTS_VALUE)
						{
							d->next_pts = frame->pts + frame->nb_samples;
							d->next_pts_tb = tb;
						}
					}
					break;
				}
				if (ret == AVERROR_EOF)
				{
					d->finished = d->pkt_serial;
					avcodec_flush_buffers(d->avctx);
					return 0;
				}
				if (ret >= 0)
					return 1;
			} while (ret != AVERROR(EAGAIN));
		}

		do
		{
			if (d->queue->nb_packets == 0)
				SDL_CondSignal(d->empty_queue_cond);
			if (d->packet_pending)
				d->packet_pending = 0;
			else
			{
				int old_serial = d->pkt_serial;
				if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
					return -1;
				if (old_serial != d->pkt_serial)
				{
					avcodec_flush_buffers(d->avctx);
					d->finished = 0;
					d->next_pts = d->start_pts;
					d->next_pts_tb = d->start_pts_tb;
				}
			}
			if (d->queue->serial == d->pkt_serial)
				break;
			av_packet_unref(d->pkt);
		} while (1);

		if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE)
		{
			int got_frame = 0;
			ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
			if (ret < 0)
				ret = AVERROR(EAGAIN);
			else
			{
				if (got_frame && !d->pkt->data)
					d->packet_pending = 1;
				ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
			}
			av_packet_unref(d->pkt);
		}
		else
		{
			if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN))
			{
				av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, hwich is an API violation.\n");
				d->packet_pending = 1;
			}
			else
				av_packet_unref(d->pkt);
		}
	}
}

static void decoder_destroy(Decoder* d)
{
	av_packet_free(&d->pkt);
	avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame* vp)
{
	av_frame_unref(vp->frame);
	avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last)
{
	int i;
	memset(f, 0, sizeof(FrameQueue));
	if (!(f->mutex = SDL_CreateMutex()))
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	if (!(f->cond = SDL_CreateCond()))
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond: %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	f->pktq = pktq;
	f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
	f->keep_last = !!keep_last;
	for (i = 0; i < f->max_size; i++)
		if (!(f->queue[i].frame = av_frame_alloc()))
			return AVERROR(ENOMEM);
	return 0;
}

static void frame_queue_destroy(FrameQueue* f)
{
	int i;
	for (i = 0; i < f->max_size; i++)
	{
		Frame* vp = &f->queue[i];
		frame_queue_unref_item(vp);
		av_frame_free(&vp->frame);
	}
	SDL_DestroyMutex(f->mutex);
	SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue* f)
{
	SDL_LockMutex(f->mutex);
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

static Frame* frame_queue_peek(FrameQueue* f)
{
	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame* frame_queue_peek_next(FrameQueue* f)
{
	return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame* frame_queue_pkke_last(FrameQueue* f)
{
	return &f->queue[f->rindex];
}

static Frame* frame_queue_peek_writable(FrameQueue* f)
{
	SDL_LockMutex(f->mutex);
	while (f->size >= f->max_size && !f->pktq->abort_request)
		SDL_CondWait(f->cond, f->mutex);

	SDL_UnlockMutex(f->mutex);
	if (f->pktq->abort_request)
		return NULL;
	return &f->queue[f->windex];
}

static Frame* frame_queue_peek_readable(FrameQueue* f)
{
	SDL_LockMutex(f->mutex);
	while (f->size - f->rindex_shown <= 0 &&
		!f->pktq->abort_request)
		SDL_CondWait(f->cond, f->mutex);
	SDL_UnlockMutex(f->mutex);
	if (f->pktq->abort_request)
		return NULL;
	return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue* f)
{
	if (++f->windex == f->max_size)
		f->windex = 0;
	SDL_LockMutex(f->mutex);
	f->size++;
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue* f)
{
	if (f->keep_last && !f->rindex_shown)
	{
		f->rindex_shown = 1;
		return;
	}
	frame_queue_unref_item(&f->queue[f->rindex]);
	if (++f->rindex == f->max_size)
		f->rindex = 0;
	SDL_LockMutex(f->mutex);
	f->size--;
	SDL_CondSignal(f->cond);
	SDL_UnlockMutex(f->mutex);
}

static int frame_queue_nb_remaining(FrameQueue* f)
{
	return f->size - f->rindex_shown;
}

static int64_t frame_queue_last_pos(FrameQueue* f)
{
	Frame* fp = &f->queue[f->rindex];
	if (f->rindex_shown && fp->serial == f->pktq->serial)
		return fp->pos;
	else
		return -1;
}

static void decoder_abort(Decoder* d, FrameQueue* fq)
{
	packet_queue_abort(d->queue);
	frame_queue_signal(fq);
	SDL_WaitThread(d->decoder_tid, NULL);
	d->decoder_tid = NULL;
	packet_queue_flush(d->queue);
}

static inline void fill_rectangle(int x, int y, int w, int h)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	if (w && h)
		SDL_RenderFillRect(renderer, &rect);
}

static int realloc_texture(SDL_Texture** texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
	Uint32 format;
	int access, w, h;
	if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format)
	{
		void* pixels;
		int pitch;
		if (*texture)
			SDL_DestroyTexture(*texture);
		if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
			return -1;
		if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
			return -1;
		if (init_texture)
		{
			if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
				return -1;
			memset(pixels, 0, pitch * new_height);
			SDL_UnlockTexture(*texture);
		}
		av_log(NULL, AV_LOG_VERBOSE, "Create %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
	}
	return 0;
}

static void calculate_display_rect(SDL_Rect* rect, int scr_xleft, int scr_ytop, int scr_width, int scr_height,
	int pic_width, int pic_height, AVRational pic_sar)
{
	AVRational aspect_ratio = pic_sar;
	int64_t width, height, x, y;
	if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
		aspect_ratio = av_make_q(1, 1);
	aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));
	height = scr_height;
	width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & -1;
	if (width > scr_width)
	{
		width = scr_width;
		height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & -1;
	}
	x = (scr_width - width) / 2;
	y = (scr_height - height) / 2;
	rect->x = scr_xleft + x;
	rect->y = scr_ytop + y;
	rect->w = FFMAX((int)width, 1);
	rect->h = FFMIN((int)height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32* sdl_pix_fmt, SDL_BlendMode* sdl_blendmode)
{
	int i;
	*sdl_blendmode = SDL_BLENDMODE_NONE;
	*sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
	if (format == AV_PIX_FMT_RGB32 ||
		format == AV_PIX_FMT_RGB32_1 ||
		format == AV_PIX_FMT_BGR32 ||
		format == AV_PIX_FMT_BGR32_1)
		*sdl_blendmode = SDL_BLENDMODE_BLEND;
	for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++)
	{
		if (format == sdl_texture_format_map[i].format)
		{
			*sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
			return;
		}
	}
}

static int upload_texture(SDL_Texture** tex, AVFrame* frame, struct SwsContext** img_convert_ctx)
{
	int ret = 0;
	Uint32 sdl_pix_fmt;
	SDL_BlendMode sdl_blendmode;
	get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
	if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
		return -1;
	switch (sdl_pix_fmt)
	{
	case SDL_PIXELFORMAT_UNKNOWN:
		*img_convert_ctx = sws_getCachedContext(*img_convert_ctx, frame->width, frame->height, frame->format, frame->width, frame->height,
			AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
		if (*img_convert_ctx != NULL)
		{
			uint8_t* pixels[4];
			int pitch[4];
			if (!SDL_LockTexture(*tex, NULL, (void**)pixels, pitch))
			{
				sws_scale(*img_convert_ctx, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, pixels, pitch);
				SDL_UnlockTexture(*tex);
			}
		}
		else
		{
			av_log(NULL, AV_LOG_FATAL, "Can't initialize the conversion context\n");
			ret = -1;
		}
		break;
	case SDL_PIXELFORMAT_IYUV:
		if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0)
		{
			ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
				frame->data[1], frame->linesize[1],
				frame->data[2], frame->linesize[2]);
		}
		else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0)
		{
			ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
				frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
				frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
		}
		else
		{
			av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
			return -1;
		}
		break;
	default:
		if (frame->linesize[0] < 0)
			ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
		else
			ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
		break;
	}
	return ret;
}

static void set_sdl_yuv_conversion_mode(AVFrame* frame)
{
#if SDL_VERSION_ATLEAST(2,0,8)
	SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
	if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422))
	{
		if (frame->color_range == AVCOL_RANGE_JPEG)
			mode = SDL_YUV_CONVERSION_JPEG;
		else if (frame->colorspace == AVCOL_SPC_BT709)
			mode = SDL_YUV_CONVERSION_BT709;
		else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
			mode = SDL_YUV_CONVERSION_BT601;
	}
	SDL_SetYUVConversionMode(mode);
#endif
}

static void video_image_display(VideoState* is)
{
	Frame* vp;
	Frame* sp = NULL;
	SDL_Rect rect;

	vp = frame_queue_peek_last(&is->pictq);
	if (is->subtitle_st)
	{
		if (frame_queue_nb_remaining(&is->subpq) > 0)
		{
			sp = frame_queue_peek(&is->subpq);
			if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000))
			{
				if (!sp->uploaded)
				{
					uint8_t* pixels[4];
					int pitch[4];
					int i;
					if (!sp->width || !sp->height)
					{
						sp->width = vp->width;
						sp->height = vp->height;
					}
					if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
						return;
					for (i = 0; i < sp->sub.num_rects; i++)
					{
						AVSubtitleRect* sub_rect = sp->sub.rects[i];
						sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
						sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
						sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
						sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);
						is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
							sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
							sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
							0, NULL, NULL, NULL);
						if (!is->sub_convert_ctx)
						{
							av_log(NULL, AV_LOG_FATAL, "Can't initialize the conversion context.\n");
							return;
						}
						if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)pixels, pitch))
						{
							sws_scale(is->sub_convert_ctx, (const uint8_t* const*)sub_rect->data, sub_rect->linesize,
								0, sub_rect->h, pixels, pitch);
							SDL_UnlockTexture(is->sub_texture);
						}
					}
					sp->uploaded = 1;
				}
			}
			else
				sp = NULL;
		}
	}
	calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);
	if (!vp->uploaded)
	{
		if (upload_texture(&is->vid_texture, vp->frame, &is->img_convert_ctx) < 0)
			return;
		vp->uploaded = 1;
		vp->flip_v = vp->frame->linesize[0] < 0;
	}

	set_sdl_yuv_conversion_mode(vp->frame);
	SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);
	set_sdl_yuv_conversion_mode(NULL);
	if (sp)
	{
#if USE_ONEPASS_SUBTITLE_RENDER
		SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
#else
		int i;
		double xratio = (double)rect.w / (double)sp->width;
		double yratio = (double)rect.h / (double)sp->height;
		for (i = 0; i < sp->sub.num_rects; i++)
		{
			SDL_Rect* sub_rect = (SDL_Rect*)sp->sub.rects[i];
			SDL_Rect target = { .x = rect.x + sub_rect->x * xratio,
							   .y = rect.y + sub_rect->y * yratio,
							   .w = sub_rect->w * xratio,
							   .h = sub_rect->h * yratio };
			SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
		}
#endif
	}

}

static inline int compute_mod(int a, int b)
{
	return a < 0 ? a % b + b : a % b;
}

static void video_audio_display(VideoState* s)
{
	int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
	int ch, channels, h, h2;
	int64_t time_diff;
	int rdft_bits, nb_freq;
	for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
		;
	nb_freq = 1 << (rdft_bits - 1);

	channels = s->audio_tgt.channels;
	if (!s->paused)
	{
		int data_used = s->show_mode == SHOW_MODE_WAVES ? s->width : (2 * nb_freq);
		n = 2 * channels;
		delay = s->audio_write_buf_size;
		delay /= n;

		if (audio_callback_time)
		{
			time_diff = av_gettime_relative() - audio_callback_time;
			delay -= (time_diff * s->audio_tgt.freq) / 1000000;
		}
		delay += 2 * data_used;
		if (delay < data_used)
			delay = data_used;
		i_start = x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
		if (s->show_mode == SHOW_MODE_WAVES)
		{
			h = INT_MIN;
			for (i = 0; i < 1000; i += channels)
			{
				int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
				int a = s->sample_array[idx];
				int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
				int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
				int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
				int score = a - d;
				if (h < score && (b ^ c) < 0)
				{
					h = score;
					i_start = idx;
				}
			}
		}
		s->last_i_start = i_start;
	}
	else
		i_start = s->last_i_start;

	if (s->show_mode == SHOW_MODE_WAVES)
	{
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
		h = s->height / nb_display_channels;
		h2 = (h * 9) / 20;
		for (ch = 0; ch < nb_display_channels; ch++)
		{
			i = i_start + ch;
			y1 = s->ytop + ch * h + (h / 2);
			for (x = 0; x < s->width; x++)
			{
				y = (s->sample_array[i] * h2) >> 15;
				if (y < 0)
				{
					y = -y;
					ys = y1 - y;
				}
				else
					ys = y1;
				fill_rectangle(s->xleft + x, ys, 1, y);
				i += channels;
				if (i >= SAMPLE_ARRAY_SIZE)
					i -= SAMPLE_ARRAY_SIZE;
			}
		}
		SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
		for (ch = 1; ch < nb_display_channels; ch++)
		{
			y = s->ytop + ch * h;
			fill_rectangle(s->xleft, y, s->width, 1);
		}
	}
	else
	{
		if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
			return;
		if (s->xpos >= s->width)
			s->xpos = 0;
		nb_display_channels = FFMIN(nb_display_channels, 2);
		if (rdft_bits != s->rdft_bits)
		{
			av_rdft_end(s->rdft);
			av_free(s->rdft_data);
			s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
			s->rdft_bits = rdft_bits;
			s->rdft_data = av_malloc_array(nb_freq, 4 * sizeof(*s->rdft_data));
		}
		if (!s->rdft || !s->rdft_data)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching wo waves display\n");
			s->show_mode = SHOW_MODE_WAVES;
		}
		else
		{
			FFTSample* data[2];
			SDL_Rect rect = { .x = s->xpos, .y = 0, .w = 1, .h = s->height };
			uint32_t* pixels;
			int pitch;
			for (ch = 0; ch < nb_display_channels; ch++)
			{
				data[ch] = s->rdft_data + 2 * nb_freq * ch;
				i = i_start + ch;
				for (x = 0; x < 2 * nb_freq; x++)
				{
					double w = (x - nb_freq) * (1.0 / nb_freq);
					data[ch][x] = s->sample_array[i] * (1.0 - w * w);
					i += channels;
					if (i >= SAMPLE_ARRAY_SIZE)
						i -= SAMPLE_ARRAY_SIZE;
				}
				av_rdft_calc(s->rdft, data[ch]);
			}
			if (!SDL_LockTexture(s->vis_texture, &rect, (void**)&pixels, &pitch))
			{
				pitch >>= 2;
				pixels += pitch * s->height;
				for (y = 0; y < s->height; y++)
				{
					double w = 1 / sqrt(nb_freq);
					int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
					int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1])) : a;

					a = FFMIN(a, 255);
					b = FFMIN(b, 255);
					pixels -= pitch;
					*pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
				}
				SDL_UnlockTexture(s->vis_texture);
			}
			SDL_RenderCopy(renderer, s->vis_texture, NULL, NULL);
		}
		if (!s->paused)
			s->xpos++;
	}
}

static void stream_component_close(VideoState* is, int stream_index)
{
	AVFormatContext* ic = is->ic;
	AVCodecParameters* codecpar;
	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return;
	codecpar = ic->streams[stream_index]->codecpar;

	switch (codecpar->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		decoder_abort(&is->auddec, &is->sampq);
		SDL_CloseAudioDevice(audio_dev);
		decoder_destroy(&is->auddec);
		swr_free(&is->swr_ctx);
		av_freep(&is->audio_buf);
		is->audio_buf1_size = 0;
		is->audio_buf = NULL;
		if (is->rdft)
		{
			av_rdft_end(is->rdft);
			av_freep(&is->rdft_data);
			is->rdft = NULL;
			is->rdft_bits = 0;
		}
		break;
	case AVMEDIA_TYPE_VIDEO:
		decoder_abort(&is->viddec, &is->pictq);
		decoder_destroy(&is->viddec);
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		decoder_abort(&is->subdec, &is->subpq);
		decoder_destroy(&is->subdec);
		break;
	default:
		break;
	}

	ic->streams[stream_index]->discard = AVDISCARD_ALL;
	switch (codecpar->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		is->audio_st = NULL;
		is->audio_stream = -1;
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->video_st = NULL;
		is->video_stream = -1;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->subtitle_st = NULL;
		is->subtitle_stream = -1;
		break;
	default:
		break;
	}
}

static void stream_close(VideoState* is)
{
	is->abort_request = 1;
	SDL_WaitThread(is->read_tid, NULL);
	if (is->audio_stream >= 0)
		stream_component_close(is, is->audio_stream);
	if (is->video_stream >= 0)
		stream_component_close(is, is->video_stream);
	if (is->subtitle_stream >= 0)
		stream_component_close(is, is->subtitle_stream);

	avformat_close_input(&is->ic);

	packet_queue_destroy(&is->videoq);
	packet_queue_destroy(&is->audioq);
	packet_queue_destroy(&is->subtitleq);

	frame_queue_destroy(&is->pictq);
	frame_queue_destroy(&is->sampq);
	frame_queue_destroy(&is->subpq);
	SDL_DestroyCond(is->continue_read_thread);
	sws_freeContext(is->img_convert_ctx);
	sws_freeContext(is->sub_convert_ctx);
	av_free(is->filename);
	if (is->vis_texture)
		SDL_DestroyTexture(is->vis_texture);
	if (is->vid_texture)
		SDL_DestroyTexture(is->vid_texture);
	if (is->sub_texture)
		SDL_DestroyTexture(is->vid_texture);
	av_free(is);
}

static void do_exit(VideoState* is)
{
	if (is)
		stream_close(is);
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (window)
		SDL_DestroyWindow(window);
	uninit_opts();
	avformat_network_deinit();
	if (show_status)
		print("\n");
	SDL_Quit();
	av_log(NULL, AV_LOG_QUIET, "%s", "");
	exit(0);
}

static void sigterm_handler(int sig)
{
	exit(123);
}

static void set_default_window_size(int width, int height, AVRational sar)
{
	SDL_Rect rect;
	int max_width = screen_width ? screen_width : INT_MAX;
	int max_height = screen_height ? screen_height : INT_MAX;
	if (max_width == INT_MAX && max_height == INT_MAX)
		max_height = height;
	calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
	default_width = rect.w;
	default_height = rect.h;
}

static int video_open(VideoState* is)
{
	int w, h;
	w = screen_width ? screen_width : default_width;
	h = screen_height ? screen_height : default_height;
	if (!window_title)
		window_title = input_filename;
	SDL_SetWindowTitle(window, window_title);
	SDL_SetWindowSize(window, w, h);
	SDL_SetWindowPosition(window, screen_left, screen_top);
	if (is_full_screen)
		SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
	SDL_ShowWindow(window);
	is->width = w;
	is->height = h;
	return 0;
}

static void video_display(VideoState* is)
{
	if (!is->width)
		video_open(is);

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
		video_audio_display(is);
	else if (is->video_st)
		video_image_display(is);
	SDL_RenderPresent(renderer);
}


static double get_clock(Clock* c)
{
	if (*c->queue_serial != c->serial)
		return NAN;
	if (c->paused)
		return c->pts;
	else
	{
		double time = av_gettime_relative() / 1000000.0;
		return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
	}
}

static void set_clock_at(Clock* c, double pts, int serial, double time)
{
	c->pts = pts;
	c->last_updated = time;
	c->pts_drift = c->pts - time;
	c->serial = serial;
}

static void set_clock(Clock* c, double pts, int serial)
{
	double time = av_gettime_relative() / 1000000.0;
	set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock* c, double speed)
{
	set_clock(c, get_clock(c), c->serial);
	c->speed = speed;
}

static void init_clock(Clock* c, int* queue_serial)
{
	c->speed = 1.0;
	c->paused = 0;
	c->queue_serial = queue_serial;
	set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock* c, Clock* slave)
{
	double clock = get_clock(c);
	double slave_clock = get_clock(slave);
	if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
		set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState* is)
{
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
	{
		if (is->video_st)
			return AV_SYNC_VIDEO_MASTER;
		else
			return AV_SYNC_AUDIO_MASTYER;
	}
	else if (is->av_sync_type == AV_SYNC_AUDIO_MASTYER)
	{
		if (is->audio_st)
			return AV_SYNC_AUDIO_MASTYER;
		else
			return AV_SYNC_EXTERNAL_CLOCK;
	}
	else
		return AV_SYNC_EXTERNAL_CLOCK;
}

static double get_master_clock(VideoState* is)
{
	double val;
	switch (get_master_sync_type(is))
	{
	case AV_SYNC_VIDEO_MASTER:
		val = get_clock(&is->vidclk);
		break;
	case AV_SYNC_AUDIO_MASTYER:
		val = get_clock(&is->audclk);
		break;
	default:
		val = get_clock(&is->extclk);
		break;
	}
	return val;
}

static void check_external_clock_speed(VideoState* is)
{
	if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
		is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)
	{
		set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
	}
	else if ((is->video_stream<0 || is->videoq.nb_packets>EXTERNAL_CLOCK_MAX_FRAMES) &&
		(is->audio_stream<0 || is->audioq.nb_packets>EXTERNAL_CLOCK_MAX_FRAMES))
	{
		set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
	}
	else
	{
		double speed = is->extclk.speed;
		if (speed != 1.0)
			set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
	}
}

static void stream_seek(VideoState* is, int64_t pos, int64_t rel, int seek_by_bytes)
{
	if (!is->seek_req)
	{
		is->seek_pos = pos;
		is->seek_rel = rel;
		is->seek_flags &= -AVSEEK_FLAG_BYTE;
		if (seek_by_bytes)
			is->seek_flags |= AVSEEK_FLAG_BYTE;
		is->seek_req = 1;
		SDL_CondSignal(is->continue_read_thread);
	}
}

static void stream_toggle_pause(VideoState* is)
{
	if (is->paused)
	{
		is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
		if (is->read_pause_return != AVERROR(ENOSYS))
			is->vidclk.paused = 0;
		set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
	}
	set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
	is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState* is)
{
	stream_toggle_pause(is);
	is->step = 0;
}

static void toggle_mute(VideoState* is)
{
	is->muted = !is->muted;
}

static void update_volume(VideoState* is, int sign, double step)
{
	double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)SDL_MIN_MAXVOLUME) / log(10)) : -1000.0;
	int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
	is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, SDL_MIN_MAXVOLUME);
}

static void step_to_next_frame(VideoState* is)
{
	if (is->paused)
		stream_toggle_pause(is);
	is->step = 1;
}

static double compute_target_delay(double delay, VideoState* is)
{
	double sync_threhold, diff = 0;
	if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)
	{
		diff = get_clock(&is->vidclk) - get_master_clock(is);
		sync_threhold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
		if (!isnan(diff) && fabs(diff) < is->max_frame_duration)
		{
			if (diff <= -sync_threhold)
				delay = FFMAX(0, delay + diff);
			else if (diff >= sync_threhold && delay > AV_SYNC_FRAMEUP_THRESHOLD)
				delay += diff;
			else if (diff >= sync_threhold)
				delay *= 2;
		}
	}
	av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);
	return delay;
}

static double vp_duration(VideoState* is, Frame* vp, Frame* nextvp)
{
	if (vp->serial == nextvp->serial)
	{
		double duration = nextvp->pts - vp->pts;
		if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
			return vp->duration;
		else
			return duration;
	}
	else
		return 0.0;
}




























